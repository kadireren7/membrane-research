/*
 * membrane-kv-variance (Phase 4.3): investigates the root cause of the
 * real-vs-real measurement variance Phase 4.2 discovered (§10 of
 * docs/phase4-runtime-calibration.md) -- the SAME final policy, measured
 * twice through this project's own tools, produced different cosine
 * numbers, large enough to flip a zero-margin accept decision at the
 * threshold boundary.
 *
 * A code-level determinism audit (before any of this file was written)
 * found two concrete, real asymmetries between how the FP16 reference is
 * captured and how every candidate is evaluated in
 * tools/membrane-kv-runtime-optimizer/main.cpp:
 *   1. capture_baseline()'s context never sets flash_attn_type, so it
 *      stays at llama.cpp's default LLAMA_FLASH_ATTN_TYPE_AUTO; every
 *      eval_live() context passes a non-NULL kv_type_override callback,
 *      which unconditionally forces LLAMA_FLASH_ATTN_TYPE_ENABLED --
 *      even for a policy that maps every layer back to F16.
 *   2. capture_baseline() decodes the prompt as (prefix, last-token)
 *      across two separate llama_decode calls; eval_live() decodes the
 *      whole prompt in one llama_decode call.
 * Both are real, deterministic differences in how the reference and the
 * candidate are computed, independent of any quantization choice. This
 * tool makes both of them explicit, controllable parameters instead of
 * fixed/asymmetric choices, so their effect can be measured directly
 * (item 1's determinism audit, item 8's "apply a minimal fix if the root
 * cause is found").
 *
 * Modes (each a standalone experiment, see docs/phase4-runtime-variance.md
 * for what was actually measured with each):
 *   repeat        - run the SAME policy+prompt N times, report variance.
 *   threads       - repeat, swept across a list of thread counts.
 *   flashattn     - repeat, swept across {auto,disabled,enabled}, applied
 *                   SYMMETRICALLY to both the reference and the candidate
 *                   context, to isolate finding #1 above.
 *   quant-timing  - compares "quantize while writing" (native
 *                   kv_type_override, ggml's own quantize-on-write) vs
 *                   "quantize after the fact" (blob-splicing's
 *                   quant_roundtrip, membrane's own quantize function)
 *                   for the identical source values.
 *   drift         - token-by-token offline-vs-runtime comparison for one
 *                   policy, plus incremental per-slot attribution (which
 *                   slot addition grows the gap most).
 *   trace         - full diagnostic per-token trace: cosine, KL, max
 *                   logit delta, a KV-state checksum, and the active
 *                   policy's hash, so two runs can be diffed token by
 *                   token.
 */
#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "llama.h"
#include "membrane/f16convert.h"
#include "membrane/ggml_quant.h"
#include "membrane/hash.h"
#include "membrane/llama_commit.h"
#include "membrane/policy.h"

# define SEQ_STATE_MAGIC 0xaf143cd8u
# define GROUP_ELEMS 32
# define TOOL_VERSION "membrane-kv-variance-1.0"

static int	die(const char *msg)
{
	fprintf(stderr, "membrane-kv-variance: %s\n", msg);
	return (-1);
}

/* ------------------------------------------------------------------ */
/* Live progress reporting, copied unchanged from                       */
/* membrane-kv-runtime-optimizer/main.cpp -- item 7 asks that long runs */
/* keep the same live progress / 60s heartbeat this phase already has.  */
/* ------------------------------------------------------------------ */

typedef struct s_progress_state
{
	std::atomic<const char *>	stage;
	std::atomic<const char *>	tier;
	std::atomic<int>			cand_index;
	std::atomic<int>			cand_total;
	std::atomic<long long>		evals_done;
	std::atomic<double>			avg_eval_seconds;
	std::chrono::steady_clock::time_point	run_start;
}	progress_state_t;

static progress_state_t	g_progress;

static void	progress_init(void)
{
	g_progress.stage.store("starting");
	g_progress.tier.store("");
	g_progress.cand_index.store(0);
	g_progress.cand_total.store(0);
	g_progress.evals_done.store(0);
	g_progress.avg_eval_seconds.store(0.0);
	g_progress.run_start = std::chrono::steady_clock::now();
}

static double	progress_elapsed_seconds(void)
{
	return (std::chrono::duration<double>(std::chrono::steady_clock::now()
			- g_progress.run_start).count());
}

static void	progress_stage(const char *stage, const char *tier,
				int cand_total)
{
	g_progress.stage.store(stage);
	g_progress.tier.store(tier);
	g_progress.cand_index.store(0);
	g_progress.cand_total.store(cand_total);
	g_progress.evals_done.store(0);
	g_progress.avg_eval_seconds.store(0.0);
}

static void	progress_candidate_done(double seconds)
{
	long long	n;
	double		prev_avg;

	n = g_progress.evals_done.fetch_add(1) + 1;
	prev_avg = g_progress.avg_eval_seconds.load();
	g_progress.avg_eval_seconds.store(
		prev_avg + (seconds - prev_avg) / (double)n);
}

static void	progress_print_eta(double avg_eval_seconds, int done, int total)
{
	double	eta;

	if (avg_eval_seconds <= 0.0 || total <= done)
		return ;
	eta = avg_eval_seconds * (double)(total - done);
	fprintf(stderr, "  ETA ~%.0fs (avg %.1fs/candidate)\n", eta,
		avg_eval_seconds);
}

static void	heartbeat_loop(void)
{
	while (true)
	{
		std::this_thread::sleep_for(std::chrono::seconds(60));
		int		done;
		int		total;
		double	avg;
		double	eta;

		done = g_progress.cand_index.load();
		total = g_progress.cand_total.load();
		avg = g_progress.avg_eval_seconds.load();
		eta = (avg > 0.0 && total > done)
			? avg * (double)(total - done) : -1.0;
		fprintf(stderr, "  [heartbeat] elapsed %.0fs  stage=%s tier=%s  "
			"candidates %d/%d", progress_elapsed_seconds(),
			g_progress.stage.load(), g_progress.tier.load(), done, total);
		if (eta >= 0.0)
			fprintf(stderr, "  ETA ~%.0fs\n", eta);
		else
			fprintf(stderr, "\n");
	}
}

/* ------------------------------------------------------------------ */
/* Blob-splicing primitives (Phase 3.3, copied unchanged) -- needed for */
/* the quant-timing and drift modes' "post-hoc quantize" path.          */
/* ------------------------------------------------------------------ */

typedef struct s_cursor
{
	const uint8_t	*p;
	size_t			left;
	size_t			base_off;
}	cursor_t;

static int	cur_read(cursor_t *c, void *out, size_t n)
{
	if (c->left < n)
		return (-1);
	memcpy(out, c->p, n);
	c->p += n;
	c->left -= n;
	c->base_off += n;
	return (0);
}

static int	cur_skip(cursor_t *c, size_t n)
{
	if (c->left < n)
		return (-1);
	c->p += n;
	c->left -= n;
	c->base_off += n;
	return (0);
}

typedef struct s_layer_slot
{
	size_t		k_offset;
	size_t		k_row_size;
	size_t		v_offset;
	size_t		v_row_size;
}	layer_slot_t;

typedef struct s_blob_index
{
	uint32_t					cell_count;
	uint32_t					n_layer;
	std::vector<layer_slot_t>	layers;
}	blob_index_t;

static int	skip_cell_meta(cursor_t *c, uint32_t cell_count)
{
	uint32_t	i;
	uint32_t	n_seq_id;

	i = 0;
	while (i < cell_count)
	{
		if (cur_skip(c, 4) != 0 || cur_read(c, &n_seq_id, 4) != 0)
			return (die("truncated cell meta"));
		if (n_seq_id > 64)
			return (die("implausible n_seq_id: layout mismatch?"));
		if (cur_skip(c, (size_t)n_seq_id * 4) != 0)
			return (die("truncated seq id list"));
		i++;
	}
	return (0);
}

static int	index_rows(cursor_t *c, uint32_t cell_count, size_t *out_offset,
				size_t *out_row_size)
{
	int32_t		elem_type;
	uint64_t	size_row;
	uint64_t	total;

	if (cur_read(c, &elem_type, 4) != 0 || cur_read(c, &size_row, 8) != 0)
		return (die("truncated tensor header"));
	if (size_row == 0 || size_row > (1u << 20) || size_row % 2 != 0)
		return (die("implausible row size: layout mismatch?"));
	total = size_row * cell_count;
	if (c->left < total)
		return (die("truncated tensor payload"));
	*out_offset = c->base_off;
	*out_row_size = (size_t)size_row;
	return (cur_skip(c, total));
}

static int	parse_prologue(cursor_t *c, uint32_t *cell_count)
{
	uint32_t	magic;
	int32_t		seq;
	uint32_t	n_stream;

	if (cur_read(c, &magic, 4) != 0 || magic != SEQ_STATE_MAGIC)
		return (die("bad state magic: llama.cpp layout changed"));
	if (cur_read(c, &seq, 4) != 0 || cur_read(c, &n_stream, 4) != 0)
		return (die("truncated state prologue"));
	if (n_stream != 1)
		return (die("multi-stream KV not supported by this tool"));
	if (cur_read(c, cell_count, 4) != 0 || *cell_count == 0)
		return (die("empty KV cache"));
	if (*cell_count > (1u << 24))
		return (die("implausible cell count: layout mismatch?"));
	return (skip_cell_meta(c, *cell_count));
}

static bool	parse_blob(const uint8_t *blob, size_t size, blob_index_t *idx)
{
	cursor_t		c;
	uint32_t		v_trans;
	uint32_t		il;
	layer_slot_t	slot;

	c.p = blob;
	c.left = size;
	c.base_off = 0;
	if (parse_prologue(&c, &idx->cell_count) != 0)
		return (false);
	if (cur_read(&c, &v_trans, 4) != 0 || cur_read(&c, &idx->n_layer, 4) != 0)
		return (die("truncated data prologue"), false);
	if (v_trans != 0)
		return (die("V cache is transposed (flash attention disabled?) -- "
				"this tool only supports the row-major layout"), false);
	if (idx->n_layer == 0 || idx->n_layer > 512)
		return (die("implausible layer count: layout mismatch?"), false);
	idx->layers.assign(idx->n_layer, layer_slot_t());
	il = 0;
	while (il < idx->n_layer)
	{
		if (index_rows(&c, idx->cell_count, &slot.k_offset,
				&slot.k_row_size) != 0)
			return (false);
		idx->layers[il].k_offset = slot.k_offset;
		idx->layers[il].k_row_size = slot.k_row_size;
		il++;
	}
	il = 0;
	while (il < idx->n_layer)
	{
		if (index_rows(&c, idx->cell_count, &slot.v_offset,
				&slot.v_row_size) != 0)
			return (false);
		idx->layers[il].v_offset = slot.v_offset;
		idx->layers[il].v_row_size = slot.v_row_size;
		il++;
	}
	return (true);
}

/* Phase 3.3's own linear per-32-element max-abs quantizer. Phase 4.4
 * (docs/phase4-ggml-quant-parity.md) found this does not match ggml's
 * real Q8_0/Q4_0 block format or rounding, and every OTHER tool that
 * used it has since switched to membrane_ggml_quant_roundtrip (the real
 * ggml math). It stays here, actively used, ONLY because this specific
 * tool's --mode quant-timing exists to measure the gap between the two
 * -- see eval_offline's `ggml_exact` parameter below. */
static void	quant_roundtrip_group_LEGACY(uint16_t *elems, size_t n,
					int bits)
{
	int		qmax;
	float	max_abs;
	float	scale;
	float	v;
	long	q;
	size_t	i;

	qmax = (1 << (bits - 1)) - 1;
	max_abs = 0.0f;
	i = 0;
	while (i < n)
	{
		v = fabsf(membrane_f16_to_f32(elems[i]));
		if (v > max_abs)
			max_abs = v;
		i++;
	}
	scale = (max_abs > 0.0f) ? max_abs / (float)qmax : 0.0f;
	i = 0;
	while (i < n)
	{
		if (scale > 0.0f)
		{
			q = lroundf(membrane_f16_to_f32(elems[i]) / scale);
			if (q > qmax)
				q = qmax;
			if (q < -qmax)
				q = -qmax;
			elems[i] = membrane_f32_to_f16((float)q * scale);
		}
		else
			elems[i] = membrane_f32_to_f16(0.0f);
		i++;
	}
}

static void	quant_roundtrip_inplace_LEGACY(uint8_t *data, size_t len,
					int bits)
{
	size_t	elements;
	size_t	off;
	size_t	n;

	if (bits == 16)
		return ;
	elements = len / 2;
	off = 0;
	while (off < elements)
	{
		n = elements - off;
		if (n > GROUP_ELEMS)
			n = GROUP_ELEMS;
		quant_roundtrip_group_LEGACY((uint16_t *)(void *)(data + off * 2), n,
			bits);
		off += n;
	}
}

/* `ggml_exact` selects which quantize math apply_targets uses: true (the
 * default everywhere except --mode quant-timing's legacy arm) calls
 * membrane_ggml_quant_roundtrip -- ggml's own Q8_0/Q4_0 math, see
 * membrane/ggml_quant.h; false calls the retired LEGACY function above,
 * kept only so --mode quant-timing can measure the difference it makes
 * (docs/phase4-ggml-quant-parity.md item 6's regression benchmark). */
static void	quant_roundtrip_inplace(uint8_t *data, size_t len, int bits,
				bool ggml_exact)
{
	if (bits == 16)
		return ;
	if (ggml_exact)
		membrane_ggml_quant_roundtrip((uint16_t *)(void *)data, len / 2,
			bits);
	else
		quant_roundtrip_inplace_LEGACY(data, len, bits);
}

typedef struct s_perturb_target
{
	int			layer;
	bool		do_k;
	bool		do_v;
	uint32_t	row_start;
	uint32_t	row_end;
	int			bits;
}	perturb_target_t;

static void	apply_targets(uint8_t *blob, const blob_index_t &idx,
				const std::vector<perturb_target_t> &targets, bool ggml_exact)
{
	uint32_t	r;

	for (const perturb_target_t &t : targets)
	{
		if (t.bits == 16 || t.layer < 0 || (uint32_t)t.layer >= idx.n_layer)
			continue ;
		const layer_slot_t &ls = idx.layers[t.layer];
		r = t.row_start;
		while (r < t.row_end && r < idx.cell_count)
		{
			if (t.do_k)
				quant_roundtrip_inplace(blob + ls.k_offset
						+ (size_t)r * ls.k_row_size, ls.k_row_size, t.bits,
					ggml_exact);
			if (t.do_v)
				quant_roundtrip_inplace(blob + ls.v_offset
						+ (size_t)r * ls.v_row_size, ls.v_row_size, t.bits,
					ggml_exact);
			r++;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Context/model helpers. n_threads and flash_attn are explicit,        */
/* controllable parameters here (item 1's determinism-audit finding):   */
/* the optimizer tool leaves them asymmetric between the reference and  */
/* the candidate; this tool lets every mode choose deliberately.        */
/* ------------------------------------------------------------------ */

typedef struct s_ctx_cfg
{
	int						n_threads;
	llama_flash_attn_type	flash_attn;
}	ctx_cfg_t;

static ctx_cfg_t	default_ctx_cfg(void)
{
	ctx_cfg_t	c;

	c.n_threads = 4;
	c.flash_attn = LLAMA_FLASH_ATTN_TYPE_AUTO;
	return (c);
}

static std::string	read_file(const char *path)
{
	FILE		*f;
	std::string	s;
	char		buf[4096];
	size_t		n;

	f = fopen(path, "rb");
	if (f == NULL)
		return (s);
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		s.append(buf, n);
	fclose(f);
	return (s);
}

static bool	tokenize_prompt(const llama_vocab *vocab, const char *path,
				std::vector<llama_token> *out)
{
	std::string	text;
	int			n;

	text = read_file(path);
	if (text.empty())
		return (die("empty or unreadable prompt file"), false);
	out->resize(text.size() + 8);
	n = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(),
			out->data(), (int32_t)out->size(), true, false);
	if (n < 0)
		return (die("tokenization failed"), false);
	out->resize(n);
	return (true);
}

static llama_context	*make_context(llama_model *model, int n_ctx,
						ggml_type type_k, ggml_type type_v,
						const ctx_cfg_t &cfg,
						ggml_type (*override_cb)(int32_t, bool, void *) = NULL,
						void *override_ud = NULL)
{
	llama_context_params	cp;

	cp = llama_context_default_params();
	cp.n_ctx = (uint32_t)n_ctx;
	cp.n_batch = 256;
	cp.n_threads = cfg.n_threads;
	cp.n_threads_batch = cfg.n_threads;
	cp.type_k = type_k;
	cp.type_v = type_v;
	cp.no_perf = true;
	cp.kv_type_override = override_cb;
	cp.kv_type_override_ud = override_ud;
	cp.flash_attn_type = cfg.flash_attn;
	if (cfg.flash_attn == LLAMA_FLASH_ATTN_TYPE_AUTO
			&& (type_k != GGML_TYPE_F16 || type_v != GGML_TYPE_F16
				|| override_cb != NULL))
		cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
	return (llama_init_from_model(model, cp));
}

typedef struct s_prompt_decode_mode
{
	bool	split_last_token;	/* true: capture_baseline's shape (prefix
								 * batch + separate 1-token decode); false:
								 * eval_live's shape (one whole-prompt
								 * decode). Item 1 finding #2. */
}	prompt_decode_mode_t;

static bool	decode_prompt_batched(llama_context *ctx,
				const std::vector<llama_token> &tokens, int n_batch)
{
	size_t	off;
	size_t	n;

	off = 0;
	while (off < tokens.size())
	{
		n = tokens.size() - off;
		if (n > (size_t)n_batch)
			n = (size_t)n_batch;
		if (llama_decode(ctx,
				llama_batch_get_one((llama_token *)tokens.data() + off,
					(int32_t)n)) != 0)
			return (die("llama_decode (prompt) failed"), false);
		off += n;
	}
	return (true);
}

static bool	decode_one(llama_context *ctx, llama_token tok)
{
	return (llama_decode(ctx, llama_batch_get_one(&tok, 1)) == 0);
}

static bool	decode_prompt_shaped(llama_context *ctx,
				const std::vector<llama_token> &tokens,
				const prompt_decode_mode_t &mode)
{
	std::vector<llama_token>	prefix;

	if (!mode.split_last_token || tokens.size() < 2)
		return (decode_prompt_batched(ctx, tokens, 256));
	prefix.assign(tokens.begin(), tokens.end() - 1);
	return (decode_prompt_batched(ctx, prefix, 256)
		&& decode_one(ctx, tokens.back()));
}

static int	argmax(const float *v, int n)
{
	int	best;
	int	i;

	best = 0;
	for (i = 1; i < n; i++)
		if (v[i] > v[best])
			best = i;
	return (best);
}

typedef struct s_pass_result
{
	std::vector<llama_token>			tokens;
	std::vector<std::vector<float>>	logits;
}	pass_result_t;

static bool	run_gen(llama_context *ctx, const llama_vocab *vocab,
				int gen_steps, const std::vector<llama_token> *forced,
				pass_result_t *out)
{
	llama_token	tok;
	const float	*logits;
	int32_t		n_vocab;
	int			i;

	n_vocab = llama_vocab_n_tokens(vocab);
	i = 0;
	while (i < gen_steps)
	{
		logits = llama_get_logits_ith(ctx, -1);
		if (logits == NULL)
			return (die("llama_get_logits_ith failed"), false);
		out->logits.emplace_back(logits, logits + n_vocab);
		if (forced != NULL && i < (int)forced->size())
			tok = (*forced)[i];
		else
			tok = argmax(logits, n_vocab);
		out->tokens.push_back(tok);
		if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0)
			return (die("llama_decode (gen) failed"), false);
		i++;
	}
	return (true);
}

static std::string	tokens_to_text(const llama_vocab *vocab,
						const std::vector<llama_token> &toks)
{
	std::string	out;
	char		buf[256];
	int			n;

	for (llama_token t : toks)
	{
		n = llama_token_to_piece(vocab, t, buf, sizeof(buf), 0, true);
		if (n > 0)
			out.append(buf, n);
	}
	return (out);
}

static bool	in_top5(const std::vector<float> &logits, int token)
{
	int	rank;
	int	i;

	rank = 0;
	for (i = 0; i < (int)logits.size(); i++)
		if (logits[i] > logits[token])
			rank++;
	return (rank < 5);
}

static void	compare_step(const std::vector<float> &base,
				const std::vector<float> &cand, double *cos_sum,
				double *kl_sum, uint64_t *top1, uint64_t *top5,
				double *out_cos, double *out_max_delta, double *out_kl)
{
	double	dot;
	double	na;
	double	nb;
	double	mx;
	double	sum_pb;
	double	sum_pc;
	double	step_cos;
	double	max_delta;
	double	step_kl;
	std::vector<double>	pb;
	std::vector<double>	pc;
	size_t	i;
	int		base_top1;

	dot = 0.0;
	na = 0.0;
	nb = 0.0;
	max_delta = 0.0;
	for (i = 0; i < base.size(); i++)
	{
		double	d = fabs((double)base[i] - (double)cand[i]);

		if (d > max_delta)
			max_delta = d;
		dot += (double)base[i] * (double)cand[i];
		na += (double)base[i] * (double)base[i];
		nb += (double)cand[i] * (double)cand[i];
	}
	if (out_max_delta != NULL)
		*out_max_delta = max_delta;
	if (na > 0.0 && nb > 0.0)
		step_cos = dot / (sqrt(na) * sqrt(nb));
	else
		step_cos = (na == 0.0 && nb == 0.0) ? 1.0 : 0.0;
	*cos_sum += step_cos;
	if (out_cos != NULL)
		*out_cos = step_cos;
	mx = base[0];
	for (float v : base)
		if (v > mx)
			mx = v;
	pb.resize(base.size());
	pc.resize(base.size());
	sum_pb = 0.0;
	sum_pc = 0.0;
	for (i = 0; i < base.size(); i++)
	{
		pb[i] = exp((double)base[i] - mx);
		pc[i] = exp((double)cand[i] - mx);
		sum_pb += pb[i];
		sum_pc += pc[i];
	}
	step_kl = 0.0;
	for (i = 0; i < base.size(); i++)
	{
		pb[i] /= sum_pb;
		pc[i] /= sum_pc;
		if (pb[i] > 0.0)
			step_kl += pb[i] * log(pb[i] / (pc[i] > 1e-300 ? pc[i] : 1e-300));
	}
	*kl_sum += step_kl;
	if (out_kl != NULL)
		*out_kl = step_kl;
	base_top1 = argmax(base.data(), (int)base.size());
	*top1 += (base_top1 == argmax(cand.data(), (int)cand.size()));
	*top5 += in_top5(cand, base_top1);
}

typedef struct s_metrics
{
	double				top1_pct;
	double				top5_pct;
	double				logit_cosine;
	double				kl_mean;
	long				first_divergence;
	bool				recall_ok;
	std::string			text;
	std::vector<double>	per_step_cosine;
	std::vector<double>	per_step_max_delta;
	std::vector<double>	per_step_kl;
}	metrics_t;

static void	fill_metrics(const llama_vocab *vocab, int gen_tokens,
				const pass_result_t &ref_free, const pass_result_t &free_run,
				const pass_result_t &forced_run, const char *answer,
				metrics_t *m)
{
	double		cos_sum;
	double		kl_sum;
	uint64_t	top1;
	uint64_t	top5;
	size_t		steps;
	size_t		i;
	double		step_cos;
	double		step_max_delta;
	double		step_kl;

	cos_sum = 0.0;
	kl_sum = 0.0;
	top1 = 0;
	top5 = 0;
	steps = ref_free.logits.size() < forced_run.logits.size()
		? ref_free.logits.size() : forced_run.logits.size();
	m->per_step_cosine.resize(steps);
	m->per_step_max_delta.resize(steps);
	m->per_step_kl.resize(steps);
	for (i = 0; i < steps; i++)
	{
		compare_step(ref_free.logits[i], forced_run.logits[i], &cos_sum,
			&kl_sum, &top1, &top5, &step_cos, &step_max_delta, &step_kl);
		m->per_step_cosine[i] = step_cos;
		m->per_step_max_delta[i] = step_max_delta;
		m->per_step_kl[i] = step_kl;
	}
	m->top1_pct = steps ? 100.0 * (double)top1 / (double)steps : 0.0;
	m->top5_pct = steps ? 100.0 * (double)top5 / (double)steps : 0.0;
	m->logit_cosine = steps ? cos_sum / (double)steps : 0.0;
	m->kl_mean = steps ? kl_sum / (double)steps : 0.0;
	m->first_divergence = (long)gen_tokens;
	for (i = 0; i < ref_free.tokens.size() && i < free_run.tokens.size(); i++)
		if (ref_free.tokens[i] != free_run.tokens[i])
		{
			m->first_divergence = (long)i;
			break ;
		}
	m->text = tokens_to_text(vocab, free_run.tokens);
	m->recall_ok = (answer == NULL)
		|| (m->text.find(answer) != std::string::npos);
}

/* ------------------------------------------------------------------ */
/* Baseline capture -- n_threads/flash_attn/decode-shape are explicit   */
/* parameters here (unlike the optimizer's hardcoded, asymmetric ones). */
/* ------------------------------------------------------------------ */

typedef struct s_baseline
{
	std::vector<llama_token>	prompt_tokens;
	llama_token					last_token;
	pass_result_t				free_run;
	std::vector<uint8_t>		blob;
	blob_index_t				idx;
	std::string					text;
	std::string					name;
	const char					*answer;
}	baseline_t;

static bool	capture_baseline(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const char *prompt_path,
				const char *answer, const ctx_cfg_t &cfg,
				const prompt_decode_mode_t &shape, baseline_t *out)
{
	llama_context	*ctx;
	size_t			blob_size;
	bool			ok;

	if (!tokenize_prompt(vocab, prompt_path, &out->prompt_tokens)
		|| out->prompt_tokens.empty())
		return (die("prompt tokenized to zero tokens"), false);
	out->name = prompt_path;
	out->last_token = out->prompt_tokens.back();
	ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16, cfg);
	if (ctx == NULL)
		return (die("baseline context creation failed"), false);
	ok = decode_prompt_shaped(ctx, out->prompt_tokens, shape);
	if (ok)
	{
		blob_size = llama_state_seq_get_size(ctx, 0);
		out->blob.resize(blob_size);
		llama_state_seq_get_data(ctx, out->blob.data(), blob_size, 0);
		/* A non-row-major V layout (flash attention disabled) can't be
		 * parsed by this tool's blob format -- that is itself a real,
		 * disclosed determinism-audit finding (see mode_flashattn), not
		 * fatal here: eval_offline/quant-timing/drift are the only
		 * consumers of out->idx, and modes that don't touch them (repeat,
		 * threads, flashattn, trace) must still be able to run. */
		if (!parse_blob(out->blob.data(), out->blob.size(), &out->idx))
			out->idx = blob_index_t();
	}
	if (ok)
		ok = run_gen(ctx, vocab, gen_tokens, NULL, &out->free_run);
	if (ok)
	{
		out->text = tokens_to_text(vocab, out->free_run.tokens);
		out->answer = answer;
	}
	llama_free(ctx);
	return (ok);
}

/* ------------------------------------------------------------------ */
/* Policy representation, and the two evaluation backends. Both take an */
/* explicit ctx_cfg_t/shape now, applied to BOTH the reference re-decode */
/* (where relevant) and the candidate -- item 1's fix-ready surface.    */
/* ------------------------------------------------------------------ */

typedef struct s_kv_policy
{
	std::vector<int>	kbits;
	std::vector<int>	vbits;
}	kv_policy_t;

static kv_policy_t	all_bits_policy(uint32_t n_layer, int bits)
{
	kv_policy_t	p;

	p.kbits.assign(n_layer, bits);
	p.vbits.assign(n_layer, bits);
	return (p);
}

static std::vector<perturb_target_t>	policy_targets(const blob_index_t &idx,
											const kv_policy_t &pol)
{
	std::vector<perturb_target_t>	out;
	uint32_t						l;

	l = 0;
	while (l < idx.n_layer && l < pol.kbits.size())
	{
		perturb_target_t	t;

		t.layer = (int)l;
		t.row_start = 0;
		t.row_end = idx.cell_count;
		if (pol.kbits[l] != 16)
		{
			t.do_k = true;
			t.do_v = false;
			t.bits = pol.kbits[l];
			out.push_back(t);
		}
		if (pol.vbits[l] != 16)
		{
			t.do_k = false;
			t.do_v = true;
			t.bits = pol.vbits[l];
			out.push_back(t);
		}
		l++;
	}
	return (out);
}

static ggml_type	bits_to_ggml(int bits)
{
	if (bits == 4)
		return (GGML_TYPE_Q4_0);
	if (bits == 8)
		return (GGML_TYPE_Q8_0);
	return (GGML_TYPE_F16);
}

typedef struct s_kv_policy_cb_ctx
{
	const kv_policy_t	*policy;
}	kv_policy_cb_ctx_t;

static ggml_type	kv_policy_type_cb(int32_t il, bool is_v, void *ud)
{
	kv_policy_cb_ctx_t	*c;

	c = (kv_policy_cb_ctx_t *)ud;
	if (il < 0 || (size_t)il >= c->policy->kbits.size())
		return (GGML_TYPE_F16);
	return (bits_to_ggml(is_v ? c->policy->vbits[il] : c->policy->kbits[il]));
}

/* EVAL_OFFLINE_BLOB path: quantize-after-the-fact, membrane's own
 * quant_roundtrip applied to an already-captured F16 blob (item 6's
 * "post-hoc" arm). */
static bool	eval_offline(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &pol, const ctx_cfg_t &cfg,
				bool ggml_exact, metrics_t *m)
{
	std::vector<uint8_t>	blob;
	llama_context			*free_ctx;
	llama_context			*forced_ctx;
	pass_result_t			free_run;
	pass_result_t			forced_run;
	std::vector<perturb_target_t>	targets;
	bool					ok;

	blob = base.blob;
	targets = policy_targets(base.idx, pol);
	apply_targets(blob.data(), base.idx, targets, ggml_exact);
	free_ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16, cfg);
	forced_ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16,
			cfg);
	ok = free_ctx != NULL && forced_ctx != NULL;
	if (ok)
		ok = llama_state_seq_set_data(free_ctx, blob.data(), blob.size(),
				0) > 0 && decode_one(free_ctx, base.last_token);
	if (ok)
		ok = run_gen(free_ctx, vocab, gen_tokens, NULL, &free_run);
	if (ok)
		ok = llama_state_seq_set_data(forced_ctx, blob.data(), blob.size(),
				0) > 0 && decode_one(forced_ctx, base.last_token);
	if (ok)
		ok = run_gen(forced_ctx, vocab, gen_tokens, &base.free_run.tokens,
				&forced_run);
	if (ok)
		fill_metrics(vocab, gen_tokens, base.free_run, free_run, forced_run,
			base.answer, m);
	llama_free(free_ctx);
	llama_free(forced_ctx);
	return (ok);
}

typedef struct s_live_result
{
	metrics_t	m;
	size_t		kv_bytes;
	double		ttft_ms;
	double		tok_per_sec;
	std::string	kv_checksum_hex;
}	live_result_t;

static void	hash_bytes_hex(const uint8_t *data, size_t len,
				char out_hex[MEMBRANE_SHA256_HEX_LEN + 1])
{
	uint8_t	digest[MEMBRANE_SHA256_DIGEST_BYTES];

	membrane_sha256(data, len, digest);
	membrane_sha256_to_hex(digest, out_hex);
}

/* EVAL_LIVE_RUNTIME path: quantize-while-writing, ggml's own native
 * per-layer KV cache quantization via kv_type_override (item 6's
 * "native" arm; also the sole real-acceptance backend Phase 4.2 built). */
static bool	eval_live(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &pol, const ctx_cfg_t &cfg,
				const prompt_decode_mode_t &shape, live_result_t *out)
{
	kv_policy_cb_ctx_t						cb_ctx;
	llama_context							*free_ctx;
	llama_context							*forced_ctx;
	pass_result_t							free_run;
	pass_result_t							forced_run;
	std::chrono::steady_clock::time_point	t0;
	std::chrono::steady_clock::time_point	t_first;
	std::chrono::steady_clock::time_point	t_gen_done;
	bool									ok;

	cb_ctx.policy = &pol;
	free_ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16, cfg,
			kv_policy_type_cb, &cb_ctx);
	forced_ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16,
			cfg, kv_policy_type_cb, &cb_ctx);
	ok = free_ctx != NULL && forced_ctx != NULL;
	if (ok)
	{
		t0 = std::chrono::steady_clock::now();
		ok = decode_prompt_shaped(free_ctx, base.prompt_tokens, shape);
	}
	if (ok)
	{
		out->kv_bytes = llama_state_seq_get_size(free_ctx, 0);
		std::vector<uint8_t>	kv_snapshot(out->kv_bytes);
		char					hex[MEMBRANE_SHA256_HEX_LEN + 1];

		llama_state_seq_get_data(free_ctx, kv_snapshot.data(), out->kv_bytes,
			0);
		hash_bytes_hex(kv_snapshot.data(), kv_snapshot.size(), hex);
		out->kv_checksum_hex = hex;
		ok = run_gen(free_ctx, vocab, 1, NULL, &free_run);
		t_first = std::chrono::steady_clock::now();
	}
	if (ok && gen_tokens > 1)
	{
		pass_result_t	rest;

		ok = run_gen(free_ctx, vocab, gen_tokens - 1, NULL, &rest);
		free_run.tokens.insert(free_run.tokens.end(), rest.tokens.begin(),
			rest.tokens.end());
		free_run.logits.insert(free_run.logits.end(), rest.logits.begin(),
			rest.logits.end());
	}
	t_gen_done = std::chrono::steady_clock::now();
	if (ok)
		ok = decode_prompt_shaped(forced_ctx, base.prompt_tokens, shape)
			&& run_gen(forced_ctx, vocab, gen_tokens, &base.free_run.tokens,
				&forced_run);
	if (ok)
	{
		out->ttft_ms = std::chrono::duration<double, std::milli>(
				t_first - t0).count();
		out->tok_per_sec = gen_tokens > 1 ? (double)(gen_tokens - 1)
			/ std::chrono::duration<double>(t_gen_done - t_first).count()
			: 0.0;
		fill_metrics(vocab, gen_tokens, base.free_run, free_run, forced_run,
			base.answer, &out->m);
	}
	llama_free(free_ctx);
	llama_free(forced_ctx);
	return (ok);
}

/* ------------------------------------------------------------------ */
/* Policy hashing -- item 7's "active policy hash", so two trace runs   */
/* can be matched to the exact same precision map without printing the  */
/* whole per-layer table every time.                                    */
/* ------------------------------------------------------------------ */

static std::string	policy_hash_hex(const kv_policy_t &pol)
{
	std::vector<uint8_t>	buf;
	char					hex[MEMBRANE_SHA256_HEX_LEN + 1];
	size_t					i;

	buf.resize(pol.kbits.size() + pol.vbits.size());
	i = 0;
	while (i < pol.kbits.size())
	{
		buf[i] = (uint8_t)pol.kbits[i];
		i++;
	}
	i = 0;
	while (i < pol.vbits.size())
	{
		buf[pol.kbits.size() + i] = (uint8_t)pol.vbits[i];
		i++;
	}
	hash_bytes_hex(buf.data(), buf.size(), hex);
	return (hex);
}

static bool	load_policy_spec(const char *spec, uint32_t n_layer,
				kv_policy_t *out)
{
	if (strcmp(spec, "all-f16") == 0)
	{
		*out = all_bits_policy(n_layer, 16);
		return (true);
	}
	if (strcmp(spec, "all-q8") == 0)
	{
		*out = all_bits_policy(n_layer, 8);
		return (true);
	}
	if (strcmp(spec, "all-q4") == 0)
	{
		*out = all_bits_policy(n_layer, 4);
		return (true);
	}
	{
		membrane_policy_t	*p;
		bool				ok;
		uint32_t			l;

		if (membrane_policy_load(spec, &p) != MEMBRANE_OK)
			return (die("--policy: not a preset (all-f16/all-q8/all-q4) "
					"and failed to load as a .mpol file"), false);
		out->kbits.resize(n_layer);
		out->vbits.resize(n_layer);
		ok = (membrane_policy_layer_count(p) == n_layer);
		l = 0;
		while (ok && l < n_layer)
		{
			membrane_precision_t	pk;
			membrane_precision_t	pv;

			ok = membrane_policy_query(p, l, 0, &pk) == MEMBRANE_OK
				&& membrane_policy_query(p, l, 1, &pv) == MEMBRANE_OK;
			if (ok)
			{
				out->kbits[l] = (int)pk;
				out->vbits[l] = (int)pv;
			}
			l++;
		}
		membrane_policy_destroy(p);
		if (!ok)
			return (die("--policy: layer count mismatch or query failure"),
				false);
		return (true);
	}
}

/* ------------------------------------------------------------------ */
/* Statistics helper, shared by every repeat-based mode (items 2, 3).   */
/* ------------------------------------------------------------------ */

typedef struct s_stat
{
	double	min;
	double	max;
	double	mean;
	double	stddev;
}	stat_t;

static stat_t	compute_stat(const std::vector<double> &v)
{
	stat_t	s;
	double	sum;
	double	var;

	s.min = v.empty() ? 0.0 : v[0];
	s.max = v.empty() ? 0.0 : v[0];
	sum = 0.0;
	for (double x : v)
	{
		if (x < s.min)
			s.min = x;
		if (x > s.max)
			s.max = x;
		sum += x;
	}
	s.mean = v.empty() ? 0.0 : sum / (double)v.size();
	var = 0.0;
	for (double x : v)
		var += (x - s.mean) * (x - s.mean);
	s.stddev = v.size() > 1 ? sqrt(var / (double)(v.size() - 1)) : 0.0;
	return (s);
}

static void	print_stat(const char *label, const stat_t &s)
{
	fprintf(stderr, "    %-14s min %.8f  max %.8f  mean %.8f  stddev "
		"%.10f\n", label, s.min, s.max, s.mean, s.stddev);
}

/* ------------------------------------------------------------------ */
/* One repeated-measurement run: the SAME baseline+policy+cfg+shape     */
/* evaluated `repeats` times via EVAL_LIVE_RUNTIME. Shared by items 2   */
/* (repeat mode) and 3 (thread-sweep mode, which calls this once per    */
/* thread count).                                                       */
/* ------------------------------------------------------------------ */

typedef struct s_repeat_result
{
	std::vector<double>		cosine;
	std::vector<double>		top1;
	std::vector<double>		top5;
	std::vector<double>		kl;
	std::vector<double>		first_div;
	std::vector<double>		kv_bytes;
	std::vector<double>		tok_per_sec;
	std::vector<double>		ttft_ms;
	std::vector<std::string>	texts;
	std::vector<std::string>	kv_checksums;
}	repeat_result_t;

static bool	run_repeated(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &pol, const ctx_cfg_t &cfg,
				const prompt_decode_mode_t &shape, int repeats,
				repeat_result_t *out)
{
	int	i;

	i = 0;
	while (i < repeats)
	{
		live_result_t							r;
		std::chrono::steady_clock::time_point	t0;

		t0 = std::chrono::steady_clock::now();
		g_progress.cand_index.store(i + 1);
		if (!eval_live(model, vocab, n_ctx, gen_tokens, base, pol, cfg, shape,
				&r))
			return (false);
		progress_candidate_done(std::chrono::duration<double>(
			std::chrono::steady_clock::now() - t0).count());
		fprintf(stderr, "    run %2d/%d: cosine %.8f top1 %.4f%% top5 "
			"%.4f%% kl %.8f first_div %ld kv_bytes %zu tok/s %.2f ttft "
			"%.1fms checksum %.8s\n", i + 1, repeats, r.m.logit_cosine,
			r.m.top1_pct, r.m.top5_pct, r.m.kl_mean, r.m.first_divergence,
			r.kv_bytes, r.tok_per_sec, r.ttft_ms,
			r.kv_checksum_hex.c_str());
		out->cosine.push_back(r.m.logit_cosine);
		out->top1.push_back(r.m.top1_pct);
		out->top5.push_back(r.m.top5_pct);
		out->kl.push_back(r.m.kl_mean);
		out->first_div.push_back((double)r.m.first_divergence);
		out->kv_bytes.push_back((double)r.kv_bytes);
		out->tok_per_sec.push_back(r.tok_per_sec);
		out->ttft_ms.push_back(r.ttft_ms);
		out->texts.push_back(r.m.text);
		out->kv_checksums.push_back(r.kv_checksum_hex);
		progress_print_eta(g_progress.avg_eval_seconds.load(), i + 1,
			repeats);
		i++;
	}
	return (true);
}

static void	report_repeated(const repeat_result_t &r)
{
	bool	text_stable;
	bool	checksum_stable;
	size_t	i;

	fprintf(stderr, "\n  aggregate over %zu runs:\n", r.cosine.size());
	print_stat("cosine", compute_stat(r.cosine));
	print_stat("top1_pct", compute_stat(r.top1));
	print_stat("top5_pct", compute_stat(r.top5));
	print_stat("kl", compute_stat(r.kl));
	print_stat("first_div", compute_stat(r.first_div));
	print_stat("kv_bytes", compute_stat(r.kv_bytes));
	print_stat("tok_per_sec", compute_stat(r.tok_per_sec));
	print_stat("ttft_ms", compute_stat(r.ttft_ms));
	text_stable = true;
	checksum_stable = true;
	i = 1;
	while (i < r.texts.size())
	{
		if (r.texts[i] != r.texts[0])
			text_stable = false;
		if (r.kv_checksums[i] != r.kv_checksums[0])
			checksum_stable = false;
		i++;
	}
	fprintf(stderr, "  generated text identical across all runs: %s\n",
		text_stable ? "YES" : "NO");
	fprintf(stderr, "  KV-state checksum identical across all runs: %s\n",
		checksum_stable ? "YES" : "NO");
	if (!text_stable)
	{
		i = 0;
		while (i < r.texts.size())
		{
			fprintf(stderr, "    run %zu text: %s\n", i + 1,
				r.texts[i].c_str());
			i++;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Mode: repeat (item 2). Same policy+prompt, >=20 times by default.    */
/* ------------------------------------------------------------------ */

static bool	mode_repeat(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &pol, int repeats)
{
	ctx_cfg_t				cfg;
	prompt_decode_mode_t	shape;
	repeat_result_t			r;

	cfg = default_ctx_cfg();
	shape.split_last_token = false;
	fprintf(stderr, "\n=== mode: repeat (%d runs, n_threads=%d, "
		"flash_attn=%s, whole-prompt decode) ===\n", repeats, cfg.n_threads,
		llama_flash_attn_type_name(cfg.flash_attn));
	progress_stage("repeat", "", repeats);
	if (!run_repeated(model, vocab, n_ctx, gen_tokens, base, pol, cfg, shape,
			repeats, &r))
		return (false);
	report_repeated(r);
	return (true);
}

/* ------------------------------------------------------------------ */
/* Mode: threads (item 3). Same experiment at 1/2/4 threads.            */
/* ------------------------------------------------------------------ */

static bool	mode_threads(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &pol, int repeats,
				const std::vector<int> &thread_counts)
{
	prompt_decode_mode_t		shape;
	std::vector<repeat_result_t>	all;

	shape.split_last_token = false;
	progress_stage("threads", "", (int)thread_counts.size() * repeats);
	for (int nt : thread_counts)
	{
		ctx_cfg_t		cfg;
		repeat_result_t	r;

		cfg.n_threads = nt;
		cfg.flash_attn = LLAMA_FLASH_ATTN_TYPE_AUTO;
		fprintf(stderr, "\n=== mode: threads, n_threads=%d (%d runs) "
			"===\n", nt, repeats);
		if (!run_repeated(model, vocab, n_ctx, gen_tokens, base, pol, cfg,
				shape, repeats, &r))
			return (false);
		report_repeated(r);
		all.push_back(r);
	}
	fprintf(stderr, "\n  cross-thread-count comparison (mean cosine per "
		"thread count):\n");
	{
		size_t	i;

		i = 0;
		while (i < thread_counts.size())
		{
			fprintf(stderr, "    n_threads=%d: mean cosine %.8f (stddev "
				"within that thread count: %.10f)\n", thread_counts[i],
				compute_stat(all[i].cosine).mean,
				compute_stat(all[i].cosine).stddev);
			i++;
		}
	}
	return (true);
}

/* ------------------------------------------------------------------ */
/* Mode: flashattn (item 1's determinism-audit finding, directly        */
/* tested). Re-captures the baseline WITH each flash_attn setting so    */
/* the reference and the candidate always share the same setting        */
/* (symmetric) -- isolates flash_attn's effect from the asymmetry the   */
/* optimizer tool actually has.                                         */
/* ------------------------------------------------------------------ */

static const char	*fa_name(llama_flash_attn_type t)
{
	return (llama_flash_attn_type_name(t));
}

static bool	mode_flashattn(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const char *prompt_path,
				const char *answer, const kv_policy_t &pol)
{
	/* LLAMA_FLASH_ATTN_TYPE_DISABLED is deliberately excluded from this
	 * sweep: on a real (n_ctx=1024) context it crashes
	 * llama_state_seq_get_data with a "tensor read out of bounds"
	 * GGML_ASSERT (see docs/phase4-runtime-variance.md's discovered-but-
	 * out-of-scope section) -- a real bug in how the MEMBRANE
	 * kv_type_override patch interacts with non-flash-attention KV
	 * layout, but neither membrane-kv-runtime nor
	 * membrane-kv-runtime-optimizer ever actually requests DISABLED in
	 * production (both only ever use AUTO for the reference or ENABLED
	 * for a candidate), so it is out of scope for this determinism
	 * investigation and not fixed here. */
	llama_flash_attn_type	settings[2] = {LLAMA_FLASH_ATTN_TYPE_AUTO,
		LLAMA_FLASH_ATTN_TYPE_ENABLED};
	prompt_decode_mode_t	shape_split;
	prompt_decode_mode_t	shape_whole;

	shape_split.split_last_token = true;
	shape_whole.split_last_token = false;
	progress_stage("flashattn", "", 2);
	fprintf(stderr, "\n=== mode: flashattn -- symmetric sweep (reference "
		"and candidate share the same setting; DISABLED excluded, see "
		"comment above) ===\n");
	for (llama_flash_attn_type fa : settings)
	{
		ctx_cfg_t	cfg;
		baseline_t	base;
		live_result_t	r;

		cfg.n_threads = 4;
		cfg.flash_attn = fa;
		if (!capture_baseline(model, vocab, n_ctx, gen_tokens, prompt_path,
				answer, cfg, shape_whole, &base))
			return (false);
		if (!eval_live(model, vocab, n_ctx, gen_tokens, base, pol, cfg,
				shape_whole, &r))
			return (false);
		fprintf(stderr, "  flash_attn=%-9s (symmetric): candidate-vs-its-"
			"own-baseline cosine %.8f top1 %.4f%%\n", fa_name(fa),
			r.m.logit_cosine, r.m.top1_pct);
	}
	fprintf(stderr, "\n  --- now reproducing the OPTIMIZER TOOL's actual "
		"asymmetry: reference captured with flash_attn=auto and a "
		"whole-prompt-minus-last-token/one-token split, candidate "
		"evaluated with flash_attn forced ENABLED and a whole-prompt "
		"decode (tools/membrane-kv-runtime-optimizer/main.cpp's real "
		"shapes) ---\n");
	{
		ctx_cfg_t	ref_cfg;
		ctx_cfg_t	cand_cfg;
		baseline_t	base;
		live_result_t	r;

		ref_cfg.n_threads = 4;
		ref_cfg.flash_attn = LLAMA_FLASH_ATTN_TYPE_AUTO;
		cand_cfg.n_threads = 4;
		cand_cfg.flash_attn = LLAMA_FLASH_ATTN_TYPE_ENABLED;
		if (!capture_baseline(model, vocab, n_ctx, gen_tokens, prompt_path,
				answer, ref_cfg, shape_split, &base))
			return (false);
		if (!eval_live(model, vocab, n_ctx, gen_tokens, base, pol, cand_cfg,
				shape_whole, &r))
			return (false);
		fprintf(stderr, "  asymmetric (optimizer's real shapes): cosine "
			"%.8f top1 %.4f%%\n", r.m.logit_cosine, r.m.top1_pct);
	}
	fprintf(stderr, "\n  --- isolating the SHAPE asymmetry alone: "
		"flash_attn held ENABLED for both reference and candidate, only "
		"the reference's decode shape varies (split vs whole) -- this is "
		"the one difference between membrane-kv-runtime-optimizer's "
		"capture_baseline (split) and membrane-kv-runtime's run_config "
		"reference call (whole), found by diffing the two tools ---\n");
	for (const prompt_decode_mode_t &ref_shape : {shape_split, shape_whole})
	{
		ctx_cfg_t	cfg;
		baseline_t	base;
		live_result_t	r;

		cfg.n_threads = 4;
		cfg.flash_attn = LLAMA_FLASH_ATTN_TYPE_ENABLED;
		if (!capture_baseline(model, vocab, n_ctx, gen_tokens, prompt_path,
				answer, cfg, ref_shape, &base))
			return (false);
		if (!eval_live(model, vocab, n_ctx, gen_tokens, base, pol, cfg,
				shape_whole, &r))
			return (false);
		fprintf(stderr, "  reference shape=%-6s (flash_attn=enabled both "
			"sides): cosine %.8f top1 %.4f%%\n",
			ref_shape.split_last_token ? "split" : "whole",
			r.m.logit_cosine, r.m.top1_pct);
	}
	return (true);
}

/* ------------------------------------------------------------------ */
/* Mode: quant-timing (item 6). Same target policy, same symmetric      */
/* ctx_cfg, compared through both backends: eval_live quantizes while   */
/* writing the KV cache (ggml's own quantize kernels, native),          */
/* eval_offline quantizes an already-captured F16 blob after the fact   */
/* (membrane's own quant_roundtrip). Any difference here is NOT a       */
/* generation-path/threading artifact -- both arms use the identical    */
/* ctx_cfg and the identical baseline.                                  */
/* ------------------------------------------------------------------ */

/* Phase 4.4 extended this mode to three arms instead of two: native
 * (real ggml write-time quantization, unchanged), post-hoc-ggml-exact
 * (the NEW membrane_ggml_quant_roundtrip post-hoc path), and
 * post-hoc-LEGACY (the OLD per-32-element linear quantizer this mode
 * originally compared against, kept only for this side-by-side
 * regression comparison -- docs/phase4-ggml-quant-parity.md item 6). */
static bool	mode_quant_timing(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &pol, int repeats)
{
	ctx_cfg_t				cfg;
	prompt_decode_mode_t	shape;
	std::vector<double>	native_cos;
	std::vector<double>	exact_cos;
	std::vector<double>	legacy_cos;
	std::vector<double>	native_top1;
	std::vector<double>	exact_top1;
	std::vector<double>	legacy_top1;
	int						i;

	cfg = default_ctx_cfg();
	shape.split_last_token = false;
	progress_stage("quant-timing", "", repeats * 3);
	fprintf(stderr, "\n=== mode: quant-timing (native write-time vs "
		"post-hoc-ggml-exact vs post-hoc-LEGACY, %d repeats each, "
		"symmetric n_threads=%d flash_attn=%s) ===\n", repeats,
		cfg.n_threads, fa_name(cfg.flash_attn));
	i = 0;
	while (i < repeats)
	{
		live_result_t	lr;
		metrics_t		om_exact;
		metrics_t		om_legacy;

		if (!eval_live(model, vocab, n_ctx, gen_tokens, base, pol, cfg,
				shape, &lr))
			return (false);
		g_progress.cand_index.fetch_add(1);
		if (!eval_offline(model, vocab, n_ctx, gen_tokens, base, pol, cfg,
				true, &om_exact))
			return (false);
		g_progress.cand_index.fetch_add(1);
		if (!eval_offline(model, vocab, n_ctx, gen_tokens, base, pol, cfg,
				false, &om_legacy))
			return (false);
		g_progress.cand_index.fetch_add(1);
		fprintf(stderr, "  run %2d/%d: native cosine %.8f top1 %.4f%%   "
			"post-hoc-exact cosine %.8f top1 %.4f%%   post-hoc-LEGACY "
			"cosine %.8f top1 %.4f%%   delta(native-exact) %+.8f   "
			"delta(native-legacy) %+.8f\n", i + 1, repeats,
			lr.m.logit_cosine, lr.m.top1_pct, om_exact.logit_cosine,
			om_exact.top1_pct, om_legacy.logit_cosine, om_legacy.top1_pct,
			lr.m.logit_cosine - om_exact.logit_cosine,
			lr.m.logit_cosine - om_legacy.logit_cosine);
		native_cos.push_back(lr.m.logit_cosine);
		exact_cos.push_back(om_exact.logit_cosine);
		legacy_cos.push_back(om_legacy.logit_cosine);
		native_top1.push_back(lr.m.top1_pct);
		exact_top1.push_back(om_exact.top1_pct);
		legacy_top1.push_back(om_legacy.top1_pct);
		i++;
	}
	fprintf(stderr, "\n  native            "); print_stat("cosine", compute_stat(native_cos));
	fprintf(stderr, "  post-hoc-exact     "); print_stat("cosine", compute_stat(exact_cos));
	fprintf(stderr, "  post-hoc-LEGACY    "); print_stat("cosine", compute_stat(legacy_cos));
	fprintf(stderr, "  native            "); print_stat("top1_pct", compute_stat(native_top1));
	fprintf(stderr, "  post-hoc-exact     "); print_stat("top1_pct", compute_stat(exact_top1));
	fprintf(stderr, "  post-hoc-LEGACY    "); print_stat("top1_pct", compute_stat(legacy_top1));
	return (true);
}

/* ------------------------------------------------------------------ */
/* Mode: drift (item 5). Token-by-token offline-vs-runtime comparison   */
/* for one policy, plus incremental per-slot attribution: starting from */
/* all-Q8, add each of the target policy's Q4 slots one at a time and   */
/* measure how much each addition grows the offline-vs-runtime gap.     */
/* ------------------------------------------------------------------ */

typedef struct s_slot
{
	int		layer;
	bool	is_k;
}	slot_t;

static std::vector<slot_t>	q4_slots_of(const kv_policy_t &pol)
{
	std::vector<slot_t>	out;
	size_t					l;

	l = 0;
	while (l < pol.kbits.size())
	{
		if (pol.kbits[l] == 4)
			out.push_back({(int)l, true});
		if (pol.vbits[l] == 4)
			out.push_back({(int)l, false});
		l++;
	}
	return (out);
}

static bool	mode_drift(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &pol)
{
	ctx_cfg_t				cfg;
	prompt_decode_mode_t	shape;
	metrics_t				off_m;
	live_result_t			live_r;
	std::vector<slot_t>	slots;
	kv_policy_t				running;
	double					epsilon;
	size_t					i;

	cfg = default_ctx_cfg();
	shape.split_last_token = false;
	epsilon = 0.001;
	fprintf(stderr, "\n=== mode: drift -- token-by-token offline vs "
		"runtime for the target policy (symmetric n_threads=%d "
		"flash_attn=%s) ===\n", cfg.n_threads, fa_name(cfg.flash_attn));
	if (!eval_offline(model, vocab, n_ctx, gen_tokens, base, pol, cfg,
			true, &off_m))
		return (false);
	if (!eval_live(model, vocab, n_ctx, gen_tokens, base, pol, cfg, shape,
			&live_r))
		return (false);
	fprintf(stderr, "  aggregate: offline cosine %.6f  runtime cosine "
		"%.6f  (delta %+.6f)\n", off_m.logit_cosine, live_r.m.logit_cosine,
		live_r.m.logit_cosine - off_m.logit_cosine);
	fprintf(stderr, "  per-step (first divergence beyond epsilon=%.4f):\n",
		epsilon);
	{
		bool	found;
		size_t	steps;

		found = false;
		steps = off_m.per_step_cosine.size() < live_r.m.per_step_cosine.size()
			? off_m.per_step_cosine.size() : live_r.m.per_step_cosine.size();
		i = 0;
		while (i < steps)
		{
			double	gap = fabs(off_m.per_step_cosine[i]
					- live_r.m.per_step_cosine[i]);

			if (!found && gap > epsilon)
			{
				fprintf(stderr, "    first divergence at token %zu: "
					"offline_cos %.6f runtime_cos %.6f gap %.6f\n", i,
					off_m.per_step_cosine[i], live_r.m.per_step_cosine[i],
					gap);
				found = true;
			}
			i++;
		}
		if (!found)
			fprintf(stderr, "    no token exceeded epsilon=%.4f across "
				"%zu steps\n", epsilon, steps);
	}
	slots = q4_slots_of(pol);
	fprintf(stderr, "\n  incremental per-slot attribution (%zu Q4 slots, "
		"added one at a time from an all-Q8 starting point):\n",
		slots.size());
	running = all_bits_policy((uint32_t)pol.kbits.size(), 8);
	progress_stage("drift-attribution", "", (int)slots.size());
	i = 0;
	while (i < slots.size())
	{
		metrics_t		step_off;
		live_result_t	step_live;

		if (slots[i].is_k)
			running.kbits[slots[i].layer] = 4;
		else
			running.vbits[slots[i].layer] = 4;
		if (!eval_offline(model, vocab, n_ctx, gen_tokens, base, running,
				cfg, true, &step_off))
			return (false);
		if (!eval_live(model, vocab, n_ctx, gen_tokens, base, running, cfg,
				shape, &step_live))
			return (false);
		g_progress.cand_index.store((int)i + 1);
		fprintf(stderr, "    + layer %2d %s: offline %.6f  runtime %.6f  "
			"gap %.6f\n", slots[i].layer, slots[i].is_k ? "K" : "V",
			step_off.logit_cosine, step_live.m.logit_cosine,
			fabs(step_off.logit_cosine - step_live.m.logit_cosine));
		i++;
	}
	return (true);
}

/* ------------------------------------------------------------------ */
/* Mode: trace (item 7). Full diagnostic per-token trace: token index,  */
/* per-step cosine/KL/max-logit-delta (vs the F16 reference, teacher-   */
/* forced against its own token sequence, matching how every other      */
/* mode's aggregate metrics are computed), a KV-state checksum captured */
/* after each decode step, and the active policy's hash (printed once,  */
/* the policy/layer/K-V map does not change mid-trace).                 */
/* ------------------------------------------------------------------ */

static bool	mode_trace(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &pol)
{
	ctx_cfg_t				cfg;
	prompt_decode_mode_t	shape;
	kv_policy_cb_ctx_t		cb_ctx;
	llama_context			*ctx;
	int32_t					n_vocab;
	double					cos_sum;
	double					kl_sum;
	uint64_t				top1_hits;
	uint64_t				top5_hits;
	int						i;
	bool					ok;

	cfg = default_ctx_cfg();
	shape.split_last_token = false;
	cb_ctx.policy = &pol;
	fprintf(stderr, "\n=== mode: trace ===\n");
	fprintf(stderr, "  active policy hash: %s\n",
		policy_hash_hex(pol).c_str());
	{
		size_t	l;

		fprintf(stderr, "  per-layer K/V precision:");
		l = 0;
		while (l < pol.kbits.size())
		{
			fprintf(stderr, " %zu:K%d/V%d", l, pol.kbits[l], pol.vbits[l]);
			l++;
		}
		fprintf(stderr, "\n");
	}
	ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16, cfg,
			kv_policy_type_cb, &cb_ctx);
	if (ctx == NULL)
		return (die("trace: context creation failed"), false);
	ok = decode_prompt_shaped(ctx, base.prompt_tokens, shape);
	n_vocab = llama_vocab_n_tokens(vocab);
	cos_sum = 0.0;
	kl_sum = 0.0;
	top1_hits = 0;
	top5_hits = 0;
	fprintf(stderr, "  %-6s %-12s %-12s %-14s %-10s\n", "token",
		"cosine", "kl", "max_delta", "kv_checksum");
	i = 0;
	progress_stage("trace", "", gen_tokens);
	while (ok && i < gen_tokens && i < (int)base.free_run.logits.size())
	{
		const float	*logits;
		llama_token	forced_tok;
		double		step_cos;
		double		step_kl;
		double		step_max_delta;
		size_t		kv_bytes;
		char		hex[MEMBRANE_SHA256_HEX_LEN + 1];

		logits = llama_get_logits_ith(ctx, -1);
		if (logits == NULL)
		{
			ok = die("trace: llama_get_logits_ith failed");
			break ;
		}
		std::vector<float>	cand(logits, logits + n_vocab);

		compare_step(base.free_run.logits[i], cand, &cos_sum, &kl_sum,
			&top1_hits, &top5_hits, &step_cos, &step_max_delta, &step_kl);
		kv_bytes = llama_state_seq_get_size(ctx, 0);
		std::vector<uint8_t>	snapshot(kv_bytes);

		llama_state_seq_get_data(ctx, snapshot.data(), kv_bytes, 0);
		hash_bytes_hex(snapshot.data(), snapshot.size(), hex);
		fprintf(stderr, "  %-6d %-12.8f %-12.8f %-14.8f %.16s\n", i,
			step_cos, step_kl, step_max_delta, hex);
		forced_tok = base.free_run.tokens[i];
		ok = llama_decode(ctx, llama_batch_get_one(&forced_tok, 1)) == 0;
		g_progress.cand_index.store(i + 1);
		i++;
	}
	llama_free(ctx);
	if (ok)
		fprintf(stderr, "\n  aggregate cosine %.8f  aggregate kl %.8f  "
			"top1 %.2f%%  top5 %.2f%%\n", cos_sum / (double)i,
			kl_sum / (double)i, 100.0 * (double)top1_hits / (double)i,
			100.0 * (double)top5_hits / (double)i);
	return (ok);
}

/* ------------------------------------------------------------------ */
/* Arg parsing and dispatch.                                             */
/* ------------------------------------------------------------------ */

typedef struct s_opts
{
	const char			*model_path;
	const char			*prompt_path;
	const char			*answer;
	const char			*policy_spec;
	const char			*mode;
	int					n_tokens;
	int					gen_tokens;
	int					repeats;
	std::vector<int>	thread_list;
}	opts_t;

static void	usage(void)
{
	fprintf(stderr, "membrane-kv-variance: usage: --model PATH --prompt "
		"PATH ANSWER|- --policy {all-f16|all-q8|all-q4|PATH.mpol} --mode "
		"{repeat|threads|flashattn|quant-timing|drift|trace} "
		"[--n-tokens N] [--gen-tokens G] [--repeats N] "
		"[--thread-list 1,2,4]\n");
}

static bool	parse_thread_list(const char *s, std::vector<int> *out)
{
	std::string	str;
	size_t		start;
	size_t		comma;

	str = s;
	start = 0;
	while (start <= str.size())
	{
		comma = str.find(',', start);
		std::string	tok = str.substr(start,
				comma == std::string::npos ? std::string::npos
					: comma - start);
		if (!tok.empty())
			out->push_back(atoi(tok.c_str()));
		if (comma == std::string::npos)
			break ;
		start = comma + 1;
	}
	return (!out->empty());
}

static int	parse_args(int argc, char **argv, opts_t *o)
{
	int	i;

	o->model_path = NULL;
	o->prompt_path = NULL;
	o->answer = NULL;
	o->policy_spec = NULL;
	o->mode = NULL;
	o->n_tokens = 512;
	o->gen_tokens = 32;
	o->repeats = 20;
	o->thread_list.clear();
	i = 1;
	while (i < argc)
	{
		if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
			o->model_path = argv[++i];
		else if (strcmp(argv[i], "--prompt") == 0 && i + 2 < argc)
		{
			o->prompt_path = argv[++i];
			o->answer = strcmp(argv[++i], "-") == 0 ? NULL : argv[i];
		}
		else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc)
			o->policy_spec = argv[++i];
		else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
			o->mode = argv[++i];
		else if (strcmp(argv[i], "--n-tokens") == 0 && i + 1 < argc)
			o->n_tokens = atoi(argv[++i]);
		else if (strcmp(argv[i], "--gen-tokens") == 0 && i + 1 < argc)
			o->gen_tokens = atoi(argv[++i]);
		else if (strcmp(argv[i], "--repeats") == 0 && i + 1 < argc)
			o->repeats = atoi(argv[++i]);
		else if (strcmp(argv[i], "--thread-list") == 0 && i + 1 < argc)
		{
			if (!parse_thread_list(argv[++i], &o->thread_list))
				return (usage(), -1);
		}
		else
			return (usage(), -1);
		i++;
	}
	if (o->model_path == NULL || o->prompt_path == NULL
			|| o->policy_spec == NULL || o->mode == NULL)
		return (usage(), -1);
	if (o->thread_list.empty())
		o->thread_list = {1, 2, 4};
	return (0);
}

int	main(int argc, char **argv)
{
	opts_t				o;
	llama_model			*model;
	const llama_vocab	*vocab;
	kv_policy_t			pol;
	baseline_t			base;
	ctx_cfg_t			base_cfg;
	prompt_decode_mode_t	base_shape;
	bool				ok;

	if (parse_args(argc, argv, &o) != 0)
		return (1);
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	progress_init();
	std::thread(heartbeat_loop).detach();
	llama_backend_init();
	model = llama_model_load_from_file(o.model_path,
			llama_model_default_params());
	if (model == NULL)
		return (die("model load failed"), 1);
	vocab = llama_model_get_vocab(model);
	if (!load_policy_spec(o.policy_spec,
			(uint32_t)llama_model_n_layer(model), &pol))
		return (llama_model_free(model), 1);
	base_cfg = default_ctx_cfg();
	base_shape.split_last_token = (strcmp(o.mode, "flashattn") != 0);
	if (!capture_baseline(model, vocab, o.n_tokens, o.gen_tokens,
			o.prompt_path, o.answer, base_cfg, base_shape, &base))
		return (llama_model_free(model), 1);
	if (strcmp(o.mode, "repeat") == 0)
		ok = mode_repeat(model, vocab, o.n_tokens, o.gen_tokens, base, pol,
				o.repeats);
	else if (strcmp(o.mode, "threads") == 0)
		ok = mode_threads(model, vocab, o.n_tokens, o.gen_tokens, base, pol,
				o.repeats, o.thread_list);
	else if (strcmp(o.mode, "flashattn") == 0)
		ok = mode_flashattn(model, vocab, o.n_tokens, o.gen_tokens,
				o.prompt_path, o.answer, pol);
	else if (strcmp(o.mode, "quant-timing") == 0)
		ok = mode_quant_timing(model, vocab, o.n_tokens, o.gen_tokens, base,
				pol, o.repeats);
	else if (strcmp(o.mode, "drift") == 0)
		ok = mode_drift(model, vocab, o.n_tokens, o.gen_tokens, base, pol);
	else if (strcmp(o.mode, "trace") == 0)
		ok = mode_trace(model, vocab, o.n_tokens, o.gen_tokens, base, pol);
	else
		ok = (usage(), false);
	llama_model_free(model);
	return (ok ? 0 : 1);
}
