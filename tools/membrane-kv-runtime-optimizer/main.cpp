/*
 * membrane-kv-runtime-optimizer (Phase 4.2): a KV precision optimizer
 * whose accept/reject decisions are made by REAL per-layer runtime
 * inference (Phase 4.1's kv_type_override engine), not by blob-splicing
 * simulation. Phase 4.1 measured that a policy blob-splicing validated as
 * safe only cleared its own real-runtime quality bar on 12/26 tested
 * (model, tier, prompt) combinations -- so this optimizer treats
 * blob-splicing as a CHEAP PRE-SCREEN ONLY: it can rank and discard
 * candidates fast, but it can never itself accept one. Every acceptance
 * comes from a real llama_context built with the candidate policy.
 *
 * Two explicit evaluation backends (item 1):
 *   EVAL_OFFLINE_BLOB  - blob-splicing (Phase 3.3's technique), used only
 *                        to rank candidates cheaply before spending a
 *                        real evaluation on them.
 *   EVAL_LIVE_RUNTIME  - a real per-layer-quantized llama_context (Phase
 *                        4.1's kv_type_override), the ONLY backend that
 *                        can accept a candidate.
 *
 * The blob-splicing primitives below are copied from
 * tools/membrane-kv-sensitivity/main.cpp (Phase 3.3-3.6, unchanged) --
 * still needed here for the pre-screen stage. The real-runtime primitives
 * are copied from tools/membrane-kv-runtime/main.cpp (Phase 4.1,
 * unchanged) -- needed for the acceptance stage. Both source tools stay
 * as they are; this tool is a new, self-contained combination of the two,
 * matching the project's existing pattern of each tool being independent
 * rather than sharing a library.
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
#include <ctime>
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
#include "membrane/quant_simd.h"

#include "checkpoint.h"

# define SEQ_STATE_MAGIC 0xaf143cd8u
# define GROUP_ELEMS 32
# define TOOL_VERSION "membrane-kv-runtime-optimizer-1.0"

static int	die(const char *msg)
{
	fprintf(stderr, "membrane-kv-runtime-optimizer: %s\n", msg);
	return (-1);
}

/* ------------------------------------------------------------------ */
/* Live progress reporting for long real-runtime searches. Purely       */
/* additive observability: it never influences a search/accept/reject   */
/* decision, a threshold, a budget, or which candidates get tested --   */
/* it only reports, on stderr (unbuffered, see main()), what the        */
/* optimizer is doing right now and how long it has been doing it.      */
/* A single background heartbeat thread reads the same atomics the main */
/* thread updates around each real eval, so "looks stuck" during a long */
/* Stage A/B pass is always distinguishable from actually stuck.        */
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

/* Cumulative mean of per-candidate wall time within the CURRENT stage;
 * reset by progress_stage() whenever the stage/tier changes, since a
 * Stage A offline pre-screen and a Stage B live eval have wildly
 * different per-candidate costs and mixing them would make the ETA
 * meaningless. */
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

/* Phase 5.1 item 9: quantization-engine visibility. g_quant_backend/
 * g_quant_threads are fixed once at startup (membrane_simd_best_backend()
 * is deterministic for a given CPU; this integration point calls the
 * single-row, non-parallel membrane_simd_* API from inside an already
 * hot serial loop -- see quant_roundtrip_inplace -- so threads used is
 * always 1 here, honestly reported as such rather than implying a
 * worker pool that this call site doesn't use). g_quant_elems_window/
 * g_quant_ns_window are windowed accumulators, reset every heartbeat
 * tick, so the reported throughput is a real measured rate over the
 * last ~60s, not a cumulative average that hides slowdowns. */
static const char				*g_quant_backend = "unknown";
static int						g_quant_threads = 1;
static std::atomic<long long>	g_quant_elems_window{0};
static std::atomic<long long>	g_quant_ns_window{0};

static void	heartbeat_loop(void)
{
	while (true)
	{
		std::this_thread::sleep_for(std::chrono::seconds(60));
		int			done;
		int			total;
		double		avg;
		double		eta;
		long long	elems;
		long long	ns;
		double		mb_s;

		done = g_progress.cand_index.load();
		total = g_progress.cand_total.load();
		avg = g_progress.avg_eval_seconds.load();
		eta = (avg > 0.0 && total > done)
			? avg * (double)(total - done) : -1.0;
		elems = g_quant_elems_window.exchange(0);
		ns = g_quant_ns_window.exchange(0);
		mb_s = ns > 0 ? (double)elems * 2.0 / ((double)ns / 1e9) / 1e6 : 0.0;
		fprintf(stderr, "  [heartbeat] elapsed %.0fs  stage=%s tier=%s  "
			"candidates %d/%d  quant backend=%s threads=%d "
			"throughput=%.1fMB/s(60s window)", progress_elapsed_seconds(),
			g_progress.stage.load(), g_progress.tier.load(), done, total,
			g_quant_backend, g_quant_threads, mb_s);
		if (eta >= 0.0)
			fprintf(stderr, "  ETA ~%.0fs\n", eta);
		else
			fprintf(stderr, "\n");
	}
}

/* ------------------------------------------------------------------ */
/* Blob-splicing primitives (Phase 3.3, copied unchanged from           */
/* membrane-kv-sensitivity/main.cpp) -- used only by the OFFLINE_BLOB   */
/* pre-screen backend.                                                  */
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
	cursor_t	c;
	uint32_t	v_trans;
	uint32_t	il;
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

/* Phase 5.1: routes through the portable membrane_simd_* engine
 * (src/quant/quant_simd.c), which is bit-exact with membrane_ggml_quant
 * (verified across 100,000+ random blocks in
 * tests/unit/test_quant_simd_parity.c) but avoids that path's per-call
 * ggml dispatch overhead -- this function sits in the optimizer's
 * hottest inner loop (called once per K/V row per candidate per
 * simulated token), so the saved overhead compounds across a whole
 * search run (docs/phase5-quant-engine.md). Every real Q8_0/Q4_0-
 * eligible KV row length is already a multiple of the 32-element block
 * size (ggml requires n_embd_head % blck_size == 0 to construct a
 * quantized KV cache type at all), so the size check below never
 * actually trips in practice; it, and the oversized-row case, fall back
 * to the ggml-backed oracle rather than growing a heap path in this hot
 * loop, since both are defensive-only and not expected to be hit. */
static void	quant_roundtrip_inplace(uint8_t *data, size_t len, int bits)
{
	static const size_t		k_max_row_elems = 8192;
	uint16_t					*x_f16;
	size_t						n;
	uint8_t						packed[(8192 / MEMBRANE_QSIMD_BLOCK_ELEMS)
									* MEMBRANE_QSIMD_Q8_0_BLOCK_BYTES];
	membrane_simd_backend_t	backend;
	struct timespec				t0;
	struct timespec				t1;

	if (bits == 16)
		return ;
	x_f16 = (uint16_t *)(void *)data;
	n = len / 2;
	if (n == 0 || n % MEMBRANE_QSIMD_BLOCK_ELEMS != 0 || n > k_max_row_elems)
	{
		membrane_ggml_quant_roundtrip(x_f16, n, bits);
		return ;
	}
	backend = membrane_simd_best_backend();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (bits == 8)
	{
		membrane_simd_q8_0_quantize(backend, x_f16, n, packed);
		membrane_simd_q8_0_dequantize(backend, packed, n, x_f16);
	}
	else
	{
		membrane_simd_q4_0_quantize(backend, x_f16, n, packed);
		membrane_simd_q4_0_dequantize(backend, packed, n, x_f16);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	g_quant_elems_window.fetch_add((long long)n);
	g_quant_ns_window.fetch_add((t1.tv_sec - t0.tv_sec) * 1000000000LL
		+ (t1.tv_nsec - t0.tv_nsec));
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
				const std::vector<perturb_target_t> &targets)
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
						+ (size_t)r * ls.k_row_size, ls.k_row_size, t.bits);
			if (t.do_v)
				quant_roundtrip_inplace(blob + ls.v_offset
						+ (size_t)r * ls.v_row_size, ls.v_row_size, t.bits);
			r++;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Model / context / generation helpers, shared by both backends.       */
/* make_context's kv_type_override plumbing is Phase 4.1's real         */
/* per-layer engine, copied unchanged from membrane-kv-runtime.         */
/* ------------------------------------------------------------------ */

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
						ggml_type (*override_cb)(int32_t, bool, void *) = NULL,
						void *override_ud = NULL)
{
	llama_context_params	cp;

	cp = llama_context_default_params();
	cp.n_ctx = (uint32_t)n_ctx;
	cp.n_batch = 256;
	cp.n_threads = 4;
	cp.n_threads_batch = 4;
	cp.type_k = type_k;
	cp.type_v = type_v;
	cp.no_perf = true;
	cp.kv_type_override = override_cb;
	cp.kv_type_override_ud = override_ud;
	if (type_k != GGML_TYPE_F16 || type_v != GGML_TYPE_F16
			|| override_cb != NULL)
		cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
	return (llama_init_from_model(model, cp));
}

static bool	decode_prompt(llama_context *ctx,
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

/* Phase 4.3's determinism audit (docs/phase4-runtime-variance.md) found
 * that capture_baseline() decodes the prompt as (prefix, last-token) in
 * two llama_decode calls while eval_live() decoded it as one whole-prompt
 * call -- a real, measured, deterministic source of cosine bias between
 * the reference and every candidate (confirmed: forcing eval_live to
 * decode this same (prefix, last-token) shape reproduces the exact
 * cosine capture_baseline's own shape would predict, eliminating the
 * gap). This helper makes eval_live's decode match capture_baseline's,
 * instead of the two silently differing. */
static bool	decode_prompt_matched(llama_context *ctx,
				const std::vector<llama_token> &tokens)
{
	std::vector<llama_token>	prefix;

	prefix.assign(tokens.begin(), tokens.end() - 1);
	return (decode_prompt(ctx, prefix, 256) && decode_one(ctx, tokens.back()));
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

/* Per-step comparison; `out_cos` (if non-NULL) additionally receives
 * this single step's cosine, for the drift-by-position analysis (item
 * 6) that needs the sequence, not just the mean. */
static void	compare_step(const std::vector<float> &base,
				const std::vector<float> &cand, double *cos_sum,
				double *kl_sum, uint64_t *top1, uint64_t *top5,
				double *out_cos)
{
	double	dot;
	double	na;
	double	nb;
	double	mx;
	double	sum_pb;
	double	sum_pc;
	double	step_cos;
	std::vector<double>	pb;
	std::vector<double>	pc;
	size_t	i;
	int		base_top1;

	dot = 0.0;
	na = 0.0;
	nb = 0.0;
	for (i = 0; i < base.size(); i++)
	{
		dot += (double)base[i] * (double)cand[i];
		na += (double)base[i] * (double)base[i];
		nb += (double)cand[i] * (double)cand[i];
	}
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
	for (i = 0; i < base.size(); i++)
	{
		pb[i] /= sum_pb;
		pc[i] /= sum_pc;
		if (pb[i] > 0.0)
			*kl_sum += pb[i] * log(pb[i] / (pc[i] > 1e-300 ? pc[i] : 1e-300));
	}
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
	std::vector<double>	per_step_cosine;	/* for drift-by-position */
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

	cos_sum = 0.0;
	kl_sum = 0.0;
	top1 = 0;
	top5 = 0;
	steps = ref_free.logits.size() < forced_run.logits.size()
		? ref_free.logits.size() : forced_run.logits.size();
	m->per_step_cosine.resize(steps);
	for (i = 0; i < steps; i++)
	{
		compare_step(ref_free.logits[i], forced_run.logits[i], &cos_sum,
			&kl_sum, &top1, &top5, &step_cos);
		m->per_step_cosine[i] = step_cos;
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
/* Baseline capture: one real F16 decode per prompt, used as the        */
/* reference for BOTH backends (it is genuinely real either way -- only */
/* the CANDIDATE evaluation differs between splicing and live runtime). */
/* Also captures the prefix blob the OFFLINE_BLOB backend splices into. */
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
	size_t						f16_state_bytes;
}	baseline_t;

static bool	capture_baseline(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const char *prompt_path,
				const char *answer, baseline_t *out)
{
	llama_context				*ctx;
	std::vector<llama_token>	prefix;
	size_t						blob_size;
	bool						ok;

	if (!tokenize_prompt(vocab, prompt_path, &out->prompt_tokens)
		|| out->prompt_tokens.empty())
		return (die("prompt tokenized to zero tokens"), false);
	out->name = prompt_path;
	out->last_token = out->prompt_tokens.back();
	prefix.assign(out->prompt_tokens.begin(), out->prompt_tokens.end() - 1);
	ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16);
	if (ctx == NULL)
		return (die("baseline context creation failed"), false);
	ok = decode_prompt(ctx, prefix, 256);
	if (ok)
	{
		blob_size = llama_state_seq_get_size(ctx, 0);
		out->blob.resize(blob_size);
		llama_state_seq_get_data(ctx, out->blob.data(), blob_size, 0);
		ok = parse_blob(out->blob.data(), out->blob.size(), &out->idx);
	}
	if (ok)
		ok = decode_one(ctx, out->last_token);
	if (ok)
		ok = run_gen(ctx, vocab, gen_tokens, NULL, &out->free_run);
	if (ok)
	{
		out->text = tokens_to_text(vocab, out->free_run.tokens);
		out->answer = answer;
	}
	llama_free(ctx);
	if (!ok)
		return (false);
	ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16);
	if (ctx == NULL)
		return (die("f16 reference context creation failed"), false);
	ok = decode_prompt(ctx, out->prompt_tokens, 256);
	if (ok)
		out->f16_state_bytes = llama_state_seq_get_size(ctx, 0);
	llama_free(ctx);
	return (ok);
}

/* ------------------------------------------------------------------ */
/* Policy representation, shared by both backends.                     */
/* ------------------------------------------------------------------ */

typedef struct s_kv_policy
{
	std::vector<int>	kbits;	/* per layer, 16/8/4 */
	std::vector<int>	vbits;
}	kv_policy_t;

static kv_policy_t	all_q8_policy(uint32_t n_layer)
{
	kv_policy_t	p;

	p.kbits.assign(n_layer, 8);
	p.vbits.assign(n_layer, 8);
	return (p);
}

static std::vector<perturb_target_t>	policy_targets(const blob_index_t &idx,
		const kv_policy_t &pol)
{
	std::vector<perturb_target_t>	targets;
	uint32_t	l;

	l = 0;
	while (l < idx.n_layer)
	{
		targets.push_back({(int)l, true, false, 0, idx.cell_count,
				pol.kbits[l]});
		targets.push_back({(int)l, false, true, 0, idx.cell_count,
				pol.vbits[l]});
		l++;
	}
	return (targets);
}

static ggml_type	bits_to_ggml(int bits)
{
	if (bits == 8)
		return (GGML_TYPE_Q8_0);
	if (bits == 4)
		return (GGML_TYPE_Q4_0);
	return (GGML_TYPE_F16);
}

typedef struct s_kv_policy_cb_ctx
{
	const kv_policy_t	*policy;
	uint64_t			calls;
	double				total_ns;
}	kv_policy_cb_ctx_t;

static ggml_type	kv_policy_type_cb(int32_t il, bool is_v, void *user_data)
{
	kv_policy_cb_ctx_t						*ctx;
	std::chrono::steady_clock::time_point	t0;
	std::chrono::steady_clock::time_point	t1;
	int										bits;

	ctx = (kv_policy_cb_ctx_t *)user_data;
	t0 = std::chrono::steady_clock::now();
	bits = is_v ? ctx->policy->vbits[il] : ctx->policy->kbits[il];
	t1 = std::chrono::steady_clock::now();
	ctx->calls++;
	ctx->total_ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
	return (bits_to_ggml(bits));
}

/* ------------------------------------------------------------------ */
/* Prompt classification and thresholds (Phase 3.5, unchanged) -- item  */
/* 4's thresholds are exactly g_default_thresholds / recall-critical    */
/* below, now checked against LIVE_RUNTIME metrics instead of spliced   */
/* ones.                                                                */
/* ------------------------------------------------------------------ */

typedef enum e_prompt_class
{
	PROMPT_RECALL_CRITICAL = 0,
	PROMPT_CODE,
	PROMPT_NATURAL,
	PROMPT_REPEATED,
	PROMPT_GENERAL
}	prompt_class_t;

static const char	*class_name(prompt_class_t c)
{
	static const char	*const names[] = {"recall-critical", "code",
		"natural", "repeated", "general"};

	return (names[c]);
}

static prompt_class_t	classify_prompt(const baseline_t &b)
{
	if (b.answer != NULL)
		return (PROMPT_RECALL_CRITICAL);
	if (b.name.find("code") != std::string::npos)
		return (PROMPT_CODE);
	if (b.name.find("repeat") != std::string::npos)
		return (PROMPT_REPEATED);
	if (b.name.find("natural") != std::string::npos)
		return (PROMPT_NATURAL);
	return (PROMPT_GENERAL);
}

typedef struct s_prompt_thresholds
{
	double	top1_min;
	double	top5_min;
	double	cosine_min;
}	prompt_thresholds_t;

static const prompt_thresholds_t	g_default_thresholds = {98.0, 99.0, 0.995};

static prompt_thresholds_t	thresholds_for_class(prompt_class_t c)
{
	if (c == PROMPT_RECALL_CRITICAL)
		return {99.0, 99.0, 0.9975};
	return {g_default_thresholds.top1_min, g_default_thresholds.top5_min,
		g_default_thresholds.cosine_min};
}

typedef struct s_margin
{
	double	cosine_margin;
	double	top1_margin;
	double	top5_margin;
}	margin_t;

static const margin_t	g_margin_conservative = {0.0015, 1.0, 0.2};
static const margin_t	g_margin_balanced = {0.001, 0.5, 0.1};
static const margin_t	g_margin_aggressive = {0.0, 0.0, 0.0};

# define K_STRICT_COSINE 0.9975

/* ------------------------------------------------------------------ */
/* item 1: the two evaluation backends.                                */
/* ------------------------------------------------------------------ */

typedef enum e_eval_backend
{
	EVAL_OFFLINE_BLOB = 0,
	EVAL_LIVE_RUNTIME
}	eval_backend_t;

/* EVAL_OFFLINE_BLOB: splices `pol` into a copy of the baseline's blob
 * and continues generation from there (Phase 3.3's technique). Cheap,
 * but Phase 4.1 measured it does not reliably predict real quality --
 * pre-screen only, never a final acceptance authority. */
static bool	eval_offline(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &pol, metrics_t *m)
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
	apply_targets(blob.data(), base.idx, targets);
	free_ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16);
	forced_ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16);
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
	long		peak_rss_kb;
}	live_result_t;

static long	peak_rss_kb(void)
{
	struct rusage	ru;

	getrusage(RUSAGE_SELF, &ru);
	return (ru.ru_maxrss);
}

/* EVAL_LIVE_RUNTIME: builds a REAL llama_context with `pol` applied via
 * kv_type_override (Phase 4.1's engine) and decodes the prompt from
 * scratch through genuinely per-layer-quantized tensors -- no splicing.
 * The only backend allowed to accept a candidate. */
static bool	eval_live(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &pol, live_result_t *out)
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
	cb_ctx.calls = 0;
	cb_ctx.total_ns = 0.0;
	free_ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16,
			kv_policy_type_cb, &cb_ctx);
	forced_ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16,
			kv_policy_type_cb, &cb_ctx);
	ok = free_ctx != NULL && forced_ctx != NULL;
	if (ok)
	{
		t0 = std::chrono::steady_clock::now();
		ok = decode_prompt_matched(free_ctx, base.prompt_tokens);
	}
	if (ok)
	{
		out->kv_bytes = llama_state_seq_get_size(free_ctx, 0);
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
		ok = decode_prompt_matched(forced_ctx, base.prompt_tokens)
			&& run_gen(forced_ctx, vocab, gen_tokens, &base.free_run.tokens,
				&forced_run);
	if (ok)
	{
		out->ttft_ms = std::chrono::duration<double, std::milli>(
				t_first - t0).count();
		out->tok_per_sec = gen_tokens > 1 ? (double)(gen_tokens - 1)
			/ std::chrono::duration<double>(t_gen_done - t_first).count()
			: 0.0;
		out->peak_rss_kb = peak_rss_kb();
		fill_metrics(vocab, gen_tokens, base.free_run, free_run, forced_run,
			base.answer, &out->m);
	}
	llama_free(free_ctx);
	llama_free(forced_ctx);
	return (ok);
}

/* ------------------------------------------------------------------ */
/* item 4: shared per-prompt hard-constraint check, backend-agnostic -- */
/* the SAME function checks an OFFLINE_BLOB pre-screen result and a     */
/* LIVE_RUNTIME acceptance result; only LIVE_RUNTIME's outcome ever     */
/* gates an accept/reject decision (item 3).                            */
/* ------------------------------------------------------------------ */

static std::string	check_prompt(const metrics_t &m, const std::string &name,
					prompt_class_t cls, bool is_k, const margin_t &margin,
					bool must_stay_correct)
{
	prompt_thresholds_t	th;
	double					cos_bar;
	double					k_bar;
	char					buf[256];

	th = thresholds_for_class(cls);
	cos_bar = th.cosine_min + margin.cosine_margin;
	if (is_k)
	{
		k_bar = K_STRICT_COSINE + margin.cosine_margin;
		if (k_bar > cos_bar)
			cos_bar = k_bar;
	}
	if (m.logit_cosine < cos_bar)
	{
		snprintf(buf, sizeof(buf), "prompt '%s' (%s): cosine %.6f < %.6f",
			name.c_str(), class_name(cls), m.logit_cosine, cos_bar);
		return (buf);
	}
	if (m.top1_pct < th.top1_min + margin.top1_margin)
	{
		snprintf(buf, sizeof(buf), "prompt '%s' (%s): top1 %.2f%% < %.2f%%",
			name.c_str(), class_name(cls), m.top1_pct,
			th.top1_min + margin.top1_margin);
		return (buf);
	}
	if (m.top5_pct < th.top5_min + margin.top5_margin)
	{
		snprintf(buf, sizeof(buf), "prompt '%s' (%s): top5 %.2f%% < %.2f%%",
			name.c_str(), class_name(cls), m.top5_pct,
			th.top5_min + margin.top5_margin);
		return (buf);
	}
	/* item 4: exact-answer gate is mandatory whenever the reference this
	 * candidate is compared against (all-Q8, item 2) itself answered
	 * correctly -- recall-critical prompts are always in this set, since
	 * a recall-critical prompt whose all-Q8 reference got the WRONG
	 * answer would never have entered the valid set (item 4's own
	 * "baseline limit" carve-out handles that case separately). */
	if (must_stay_correct && !m.recall_ok)
	{
		snprintf(buf, sizeof(buf), "prompt '%s' (%s): recall broke (the "
			"all-Q8 starting policy answered it correctly, this "
			"candidate does not)", name.c_str(), class_name(cls));
		return (buf);
	}
	return ("");
}

typedef struct s_per_prompt_result
{
	std::vector<metrics_t>	per_prompt;
	std::vector<bool>		recall_ok;
	double					agg_cosine;
	double					agg_top1;
	double					agg_top5;
	double					agg_kl;
	size_t					n_evaluated;
}	per_prompt_result_t;

/* Evaluates `trial` over `valid` in priority `order` (recall-critical
 * first), stopping the moment one prompt fails -- pre-screening (item 8
 * of Phase 3.6, reused) applies to BOTH backends, since a LIVE_RUNTIME
 * evaluation is expensive enough that stopping early matters there too. */
static bool	evaluate_and_check(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const std::vector<baseline_t> &valid,
				const std::vector<size_t> &order,
				const std::vector<prompt_class_t> &classes, bool is_k,
				const margin_t &margin, const std::vector<bool> &must_stay,
				eval_backend_t backend, const kv_policy_t &trial,
				per_prompt_result_t *out, std::string *reason)
{
	double	cos_sum;
	double	top1_sum;
	double	top5_sum;
	double	kl_sum;
	bool	ok;

	out->per_prompt.assign(valid.size(), metrics_t());
	out->recall_ok.assign(valid.size(), false);
	cos_sum = 0.0;
	top1_sum = 0.0;
	top5_sum = 0.0;
	kl_sum = 0.0;
	out->n_evaluated = 0;
	*reason = "";
	for (size_t idx : order)
	{
		std::chrono::steady_clock::time_point	prompt_t0;

		prompt_t0 = std::chrono::steady_clock::now();
		if (backend == EVAL_LIVE_RUNTIME)
		{
			live_result_t	lr;

			ok = eval_live(model, vocab, n_ctx, gen_tokens, valid[idx], trial,
					&lr);
			out->per_prompt[idx] = lr.m;
		}
		else
			ok = eval_offline(model, vocab, n_ctx, gen_tokens, valid[idx],
					trial, &out->per_prompt[idx]);
		if (!ok)
			return (false);
		fprintf(stderr, "    prompt %zu/%zu '%s' done in %.1fs\n",
			out->n_evaluated + 1, valid.size(), valid[idx].name.c_str(),
			std::chrono::duration<double>(std::chrono::steady_clock::now()
				- prompt_t0).count());
		out->n_evaluated++;
		cos_sum += out->per_prompt[idx].logit_cosine;
		top1_sum += out->per_prompt[idx].top1_pct;
		top5_sum += out->per_prompt[idx].top5_pct;
		kl_sum += out->per_prompt[idx].kl_mean;
		out->recall_ok[idx] = out->per_prompt[idx].recall_ok;
		*reason = check_prompt(out->per_prompt[idx], valid[idx].name,
				classes[idx], is_k, margin, must_stay[idx]);
		if (!reason->empty())
			break ;
	}
	out->agg_cosine = out->n_evaluated
		? cos_sum / (double)out->n_evaluated : 0.0;
	out->agg_top1 = out->n_evaluated
		? top1_sum / (double)out->n_evaluated : 0.0;
	out->agg_top5 = out->n_evaluated
		? top5_sum / (double)out->n_evaluated : 0.0;
	out->agg_kl = out->n_evaluated ? kl_sum / (double)out->n_evaluated : 0.0;
	return (true);
}

/* Full (non-early-exit) evaluation, for reporting/drift analysis where
 * every prompt's numbers are needed regardless of an early failure. */
static bool	evaluate_full(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const std::vector<baseline_t> &valid,
				eval_backend_t backend, const kv_policy_t &trial,
				per_prompt_result_t *out)
{
	std::vector<size_t>	order;
	size_t					i;

	order.resize(valid.size());
	i = 0;
	while (i < valid.size())
	{
		order[i] = i;
		i++;
	}
	std::vector<prompt_class_t>	dummy_classes(valid.size(), PROMPT_GENERAL);
	std::vector<bool>				dummy_must_stay(valid.size(), false);
	std::string						reason;
	margin_t						no_gate = {-1e9, -1e9, -1e9};

	return (evaluate_and_check(model, vocab, n_ctx, gen_tokens, valid, order,
			dummy_classes, false, no_gate, dummy_must_stay, backend, trial,
			out, &reason));
}

/* ------------------------------------------------------------------ */
/* Slots, priority queue, memory cost model (Phase 3.4/3.6, unchanged). */
/* ------------------------------------------------------------------ */

typedef struct s_slot
{
	int		layer;
	bool	is_k;
}	slot_t;

static std::vector<slot_t>	priority_queue_slots(uint32_t n_layer)
{
	std::vector<slot_t>	q;
	uint32_t	l;

	l = 0;
	while (l < n_layer)
		q.push_back({(int)l++, false});
	l = 0;
	while (l < n_layer)
		q.push_back({(int)l++, true});
	return (q);
}

static double	bytes_per_row(size_t row_size, int bits)
{
	size_t	groups;

	groups = (row_size / 2 + GROUP_ELEMS - 1) / GROUP_ELEMS;
	if (bits == 8)
		return ((double)row_size / 2.0 + (double)groups * 2.0);
	if (bits == 4)
		return ((double)row_size / 4.0 + (double)groups * 2.0);
	return ((double)row_size);
}

static double	slot_memory_gain(const blob_index_t &idx, const slot_t &s)
{
	size_t	row_size;

	row_size = s.is_k ? idx.layers[s.layer].k_row_size
		: idx.layers[s.layer].v_row_size;
	return ((bytes_per_row(row_size, 8) - bytes_per_row(row_size, 4))
		* idx.cell_count);
}

/* ------------------------------------------------------------------ */
/* item 5: two-stage candidate selection + greedy composition-aware     */
/* search.                                                              */
/*                                                                      */
/* Stage A (offline pre-screen) ranks every slot ONCE, up front, against */
/* the all-Q8 starting policy -- cheap, and it never itself accepts a   */
/* candidate (item 3). Stage B processes that fixed order with real     */
/* LIVE_RUNTIME evaluations against the CURRENT, already-accepted       */
/* policy (composition-aware, item 5): the first candidate to clear the */
/* real hard constraints is accepted, every other slot tested is        */
/* permanently rejected -- at most one live evaluation per slot, ever,  */
/* the same bound Phase 3.4/3.5 used. See the comment on greedy_search()*/
/* itself for why Stage A runs once per slot rather than once per round */
/* (an earlier version did the latter and was measured to cost more     */
/* than Phase 3.6's entire search). Backtracking (Phase 3.4/3.5's       */
/* re-check of the last few accepted slots) is deliberately NOT ported  */
/* here: it would roughly double live-evaluation cost, which is a much  */
/* bigger cost increase against LIVE_RUNTIME than it ever was against   */
/* blob-splicing -- a disclosed scope decision, not an oversight.       */
/* ------------------------------------------------------------------ */

typedef struct s_decision2
{
	slot_t		slot;
	metrics_t	agg_like;	/* aggregate-shaped live metrics at decision time */
	double		offline_cosine;
	double		offline_top1;
	double		memory_gain_bytes;
	double		ratio;
	bool		accepted;
	std::string	reason;
}	decision2_t;

typedef struct s_optimizer_result
{
	kv_policy_t					policy;
	std::vector<decision2_t>	accepted;
	std::vector<decision2_t>	rejected;
	int							live_evals_used;
	int							offline_prescreens_used;
	double						search_seconds;
}	optimizer_result_t;

static std::vector<size_t>	screening_order(
		const std::vector<prompt_class_t> &classes)
{
	std::vector<size_t>	order;
	size_t	i;

	i = 0;
	while (i < classes.size())
	{
		if (classes[i] == PROMPT_RECALL_CRITICAL)
			order.push_back(i);
		i++;
	}
	i = 0;
	while (i < classes.size())
	{
		if (classes[i] != PROMPT_RECALL_CRITICAL)
			order.push_back(i);
		i++;
	}
	return (order);
}

static double	ratio_from_worst_cosine(const per_prompt_result_t &r,
					const std::vector<prompt_class_t> &classes,
					double memory_gain)
{
	double	worst;
	double	loss;
	size_t	i;

	worst = 1e300;
	i = 0;
	while (i < r.per_prompt.size())
	{
		loss = thresholds_for_class(classes[i]).cosine_min
			- r.per_prompt[i].logit_cosine;
		if (loss < worst)
			worst = loss;
		i++;
	}
	loss = -worst;
	return (memory_gain / (loss > 1e-9 ? loss : 1e-9));
}

/*
 * Cost note (found empirically, not anticipated in the original design):
 * an earlier version of this function re-ran Stage A (offline pre-screen)
 * over every still-undecided slot EVERY round, to stay maximally
 * composition-aware. At gen_tokens=128 with several valid prompts, a
 * single round's pre-screen over ~60 slots took over 40 minutes on real
 * hardware -- more expensive than Phase 3.6's entire single-pass search
 * over the same slot count, because re-screening the whole remaining
 * queue every round is O(rounds x remaining_slots), not O(slots). Fixed
 * by pre-screening each slot exactly ONCE (against the all-Q8 starting
 * policy, before any acceptance), then processing that fixed,
 * offline-ranked order with LIVE_RUNTIME first-improvement acceptance --
 * this is the same total-live-eval bound Phase 3.4/3.5 used (at most one
 * live evaluation per slot, ever), now also bounding the offline side to
 * exactly one pass. Composition-awareness is kept where it matters most:
 * every LIVE_RUNTIME evaluation is still against the CURRENT, already-
 * accepted policy, not a cached score -- only the offline RANKING is
 * computed once, up front, against the starting policy.
 */
static bool	greedy_search(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const std::vector<baseline_t> &valid,
				const std::vector<prompt_class_t> &classes,
				const std::vector<bool> &must_stay_correct,
				const margin_t &margin, int search_budget,
				const kv_policy_t &start_policy, optimizer_result_t *out,
				const char *model_name, const char *tier,
				FILE *checkpoint_out, const checkpoint_state_t *resume)
{
	std::vector<slot_t>	queue;
	std::vector<size_t>	order;
	std::vector<bool>		decided;
	std::chrono::steady_clock::time_point	t0;

	t0 = std::chrono::steady_clock::now();
	queue = priority_queue_slots(valid[0].idx.n_layer);
	order = screening_order(classes);
	out->policy = start_policy;
	out->live_evals_used = 0;
	out->offline_prescreens_used = 0;
	decided.assign(queue.size(), false);
	if (resume != NULL && resume->header_matches)
	{
		out->live_evals_used = (int)resume->decisions.size();
		for (const resume_decision_t &rd : resume->decisions)
		{
			size_t	qi;

			qi = 0;
			while (qi < queue.size())
			{
				if (queue[qi].layer == rd.layer && queue[qi].is_k == rd.is_k)
					break ;
				qi++;
			}
			if (qi == queue.size())
				continue ;
			decided[qi] = true;
			decision2_t	d{};

			d.slot = queue[qi];
			d.agg_like.logit_cosine = rd.cosine;
			d.agg_like.top1_pct = rd.top1;
			d.agg_like.top5_pct = rd.top5;
			d.agg_like.kl_mean = rd.kl;
			d.agg_like.first_divergence = rd.first_divergence;
			d.offline_cosine = rd.offline_cosine;
			d.offline_top1 = rd.offline_top1;
			d.memory_gain_bytes = slot_memory_gain(valid[0].idx, queue[qi]);
			d.ratio = 0.0;
			d.accepted = rd.accepted;
			d.reason = rd.accepted ? "accepted (resumed)" : rd.reason;
			if (rd.accepted)
			{
				if (queue[qi].is_k)
					out->policy.kbits[queue[qi].layer] = 4;
				else
					out->policy.vbits[queue[qi].layer] = 4;
				out->accepted.push_back(d);
			}
			else
				out->rejected.push_back(d);
		}
		fprintf(stderr, "  resuming %s/%s: %zu prior decisions fast-forwarded "
			"(counted against search_budget=%d)\n", model_name, tier,
			resume->decisions.size(), search_budget);
	}
	/* Stage A, run exactly ONCE per slot: offline pre-screen every
	 * NOT-YET-DECIDED slot against the all-Q8 STARTING policy (item 2),
	 * ranked by (passed its own offline check) then by a memory-gain x
	 * offline-quality score. Never counted against search_budget and
	 * never itself accepts anything (item 3) -- it only decides the
	 * ORDER Stage B considers candidates in. */
	struct s_cand
	{
		size_t	qi;
		bool	offline_passed;
		double	offline_score;
		double	offline_cosine;
		double	offline_top1;
	};
	std::vector<s_cand>	cands;

	{
		size_t	qi;
		size_t	stage_a_total;
		size_t	stage_a_index;

		stage_a_total = 0;
		qi = 0;
		while (qi < queue.size())
		{
			if (!decided[qi])
				stage_a_total++;
			qi++;
		}
		progress_stage("stage_a_prescreen", tier, (int)stage_a_total);
		stage_a_index = 0;
		qi = 0;
		while (qi < queue.size())
		{
			if (!decided[qi])
			{
				kv_policy_t				trial;
				per_prompt_result_t	off_res;
				std::string				reason;
				double					mem_gain;
				s_cand					c;
				std::chrono::steady_clock::time_point	cand_t0;
				double					cand_seconds;

				stage_a_index++;
				g_progress.cand_index.store((int)stage_a_index);
				fprintf(stderr, "  [%s/%s] stage_a candidate %zu/%zu: "
					"layer %2d %s -> Q4 (pre-screen)\n", model_name, tier,
					stage_a_index, stage_a_total, queue[qi].layer,
					queue[qi].is_k ? "K" : "V");
				cand_t0 = std::chrono::steady_clock::now();
				trial = start_policy;
				if (queue[qi].is_k)
					trial.kbits[queue[qi].layer] = 4;
				else
					trial.vbits[queue[qi].layer] = 4;
				if (!evaluate_and_check(model, vocab, n_ctx, gen_tokens,
						valid, order, classes, queue[qi].is_k, margin,
						must_stay_correct, EVAL_OFFLINE_BLOB, trial, &off_res,
						&reason))
					return (false);
				out->offline_prescreens_used++;
				mem_gain = slot_memory_gain(valid[0].idx, queue[qi]);
				c.qi = qi;
				c.offline_passed = reason.empty();
				c.offline_cosine = off_res.agg_cosine;
				c.offline_top1 = off_res.agg_top1;
				c.offline_score = c.offline_passed
					? mem_gain * off_res.agg_cosine
					: -mem_gain;
				cands.push_back(c);
				cand_seconds = std::chrono::duration<double>(
					std::chrono::steady_clock::now() - cand_t0).count();
				progress_candidate_done(cand_seconds);
				fprintf(stderr, "  [%s/%s] stage_a candidate %zu/%zu done "
					"in %.1fs: %s  cosine %.6f  top1 %.2f%%  mem-gain "
					"%.0f bytes%s%s\n", model_name, tier, stage_a_index,
					stage_a_total, cand_seconds,
					c.offline_passed ? "PASS" : "FAIL", c.offline_cosine,
					c.offline_top1, mem_gain,
					c.offline_passed ? "" : "  reason: ",
					c.offline_passed ? "" : reason.c_str());
				progress_print_eta(g_progress.avg_eval_seconds.load(),
					(int)stage_a_index, (int)stage_a_total);
			}
			qi++;
		}
	}
	std::sort(cands.begin(), cands.end(),
		[](const s_cand &a, const s_cand &b) {
			if (a.offline_passed != b.offline_passed)
				return (a.offline_passed);
			return (a.offline_score > b.offline_score);
		});
	fprintf(stderr, "  [%s/%s] Stage A: %d slots offline pre-screened once "
		"(%zu passed their own offline check)\n", model_name, tier,
		out->offline_prescreens_used, (size_t)std::count_if(cands.begin(),
			cands.end(), [](const s_cand &c) { return (c.offline_passed); }));
	/* Stage B: process the fixed offline-ranked order with mandatory
	 * real verification (item 3) -- the ONLY stage that can accept a
	 * candidate. First-improvement (not best-of-batch): the first
	 * candidate that clears LIVE_RUNTIME's real hard constraints against
	 * the CURRENT policy is accepted; every other candidate tested is
	 * permanently rejected, one live evaluation per slot at most. */
	progress_stage("stage_b_live", tier, (int)cands.size());
	int	stage_b_index = 0;

	for (const s_cand &c : cands)
	{
		if (out->live_evals_used >= search_budget)
			break ;
		if (decided[c.qi])
			continue ;
		kv_policy_t				trial;
		per_prompt_result_t	live_res;
		std::string				reason;
		double					mem_gain;
		metrics_t				agg{};
		std::chrono::steady_clock::time_point	cand_t0;
		double					cand_seconds;

		stage_b_index++;
		g_progress.cand_index.store(stage_b_index);
		fprintf(stderr, "  [%s/%s] stage_b candidate %d/%zu: layer %2d %s "
			"-> Q4 (live)\n", model_name, tier, stage_b_index, cands.size(),
			queue[c.qi].layer, queue[c.qi].is_k ? "K" : "V");
		cand_t0 = std::chrono::steady_clock::now();
		trial = out->policy;
		if (queue[c.qi].is_k)
			trial.kbits[queue[c.qi].layer] = 4;
		else
			trial.vbits[queue[c.qi].layer] = 4;
		if (!evaluate_and_check(model, vocab, n_ctx, gen_tokens, valid, order,
				classes, queue[c.qi].is_k, margin, must_stay_correct,
				EVAL_LIVE_RUNTIME, trial, &live_res, &reason))
			return (false);
		out->live_evals_used++;
		cand_seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - cand_t0).count();
		progress_candidate_done(cand_seconds);
		decided[c.qi] = true;
		mem_gain = slot_memory_gain(valid[0].idx, queue[c.qi]);
		agg.logit_cosine = live_res.agg_cosine;
		agg.top1_pct = live_res.agg_top1;
		agg.top5_pct = live_res.agg_top5;
		agg.kl_mean = live_res.agg_kl;
		agg.first_divergence = LONG_MAX;
		for (size_t oi = 0; oi < live_res.n_evaluated && oi < order.size();
				oi++)
			if (live_res.per_prompt[order[oi]].first_divergence
					< agg.first_divergence)
				agg.first_divergence
					= live_res.per_prompt[order[oi]].first_divergence;
		if (agg.first_divergence == LONG_MAX)
			agg.first_divergence = 0;
		decision2_t	d;

		d.slot = queue[c.qi];
		d.agg_like = agg;
		d.offline_cosine = c.offline_cosine;
		d.offline_top1 = c.offline_top1;
		d.memory_gain_bytes = mem_gain;
		d.accepted = reason.empty();
		if (d.accepted)
		{
			d.ratio = ratio_from_worst_cosine(live_res, classes, mem_gain);
			d.reason = "accepted";
			fprintf(stderr, "  [%s/%s] stage_b candidate %d/%zu done in "
				"%.1fs: layer %2d %s -> Q4  ACCEPTED  cosine %.6f "
				"(offline predicted %.6f)  top1 %.2f%%  ratio %.1f\n",
				model_name, tier, stage_b_index, cands.size(), cand_seconds,
				queue[c.qi].layer, queue[c.qi].is_k ? "K" : "V",
				live_res.agg_cosine, c.offline_cosine, live_res.agg_top1,
				d.ratio);
			out->accepted.push_back(d);
			if (queue[c.qi].is_k)
				out->policy.kbits[queue[c.qi].layer] = 4;
			else
				out->policy.vbits[queue[c.qi].layer] = 4;
		}
		else
		{
			d.ratio = 0.0;
			d.reason = reason;
			fprintf(stderr, "  [%s/%s] stage_b candidate %d/%zu done in "
				"%.1fs: layer %2d %s -> Q4  REJECTED: %s  cosine %.6f "
				"(offline predicted %.6f)  top1 %.2f%%\n", model_name, tier,
				stage_b_index, cands.size(), cand_seconds, queue[c.qi].layer,
				queue[c.qi].is_k ? "K" : "V", reason.c_str(),
				live_res.agg_cosine, c.offline_cosine, live_res.agg_top1);
			out->rejected.push_back(d);
		}
		progress_print_eta(g_progress.avg_eval_seconds.load(), stage_b_index,
			(int)cands.size());
		write_checkpoint_candidate(checkpoint_out, model_name, tier,
			queue[c.qi].layer, queue[c.qi].is_k, d.accepted, agg.logit_cosine,
			agg.top1_pct, agg.top5_pct, agg.kl_mean, agg.first_divergence,
			c.offline_cosine, c.offline_top1, d.reason);
	}
	out->search_seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - t0).count();
	write_checkpoint_complete(checkpoint_out, model_name, tier,
		out->live_evals_used, out->search_seconds);
	return (true);
}

/* ------------------------------------------------------------------ */
/* item 6: runtime drift measurement -- makes the offline-vs-real gap   */
/* (Phase 4.1's central finding) measurable rather than anecdotal, for  */
/* one representative prompt per final policy (every valid prompt would */
/* double the already-expensive live-runtime cost of reporting; one     */
/* prompt is enough to characterize the drift PATTERN, which is the     */
/* stated goal -- not to exhaustively re-measure every prompt's drift). */
/* ------------------------------------------------------------------ */

typedef struct s_drift_result
{
	double				offline_agg_cosine;
	double				runtime_agg_cosine;
	double				cosine_delta;	/* runtime - offline */
	double				offline_top1;
	double				runtime_top1;
	double				top1_delta;
	long				offline_first_divergence;
	long				runtime_first_divergence;
	std::vector<double>	per_step_drift;	/* runtime - offline, per step */
	double				drift_slope;
}	drift_result_t;

static double	linreg_slope(const std::vector<double> &y)
{
	size_t	n;
	double	sum_x;
	double	sum_y;
	double	sum_xy;
	double	sum_xx;
	double	denom;
	size_t	i;

	n = y.size();
	if (n < 2)
		return (0.0);
	sum_x = 0.0;
	sum_y = 0.0;
	sum_xy = 0.0;
	sum_xx = 0.0;
	i = 0;
	while (i < n)
	{
		sum_x += (double)i;
		sum_y += y[i];
		sum_xy += (double)i * y[i];
		sum_xx += (double)i * (double)i;
		i++;
	}
	denom = (double)n * sum_xx - sum_x * sum_x;
	if (fabs(denom) < 1e-12)
		return (0.0);
	return (((double)n * sum_xy - sum_x * sum_y) / denom);
}

static bool	compute_drift(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &policy, drift_result_t *out)
{
	metrics_t		off_m;
	live_result_t	lr;
	size_t			n;
	size_t			i;

	if (!eval_offline(model, vocab, n_ctx, gen_tokens, base, policy, &off_m))
		return (false);
	if (!eval_live(model, vocab, n_ctx, gen_tokens, base, policy, &lr))
		return (false);
	out->offline_agg_cosine = off_m.logit_cosine;
	out->runtime_agg_cosine = lr.m.logit_cosine;
	out->cosine_delta = lr.m.logit_cosine - off_m.logit_cosine;
	out->offline_top1 = off_m.top1_pct;
	out->runtime_top1 = lr.m.top1_pct;
	out->top1_delta = lr.m.top1_pct - off_m.top1_pct;
	out->offline_first_divergence = off_m.first_divergence;
	out->runtime_first_divergence = lr.m.first_divergence;
	n = off_m.per_step_cosine.size() < lr.m.per_step_cosine.size()
		? off_m.per_step_cosine.size() : lr.m.per_step_cosine.size();
	out->per_step_drift.resize(n);
	i = 0;
	while (i < n)
	{
		out->per_step_drift[i] = lr.m.per_step_cosine[i]
			- off_m.per_step_cosine[i];
		i++;
	}
	out->drift_slope = linreg_slope(out->per_step_drift);
	return (true);
}

/* Which accepted slot's inclusion most increased the offline-vs-runtime
 * gap, reusing data already collected during the search (zero extra
 * live evaluations): for each accepted candidate, the gap between what
 * offline pre-screening predicted and what LIVE_RUNTIME actually
 * measured AT THE MOMENT it was tested. */
static void	report_drift_attribution(FILE *json_out, const char *model_name,
				const char *tier, const optimizer_result_t &opt)
{
	std::vector<size_t>	idx;
	size_t					i;

	idx.resize(opt.accepted.size());
	i = 0;
	while (i < idx.size())
	{
		idx[i] = i;
		i++;
	}
	std::sort(idx.begin(), idx.end(), [&opt](size_t a, size_t b) {
		double	ga = opt.accepted[a].offline_cosine
			- opt.accepted[a].agg_like.logit_cosine;
		double	gb = opt.accepted[b].offline_cosine
			- opt.accepted[b].agg_like.logit_cosine;
		return (ga > gb);
	});
	fprintf(stderr, "  [%s/%s] accepted slots ranked by how much they grew "
		"the offline-vs-runtime gap (predicted - actual cosine, at "
		"acceptance time):\n", model_name, tier);
	for (size_t k = 0; k < idx.size() && k < 5; k++)
	{
		const decision2_t	&d = opt.accepted[idx[k]];
		double				gap = d.offline_cosine - d.agg_like.logit_cosine;

		fprintf(stderr, "    layer %2d %s: offline predicted %.6f, real "
			"%.6f (gap %.6f)\n", d.slot.layer, d.slot.is_k ? "K" : "V",
			d.offline_cosine, d.agg_like.logit_cosine, gap);
		if (json_out != NULL)
			fprintf(json_out, "{\"record\":\"drift_attribution\","
				"\"model\":\"%s\",\"tier\":\"%s\",\"layer\":%d,\"kv\":\"%s\","
				"\"offline_cosine\":%.6f,\"runtime_cosine\":%.6f,"
				"\"gap\":%.6f}\n", model_name, tier, d.slot.layer,
				d.slot.is_k ? "K" : "V", d.offline_cosine,
				d.agg_like.logit_cosine, gap);
	}
}

/* ------------------------------------------------------------------ */
/* CLI                                                                  */
/* ------------------------------------------------------------------ */

typedef struct s_prompt_arg
{
	const char	*path;
	const char	*answer;
}	prompt_arg_t;

typedef struct s_opts
{
	const char					*model_path;
	std::vector<prompt_arg_t>	prompts;
	int							n_tokens;
	int							gen_tokens;
	const char					*model_name;
	int							search_budget;
	const char					*checkpoint_path;
	bool						resume;
	const char					*export_policy_prefix;
	const char					*phase36_policy_path;
	const char					*out_path;
	margin_t					balanced_margin_override;
	bool						has_margin_override;
}	opts_t;

static int	parse_args(int argc, char **argv, opts_t *o)
{
	int	i;

	o->model_path = NULL;
	o->prompts.clear();
	o->n_tokens = 1024;
	o->gen_tokens = 128;
	o->model_name = "model";
	o->search_budget = 60;
	o->checkpoint_path = NULL;
	o->resume = false;
	o->export_policy_prefix = NULL;
	o->phase36_policy_path = NULL;
	o->out_path = NULL;
	o->has_margin_override = false;
	o->balanced_margin_override = g_margin_balanced;
	i = 1;
	while (i < argc)
	{
		if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
			o->model_path = argv[++i];
		else if (strcmp(argv[i], "--prompt") == 0 && i + 2 < argc)
		{
			const char	*ans = strcmp(argv[i + 2], "-") == 0
				? NULL : argv[i + 2];

			o->prompts.push_back({argv[i + 1], ans});
			i += 2;
		}
		else if (strcmp(argv[i], "--n-tokens") == 0 && i + 1 < argc)
			o->n_tokens = atoi(argv[++i]);
		else if (strcmp(argv[i], "--gen-tokens") == 0 && i + 1 < argc)
			o->gen_tokens = atoi(argv[++i]);
		else if (strcmp(argv[i], "--model-name") == 0 && i + 1 < argc)
			o->model_name = argv[++i];
		else if (strcmp(argv[i], "--search-budget") == 0 && i + 1 < argc)
			o->search_budget = atoi(argv[++i]);
		else if (strcmp(argv[i], "--checkpoint") == 0 && i + 1 < argc)
			o->checkpoint_path = argv[++i];
		else if (strcmp(argv[i], "--resume") == 0)
			o->resume = true;
		else if (strcmp(argv[i], "--export-policy-prefix") == 0
				&& i + 1 < argc)
			o->export_policy_prefix = argv[++i];
		else if (strcmp(argv[i], "--phase36-policy") == 0 && i + 1 < argc)
			o->phase36_policy_path = argv[++i];
		else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
			o->out_path = argv[++i];
		else if (strcmp(argv[i], "--cosine-margin") == 0 && i + 1 < argc)
		{
			o->balanced_margin_override.cosine_margin = atof(argv[++i]);
			o->has_margin_override = true;
		}
		else if (strcmp(argv[i], "--top1-margin") == 0 && i + 1 < argc)
		{
			o->balanced_margin_override.top1_margin = atof(argv[++i]);
			o->has_margin_override = true;
		}
		else if (strcmp(argv[i], "--top5-margin") == 0 && i + 1 < argc)
		{
			o->balanced_margin_override.top5_margin = atof(argv[++i]);
			o->has_margin_override = true;
		}
		else
			return (die("unknown or malformed option"));
		i++;
	}
	if (o->model_path == NULL || o->prompts.empty())
		return (die("usage: --model PATH --prompt PATH ANSWER|- [...] "
				"[--n-tokens N] [--gen-tokens G] [--model-name LABEL] "
				"[--search-budget N] "
				"[--checkpoint PATH] [--resume] "
				"[--export-policy-prefix PREFIX] "
				"[--phase36-policy POLICY.mpol] [--out JSONL]"));
	return (0);
}

/* ------------------------------------------------------------------ */
/* item 4: baseline filtering + all-Q8 starting-policy validation.      */
/* Two distinct exclusion reasons, both reported, neither papered over: */
/*   - FP16 baseline itself doesn't answer correctly (Phase 3's         */
/*     original gate).                                                  */
/*   - FP16 baseline DOES answer correctly, but the all-Q8 STARTING     */
/*     policy (item 2) -- evaluated for real, via LIVE_RUNTIME -- fails */
/*     a class threshold anyway. This is a quantization-noise limit     */
/*     that exists before the search even starts; loosening the        */
/*     threshold to "fix" it would hide a real property of the model,   */
/*     not of any candidate this optimizer chooses.                    */
/* ------------------------------------------------------------------ */

typedef struct s_prepared
{
	std::vector<baseline_t>		all_bases;
	std::vector<bool>				fp16_correct;
	std::vector<prompt_class_t>	all_classes;
	std::vector<baseline_t>		valid;
	std::vector<prompt_class_t>	valid_classes;
	std::vector<bool>				must_stay_correct;	/* all-Q8's own recall */
	std::vector<std::string>		baseline_limited;	/* prompt names */
	kv_policy_t						q8_start;
	per_prompt_result_t				q8_live_all;	/* over all_bases */
}	prepared_t;

static bool	prepare_valid_set(llama_model *model, const llama_vocab *vocab,
				const opts_t &o, prepared_t *out)
{
	size_t	i;

	fprintf(stderr, "\n=== Phase 4.2: baseline filtering ===\n");
	progress_stage("baseline_prep", "", (int)o.prompts.size());
	i = 0;
	while (i < o.prompts.size())
	{
		baseline_t	b;
		std::chrono::steady_clock::time_point	prompt_t0;
		double		prompt_seconds;

		prompt_t0 = std::chrono::steady_clock::now();
		g_progress.cand_index.store((int)i + 1);
		if (!capture_baseline(model, vocab, o.n_tokens, o.gen_tokens,
				o.prompts[i].path, o.prompts[i].answer, &b))
			return (false);
		prompt_seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - prompt_t0).count();
		progress_candidate_done(prompt_seconds);
		bool	fp16_ok = (b.answer == NULL)
			|| (b.text.find(b.answer) != std::string::npos);
		prompt_class_t	c = classify_prompt(b);

		fprintf(stderr, "  prompt %zu/%zu done in %.1fs\n", i + 1,
			o.prompts.size(), prompt_seconds);
		fprintf(stderr, "  %-32s [%-16s] FP16 baseline %s\n", o.prompts[i].path,
			class_name(c), fp16_ok ? "OK" : "WRONG (excluded)");
		out->all_classes.push_back(c);
		out->fp16_correct.push_back(fp16_ok);
		out->all_bases.push_back(std::move(b));
		i++;
	}
	if (out->all_bases.empty())
		return (die("no prompts"), false);
	out->q8_start = all_q8_policy(out->all_bases[0].idx.n_layer);
	if (!evaluate_full(model, vocab, o.n_tokens, o.gen_tokens, out->all_bases,
			EVAL_LIVE_RUNTIME, out->q8_start, &out->q8_live_all))
		return (false);
	fprintf(stderr, "\n=== all-Q8 starting policy, real LIVE_RUNTIME "
		"reference ===\n");
	i = 0;
	while (i < out->all_bases.size())
	{
		std::string	reason = check_prompt(out->q8_live_all.per_prompt[i],
				out->all_bases[i].name, out->all_classes[i], false,
				g_margin_aggressive, false);
		bool	threshold_ok = reason.empty();

		fprintf(stderr, "  %-32s cosine %.6f top1 %.2f%% top5 %.2f%% "
			"recall %s  %s\n", out->all_bases[i].name.c_str(),
			out->q8_live_all.per_prompt[i].logit_cosine,
			out->q8_live_all.per_prompt[i].top1_pct,
			out->q8_live_all.per_prompt[i].top5_pct,
			out->q8_live_all.per_prompt[i].recall_ok ? "OK" : "FAIL",
			threshold_ok ? "" : "-- BASELINE LIMIT (not a candidate's "
				"fault, see below)");
		if (out->fp16_correct[i] && !threshold_ok)
		{
			out->baseline_limited.push_back(out->all_bases[i].name);
			fprintf(stderr, "      quantization-caused baseline limit: "
				"all-Q8 itself does not clear the %s threshold for this "
				"prompt (%s); excluded from the search's hard-gate set, "
				"threshold NOT loosened.\n",
				class_name(out->all_classes[i]), reason.c_str());
		}
		if (out->fp16_correct[i] && threshold_ok)
		{
			out->valid.push_back(out->all_bases[i]);
			out->valid_classes.push_back(out->all_classes[i]);
			out->must_stay_correct.push_back(
				out->q8_live_all.per_prompt[i].recall_ok);
		}
		i++;
	}
	fprintf(stderr, "\nvalid (hard-gate) evaluation set: %zu/%zu prompts "
		"(%zu excluded: FP16 baseline wrong; %zu excluded: all-Q8 "
		"baseline limit)\n", out->valid.size(), out->all_bases.size(),
		(size_t)std::count(out->fp16_correct.begin(),
			out->fp16_correct.end(), false), out->baseline_limited.size());
	if (out->valid.empty())
		return (die("no valid prompts after baseline filtering -- cannot "
				"search"), false);
	return (true);
}

/* ------------------------------------------------------------------ */
/* item 7: three Pareto tiers, each decided purely by LIVE_RUNTIME.     */
/* ------------------------------------------------------------------ */

typedef struct s_pareto_point
{
	const char			*name;
	margin_t			margin;
	optimizer_result_t	opt;
}	pareto_point_t;

static bool	run_pareto(llama_model *model, const llama_vocab *vocab,
				const opts_t &o, const prepared_t &prep,
				const std::string &model_hash, const std::string &prompt_hash,
				FILE *json_out, std::vector<pareto_point_t> *out)
{
	const struct { const char *name; margin_t margin; }	levels[] = {
		{"conservative", g_margin_conservative},
		{"balanced", o.has_margin_override ? o.balanced_margin_override
			: g_margin_balanced},
		{"aggressive", g_margin_aggressive},
	};
	FILE	*checkpoint_out;
	size_t	li;

	/* Peek at any EXISTING header for this model before writing anything
	 * new -- writing a fresh header first (from THIS run's own, by
	 * definition self-consistent values) and only then checking would
	 * make every staleness check trivially pass against itself,
	 * defeating item 8's stale-checkpoint rejection entirely. A stale
	 * header refuses the run outright, regardless of --resume: even a
	 * fresh (non-resuming) run must not append new decisions under a
	 * model/commit/prompt-set identity that contradicts what is already
	 * in the file. */
	if (o.checkpoint_path != NULL)
	{
		checkpoint_state_t	peek;

		peek = load_checkpoint(o.checkpoint_path, o.model_name,
				"conservative", model_hash, MEMBRANE_LLAMA_CPP_COMMIT,
				prompt_hash, TOOL_VERSION);
		if (peek.header_present && !peek.header_matches)
			return (die(("STALE checkpoint, refusing to write or resume: "
					+ peek.mismatch_reason).c_str()), false);
		checkpoint_out = fopen(o.checkpoint_path, "a");
		if (checkpoint_out == NULL)
			return (die("failed to open --checkpoint for append"), false);
		if (!peek.header_present
				&& !write_checkpoint_header(checkpoint_out, o.model_name,
					model_hash, MEMBRANE_LLAMA_CPP_COMMIT, prompt_hash,
					TOOL_VERSION))
			return (fclose(checkpoint_out), die("failed to write checkpoint "
					"header"), false);
	}
	else
		checkpoint_out = NULL;
	li = 0;
	while (li < sizeof(levels) / sizeof(levels[0]))
	{
		pareto_point_t		p;
		checkpoint_state_t	resume_state;

		p.name = levels[li].name;
		p.margin = levels[li].margin;
		resume_state.header_present = false;
		resume_state.header_matches = false;
		resume_state.tier_complete = false;
		if (o.resume && o.checkpoint_path != NULL)
		{
			resume_state = load_checkpoint(o.checkpoint_path, o.model_name,
					p.name, model_hash, MEMBRANE_LLAMA_CPP_COMMIT, prompt_hash,
					TOOL_VERSION);
			if (resume_state.header_present && !resume_state.header_matches)
			{
				if (checkpoint_out != NULL)
					fclose(checkpoint_out);
				fprintf(stderr, "membrane-kv-runtime-optimizer: STALE "
					"checkpoint, refusing to resume: %s\n",
					resume_state.mismatch_reason.c_str());
				return (false);
			}
		}
		fprintf(stderr, "\n=== Pareto point: %s (cosine_margin=%.4f "
			"top1_margin=%.3f top5_margin=%.3f) ===\n", p.name,
			p.margin.cosine_margin, p.margin.top1_margin,
			p.margin.top5_margin);
		if (!greedy_search(model, vocab, o.n_tokens, o.gen_tokens, prep.valid,
				prep.valid_classes, prep.must_stay_correct, p.margin,
				o.search_budget, prep.q8_start, &p.opt, o.model_name, p.name,
				checkpoint_out, o.resume ? &resume_state : NULL))
		{
			if (checkpoint_out != NULL)
				fclose(checkpoint_out);
			return (false);
		}
		fprintf(stderr, "  %s: %zu accepted, %zu rejected, %d live evals, "
			"%d offline pre-screens, %.1fs\n", p.name, p.opt.accepted.size(),
			p.opt.rejected.size(), p.opt.live_evals_used,
			p.opt.offline_prescreens_used, p.opt.search_seconds);
		report_drift_attribution(json_out, o.model_name, p.name, p.opt);
		progress_stage("drift", p.name, 1);
		{
			drift_result_t	drift;

			if (!compute_drift(model, vocab, o.n_tokens, o.gen_tokens,
					prep.valid[0], p.opt.policy, &drift))
			{
				if (checkpoint_out != NULL)
					fclose(checkpoint_out);
				return (false);
			}
			fprintf(stderr, "  [%s/%s] drift on '%s': offline cosine %.6f "
				"vs runtime %.6f (delta %+.6f), offline top1 %.2f%% vs "
				"runtime %.2f%% (delta %+.2f), first divergence offline=%ld "
				"runtime=%ld, per-step drift slope %+.6f/step\n",
				o.model_name, p.name, prep.valid[0].name.c_str(),
				drift.offline_agg_cosine, drift.runtime_agg_cosine,
				drift.cosine_delta, drift.offline_top1, drift.runtime_top1,
				drift.top1_delta, drift.offline_first_divergence,
				drift.runtime_first_divergence, drift.drift_slope);
			if (json_out != NULL)
			{
				fprintf(json_out, "{\"record\":\"drift\",\"model\":\"%s\","
					"\"tier\":\"%s\",\"prompt\":\"%s\","
					"\"offline_cosine\":%.6f,\"runtime_cosine\":%.6f,"
					"\"cosine_delta\":%.6f,\"offline_top1\":%.4f,"
					"\"runtime_top1\":%.4f,\"top1_delta\":%.4f,"
					"\"offline_first_divergence\":%ld,"
					"\"runtime_first_divergence\":%ld,"
					"\"drift_slope\":%.6f,\"per_step_drift\":[",
					o.model_name, p.name, prep.valid[0].name.c_str(),
					drift.offline_agg_cosine, drift.runtime_agg_cosine,
					drift.cosine_delta, drift.offline_top1,
					drift.runtime_top1, drift.top1_delta,
					drift.offline_first_divergence,
					drift.runtime_first_divergence, drift.drift_slope);
				for (size_t si = 0; si < drift.per_step_drift.size(); si++)
					fprintf(json_out, "%s%.6f", si ? "," : "",
						drift.per_step_drift[si]);
				fprintf(json_out, "]}\n");
			}
		}
		out->push_back(std::move(p));
		li++;
	}
	if (checkpoint_out != NULL)
		fclose(checkpoint_out);
	return (true);
}

/* ------------------------------------------------------------------ */
/* item 10: final comparison table -- FP16 / all-Q8 / all-Q4 / an       */
/* optional Phase 3.6 offline-derived policy / the three new tiers,     */
/* every row measured the SAME way (real LIVE_RUNTIME, no splicing).    */
/* ------------------------------------------------------------------ */

static kv_policy_t	all_f16_policy(uint32_t n_layer)
{
	kv_policy_t	p;

	p.kbits.assign(n_layer, 16);
	p.vbits.assign(n_layer, 16);
	return (p);
}

static kv_policy_t	all_q4_policy(uint32_t n_layer)
{
	kv_policy_t	p;

	p.kbits.assign(n_layer, 4);
	p.vbits.assign(n_layer, 4);
	return (p);
}

static void	print_and_emit_row(FILE *json_out, const char *model_name,
				const char *config, const char *prompt_name,
				const live_result_t &r, double kv_ratio)
{
	fprintf(stderr, "  %-24s %-24s top1 %6.2f%%  top5 %6.2f%%  cosine "
		"%.6f  KL %.6f  recall %-4s  KV %10zu bytes (%.3fx)  TTFT %8.1fms  "
		"%6.1f tok/s  peakRSS %6ldMB\n", config, prompt_name, r.m.top1_pct,
		r.m.top5_pct, r.m.logit_cosine, r.m.kl_mean,
		r.m.recall_ok ? "OK" : "FAIL", r.kv_bytes, kv_ratio, r.ttft_ms,
		r.tok_per_sec, r.peak_rss_kb / 1024);
	if (json_out == NULL)
		return ;
	fprintf(json_out, "{\"record\":\"comparison_row\",\"model\":\"%s\","
		"\"config\":\"%s\",\"prompt\":\"%s\",\"top1_pct\":%.6f,"
		"\"top5_pct\":%.6f,\"logit_cosine\":%.6f,\"kl_divergence\":%.6f,"
		"\"recall_ok\":%s,\"kv_bytes\":%zu,\"kv_reduction_x\":%.6f,"
		"\"ttft_ms\":%.3f,\"tok_per_sec\":%.3f,\"peak_rss_kb\":%ld,"
		"\"first_divergence\":%ld}\n", model_name, config, prompt_name,
		r.m.top1_pct, r.m.top5_pct, r.m.logit_cosine, r.m.kl_mean,
		r.m.recall_ok ? "true" : "false", r.kv_bytes, kv_ratio, r.ttft_ms,
		r.tok_per_sec, r.peak_rss_kb, r.m.first_divergence);
}

static bool	run_final_comparison(llama_model *model, const llama_vocab *vocab,
				const opts_t &o, const prepared_t &prep,
				const std::vector<pareto_point_t> &pareto, FILE *json_out)
{
	uint32_t	n_layer;
	std::vector<std::pair<std::string, kv_policy_t>>	configs;

	n_layer = prep.all_bases[0].idx.n_layer;
	configs.push_back({"all-FP16", all_f16_policy(n_layer)});
	configs.push_back({"all-Q8", prep.q8_start});
	configs.push_back({"all-Q4", all_q4_policy(n_layer)});
	if (o.phase36_policy_path != NULL)
	{
		membrane_policy_t	*p36;

		if (membrane_policy_load(o.phase36_policy_path, &p36) == MEMBRANE_OK)
		{
			kv_policy_t	pol;
			uint32_t	l;
			bool		ok;

			pol.kbits.resize(n_layer);
			pol.vbits.resize(n_layer);
			ok = (membrane_policy_layer_count(p36) == n_layer);
			l = 0;
			while (ok && l < n_layer)
			{
				membrane_precision_t	pk;
				membrane_precision_t	pv;

				ok = membrane_policy_query(p36, l, 0, &pk) == MEMBRANE_OK
					&& membrane_policy_query(p36, l, 1, &pv) == MEMBRANE_OK;
				if (ok)
				{
					pol.kbits[l] = (int)pk;
					pol.vbits[l] = (int)pv;
				}
				l++;
			}
			if (ok)
				configs.push_back({"phase3.6-offline-policy", pol});
			else
				fprintf(stderr, "  (--phase36-policy: layer count mismatch "
					"or query failure, skipping this comparison row)\n");
			membrane_policy_destroy(p36);
		}
		else
			fprintf(stderr, "  (--phase36-policy: failed to load '%s', "
				"skipping this comparison row)\n", o.phase36_policy_path);
	}
	for (const pareto_point_t &pp : pareto)
		configs.push_back({pp.name, pp.opt.policy});
	fprintf(stderr, "\n=== Final comparison (all real LIVE_RUNTIME, %zu "
		"configs x %zu prompts) ===\n", configs.size(), prep.all_bases.size());
	progress_stage("final_comparison", "",
		(int)(configs.size() * prep.all_bases.size()));
	for (const baseline_t &b : prep.all_bases)
	{
		live_result_t	fp16_ref;

		if (!eval_live(model, vocab, o.n_tokens, o.gen_tokens, b,
				all_f16_policy(n_layer), &fp16_ref))
			return (false);
		for (const auto &cfg : configs)
		{
			live_result_t	r;
			std::chrono::steady_clock::time_point	row_t0;
			double									row_seconds;

			row_t0 = std::chrono::steady_clock::now();
			g_progress.cand_index.fetch_add(1);
			fprintf(stderr, "  final_comparison candidate %d/%d: %s x '%s'\n",
				g_progress.cand_index.load(), g_progress.cand_total.load(),
				cfg.first.c_str(), b.name.c_str());
			if (!eval_live(model, vocab, o.n_tokens, o.gen_tokens, b,
					cfg.second, &r))
				return (false);
			row_seconds = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - row_t0).count();
			progress_candidate_done(row_seconds);
			print_and_emit_row(json_out, o.model_name, cfg.first.c_str(),
				b.name.c_str(), r,
				(double)fp16_ref.kv_bytes / (double)r.kv_bytes);
			fprintf(stderr, "  final_comparison candidate %d/%d done in "
				"%.1fs\n", g_progress.cand_index.load(),
				g_progress.cand_total.load(), row_seconds);
			progress_print_eta(g_progress.avg_eval_seconds.load(),
				g_progress.cand_index.load(), g_progress.cand_total.load());
		}
	}
	return (true);
}

/* ------------------------------------------------------------------ */
/* Policy export -- reuses Phase 4.1's membrane_policy_save directly.   */
/* ------------------------------------------------------------------ */

static bool	export_pareto_policies(const opts_t &o, const prepared_t &prep,
				const std::vector<pareto_point_t> &pareto,
				const uint8_t model_sha256[MEMBRANE_SHA256_DIGEST_BYTES])
{
	if (o.export_policy_prefix == NULL)
		return (true);
	progress_stage("export", "", (int)pareto.size());
	for (const pareto_point_t &pp : pareto)
	{
		std::vector<membrane_precision_t>	kp;
		std::vector<membrane_precision_t>	vp;
		membrane_policy_build_t				b;
		std::string							path;
		uint32_t							l;

		kp.resize(pp.opt.policy.kbits.size());
		vp.resize(pp.opt.policy.vbits.size());
		l = 0;
		while (l < kp.size())
		{
			kp[l] = (membrane_precision_t)pp.opt.policy.kbits[l];
			vp[l] = (membrane_precision_t)pp.opt.policy.vbits[l];
			l++;
		}
		memset(&b, 0, sizeof(b));
		memcpy(b.model_sha256, model_sha256, MEMBRANE_SHA256_DIGEST_BYTES);
		b.llama_cpp_commit = MEMBRANE_LLAMA_CPP_COMMIT;
		b.layer_count = (uint32_t)kp.size();
		b.k_prec = kp.data();
		b.v_prec = vp.data();
		b.model_name = o.model_name;
		b.tier_name = pp.name;
		b.cosine_min = g_default_thresholds.cosine_min;
		b.top1_min = g_default_thresholds.top1_min;
		b.top5_min = g_default_thresholds.top5_min;
		b.cosine_margin = pp.margin.cosine_margin;
		b.top1_margin = pp.margin.top1_margin;
		b.top5_margin = pp.margin.top5_margin;
		b.search_budget = (uint32_t)o.search_budget;
		b.evals_used = (uint32_t)pp.opt.live_evals_used;
		b.created_unix_time = (uint64_t)time(NULL);
		path = std::string(o.export_policy_prefix) + "-" + pp.name + ".mpol";
		if (membrane_policy_save(path.c_str(), &b) != MEMBRANE_OK)
		{
			fprintf(stderr, "  (--export-policy-prefix: failed to write "
				"%s)\n", path.c_str());
			continue ;
		}
		fprintf(stderr, "  exported %s (%zu accepted Q4 slots)\n",
			path.c_str(), pp.opt.accepted.size());
	}
	(void)prep;
	return (true);
}

int	main(int argc, char **argv)
{
	opts_t				o;
	llama_model			*model;
	const llama_vocab	*vocab;
	prepared_t			prep;
	char				model_hex[MEMBRANE_SHA256_HEX_LEN + 1];
	uint8_t				model_digest[MEMBRANE_SHA256_DIGEST_BYTES];
	std::string			prompt_hash;
	std::vector<pareto_point_t>	pareto;
	FILE				*json_out;
	std::chrono::steady_clock::time_point	t0;
	bool				ok;

	if (parse_args(argc, argv, &o) != 0)
		return (1);
	/* Long real-runtime searches are typically launched with stdout/stderr
	 * redirected to a log file, which switches libc's default stream
	 * buffering from line-buffered to fully block-buffered -- silently
	 * delaying every progress line until an 4-8KB buffer fills. Force both
	 * streams unbuffered so `tail -f` on the log shows progress as it
	 * actually happens. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	g_quant_backend = membrane_simd_backend_name(membrane_simd_best_backend());
	fprintf(stderr, "quant engine: backend=%s threads=%d "
		"(single-row calls; see docs/phase5-quant-engine.md)\n",
		g_quant_backend, g_quant_threads);
	progress_init();
	std::thread(heartbeat_loop).detach();
	llama_backend_init();
	model = llama_model_load_from_file(o.model_path,
			llama_model_default_params());
	if (model == NULL)
		return (die("model load failed"), 1);
	vocab = llama_model_get_vocab(model);
	if (membrane_sha256_file(o.model_path, model_hex) != MEMBRANE_OK)
		return (llama_model_free(model),
			die("could not hash --model"), 1);
	{
		size_t	i;

		i = 0;
		while (i < MEMBRANE_SHA256_DIGEST_BYTES)
		{
			unsigned int	byte;

			sscanf(model_hex + i * 2, "%2x", &byte);
			model_digest[i] = (uint8_t)byte;
			i++;
		}
	}
	t0 = std::chrono::steady_clock::now();
	ok = prepare_valid_set(model, vocab, o, &prep);
	if (ok)
	{
		std::vector<std::string>	prompt_paths;
		std::vector<std::string>	prompt_contents;

		for (const prompt_arg_t &pa : o.prompts)
		{
			prompt_paths.push_back(pa.path);
			prompt_contents.push_back(read_file(pa.path));
		}
		prompt_hash = compute_prompt_set_hash_from_contents(prompt_paths,
				prompt_contents);
		json_out = o.out_path != NULL ? fopen(o.out_path, "a") : NULL;
		ok = run_pareto(model, vocab, o, prep, std::string(model_hex),
				prompt_hash, json_out, &pareto);
		if (ok)
			ok = run_final_comparison(model, vocab, o, prep, pareto,
					json_out);
		if (ok)
			ok = export_pareto_policies(o, prep, pareto, model_digest);
		if (json_out != NULL)
			fclose(json_out);
	}
	fprintf(stderr, "\ntotal wall clock: %.1fs\n",
		std::chrono::duration<double>(
			std::chrono::steady_clock::now() - t0).count());
	fprintf(stderr, "peak RSS (whole process): %ld MB\n",
		peak_rss_kb() / 1024);
	llama_model_free(model);
	return (ok ? 0 : 1);
}
