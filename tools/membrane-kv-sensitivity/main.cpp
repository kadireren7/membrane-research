/*
 * membrane-kv-sensitivity: per-layer and per-token-age quantization
 * sensitivity profiling, and a threshold-driven mixed-precision policy
 * (Phase 3.3).
 *
 * WHY THIS TOOL EXISTS AND HOW IT DIFFERS FROM membrane-kv-quality:
 * Phase 3.2 established that llama.cpp's `type_k`/`type_v` context
 * parameters are single scalars applied UNIFORMLY to every layer (verified
 * in the vendored submodule, third_party/llama.cpp/src/llama-kv-cache.cpp:
 * one `type_k`/`type_v` passed to every layer's tensor creation, no
 * per-layer array anywhere). So there is no public API to say "only layer
 * 12 is quantized" or "only the oldest 25% of tokens are quantized" --
 * per-layer and per-token-age experiments (Phase 3.3 items 1 and 3) are
 * not reachable through llama_context_params at all.
 *
 * This tool reaches them anyway, through a different public API:
 * llama_state_seq_get_data / llama_state_seq_set_data (already used by
 * tools/membrane-kv-capture to export real KV tensors). The technique:
 *   1. Decode a prompt once on an ordinary F16 context; extract the full
 *      state blob.
 *   2. Parse the blob (bounds-checked, pinned to the same llama.cpp commit
 *      as membrane-kv-capture, aborting loudly on any layout mismatch
 *      rather than silently misreading it) to find the exact byte offset
 *      and per-token row size of every layer's K and V data.
 *   3. On a COPY of the blob, overwrite chosen (layer, K-or-V, token-row
 *      range) byte ranges in place: quantize those F16 values with
 *      MEMBRANE's own symmetric per-32-element-group math (the same math
 *      as membrane_q8_encode, ggml's Q8_0/Q4_0 block scheme) and
 *      immediately dequantize back to F16 bytes of the exact same length
 *      -- so the blob's size and structure never change, only the
 *      numeric values within the targeted ranges. Untouched ranges are
 *      bit-identical to the true F16 capture.
 *   4. Load the perturbed blob into a FRESH context via
 *      llama_state_seq_set_data and continue generation from there,
 *      comparing against the true baseline's continuation.
 * A self-test (perturbing nothing) is run first and must reproduce the
 * baseline exactly -- this is the correctness proof that the blob
 * round-trip itself introduces no distortion, before any real experiment
 * is trusted.
 *
 * The K/V-combination sweep (item 2 of the task) does NOT need this
 * technique -- type_k and type_v are independently settable (each is
 * uniform across layers, but the two are independent scalars), so those
 * six combinations use real ggml native types exactly like Phase 3.2.
 *
 * KV memory reduction for spliced (simulated) experiments cannot come
 * from llama_state_seq_get_size: physically, the context always stores
 * F16 (this technique injects numeric noise, it does not change physical
 * storage). Instead it is computed analytically from the real measured
 * per-layer row sizes and the actual ggml block-quant storage formula
 * (Q8_0: 34 bytes/32 elements; Q4_0: 18 bytes/32 elements; both real,
 * documented ggml constants, not invented), applied to the byte ranges
 * marked FP16/Q8/Q4 by whatever policy is being evaluated. This is
 * labeled clearly wherever it is reported, distinct from the K/V-sweep's
 * real, measured get_size numbers.
 *
 * Phase 3.4 adds a composition-aware greedy optimizer on top of the same
 * splicing engine: Phase 3.3's policy unioned INDEPENDENT per-layer
 * scores and found the union did not compose linearly (a combined
 * cosine below what every constituent layer cleared alone). The
 * optimizer below never accepts a candidate from an isolated score --
 * every accept/reject decision re-measures the CURRENT, already-accepted
 * policy live. See the "Phase 3.4" section further down for the search
 * design.
 */

#include <sys/resource.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "llama.h"
#include "membrane/f16convert.h"
#include "membrane/ggml_quant.h"

# define SEQ_STATE_MAGIC 0xaf143cd8u
# define GROUP_ELEMS 32

static int	die(const char *msg)
{
	fprintf(stderr, "membrane-kv-sensitivity: %s\n", msg);
	return (-1);
}

/* ------------------------------------------------------------------ */
/* State-blob cursor + bounds-checked reads (mirrors                  */
/* tools/membrane-kv-capture/main.cpp, pinned to the same commit).    */
/* ------------------------------------------------------------------ */

typedef struct s_cursor
{
	const uint8_t	*p;
	size_t			left;
	size_t			base_off;	/* offset of p from the blob's start */
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

/* Locates one layer's row-major tensor (K always; V when v_trans==0):
 * [i32 type][u64 row_size][cell_count x row_size bytes]. Records the
 * offset/row_size and skips over the payload. */
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

/* Parses the full blob, filling `idx`. Returns false (with a die()
 * message) on ANY layout deviation, including v_trans==1 (transposed V)
 * -- this tool only implements the row-major V layout, which is what
 * this build's flash-attention default produces (v_trans = !flash_attn,
 * verified in llama-model.cpp); it refuses to guess at the strided
 * layout rather than silently misindex it. */
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

/* ------------------------------------------------------------------ */
/* MEMBRANE-math quantize-then-immediately-dequantize, in place. Same  */
/* symmetric per-group scheme as membrane_q8_encode (scale =           */
/* max_abs/qmax over one 32-element group), just without the storage   */
/* wire format -- here we want the numeric effect in the F16 bytes,    */
/* not a serialized byte stream, so this reuses membrane_f16_to_f32 /  */
/* membrane_f32_to_f16 (the real conversion primitives) directly.      */
/* ------------------------------------------------------------------ */

/* ggml-exact (Phase 4.4): applies membrane_ggml_quant_roundtrip (ggml's
 * own Q8_0/Q4_0 quantize+dequantize, see membrane/ggml_quant.h) over
 * `len` bytes of F16 data. Replaces Phase 3.3's own linear
 * per-32-element max-abs quantizer, which did not match ggml's real
 * block format or rounding (docs/phase4-ggml-quant-parity.md documents
 * the retired formula and exactly how it differed). Every real
 * Q8_0/Q4_0-eligible KV row length is already a multiple of the
 * 32-element block size (ggml itself requires n_embd_head % blck_size
 * == 0 for a quantized KV cache type to be constructible at all), so no
 * partial-block handling is needed here. */
static void	quant_roundtrip_inplace(uint8_t *data, size_t len, int bits)
{
	if (bits == 16)
		return ;
	membrane_ggml_quant_roundtrip((uint16_t *)(void *)data, len / 2, bits);
}

typedef struct s_perturb_target
{
	int			layer;
	bool		do_k;
	bool		do_v;
	uint32_t	row_start;
	uint32_t	row_end;
	int			bits;	/* 16 = FP16 (no-op), 8, or 4 */
}	perturb_target_t;

/* Quantizes each targeted row independently (per-token grouping, matching
 * ggml's own per-cache-entry Q8_0/Q4_0 blocking), not across row
 * boundaries. */
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
/* Model / context / generation helpers (same design as                */
/* membrane-kv-quality, Phase 3.2).                                    */
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
						ggml_type type_k, ggml_type type_v)
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
	if (type_k != GGML_TYPE_F16 || type_v != GGML_TYPE_F16)
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

/* ------------------------------------------------------------------ */
/* Comparison metrics (same design as Phase 3.2's compare_step).       */
/* ------------------------------------------------------------------ */

typedef struct s_metrics
{
	double	top1_pct;
	double	top5_pct;
	double	logit_cosine;
	double	logit_rmse;
	double	kl_mean;
	long	first_divergence;
	bool	recall_ok;
	std::string	text;
}	metrics_t;

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
				double *sumsq, uint64_t *n_elems, double *kl_sum,
				uint64_t *top1, uint64_t *top5)
{
	double	dot;
	double	na;
	double	nb;
	double	diff;
	double	mx;
	double	sum_pb;
	double	sum_pc;
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
		diff = (double)base[i] - (double)cand[i];
		*sumsq += diff * diff;
	}
	*n_elems += base.size();
	if (na > 0.0 && nb > 0.0)
		*cos_sum += dot / (sqrt(na) * sqrt(nb));
	else
		*cos_sum += (na == 0.0 && nb == 0.0) ? 1.0 : 0.0;
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

/* ------------------------------------------------------------------ */
/* One experiment: capture a baseline once per prompt, then for each   */
/* target list, splice + continue + compare.                          */
/* ------------------------------------------------------------------ */

/*
 * The captured blob covers only the PROMPT PREFIX (all but the last
 * token) -- llama_state_seq_get_data/set_data serialize the KV cache
 * memory only, not the output/logits buffer (verified: both calls just
 * forward to llama_memory::state_write/state_read), so there is no valid
 * "logits after the restored state" available immediately after a
 * set_data call. The last prompt token is therefore always decoded FRESH
 * in every experiment (baseline included, for structural symmetry): that
 * decode attends over the full prefix cache -- perturbed or not -- and
 * produces correct, perturbation-sensitive logits for what comes next.
 * Every experiment restores this same prefix, decodes the same last
 * prompt token, then generates gen_tokens steps from there, so baseline
 * and every perturbed run follow an IDENTICAL decode structure and any
 * difference between them is attributable only to the perturbation.
 */
typedef struct s_baseline
{
	std::vector<llama_token>	prompt_tokens;	/* full prompt, incl. last */
	llama_token					last_token;
	pass_result_t				free_run;	/* reference tokens/logits/text */
	std::vector<uint8_t>		blob;		/* prefix only: prompt[0..P-2] */
	blob_index_t				idx;
	std::string					text;
	std::string					name;		/* prompt file path, for reporting
											 * and Phase 3.5 classification */
	const char					*answer;	/* substring to check, or NULL */
	size_t						f16_state_bytes;	/* real, whole-blob, full
					 * prompt: the fair denominator for NATIVE-type ratios
					 * (measured vs measured); full_f16_bytes(idx) is the
					 * analytic tensor-only figure used for the spliced/
					 * projected policy ratios (analytic vs analytic) --
					 * these two denominators are deliberately different
					 * measurements and must not be mixed (a real state
					 * blob includes prologue/cell-metadata/output-buffer
					 * bytes beyond just the K/V tensors; at short contexts
					 * that fixed overhead is non-trivial). */
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
	/* Real, whole-blob F16 baseline size (full prompt, no generation) --
	 * the fair denominator for native-type ratios, measured the same way
	 * as the candidate types in run_kv_combo. */
	ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16);
	if (ctx == NULL)
		return (die("f16 reference context creation failed"), false);
	ok = decode_prompt(ctx, out->prompt_tokens, 256);
	if (ok)
		out->f16_state_bytes = llama_state_seq_get_size(ctx, 0);
	llama_free(ctx);
	return (ok);
}

/* Fills `m` by comparing a candidate's free-running and teacher-forced
 * (on the baseline's own tokens) passes against the cached baseline. */
static void	fill_metrics(const llama_vocab *vocab, const baseline_t &base,
				int gen_tokens, const pass_result_t &free_run,
				const pass_result_t &forced_run, metrics_t *m)
{
	double		cos_sum;
	double		sumsq;
	uint64_t	n_elems;
	double		kl_sum;
	uint64_t	top1;
	uint64_t	top5;
	size_t		steps;
	size_t		i;

	cos_sum = 0.0;
	sumsq = 0.0;
	n_elems = 0;
	kl_sum = 0.0;
	top1 = 0;
	top5 = 0;
	steps = base.free_run.logits.size() < forced_run.logits.size()
		? base.free_run.logits.size() : forced_run.logits.size();
	for (i = 0; i < steps; i++)
		compare_step(base.free_run.logits[i], forced_run.logits[i], &cos_sum,
			&sumsq, &n_elems, &kl_sum, &top1, &top5);
	m->top1_pct = steps ? 100.0 * (double)top1 / (double)steps : 0.0;
	m->top5_pct = steps ? 100.0 * (double)top5 / (double)steps : 0.0;
	m->logit_cosine = steps ? cos_sum / (double)steps : 0.0;
	m->logit_rmse = n_elems ? sqrt(sumsq / (double)n_elems) : 0.0;
	m->kl_mean = steps ? kl_sum / (double)steps : 0.0;
	m->first_divergence = (long)gen_tokens;
	for (i = 0; i < base.free_run.tokens.size() && i < free_run.tokens.size();
			i++)
		if (base.free_run.tokens[i] != free_run.tokens[i])
		{
			m->first_divergence = (long)i;
			break ;
		}
	m->text = tokens_to_text(vocab, free_run.tokens);
	m->recall_ok = (base.answer == NULL)
		|| (m->text.find(base.answer) != std::string::npos);
}

/* Splices `targets` into a copy of baseline's blob, reloads it into a
 * fresh context, and continues both a free-running and a teacher-forced
 * generation from there. */
static bool	run_experiment(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const std::vector<perturb_target_t> &targets, metrics_t *m)
{
	std::vector<uint8_t>	blob;
	llama_context			*free_ctx;
	llama_context			*forced_ctx;
	pass_result_t			free_run;
	pass_result_t			forced_run;
	bool					ok;

	blob = base.blob;
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
		fill_metrics(vocab, base, gen_tokens, free_run, forced_run, m);
	llama_free(free_ctx);
	llama_free(forced_ctx);
	return (ok);
}

/* Item 2: a uniform (all-layer) K/V type combination via real ggml native
 * types -- no splicing needed, type_k/type_v are independently settable
 * scalars. Reports the REAL measured KV size via llama_state_seq_get_size. */
static bool	run_kv_combo(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				ggml_type tk, ggml_type tv, metrics_t *m, size_t *kv_bytes)
{
	llama_context	*free_ctx;
	llama_context	*forced_ctx;
	pass_result_t	free_run;
	pass_result_t	forced_run;
	bool			ok;

	free_ctx = make_context(model, n_ctx, tk, tv);
	forced_ctx = make_context(model, n_ctx, tk, tv);
	ok = free_ctx != NULL && forced_ctx != NULL;
	if (ok)
		ok = decode_prompt(free_ctx, base.prompt_tokens, 256);
	/* Measured right after the prompt decode, BEFORE run_gen -- matching
	 * base.f16_state_bytes' prompt-only cell count exactly. Measuring
	 * this after generation (the cell count grows by gen_tokens) would
	 * put the numerator (prompt-only F16 bytes) and denominator
	 * (prompt+gen_tokens native bytes) on different cell counts, which
	 * for short prompts with a large gen_tokens can make the reported
	 * "reduction" fall below 1x even though the real per-cell byte cost
	 * did shrink -- the same class of denominator-mixing bug the project
	 * caught and fixed once already for the projected/spliced metric. */
	if (ok)
		*kv_bytes = llama_state_seq_get_size(free_ctx, 0);
	if (ok)
		ok = run_gen(free_ctx, vocab, gen_tokens, NULL, &free_run);
	if (ok)
		ok = decode_prompt(forced_ctx, base.prompt_tokens, 256);
	if (ok)
		ok = run_gen(forced_ctx, vocab, gen_tokens, &base.free_run.tokens,
				&forced_run);
	if (ok)
		fill_metrics(vocab, base, gen_tokens, free_run, forced_run, m);
	llama_free(free_ctx);
	llama_free(forced_ctx);
	return (ok);
}

/* ------------------------------------------------------------------ */
/* KV memory cost model (analytic; see file header comment).           */
/* ------------------------------------------------------------------ */

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

/* Projected total KV bytes (K+V, all layers) under `targets`, given the
 * real measured row sizes/cell_count. Rows/layers not covered by any
 * target stay FP16. */
static double	projected_kv_bytes(const blob_index_t &idx,
					const std::vector<perturb_target_t> &targets)
{
	std::vector<std::vector<int8_t>>	kbits(idx.n_layer,
			std::vector<int8_t>(idx.cell_count, 16));
	std::vector<std::vector<int8_t>>	vbits(idx.n_layer,
			std::vector<int8_t>(idx.cell_count, 16));
	double	total;
	uint32_t	r;

	for (const perturb_target_t &t : targets)
	{
		if (t.layer < 0 || (uint32_t)t.layer >= idx.n_layer)
			continue ;
		r = t.row_start;
		while (r < t.row_end && r < idx.cell_count)
		{
			if (t.do_k)
				kbits[t.layer][r] = (int8_t)t.bits;
			if (t.do_v)
				vbits[t.layer][r] = (int8_t)t.bits;
			r++;
		}
	}
	total = 0.0;
	for (uint32_t l = 0; l < idx.n_layer; l++)
		for (uint32_t r2 = 0; r2 < idx.cell_count; r2++)
		{
			total += bytes_per_row(idx.layers[l].k_row_size, kbits[l][r2]);
			total += bytes_per_row(idx.layers[l].v_row_size, vbits[l][r2]);
		}
	return (total);
}

static double	full_f16_bytes(const blob_index_t &idx)
{
	double	total;

	total = 0.0;
	for (uint32_t l = 0; l < idx.n_layer; l++)
		total += (double)idx.layers[l].k_row_size * idx.cell_count
			+ (double)idx.layers[l].v_row_size * idx.cell_count;
	return (total);
}

/* ------------------------------------------------------------------ */
/* Success thresholds (item 4 defaults) and per-layer / per-age        */
/* classification.                                                     */
/* ------------------------------------------------------------------ */

typedef struct s_thresholds
{
	double	top1_min;
	double	top5_min;
	double	cosine_min;
}	thresholds_t;

static const thresholds_t	g_default_thresholds = {98.0, 99.0, 0.995};

static bool	passes(const metrics_t &m, const thresholds_t &th)
{
	return (m.top1_pct >= th.top1_min && m.top5_pct >= th.top5_min
		&& m.logit_cosine >= th.cosine_min && m.recall_ok);
}

/* Best safe precision for one (layer or age-band) slot: prefers Q4 if it
 * clears every threshold AND the recall check (item 8: a recall failure
 * disqualifies Q4 outright, regardless of the numeric metrics), else Q8
 * if that clears the bar, else FP16. */
static int	classify(const metrics_t &q8, const metrics_t &q4,
				const thresholds_t &th)
{
	if (passes(q4, th))
		return (4);
	if (passes(q8, th))
		return (8);
	return (16);
}

/* Emits non-overlapping row-range targets for one layer: `base_bits`
 * everywhere, except `age_bits` for rows in [age_start, age_end) -- so a
 * layer/age combination is quantized directly from the pristine baseline
 * exactly once per row, never compounded through two quantization
 * passes. */
static void	emit_layer_targets(std::vector<perturb_target_t> *out, int layer,
				uint32_t cell_count, int base_bits, uint32_t age_start,
				uint32_t age_end, int age_bits)
{
	if (age_start > 0)
		out->push_back({layer, true, true, 0, age_start, base_bits});
	out->push_back({layer, true, true, age_start,
			age_end < cell_count ? age_end : cell_count, age_bits});
	if (age_end < cell_count)
		out->push_back({layer, true, true, age_end, cell_count, base_bits});
}

/* ------------------------------------------------------------------ */
/* Reporting helpers.                                                   */
/* ------------------------------------------------------------------ */

static void	print_metrics_line(const char *label, const metrics_t &m)
{
	fprintf(stderr,
		"  %-28s top1 %6.2f%%  top5 %6.2f%%  cosine %.6f  KL %.6f  "
		"div@%-3ld recall %s\n", label, m.top1_pct, m.top5_pct,
		m.logit_cosine, m.kl_mean, m.first_divergence,
		m.recall_ok ? "OK" : "FAIL");
}

/* ------------------------------------------------------------------ */
/* Per-layer sensitivity sweep (item 1).                                */
/* ------------------------------------------------------------------ */

typedef struct s_layer_result
{
	metrics_t	q8;
	metrics_t	q4;
	int			classification;
}	layer_result_t;

static bool	run_layer_sweep(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const thresholds_t &th, std::vector<layer_result_t> *out)
{
	uint32_t	l;
	std::vector<perturb_target_t>	targets;
	layer_result_t	lr;

	fprintf(stderr, "\n=== Per-layer sensitivity sweep (%u layers, "
		"profiler prompt) ===\n", base.idx.n_layer);
	l = 0;
	while (l < base.idx.n_layer)
	{
		targets = {{(int)l, true, true, 0, base.idx.cell_count, 8}};
		if (!run_experiment(model, vocab, n_ctx, gen_tokens, base, targets,
				&lr.q8))
			return (false);
		targets = {{(int)l, true, true, 0, base.idx.cell_count, 4}};
		if (!run_experiment(model, vocab, n_ctx, gen_tokens, base, targets,
				&lr.q4))
			return (false);
		lr.classification = classify(lr.q8, lr.q4, th);
		fprintf(stderr, "layer %2u:\n", l);
		print_metrics_line("  Q8 (this layer only)", lr.q8);
		print_metrics_line("  Q4 (this layer only)", lr.q4);
		fprintf(stderr, "    -> classified: %s\n",
			lr.classification == 4 ? "Q4-safe"
				: lr.classification == 8 ? "Q8-safe" : "FP16 (critical)");
		out->push_back(lr);
		l++;
	}
	return (true);
}

/* ------------------------------------------------------------------ */
/* Token-age sensitivity sweep (item 3), all layers, one band at a      */
/* time; recall.txt per the task's explicit instruction.                */
/* ------------------------------------------------------------------ */

typedef struct s_age_band
{
	const char	*name;
	uint32_t	start;
	uint32_t	end;
}	age_band_t;

typedef struct s_age_result
{
	metrics_t	q8;
	metrics_t	q4;
	int			classification;
}	age_result_t;

static std::vector<age_band_t>	age_bands(uint32_t cell_count)
{
	uint32_t	q;

	q = cell_count / 4;
	return {
		{"oldest_25pct", 0, q},
		{"middle_50pct", q, cell_count - q},
		{"newest_25pct", cell_count - q, cell_count},
	};
}

static bool	run_age_sweep(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const thresholds_t &th, std::vector<age_result_t> *out)
{
	std::vector<age_band_t>	bands;
	std::vector<perturb_target_t>	targets;
	uint32_t	l;
	age_result_t	ar;

	bands = age_bands(base.idx.cell_count);
	fprintf(stderr, "\n=== Token-age sensitivity sweep (all %u layers, "
		"profiler prompt) ===\n", base.idx.n_layer);
	for (const age_band_t &b : bands)
	{
		targets.clear();
		l = 0;
		while (l < base.idx.n_layer)
			targets.push_back({(int)l++, true, true, b.start, b.end, 8});
		if (!run_experiment(model, vocab, n_ctx, gen_tokens, base, targets,
				&ar.q8))
			return (false);
		targets.clear();
		l = 0;
		while (l < base.idx.n_layer)
			targets.push_back({(int)l++, true, true, b.start, b.end, 4});
		if (!run_experiment(model, vocab, n_ctx, gen_tokens, base, targets,
				&ar.q4))
			return (false);
		ar.classification = classify(ar.q8, ar.q4, th);
		fprintf(stderr, "%s (tokens [%u,%u)):\n", b.name, b.start, b.end);
		print_metrics_line("  Q8 (this band only)", ar.q8);
		print_metrics_line("  Q4 (this band only)", ar.q4);
		fprintf(stderr, "    -> classified: %s\n",
			ar.classification == 4 ? "Q4-safe"
				: ar.classification == 8 ? "Q8-safe" : "FP16 (critical)");
		out->push_back(ar);
	}
	return (true);
}

/* ------------------------------------------------------------------ */
/* Policy: turns per-layer classifications (+ the token-age finding for */
/* the oldest band) into a concrete, non-overlapping target list for    */
/* any prompt's own blob_index (age-band boundaries are recomputed per  */
/* prompt from ITS OWN cell_count, not copied from the profiler run).   */
/* max_level floors how aggressive the policy is allowed to be: 8 for   */
/* the FP16/Q8-only variant (never emits bits=4), 4 for the full        */
/* FP16/Q8/Q4 variant. A layer's own classification always wins when it */
/* is SAFER (larger bit-width) than max_level -- a critical (FP16)      */
/* layer is never pushed to a lower precision by either policy variant. */
/* ------------------------------------------------------------------ */

static std::vector<perturb_target_t>	build_policy_targets(
		const blob_index_t &idx, const std::vector<int> &layer_bits,
		bool use_age_q4_override, int max_level)
{
	std::vector<perturb_target_t>	targets;
	uint32_t	old_end;
	uint32_t	l;
	int			eff;

	old_end = idx.cell_count / 4;
	l = 0;
	while (l < idx.n_layer)
	{
		eff = layer_bits[l] > max_level ? layer_bits[l] : max_level;
		if (eff == 16)
			; /* stays FP16: no target needed */
		else if (eff == 8 && use_age_q4_override && max_level <= 4)
			emit_layer_targets(&targets, (int)l, idx.cell_count, 8, 0,
				old_end, 4);
		else
			targets.push_back({(int)l, true, true, 0, idx.cell_count, eff});
		l++;
	}
	return (targets);
}

/* ------------------------------------------------------------------ */
/* Self-test: perturbing nothing must reproduce the true baseline       */
/* exactly -- proof the blob get/set round-trip itself is faithful      */
/* before any experiment built on top of it is trusted.                 */
/* ------------------------------------------------------------------ */

static bool	self_test(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base)
{
	metrics_t	m;
	bool		ok;
	bool		pass;

	ok = run_experiment(model, vocab, n_ctx, gen_tokens, base, {}, &m);
	pass = ok && m.top1_pct == 100.0 && m.top5_pct == 100.0
		&& m.logit_cosine == 1.0 && m.logit_rmse == 0.0
		&& m.first_divergence == gen_tokens && m.text == base.text;
	fprintf(stderr, "\nself-test (no perturbation must equal baseline "
		"exactly): %s\n", pass ? "PASS" : "FAIL");
	if (!pass)
		print_metrics_line("  no-op", m);
	return (pass);
}

/* ------------------------------------------------------------------ */
/* Comparison table (item 5): FP16 reference, all-Q8, all-Q4 (native    */
/* types, real KV bytes), adaptive-FP16/Q8, adaptive-FP16/Q8/Q4         */
/* (spliced, real generation, analytic KV bytes). One row set per       */
/* prompt.                                                              */
/* ------------------------------------------------------------------ */

static void	print_row(const char *label, const metrics_t &m, double kv_bytes,
				double f16_bytes)
{
	fprintf(stderr, "  %-24s top1 %6.2f%%  top5 %6.2f%%  cosine %.6f  "
		"KL %.6f  recall %-4s  KV %6.3fx\n", label, m.top1_pct, m.top5_pct,
		m.logit_cosine, m.kl_mean, m.recall_ok ? "OK" : "FAIL",
		f16_bytes / kv_bytes);
}

static std::string	json_escape(const std::string &s)
{
	std::string	out;

	for (char c : s)
	{
		if (c == '"' || c == '\\')
		{
			out += '\\';
			out += c;
		}
		else if (c == '\n')
			out += "\\n";
		else if (c == '\r')
			out += "\\r";
		else if (c == '\t')
			out += "\\t";
		else if ((unsigned char)c < 0x20)
			continue ;
		else
			out += c;
	}
	return (out);
}

static void	emit_json_row(FILE *f, const char *prompt_path, const char *label,
				const metrics_t &m, double kv_bytes, double f16_bytes)
{
	if (f == NULL)
		return ;
	fprintf(f, "{\"record\":\"row\",\"prompt\":\"%s\",\"config\":\"%s\","
		"\"top1_pct\":%.6f,\"top5_pct\":%.6f,\"logit_cosine\":%.6f,"
		"\"kl_divergence\":%.6f,\"recall_ok\":%s,\"kv_reduction_x\":%.6f,"
		"\"text\":\"%s\"}\n", prompt_path, label, m.top1_pct, m.top5_pct,
		m.logit_cosine, m.kl_mean, m.recall_ok ? "true" : "false",
		f16_bytes / kv_bytes, json_escape(m.text).c_str());
}

static bool	run_comparison_table(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const std::vector<int> &layer_bits, bool age_override,
				const char *prompt_path, FILE *json_out)
{
	metrics_t	m_q8;
	metrics_t	m_q4;
	metrics_t	m_ad_q8;
	metrics_t	m_ad_q4;
	size_t		kv_q8;
	size_t		kv_q4;
	double		f16b_real;		/* measured whole-blob F16 size */
	double		f16b_analytic;	/* analytic tensor-only F16 size */
	std::vector<perturb_target_t>	pol_q8;
	std::vector<perturb_target_t>	pol_full;
	bool		ok;

	fprintf(stderr, "\n--- prompt: %s ---\n", base.answer != NULL
		? "(recall-checked)" : "(no recall check)");
	f16b_real = (double)base.f16_state_bytes;
	f16b_analytic = full_f16_bytes(base.idx);
	ok = run_kv_combo(model, vocab, n_ctx, gen_tokens, base, GGML_TYPE_Q8_0,
			GGML_TYPE_Q8_0, &m_q8, &kv_q8);
	if (ok)
		ok = run_kv_combo(model, vocab, n_ctx, gen_tokens, base,
				GGML_TYPE_Q4_0, GGML_TYPE_Q4_0, &m_q4, &kv_q4);
	pol_q8 = build_policy_targets(base.idx, layer_bits, false, 8);
	pol_full = build_policy_targets(base.idx, layer_bits, age_override, 4);
	if (ok)
		ok = run_experiment(model, vocab, n_ctx, gen_tokens, base, pol_q8,
				&m_ad_q8);
	if (ok)
		ok = run_experiment(model, vocab, n_ctx, gen_tokens, base, pol_full,
				&m_ad_q4);
	if (!ok)
		return (false);
	print_row("all-Q8 (native)", m_q8, (double)kv_q8, f16b_real);
	print_row("all-Q4 (native)", m_q4, (double)kv_q4, f16b_real);
	print_row("adaptive FP16/Q8", m_ad_q8, projected_kv_bytes(base.idx,
			pol_q8), f16b_analytic);
	print_row("adaptive FP16/Q8/Q4", m_ad_q4, projected_kv_bytes(base.idx,
			pol_full), f16b_analytic);
	fprintf(stderr, "  baseline text: %s\n", base.text.c_str());
	fprintf(stderr, "  all-Q8 text:   %s\n", m_q8.text.c_str());
	fprintf(stderr, "  all-Q4 text:   %s\n", m_q4.text.c_str());
	emit_json_row(json_out, prompt_path, "fp16_baseline", metrics_t{100.0,
			100.0, 1.0, 0.0, 0.0, gen_tokens, base.answer == NULL
				|| base.text.find(base.answer) != std::string::npos,
			base.text}, f16b_real, f16b_real);
	emit_json_row(json_out, prompt_path, "all_q8", m_q8, (double)kv_q8,
		f16b_real);
	emit_json_row(json_out, prompt_path, "all_q4", m_q4, (double)kv_q4,
		f16b_real);
	emit_json_row(json_out, prompt_path, "adaptive_fp16_q8", m_ad_q8,
		projected_kv_bytes(base.idx, pol_q8), f16b_analytic);
	emit_json_row(json_out, prompt_path, "adaptive_fp16_q8_q4", m_ad_q4,
		projected_kv_bytes(base.idx, pol_full), f16b_analytic);
	return (true);
}

/* ------------------------------------------------------------------ */
/* Item 2: six asymmetric K/V native-type combinations, uniform across  */
/* all layers (the only granularity type_k/type_v support).             */
/* ------------------------------------------------------------------ */

typedef struct s_kv_combo
{
	const char	*name;
	ggml_type	tk;
	ggml_type	tv;
}	kv_combo_t;

static const kv_combo_t	g_kv_combos[] = {
	{"K=Q8,V=F16", GGML_TYPE_Q8_0, GGML_TYPE_F16},
	{"K=F16,V=Q8", GGML_TYPE_F16, GGML_TYPE_Q8_0},
	{"K=Q4,V=Q8", GGML_TYPE_Q4_0, GGML_TYPE_Q8_0},
	{"K=Q8,V=Q4", GGML_TYPE_Q8_0, GGML_TYPE_Q4_0},
	{"K=Q4,V=F16", GGML_TYPE_Q4_0, GGML_TYPE_F16},
	{"K=F16,V=Q4", GGML_TYPE_F16, GGML_TYPE_Q4_0},
};
# define N_KV_COMBOS 6

static void	run_kv_combo_sweep(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const char *prompt_path)
{
	metrics_t	m;
	size_t		kv_bytes;
	double		f16b;
	int			i;

	f16b = (double)base.f16_state_bytes;
	fprintf(stderr, "\n=== K/V asymmetric combinations: %s ===\n",
		prompt_path);
	i = 0;
	while (i < N_KV_COMBOS)
	{
		if (run_kv_combo(model, vocab, n_ctx, gen_tokens, base,
				g_kv_combos[i].tk, g_kv_combos[i].tv, &m, &kv_bytes))
			print_row(g_kv_combos[i].name, m, (double)kv_bytes, f16b);
		else
			fprintf(stderr, "  %-24s FAILED\n", g_kv_combos[i].name);
		i++;
	}
}

/* ------------------------------------------------------------------ */
/* Phase 3.4: composition-aware greedy optimizer.                       */
/*                                                                      */
/* Phase 3.3 built a policy by unioning INDEPENDENT per-layer scores,   */
/* then found the union did not compose linearly (aggregate cosine      */
/* dropped below the per-layer bar every constituent layer cleared      */
/* alone). This optimizer never trusts an independent score for an      */
/* accept/reject decision: every candidate is evaluated by splicing it  */
/* into the CURRENT, already-accepted policy and running live inference */
/* with everything else exactly as it stands -- so what gets measured   */
/* is always the actual composed effect, never an isolated one.         */
/*                                                                      */
/* Search space: 60 slots (30 layers x {K,V}), starting at Q8           */
/* everywhere (item 1). Each round considers exactly the next slot in a */
/* fixed priority queue -- all V-slots (ascending layer) before any     */
/* K-slot (ascending layer), per item 5 -- and evaluates ONE trial       */
/* (that slot alone dropped to Q4, everything else as currently          */
/* accepted) against the full valid prompt set. This is a              */
/* first-improvement greedy (accept the first candidate that clears     */
/* every threshold, in priority order) rather than a best-of-batch      */
/* greedy: the task explicitly rules out full brute force and asks for  */
/* a parametrized search budget, and evaluating only one candidate per  */
/* round bounds total live evaluations to exactly the number of slots   */
/* considered (<= 60), which a best-of-batch scheme evaluating several  */
/* candidates per round to compare ratios would not. The memory/quality */
/* ratio is still computed and reported for every candidate (accepted   */
/* or not), so the search remains ratio-aware even though the ordering  */
/* decides *which* candidate is offered before comparison, not the      */
/* ratio itself.                                                        */
/* ------------------------------------------------------------------ */

typedef struct s_kv_policy
{
	std::vector<int>	kbits;	/* per layer, 8 or 4 */
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

typedef struct s_agg_result
{
	double				top1;
	double				top5;
	double				cosine;
	double				kl;
	long				min_first_divergence;
	std::vector<bool>	recall_ok;	/* aligned with the valid prompt list */
}	agg_result_t;

/* Evaluates one candidate policy across every prompt in `valid`, folding
 * the per-prompt metrics into simple means (steps/elements are equal
 * across these prompts' fixed gen_tokens, so an unweighted mean is exact
 * enough for search purposes; the final comparison table re-measures the
 * chosen policies precisely, per-prompt, without any averaging). */
static bool	evaluate_policy(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const std::vector<baseline_t> &valid,
				const kv_policy_t &pol, agg_result_t *out)
{
	metrics_t	m;
	size_t		i;

	out->top1 = 0.0;
	out->top5 = 0.0;
	out->cosine = 0.0;
	out->kl = 0.0;
	out->min_first_divergence = LONG_MAX;
	out->recall_ok.assign(valid.size(), false);
	i = 0;
	while (i < valid.size())
	{
		if (!run_experiment(model, vocab, n_ctx, gen_tokens, valid[i],
				policy_targets(valid[i].idx, pol), &m))
			return (false);
		out->top1 += m.top1_pct;
		out->top5 += m.top5_pct;
		out->cosine += m.logit_cosine;
		out->kl += m.kl_mean;
		if (m.first_divergence < out->min_first_divergence)
			out->min_first_divergence = m.first_divergence;
		out->recall_ok[i] = m.recall_ok;
		i++;
	}
	out->top1 /= (double)valid.size();
	out->top5 /= (double)valid.size();
	out->cosine /= (double)valid.size();
	out->kl /= (double)valid.size();
	return (true);
}

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
		q.push_back({(int)l++, false});	/* V-slots first (item 5) */
	l = 0;
	while (l < n_layer)
		q.push_back({(int)l++, true});		/* then K-slots */
	return (q);
}

/* K candidates are evaluated with a stricter cosine bar than V (item 5's
 * "higher penalty" for K) -- a K-slot must not just clear the standard
 * bar but clear a materially tighter one. */
# define K_STRICT_COSINE 0.9975

typedef struct s_decision
{
	slot_t			slot;
	agg_result_t	result;
	double			memory_gain_bytes;
	double			ratio;
	bool			accepted;
	std::string		reason;
}	decision_t;

static double	slot_memory_gain(const blob_index_t &idx, const slot_t &s)
{
	size_t	row_size;

	row_size = s.is_k ? idx.layers[s.layer].k_row_size
		: idx.layers[s.layer].v_row_size;
	return ((bytes_per_row(row_size, 8) - bytes_per_row(row_size, 4))
		* idx.cell_count);
}

/* Checks a candidate's live-measured result against the thresholds and
 * the must-stay-correct recall set (item 4: every recall test the
 * all-Q8 reference answered correctly must still be answered correctly;
 * a baseline-unsolvable prompt is never a reason to reject, per item 6).
 * Returns "" (pass) or a human-readable rejection reason. */
static std::string	check_candidate(const agg_result_t &r, bool is_k,
						const std::vector<bool> &must_stay_correct)
{
	double	cos_bar;
	size_t	i;
	char	buf[160];

	cos_bar = is_k ? K_STRICT_COSINE : g_default_thresholds.cosine_min;
	if (r.cosine < cos_bar)
	{
		snprintf(buf, sizeof(buf), "cosine %.6f < %.6f (%s threshold)",
			r.cosine, cos_bar, is_k ? "K, strict" : "V");
		return (buf);
	}
	if (r.top1 < g_default_thresholds.top1_min)
	{
		snprintf(buf, sizeof(buf), "top1 %.2f%% < %.2f%%", r.top1,
			g_default_thresholds.top1_min);
		return (buf);
	}
	if (r.top5 < g_default_thresholds.top5_min)
	{
		snprintf(buf, sizeof(buf), "top5 %.2f%% < %.2f%%", r.top5,
			g_default_thresholds.top5_min);
		return (buf);
	}
	i = 0;
	while (i < must_stay_correct.size())
	{
		if (must_stay_correct[i] && !r.recall_ok[i])
		{
			snprintf(buf, sizeof(buf),
				"recall broke on valid prompt #%zu (all-Q8 answered it "
				"correctly, this candidate does not)", i);
			return (buf);
		}
		i++;
	}
	return ("");
}

typedef struct s_optimizer_result
{
	kv_policy_t					policy;
	std::vector<decision_t>	accepted;
	std::vector<decision_t>	rejected;
	int							evals_used;
	double						search_seconds;
}	optimizer_result_t;

/* The greedy pass: item 2. */
static bool	greedy_optimize(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const std::vector<baseline_t> &valid,
				const std::vector<bool> &must_stay_correct, int search_budget,
				optimizer_result_t *out)
{
	std::vector<slot_t>	queue;
	kv_policy_t				trial;
	decision_t				d;
	std::string				reason;
	std::chrono::steady_clock::time_point	t0;

	t0 = std::chrono::steady_clock::now();
	out->policy = all_q8_policy(valid[0].idx.n_layer);
	out->evals_used = 0;
	queue = priority_queue_slots(valid[0].idx.n_layer);
	fprintf(stderr, "\n=== Greedy composition-aware search (budget=%d, "
		"%zu candidate slots, V-before-K) ===\n", search_budget,
		queue.size());
	for (const slot_t &s : queue)
	{
		if (out->evals_used >= search_budget)
		{
			fprintf(stderr, "  search budget exhausted, stopping early "
				"(%d/%zu slots considered)\n", out->evals_used,
				queue.size());
			break ;
		}
		trial = out->policy;
		if (s.is_k)
			trial.kbits[s.layer] = 4;
		else
			trial.vbits[s.layer] = 4;
		d.slot = s;
		if (!evaluate_policy(model, vocab, n_ctx, gen_tokens, valid, trial,
				&d.result))
			return (false);
		out->evals_used++;
		d.memory_gain_bytes = slot_memory_gain(valid[0].idx, s);
		reason = check_candidate(d.result, s.is_k, must_stay_correct);
		d.ratio = d.memory_gain_bytes / (1.0 - d.result.cosine > 1e-9
				? 1.0 - d.result.cosine : 1e-9);
		d.accepted = reason.empty();
		d.reason = reason.empty() ? "accepted" : reason;
		fprintf(stderr, "  layer %2d %s -> Q4: cosine %.6f top1 %.2f%% "
			"top5 %.2f%% ratio %.1f B/unit  %s\n", s.layer,
			s.is_k ? "K" : "V", d.result.cosine, d.result.top1, d.result.top5,
			d.ratio, d.accepted ? "ACCEPTED" : ("rejected: " + reason).c_str());
		if (d.accepted)
		{
			out->policy = trial;
			out->accepted.push_back(d);
		}
		else
			out->rejected.push_back(d);
	}
	out->search_seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - t0).count();
	return (true);
}

/* Item 3: bounded backtracking. Re-tests reverting each of the last
 * min(4, N) accepted slots, one at a time and in most-recent-first
 * order, against the policy as it stands at that point (so a reversion
 * earlier in this pass can change what the next reversion is tested
 * against -- still composition-aware, not independent). A reversion is
 * kept if the policy without that slot clears every threshold and the
 * one with it did not, i.e. that slot was the specific cause of a
 * failure -- or if reverting it recovers a materially better cosine
 * (>=0.001 improvement) for a comparatively small memory cost (this
 * slot's own gain is < 10% of the policy's total gain so far). */
static bool	backtrack(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const std::vector<baseline_t> &valid,
				const std::vector<bool> &must_stay_correct,
				optimizer_result_t *opt)
{
	int		n_check;
	int		i;
	kv_policy_t	trial;
	agg_result_t	r;
	agg_result_t	full;
	std::string	reason;
	double	total_gain;

	n_check = (int)opt->accepted.size() < 4 ? (int)opt->accepted.size() : 4;
	if (n_check == 0)
		return (true);
	fprintf(stderr, "\n=== Backtracking: re-testing the last %d accepted "
		"downgrades individually ===\n", n_check);
	if (!evaluate_policy(model, vocab, n_ctx, gen_tokens, valid, opt->policy,
			&full))
		return (false);
	total_gain = 0.0;
	for (const decision_t &d : opt->accepted)
		total_gain += d.memory_gain_bytes;
	i = (int)opt->accepted.size() - 1;
	while (i >= (int)opt->accepted.size() - n_check)
	{
		const slot_t	&s = opt->accepted[i].slot;

		trial = opt->policy;
		if (s.is_k)
			trial.kbits[s.layer] = 8;
		else
			trial.vbits[s.layer] = 8;
		if (!evaluate_policy(model, vocab, n_ctx, gen_tokens, valid, trial,
				&r))
			return (false);
		opt->evals_used++;
		reason = check_candidate(full, s.is_k, must_stay_correct);
		bool	small_share = opt->accepted[i].memory_gain_bytes
			< 0.10 * total_gain;
		bool	recovers = check_candidate(r, s.is_k,
				must_stay_correct).empty() && !reason.empty();
		bool	improves = (r.cosine - full.cosine) >= 0.001 && small_share;
		fprintf(stderr, "  revert layer %2d %s -> Q8: without it cosine "
			"%.6f (with it %.6f)  %s\n", s.layer, s.is_k ? "K" : "V",
			r.cosine, full.cosine, (recovers || improves)
				? "REVERTED" : "kept at Q4");
		if (recovers || improves)
		{
			opt->policy = trial;
			opt->accepted[i].accepted = false;
			opt->accepted[i].reason = recovers
				? "reverted in backtracking: was the cause of a threshold "
					"failure in the full policy"
				: "reverted in backtracking: small memory share, cosine "
					"improved >=0.001 without it";
			full = r;
		}
		i--;
	}
	opt->accepted.erase(std::remove_if(opt->accepted.begin(),
			opt->accepted.end(), [](const decision_t &d) {
				return (!d.accepted); }), opt->accepted.end());
	return (true);
}

static double	projected_kv_bytes_kv(const blob_index_t &idx,
					const kv_policy_t &pol)
{
	double	total;
	uint32_t	l;

	total = 0.0;
	l = 0;
	while (l < idx.n_layer)
	{
		total += bytes_per_row(idx.layers[l].k_row_size, pol.kbits[l])
			* idx.cell_count;
		total += bytes_per_row(idx.layers[l].v_row_size, pol.vbits[l])
			* idx.cell_count;
		l++;
	}
	return (total);
}

/* Item 7: FP16 / all-Q8 / all-Q4 / Phase 3.3 independent policy /
 * Phase 3.4 composition-aware policy, for one prompt. */
static bool	run_final_comparison(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const char *prompt_path, const std::vector<int> &layer_bits,
				bool age_override, const kv_policy_t &opt_policy,
				FILE *json_out, bool is_valid)
{
	metrics_t	m_q8;
	metrics_t	m_q4;
	metrics_t	m_p33;
	metrics_t	m_p34;
	size_t		kv_q8;
	size_t		kv_q4;
	double		f16b_real;
	double		f16b_analytic;
	std::vector<perturb_target_t>	pol33;
	std::vector<perturb_target_t>	pol34;
	bool		ok;

	f16b_real = (double)base.f16_state_bytes;
	f16b_analytic = full_f16_bytes(base.idx);
	ok = run_kv_combo(model, vocab, n_ctx, gen_tokens, base, GGML_TYPE_Q8_0,
			GGML_TYPE_Q8_0, &m_q8, &kv_q8);
	if (ok)
		ok = run_kv_combo(model, vocab, n_ctx, gen_tokens, base,
				GGML_TYPE_Q4_0, GGML_TYPE_Q4_0, &m_q4, &kv_q4);
	pol33 = build_policy_targets(base.idx, layer_bits, age_override, 4);
	if (ok)
		ok = run_experiment(model, vocab, n_ctx, gen_tokens, base, pol33,
				&m_p33);
	pol34 = policy_targets(base.idx, opt_policy);
	if (ok)
		ok = run_experiment(model, vocab, n_ctx, gen_tokens, base, pol34,
				&m_p34);
	if (!ok)
		return (false);
	fprintf(stderr, "\n--- prompt: %s (%s) ---\n", prompt_path,
		is_valid ? "valid" : "EXCLUDED from optimization, informational only");
	print_row("all-Q8 (native)", m_q8, (double)kv_q8, f16b_real);
	print_row("all-Q4 (native)", m_q4, (double)kv_q4, f16b_real);
	print_row("Phase 3.3 policy", m_p33, projected_kv_bytes(base.idx, pol33),
		f16b_analytic);
	print_row("Phase 3.4 policy", m_p34, projected_kv_bytes_kv(base.idx,
			opt_policy), f16b_analytic);
	emit_json_row(json_out, prompt_path, "all_q8", m_q8, (double)kv_q8,
		f16b_real);
	emit_json_row(json_out, prompt_path, "all_q4", m_q4, (double)kv_q4,
		f16b_real);
	emit_json_row(json_out, prompt_path, "phase33_policy", m_p33,
		projected_kv_bytes(base.idx, pol33), f16b_analytic);
	emit_json_row(json_out, prompt_path, "phase34_policy", m_p34,
		projected_kv_bytes_kv(base.idx, opt_policy), f16b_analytic);
	return (true);
}

static void	emit_policy_json(FILE *out, const optimizer_result_t &opt,
				int n_layer)
{
	int	l;

	if (out == NULL)
		return ;
	fprintf(out, "{\"record\":\"phase34_policy\",\"kbits\":[");
	for (l = 0; l < n_layer; l++)
		fprintf(out, "%s%d", l ? "," : "", opt.policy.kbits[l]);
	fprintf(out, "],\"vbits\":[");
	for (l = 0; l < n_layer; l++)
		fprintf(out, "%s%d", l ? "," : "", opt.policy.vbits[l]);
	fprintf(out, "],\"evals_used\":%d,\"search_seconds\":%.3f,"
		"\"accepted\":%zu,\"rejected\":%zu}\n", opt.evals_used,
		opt.search_seconds, opt.accepted.size(), opt.rejected.size());
	for (const decision_t &d : opt.accepted)
		fprintf(out, "{\"record\":\"decision\",\"layer\":%d,\"kv\":\"%s\","
			"\"outcome\":\"accepted\",\"cosine\":%.6f,\"top1\":%.4f,"
			"\"ratio\":%.2f}\n", d.slot.layer, d.slot.is_k ? "K" : "V",
			d.result.cosine, d.result.top1, d.ratio);
	for (const decision_t &d : opt.rejected)
		fprintf(out, "{\"record\":\"decision\",\"layer\":%d,\"kv\":\"%s\","
			"\"outcome\":\"rejected\",\"cosine\":%.6f,\"top1\":%.4f,"
			"\"reason\":\"%s\"}\n", d.slot.layer, d.slot.is_k ? "K" : "V",
			d.result.cosine, d.result.top1, json_escape(d.reason).c_str());
}

/* ------------------------------------------------------------------ */
/* Phase 3.5: risk-aware validation.                                    */
/*                                                                      */
/* Phase 3.4's greedy optimizer accepted or rejected a candidate based  */
/* on the AGGREGATE (mean) metrics across the valid prompt set. That     */
/* let a critical prompt's quality erode silently as long as the mean    */
/* held up -- exactly what happened to recall.txt's top-1 (96.9%, below  */
/* the 98% bar the aggregate itself cleared). Every accept/reject        */
/* decision below is a PER-PROMPT hard constraint: a candidate is only   */
/* accepted if EVERY valid prompt individually clears its own threshold  */
/* (with an adjustable safety margin on top), never just the mean. The   */
/* aggregate is still computed and reported (item 1 asks for it), but it */
/* never gates a decision here.                                         */
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

/* Recall-critical prompts (item 1: any prompt with an expected answer to
 * check) get stricter thresholds; everything else uses the shared base
 * thresholds. Classification is by content role, not by file name magic
 * beyond distinguishing the other four content types. */
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

static prompt_thresholds_t	thresholds_for_class(prompt_class_t c)
{
	if (c == PROMPT_RECALL_CRITICAL)
		return {99.0, 99.0, 0.9975};
	return {g_default_thresholds.top1_min, g_default_thresholds.top5_min,
		g_default_thresholds.cosine_min};
}

/* Item 4: adjustable safety margins added ON TOP of the class threshold
 * (and, for K, on top of K's own stricter bar too) -- a candidate must
 * clear threshold + margin, not just threshold. */
typedef struct s_margin
{
	double	cosine_margin;
	double	top1_margin;
	double	top5_margin;
}	margin_t;

/* cosine_margin is capped low deliberately: the strictest base threshold
 * (K_STRICT_COSINE / recall-critical, both 0.9975) plus too large a
 * margin would exceed 1.0 -- an unreachable bar that cosine similarity
 * can never clear, which would silently reject every candidate outright
 * rather than being "conservative." 0.0015 keeps every class's effective
 * bar under 1.0 with headroom (worst case 0.9975+0.0015=0.999). */
static const margin_t	g_margin_conservative = {0.0015, 1.0, 0.2};
static const margin_t	g_margin_balanced = {0.001, 0.5, 0.1};
static const margin_t	g_margin_aggressive = {0.0, 0.0, 0.0};

typedef struct s_per_prompt_result
{
	std::vector<metrics_t>	per_prompt;	/* aligned with the valid prompt list */
	agg_result_t			aggregate;		/* reported, never gates (item 1) */
}	per_prompt_result_t;

static bool	evaluate_policy_detailed(llama_model *model,
				const llama_vocab *vocab, int n_ctx, int gen_tokens,
				const std::vector<baseline_t> &valid, const kv_policy_t &pol,
				per_prompt_result_t *out)
{
	double	cos_sum;
	double	top1_sum;
	double	top5_sum;
	double	kl_sum;
	size_t	i;

	out->per_prompt.resize(valid.size());
	out->aggregate.recall_ok.assign(valid.size(), false);
	out->aggregate.min_first_divergence = LONG_MAX;
	cos_sum = 0.0;
	top1_sum = 0.0;
	top5_sum = 0.0;
	kl_sum = 0.0;
	i = 0;
	while (i < valid.size())
	{
		if (!run_experiment(model, vocab, n_ctx, gen_tokens, valid[i],
				policy_targets(valid[i].idx, pol), &out->per_prompt[i]))
			return (false);
		cos_sum += out->per_prompt[i].logit_cosine;
		top1_sum += out->per_prompt[i].top1_pct;
		top5_sum += out->per_prompt[i].top5_pct;
		kl_sum += out->per_prompt[i].kl_mean;
		if (out->per_prompt[i].first_divergence
				< out->aggregate.min_first_divergence)
			out->aggregate.min_first_divergence
				= out->per_prompt[i].first_divergence;
		out->aggregate.recall_ok[i] = out->per_prompt[i].recall_ok;
		i++;
	}
	out->aggregate.top1 = top1_sum / (double)valid.size();
	out->aggregate.top5 = top5_sum / (double)valid.size();
	out->aggregate.cosine = cos_sum / (double)valid.size();
	out->aggregate.kl = kl_sum / (double)valid.size();
	return (true);
}

/* Item 3: checks EVERY valid prompt individually; returns "" (pass) or a
 * message naming the specific prompt and metric that failed, so
 * rejections are traceable, not just a pass/fail bit. */
static std::string	check_candidate_per_prompt(const per_prompt_result_t &r,
						const std::vector<baseline_t> &valid,
						const std::vector<prompt_class_t> &classes, bool is_k,
						const margin_t &margin,
						const std::vector<bool> &must_stay_correct)
{
	prompt_thresholds_t	th;
	double	cos_bar;
	double	k_bar;
	char	buf[220];
	size_t	i;

	i = 0;
	while (i < valid.size())
	{
		th = thresholds_for_class(classes[i]);
		cos_bar = th.cosine_min + margin.cosine_margin;
		if (is_k)
		{
			k_bar = K_STRICT_COSINE + margin.cosine_margin;
			if (k_bar > cos_bar)
				cos_bar = k_bar;
		}
		if (r.per_prompt[i].logit_cosine < cos_bar)
		{
			snprintf(buf, sizeof(buf), "prompt '%s' (%s): cosine %.6f < "
				"%.6f", valid[i].name.c_str(), class_name(classes[i]),
				r.per_prompt[i].logit_cosine, cos_bar);
			return (buf);
		}
		if (r.per_prompt[i].top1_pct < th.top1_min + margin.top1_margin)
		{
			snprintf(buf, sizeof(buf), "prompt '%s' (%s): top1 %.2f%% < "
				"%.2f%%", valid[i].name.c_str(), class_name(classes[i]),
				r.per_prompt[i].top1_pct, th.top1_min + margin.top1_margin);
			return (buf);
		}
		if (r.per_prompt[i].top5_pct < th.top5_min + margin.top5_margin)
		{
			snprintf(buf, sizeof(buf), "prompt '%s' (%s): top5 %.2f%% < "
				"%.2f%%", valid[i].name.c_str(), class_name(classes[i]),
				r.per_prompt[i].top5_pct, th.top5_min + margin.top5_margin);
			return (buf);
		}
		if (must_stay_correct[i] && !r.per_prompt[i].recall_ok)
		{
			snprintf(buf, sizeof(buf), "prompt '%s' (%s): recall broke "
				"(all-Q8 answered it correctly, this candidate does not)",
				valid[i].name.c_str(), class_name(classes[i]));
			return (buf);
		}
		i++;
	}
	return ("");
}

typedef struct s_decision2
{
	slot_t					slot;
	per_prompt_result_t	result;
	double					memory_gain_bytes;
	double					ratio;
	bool					accepted;
	std::string				reason;
}	decision2_t;

typedef struct s_optimizer_result2
{
	kv_policy_t					policy;
	std::vector<decision2_t>	accepted;
	std::vector<decision2_t>	rejected;
	int							evals_used;
	double						search_seconds;
}	optimizer_result2_t;

/* Item 8: candidate pre-screening. Evaluates the valid prompt set in a
 * given priority ORDER (recall-critical first -- the cheapest, highest-
 * signal check) and stops the moment one prompt fails its own threshold,
 * instead of always paying for every prompt regardless of an early
 * failure. An ACCEPTED result always has every prompt evaluated (the
 * loop only stops early on a failure); the aggregate on a REJECTED
 * result is over however many prompts were actually evaluated before
 * stopping, which is disclosed at every call site that reports it. */
static bool	evaluate_and_check(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const std::vector<baseline_t> &valid,
				const std::vector<size_t> &order,
				const std::vector<prompt_class_t> &classes, bool is_k,
				const margin_t &margin, const std::vector<bool> &must_stay,
				const kv_policy_t &trial, per_prompt_result_t *out,
				std::string *reason)
{
	prompt_thresholds_t	th;
	double	cos_bar;
	double	k_bar;
	double	cos_sum;
	double	top1_sum;
	double	top5_sum;
	double	kl_sum;
	size_t	n_evaluated;
	char	buf[220];

	out->per_prompt.assign(valid.size(), metrics_t());
	out->aggregate.recall_ok.assign(valid.size(), false);
	out->aggregate.min_first_divergence = LONG_MAX;
	cos_sum = 0.0;
	top1_sum = 0.0;
	top5_sum = 0.0;
	kl_sum = 0.0;
	n_evaluated = 0;
	*reason = "";
	for (size_t idx : order)
	{
		if (!run_experiment(model, vocab, n_ctx, gen_tokens, valid[idx],
				policy_targets(valid[idx].idx, trial), &out->per_prompt[idx]))
			return (false);
		n_evaluated++;
		cos_sum += out->per_prompt[idx].logit_cosine;
		top1_sum += out->per_prompt[idx].top1_pct;
		top5_sum += out->per_prompt[idx].top5_pct;
		kl_sum += out->per_prompt[idx].kl_mean;
		out->aggregate.recall_ok[idx] = out->per_prompt[idx].recall_ok;
		if (out->per_prompt[idx].first_divergence
				< out->aggregate.min_first_divergence)
			out->aggregate.min_first_divergence
				= out->per_prompt[idx].first_divergence;
		th = thresholds_for_class(classes[idx]);
		cos_bar = th.cosine_min + margin.cosine_margin;
		if (is_k)
		{
			k_bar = K_STRICT_COSINE + margin.cosine_margin;
			if (k_bar > cos_bar)
				cos_bar = k_bar;
		}
		if (out->per_prompt[idx].logit_cosine < cos_bar)
		{
			snprintf(buf, sizeof(buf), "prompt '%s' (%s): cosine %.6f < "
				"%.6f", valid[idx].name.c_str(), class_name(classes[idx]),
				out->per_prompt[idx].logit_cosine, cos_bar);
			*reason = buf;
			break ;
		}
		if (out->per_prompt[idx].top1_pct < th.top1_min + margin.top1_margin)
		{
			snprintf(buf, sizeof(buf), "prompt '%s' (%s): top1 %.2f%% < "
				"%.2f%%", valid[idx].name.c_str(), class_name(classes[idx]),
				out->per_prompt[idx].top1_pct,
				th.top1_min + margin.top1_margin);
			*reason = buf;
			break ;
		}
		if (out->per_prompt[idx].top5_pct < th.top5_min + margin.top5_margin)
		{
			snprintf(buf, sizeof(buf), "prompt '%s' (%s): top5 %.2f%% < "
				"%.2f%%", valid[idx].name.c_str(), class_name(classes[idx]),
				out->per_prompt[idx].top5_pct,
				th.top5_min + margin.top5_margin);
			*reason = buf;
			break ;
		}
		if (must_stay[idx] && !out->per_prompt[idx].recall_ok)
		{
			snprintf(buf, sizeof(buf), "prompt '%s' (%s): recall broke "
				"(all-Q8 answered it correctly, this candidate does not)",
				valid[idx].name.c_str(), class_name(classes[idx]));
			*reason = buf;
			break ;
		}
	}
	out->aggregate.top1 = n_evaluated ? top1_sum / (double)n_evaluated : 0.0;
	out->aggregate.top5 = n_evaluated ? top5_sum / (double)n_evaluated : 0.0;
	out->aggregate.cosine = n_evaluated ? cos_sum / (double)n_evaluated : 0.0;
	out->aggregate.kl = n_evaluated ? kl_sum / (double)n_evaluated : 0.0;
	return (true);
}

/* Recall-critical prompts first (cheapest, highest-signal rejection
 * check), everything else in its original order after. */
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

/* Item 8: resumable checkpointing. Every live decision is appended
 * immediately (and flushed) to `checkpoint_out` as it is made, so a
 * search interrupted partway through can be restarted with
 * load_checkpoint() reading the same file back: already-decided slots
 * are fast-forwarded into the policy without re-running any inference. */
typedef struct s_resume_decision
{
	int			layer;
	bool		is_k;
	bool		accepted;
	double		cosine;
	double		top1;
	double		top5;
	std::string	reason;
}	resume_decision_t;

/* cosine/top1/top5/reason are persisted (not just accepted/rejected) so
 * that a decision fast-forwarded from an EARLIER run/resume can still be
 * reconstructed into a full decision2_t and reported in the JSONL --
 * without this, a search resumed more than once would silently lose the
 * layer-position/rejection-reason data for every slot decided before the
 * most recent invocation. */
static void	write_checkpoint(FILE *f, const char *model_name,
				const char *tier, const slot_t &s, bool accepted,
				double cosine, double top1, double top5,
				const std::string &reason)
{
	if (f == NULL)
		return ;
	fprintf(f, "{\"record\":\"checkpoint\",\"model\":\"%s\",\"tier\":\"%s\","
		"\"layer\":%d,\"kv\":\"%s\",\"accepted\":%s,\"cosine\":%.6f,"
		"\"top1\":%.4f,\"top5\":%.4f,\"reason\":\"%s\"}\n", model_name, tier,
		s.layer, s.is_k ? "K" : "V", accepted ? "true" : "false", cosine,
		top1, top5, json_escape(reason).c_str());
	fflush(f);
}

static std::vector<resume_decision_t>	load_checkpoint(const char *path,
		const char *model_name, const char *tier)
{
	std::vector<resume_decision_t>	out;
	FILE		*f;
	char		line[1024];
	char		m[128];
	char		t[64];
	char		kv[4];
	int			layer;
	int			acc;
	double		cosine;
	double		top1;
	double		top5;
	const char	*rp;
	const char	*rend;
	std::string	reason;

	out.clear();
	if (path == NULL)
		return (out);
	f = fopen(path, "r");
	if (f == NULL)
		return (out);
	while (fgets(line, sizeof(line), f) != NULL)
	{
		if (strstr(line, "\"record\":\"checkpoint\"") == NULL)
			continue ;
		if (sscanf(line, "{\"record\":\"checkpoint\",\"model\":\"%127[^\"]"
				"\",\"tier\":\"%63[^\"]\",\"layer\":%d,\"kv\":\"%3[^\"]"
				"\",\"accepted\":%*[a-z]", m, t, &layer, kv) < 4)
			continue ;
		if (strcmp(m, model_name) != 0 || strcmp(t, tier) != 0)
			continue ;
		acc = strstr(line, "\"accepted\":true") != NULL;
		cosine = 0.0;
		top1 = 0.0;
		top5 = 0.0;
		rp = strstr(line, "\"cosine\":");
		if (rp != NULL)
			sscanf(rp, "\"cosine\":%lf", &cosine);
		rp = strstr(line, "\"top1\":");
		if (rp != NULL)
			sscanf(rp, "\"top1\":%lf", &top1);
		rp = strstr(line, "\"top5\":");
		if (rp != NULL)
			sscanf(rp, "\"top5\":%lf", &top5);
		reason.clear();
		rp = strstr(line, "\"reason\":\"");
		if (rp != NULL)
		{
			rp += strlen("\"reason\":\"");
			rend = strstr(rp, "\"}");
			if (rend != NULL)
				reason.assign(rp, (size_t)(rend - rp));
		}
		out.push_back({layer, kv[0] == 'K', acc != 0, cosine, top1, top5,
				reason});
	}
	fclose(f);
	return (out);
}

static double	worst_margin_ratio(const per_prompt_result_t &r,
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
	loss = -worst;	/* positive = margin remaining, negative = violation */
	return (memory_gain / (loss > 1e-9 ? loss : 1e-9));
}

/* The greedy pass, per-prompt-constraint version of Phase 3.4's
 * greedy_optimize: same priority-queue design (V before K, one candidate
 * per round, first-improvement), now using evaluate_and_check's
 * pre-screening (item 8: stop evaluating a candidate the moment one
 * prompt fails, instead of always paying for every prompt) and, if
 * `checkpoint_out`/`resume_from`/`model_name`/`tier` are supplied,
 * resumable checkpointing -- each live decision is written immediately,
 * and a prior run's checkpoint file can fast-forward already-decided
 * slots without re-running any inference. */
static bool	greedy_optimize_v2(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const std::vector<baseline_t> &valid,
				const std::vector<prompt_class_t> &classes,
				const std::vector<bool> &must_stay_correct,
				const margin_t &margin, int search_budget,
				const kv_policy_t &start_policy, optimizer_result2_t *out,
				const char *model_name = NULL, const char *tier = NULL,
				FILE *checkpoint_out = NULL, const char *resume_from = NULL)
{
	std::vector<slot_t>	queue;
	std::vector<size_t>	order;
	std::vector<resume_decision_t>	resume;
	kv_policy_t				trial;
	decision2_t				d;
	std::string				reason;
	std::chrono::steady_clock::time_point	t0;

	t0 = std::chrono::steady_clock::now();
	out->policy = start_policy;
	order = screening_order(classes);
	queue = priority_queue_slots(valid[0].idx.n_layer);
	if (model_name != NULL && tier != NULL)
		resume = load_checkpoint(resume_from, model_name, tier);
	/* `search_budget` bounds the TOTAL live evaluations across the whole
	 * logical search, interrupt/resume boundaries included -- not a
	 * fresh allowance per invocation. Each fast-forwarded checkpoint
	 * entry already cost one live evaluation in the original run, so it
	 * counts against the same budget here; otherwise repeated resumes
	 * could silently search far more than the disclosed `search_budget`
	 * candidates, breaking reproducibility with an uninterrupted run. */
	out->evals_used = (int)resume.size();
	if (!resume.empty())
		fprintf(stderr, "  resuming from checkpoint: %zu prior decisions "
			"for %s/%s (already counted against search_budget=%d)\n",
			resume.size(), model_name, tier, search_budget);
	for (const slot_t &s : queue)
	{
		bool	found_resume = false;

		for (const resume_decision_t &rd : resume)
			if (rd.layer == s.layer && rd.is_k == s.is_k)
			{
				decision2_t	rd_decision{};

				if (rd.accepted)
				{
					if (s.is_k)
						out->policy.kbits[s.layer] = 4;
					else
						out->policy.vbits[s.layer] = 4;
				}
				rd_decision.slot = s;
				rd_decision.result.aggregate.cosine = rd.cosine;
				rd_decision.result.aggregate.top1 = rd.top1;
				rd_decision.result.aggregate.top5 = rd.top5;
				rd_decision.memory_gain_bytes = slot_memory_gain(
						valid[0].idx, s);
				rd_decision.ratio = 0.0;	/* not recomputed on resume */
				rd_decision.accepted = rd.accepted;
				rd_decision.reason = rd.accepted ? "accepted (resumed)"
					: (rd.reason.empty() ? "rejected (resumed)"
						: rd.reason);
				if (rd.accepted)
					out->accepted.push_back(rd_decision);
				else
					out->rejected.push_back(rd_decision);
				found_resume = true;
				break ;
			}
		if (found_resume)
			continue ;
		if (out->evals_used >= search_budget)
			break ;
		if ((s.is_k ? out->policy.kbits[s.layer] : out->policy.vbits[s.layer])
				== 4)
			continue ;	/* already at Q4 in the starting policy */
		trial = out->policy;
		if (s.is_k)
			trial.kbits[s.layer] = 4;
		else
			trial.vbits[s.layer] = 4;
		d.slot = s;
		if (!evaluate_and_check(model, vocab, n_ctx, gen_tokens, valid, order,
				classes, s.is_k, margin, must_stay_correct, trial, &d.result,
				&reason))
			return (false);
		out->evals_used++;
		d.memory_gain_bytes = slot_memory_gain(valid[0].idx, s);
		d.ratio = worst_margin_ratio(d.result, classes, d.memory_gain_bytes);
		d.accepted = reason.empty();
		d.reason = reason.empty() ? "accepted" : reason;
		fprintf(stderr, "  layer %2d %s -> Q4: agg-cosine %.6f (screened)  "
			"%s\n", s.layer, s.is_k ? "K" : "V", d.result.aggregate.cosine,
			d.accepted ? "ACCEPTED" : ("rejected: " + reason).c_str());
		write_checkpoint(checkpoint_out, model_name, tier, s, d.accepted,
			d.result.aggregate.cosine, d.result.aggregate.top1,
			d.result.aggregate.top5, d.reason);
		if (d.accepted)
		{
			out->policy = trial;
			out->accepted.push_back(d);
		}
		else
			out->rejected.push_back(d);
	}
	out->search_seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - t0).count();
	return (true);
}

static bool	backtrack_v2(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const std::vector<baseline_t> &valid,
				const std::vector<prompt_class_t> &classes,
				const std::vector<bool> &must_stay_correct,
				const margin_t &margin, optimizer_result2_t *opt)
{
	int		n_check;
	int		i;
	kv_policy_t	trial;
	per_prompt_result_t	r;
	per_prompt_result_t	full;

	n_check = (int)opt->accepted.size() < 4 ? (int)opt->accepted.size() : 4;
	if (n_check == 0)
		return (true);
	if (!evaluate_policy_detailed(model, vocab, n_ctx, gen_tokens, valid,
			opt->policy, &full))
		return (false);
	i = (int)opt->accepted.size() - 1;
	while (i >= (int)opt->accepted.size() - n_check)
	{
		const slot_t	&s = opt->accepted[i].slot;

		trial = opt->policy;
		if (s.is_k)
			trial.kbits[s.layer] = 8;
		else
			trial.vbits[s.layer] = 8;
		if (!evaluate_policy_detailed(model, vocab, n_ctx, gen_tokens, valid,
				trial, &r))
			return (false);
		opt->evals_used++;
		bool	full_fails = !check_candidate_per_prompt(full, valid, classes,
				s.is_k, margin, must_stay_correct).empty();
		bool	recovers = full_fails && check_candidate_per_prompt(r, valid,
				classes, s.is_k, margin, must_stay_correct).empty();
		fprintf(stderr, "  revert layer %2d %s -> Q8: %s\n", s.layer,
			s.is_k ? "K" : "V", recovers ? "REVERTED (was the cause of a "
				"per-prompt failure)" : "kept at Q4");
		if (recovers)
		{
			opt->policy = trial;
			opt->accepted[i].accepted = false;
			full = r;
		}
		i--;
	}
	opt->accepted.erase(std::remove_if(opt->accepted.begin(),
			opt->accepted.end(), [](const decision2_t &d) {
				return (!d.accepted); }), opt->accepted.end());
	return (true);
}

static long	peak_rss_kb(void);

typedef struct s_perf_result
{
	double	ttft_ms;
	double	tok_per_sec;
	size_t	kv_bytes;
	long	peak_rss_kb;
}	perf_result_t;

/* Item 7: TTFT + tokens/second under a NATIVE, uniformly-typed KV cache --
 * the only KV configuration llama.cpp's public API can actually execute.
 * Composed per-layer policies (conservative/balanced/aggressive) are
 * numerically spliced after the fact into an F16-backed context purely
 * for quality evaluation, so running them through this real-decode timer
 * would measure F16 compute cost while implying a speedup the runtime
 * cannot actually deliver for a per-layer-mixed KV cache -- there is no
 * public API to build one. Only all-FP16/all-Q8/all-Q4 get a real,
 * hardware-measured TTFT/tok-s; this is disclosed explicitly wherever
 * these numbers are reported, and composed policies are documented as
 * memory-only projections instead. */
static bool	measure_native_perf(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				ggml_type tk, ggml_type tv, perf_result_t *out)
{
	llama_context	*ctx;
	pass_result_t	pr;
	bool			ok;
	std::chrono::steady_clock::time_point	t0;
	std::chrono::steady_clock::time_point	t_first_tok;
	std::chrono::steady_clock::time_point	t_gen_done;

	ctx = make_context(model, n_ctx, tk, tv);
	if (ctx == NULL)
		return (false);
	t0 = std::chrono::steady_clock::now();
	ok = decode_prompt(ctx, base.prompt_tokens, 256);
	if (ok)
		ok = run_gen(ctx, vocab, 1, NULL, &pr);
	t_first_tok = std::chrono::steady_clock::now();
	if (ok && gen_tokens > 1)
		ok = run_gen(ctx, vocab, gen_tokens - 1, NULL, &pr);
	t_gen_done = std::chrono::steady_clock::now();
	if (ok)
	{
		out->ttft_ms = std::chrono::duration<double, std::milli>(
				t_first_tok - t0).count();
		out->tok_per_sec = gen_tokens > 1 ? (double)(gen_tokens - 1)
			/ std::chrono::duration<double>(t_gen_done - t_first_tok).count()
			: 0.0;
		out->kv_bytes = llama_state_seq_get_size(ctx, 0);
		out->peak_rss_kb = peak_rss_kb();
	}
	llama_free(ctx);
	return (ok);
}

/* Item 6: tokens/second for one policy on one prompt, measured over a
 * single timed free-running generation pass on the spliced state. */
static bool	measure_speed(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const kv_policy_t &pol, double *tok_per_sec)
{
	std::vector<uint8_t>	blob;
	llama_context			*ctx;
	pass_result_t			pr;
	bool					ok;
	std::chrono::steady_clock::time_point	t0;
	std::chrono::steady_clock::time_point	t1;

	blob = base.blob;
	apply_targets(blob.data(), base.idx, policy_targets(base.idx, pol));
	ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16);
	if (ctx == NULL)
		return (false);
	ok = llama_state_seq_set_data(ctx, blob.data(), blob.size(), 0) > 0
		&& decode_one(ctx, base.last_token);
	if (ok)
	{
		t0 = std::chrono::steady_clock::now();
		ok = run_gen(ctx, vocab, gen_tokens, NULL, &pr);
		t1 = std::chrono::steady_clock::now();
	}
	if (ok)
		*tok_per_sec = (double)gen_tokens
			/ std::chrono::duration<double>(t1 - t0).count();
	llama_free(ctx);
	return (ok);
}

/* The exact policy Phase 3.4 produced (see docs/phase3-composition-aware.md):
 * V Q4 everywhere except layers 16 and 25; K Q4 only at layers
 * 0,2,3,4,6,8,11,14,20,25. Reused here (not re-derived -- its search was
 * already run and documented in Phase 3.4) purely to re-measure its live
 * performance fresh, for a fair, current comparison; it is not re-searched. */
static kv_policy_t	phase34_reused_policy(uint32_t n_layer)
{
	static const int	k_q4[] = {0, 2, 3, 4, 6, 8, 11, 14, 20, 25};
	kv_policy_t			p;
	uint32_t			l;
	size_t				i;

	p = all_q8_policy(n_layer);
	l = 0;
	while (l < n_layer)
	{
		if (l != 16 && l != 25)
			p.vbits[l] = 4;
		l++;
	}
	i = 0;
	while (i < sizeof(k_q4) / sizeof(k_q4[0]))
	{
		if ((uint32_t)k_q4[i] < n_layer)
			p.kbits[k_q4[i]] = 4;
		i++;
	}
	return (p);
}

/* ------------------------------------------------------------------ */
/* Phase 3.6 item 5: transfer-policy experiment. A policy's Q4 layers are  */
/* normalized to FRACTIONAL depth (layer / (n_layer-1)) so the same       */
/* relative "which part of the network" pattern can be denormalized onto  */
/* a model with a different layer count, then validated -- never trusted */
/* -- against that model's own hard constraints.                         */
/* ------------------------------------------------------------------ */

typedef struct s_normalized_policy
{
	std::vector<double>	k_fractions;
	std::vector<double>	v_fractions;
}	normalized_policy_t;

static normalized_policy_t	normalize_policy(const kv_policy_t &pol,
		uint32_t n_layer)
{
	normalized_policy_t	np;
	uint32_t				l;
	double					frac;

	l = 0;
	while (l < n_layer)
	{
		frac = n_layer > 1 ? (double)l / (double)(n_layer - 1) : 0.0;
		if (pol.kbits[l] == 4)
			np.k_fractions.push_back(frac);
		if (pol.vbits[l] == 4)
			np.v_fractions.push_back(frac);
		l++;
	}
	return (np);
}

static kv_policy_t	denormalize_policy(const normalized_policy_t &np,
		uint32_t n_layer)
{
	kv_policy_t	p;
	uint32_t	l;

	p = all_q8_policy(n_layer);
	for (double f : np.k_fractions)
	{
		l = (uint32_t)llround(f * (double)(n_layer > 1 ? n_layer - 1 : 0));
		if (l < n_layer)
			p.kbits[l] = 4;
	}
	for (double f : np.v_fractions)
	{
		l = (uint32_t)llround(f * (double)(n_layer > 1 ? n_layer - 1 : 0));
		if (l < n_layer)
			p.vbits[l] = 4;
	}
	return (p);
}

static bool	save_normalized_policy(const char *path,
				const normalized_policy_t &np)
{
	FILE	*f;
	size_t	i;

	f = fopen(path, "w");
	if (f == NULL)
		return (false);
	fprintf(f, "{\"k_fractions\":[");
	for (i = 0; i < np.k_fractions.size(); i++)
		fprintf(f, "%s%.6f", i ? "," : "", np.k_fractions[i]);
	fprintf(f, "],\"v_fractions\":[");
	for (i = 0; i < np.v_fractions.size(); i++)
		fprintf(f, "%s%.6f", i ? "," : "", np.v_fractions[i]);
	fprintf(f, "]}\n");
	fclose(f);
	return (true);
}

static std::vector<double>	parse_fraction_array(const std::string &content,
		const char *key)
{
	std::vector<double>	out;
	size_t					p;
	size_t					e;
	std::stringstream		ss;
	std::string				tok;

	p = content.find(key);
	if (p == std::string::npos)
		return (out);
	p = content.find('[', p);
	e = content.find(']', p);
	if (p == std::string::npos || e == std::string::npos)
		return (out);
	ss.str(content.substr(p + 1, e - p - 1));
	while (std::getline(ss, tok, ','))
		if (!tok.empty())
			out.push_back(atof(tok.c_str()));
	return (out);
}

static bool	load_normalized_policy(const char *path, normalized_policy_t *np)
{
	FILE		*f;
	std::string	content;
	char		buf[4096];
	size_t		n;

	f = fopen(path, "r");
	if (f == NULL)
		return (false);
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		content.append(buf, n);
	fclose(f);
	np->k_fractions = parse_fraction_array(content, "k_fractions");
	np->v_fractions = parse_fraction_array(content, "v_fractions");
	return (true);
}

/* ------------------------------------------------------------------ */
/* CLI                                                                  */
/* ------------------------------------------------------------------ */

typedef struct s_prompt_arg
{
	const char	*path;
	const char	*answer;	/* NULL if "-" */
}	prompt_arg_t;

typedef struct s_opts
{
	const char					*model_path;
	std::vector<prompt_arg_t>	prompts;
	int							profiler_index;
	int							n_tokens;
	int							gen_tokens;
	const char					*out_path;
	const char					*mode;	/* "sensitivity", "optimize", or "risk" */
	int							search_budget;	/* Phase 3.4/3.5 max slots considered */
	int							robustness_budget;	/* Phase 3.5 item 5 */
	margin_t					balanced_margin_override;
	bool						has_margin_override;
	const char					*model_name;	/* Phase 3.6: label for reporting
											 * and checkpoint matching */
	const char					*checkpoint_path;	/* Phase 3.6 item 8 */
	bool						resume;				/* read checkpoint_path back */
	const char					*export_policy_path;	/* Phase 3.6 item 5 */
	const char					*transfer_from_path;
	bool						skip_legacy_comparison;	/* Phase 3.6: item 3
					 * only asks for FP16/Q8/Q4/conservative/balanced/
					 * aggressive, not the Phase 3.3/3.4 policies -- this
					 * skips their comparatively expensive layer/age
					 * sweep for cross-model runs where it isn't needed. */
}	opts_t;

static int	parse_args(int argc, char **argv, opts_t *o)
{
	int	i;

	o->model_path = NULL;
	o->prompts.clear();
	o->profiler_index = 0;
	o->n_tokens = 1024;
	o->gen_tokens = 32;
	o->out_path = NULL;
	o->mode = "sensitivity";
	o->search_budget = 60;
	o->robustness_budget = 16;
	o->balanced_margin_override = g_margin_balanced;
	o->has_margin_override = false;
	o->model_name = "model";
	o->checkpoint_path = NULL;
	o->resume = false;
	o->export_policy_path = NULL;
	o->transfer_from_path = NULL;
	o->skip_legacy_comparison = false;
	i = 1;
	while (i < argc)
	{
		if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
		{
			o->model_path = argv[i + 1];
			i += 2;
		}
		else if (strcmp(argv[i], "--prompt") == 0 && i + 2 < argc)
		{
			const char *ans = strcmp(argv[i + 2], "-") == 0
				? NULL : argv[i + 2];
			o->prompts.push_back({argv[i + 1], ans});
			i += 3;
		}
		else if (strcmp(argv[i], "--profiler-index") == 0 && i + 1 < argc)
		{
			o->profiler_index = atoi(argv[i + 1]);
			i += 2;
		}
		else if (strcmp(argv[i], "--n-tokens") == 0 && i + 1 < argc)
		{
			o->n_tokens = atoi(argv[i + 1]);
			i += 2;
		}
		else if (strcmp(argv[i], "--gen-tokens") == 0 && i + 1 < argc)
		{
			o->gen_tokens = atoi(argv[i + 1]);
			i += 2;
		}
		else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
		{
			o->out_path = argv[i + 1];
			i += 2;
		}
		else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
		{
			o->mode = argv[i + 1];
			i += 2;
		}
		else if (strcmp(argv[i], "--search-budget") == 0 && i + 1 < argc)
		{
			o->search_budget = atoi(argv[i + 1]);
			i += 2;
		}
		else if (strcmp(argv[i], "--robustness-budget") == 0 && i + 1 < argc)
		{
			o->robustness_budget = atoi(argv[i + 1]);
			i += 2;
		}
		else if (strcmp(argv[i], "--cosine-margin") == 0 && i + 1 < argc)
		{
			o->balanced_margin_override.cosine_margin = atof(argv[i + 1]);
			o->has_margin_override = true;
			i += 2;
		}
		else if (strcmp(argv[i], "--top1-margin") == 0 && i + 1 < argc)
		{
			o->balanced_margin_override.top1_margin = atof(argv[i + 1]);
			o->has_margin_override = true;
			i += 2;
		}
		else if (strcmp(argv[i], "--top5-margin") == 0 && i + 1 < argc)
		{
			o->balanced_margin_override.top5_margin = atof(argv[i + 1]);
			o->has_margin_override = true;
			i += 2;
		}
		else if (strcmp(argv[i], "--model-name") == 0 && i + 1 < argc)
		{
			o->model_name = argv[i + 1];
			i += 2;
		}
		else if (strcmp(argv[i], "--checkpoint") == 0 && i + 1 < argc)
		{
			o->checkpoint_path = argv[i + 1];
			i += 2;
		}
		else if (strcmp(argv[i], "--resume") == 0)
		{
			o->resume = true;
			i += 1;
		}
		else if (strcmp(argv[i], "--export-policy") == 0 && i + 1 < argc)
		{
			o->export_policy_path = argv[i + 1];
			i += 2;
		}
		else if (strcmp(argv[i], "--transfer-from") == 0 && i + 1 < argc)
		{
			o->transfer_from_path = argv[i + 1];
			i += 2;
		}
		else if (strcmp(argv[i], "--skip-legacy-comparison") == 0)
		{
			o->skip_legacy_comparison = true;
			i += 1;
		}
		else
			return (die("unknown or malformed option"));
	}
	if (o->model_path == NULL || o->prompts.empty())
		return (die("usage: --model M --prompt PATH ANSWER|- [...]"
				" [--profiler-index N] [--n-tokens N] [--gen-tokens G]"
				" [--out OUT.jsonl] [--mode sensitivity|optimize|risk]"
				" [--search-budget N] [--model-name LABEL]"
				" [--checkpoint PATH] [--resume]"));
	return (0);
}

static long	peak_rss_kb(void)
{
	struct rusage	ru;

	getrusage(RUSAGE_SELF, &ru);
	return (ru.ru_maxrss);
}

/* Top-level Phase 3.4 flow: baseline filtering (item 6), all-Q8 recall
 * reference (item 4), greedy search (item 2), backtracking (item 3),
 * a fresh Phase 3.3 independent policy for comparison, and the final
 * 5-way table (item 7) over every prompt (valid and excluded alike, the
 * latter clearly labeled). */
static bool	run_optimizer_mode(llama_model *model, const llama_vocab *vocab,
				const opts_t &o, FILE *out)
{
	std::vector<baseline_t>	all_bases;
	std::vector<baseline_t>	valid;
	std::vector<bool>			is_valid_flags;
	kv_policy_t					q8_ref_policy;
	agg_result_t				q8_ref;
	optimizer_result_t			opt;
	std::vector<layer_result_t>	layer_results;
	std::vector<age_result_t>		age_results;
	std::vector<int>				layer_bits;
	bool						age_override;
	size_t						i;
	std::chrono::steady_clock::time_point	t0;
	double						total_seconds;

	t0 = std::chrono::steady_clock::now();
	fprintf(stderr, "\n=== Phase 3.4: baseline filtering (item 6) ===\n");
	i = 0;
	while (i < o.prompts.size())
	{
		baseline_t	b;

		if (!capture_baseline(model, vocab, o.n_tokens, o.gen_tokens,
				o.prompts[i].path, o.prompts[i].answer, &b))
			return (false);
		bool ok = (b.answer == NULL)
			|| (b.text.find(b.answer) != std::string::npos);
		fprintf(stderr, "  %-32s %s\n", o.prompts[i].path,
			ok ? "VALID" : "EXCLUDED (FP16 baseline itself does not "
				"answer correctly)");
		if (ok)
			valid.push_back(b);
		is_valid_flags.push_back(ok);
		all_bases.push_back(std::move(b));
		i++;
	}
	if (valid.empty())
		return (die("no valid prompts -- FP16 baseline fails all of them, "
				"cannot optimize"), false);
	fprintf(stderr, "valid evaluation set: %zu/%zu prompts\n", valid.size(),
		o.prompts.size());
	q8_ref_policy = all_q8_policy(valid[0].idx.n_layer);
	if (!evaluate_policy(model, vocab, o.n_tokens, o.gen_tokens, valid,
			q8_ref_policy, &q8_ref))
		return (false);
	fprintf(stderr, "\nall-Q8 reference on the valid set: cosine %.6f  "
		"top1 %.2f%%  top5 %.2f%%\n", q8_ref.cosine, q8_ref.top1,
		q8_ref.top5);
	if (!greedy_optimize(model, vocab, o.n_tokens, o.gen_tokens, valid,
			q8_ref.recall_ok, o.search_budget, &opt))
		return (false);
	if (!backtrack(model, vocab, o.n_tokens, o.gen_tokens, valid,
			q8_ref.recall_ok, &opt))
		return (false);
	fprintf(stderr, "\nFinal Phase 3.4 policy: %zu accepted, %zu rejected, "
		"%d live evaluations, %.1fs search time\n", opt.accepted.size(),
		opt.rejected.size(), opt.evals_used, opt.search_seconds);
	if (!run_layer_sweep(model, vocab, o.n_tokens, o.gen_tokens, valid[0],
			g_default_thresholds, &layer_results))
		return (false);
	if (!run_age_sweep(model, vocab, o.n_tokens, o.gen_tokens, valid[0],
			g_default_thresholds, &age_results))
		return (false);
	layer_bits.resize(layer_results.size());
	for (i = 0; i < layer_results.size(); i++)
		layer_bits[i] = layer_results[i].classification;
	age_override = !age_results.empty() && age_results[0].classification == 4;
	emit_policy_json(out, opt, (int)valid[0].idx.n_layer);
	fprintf(stderr, "\n=== Final comparison across %zu prompts ===\n",
		all_bases.size());
	i = 0;
	while (i < all_bases.size())
	{
		if (!run_final_comparison(model, vocab, o.n_tokens, o.gen_tokens,
				all_bases[i], o.prompts[i].path, layer_bits, age_override,
				opt.policy, out, is_valid_flags[i]))
			return (false);
		i++;
	}
	total_seconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - t0).count();
	fprintf(stderr, "\npolicy decision overhead (this whole run, wall "
		"clock): %.1fs\n", total_seconds);
	return (true);
}

typedef struct s_pareto_point
{
	const char			*name;
	margin_t			margin;
	optimizer_result2_t	opt;
}	pareto_point_t;

/* Item 6: three points on the memory/quality Pareto frontier, produced by
 * running the SAME per-prompt-constraint greedy search at three different
 * safety margins -- larger margins accept fewer, safer candidates. */
static bool	run_pareto(llama_model *model, const llama_vocab *vocab,
				const opts_t &o, const std::vector<baseline_t> &valid,
				const std::vector<prompt_class_t> &classes,
				const std::vector<bool> &must_stay_correct,
				std::vector<pareto_point_t> *out)
{
	const struct { const char *name; margin_t margin; } levels[] = {
		{"conservative", g_margin_conservative},
		{"balanced", o.has_margin_override ? o.balanced_margin_override
			: g_margin_balanced},
		{"aggressive", g_margin_aggressive},
	};
	kv_policy_t	start;
	size_t		li;
	FILE		*checkpoint_out;

	checkpoint_out = o.checkpoint_path != NULL
		? fopen(o.checkpoint_path, "a") : NULL;
	start = all_q8_policy(valid[0].idx.n_layer);
	li = 0;
	while (li < sizeof(levels) / sizeof(levels[0]))
	{
		pareto_point_t	p;

		p.name = levels[li].name;
		p.margin = levels[li].margin;
		fprintf(stderr, "\n=== Pareto point: %s (cosine_margin=%.4f "
			"top1_margin=%.3f top5_margin=%.3f) ===\n", p.name,
			p.margin.cosine_margin, p.margin.top1_margin,
			p.margin.top5_margin);
		if (!greedy_optimize_v2(model, vocab, o.n_tokens, o.gen_tokens, valid,
				classes, must_stay_correct, p.margin, o.search_budget, start,
				&p.opt, o.model_name, p.name, checkpoint_out,
				o.resume ? o.checkpoint_path : NULL))
		{
			if (checkpoint_out != NULL)
				fclose(checkpoint_out);
			return (false);
		}
		if (!backtrack_v2(model, vocab, o.n_tokens, o.gen_tokens, valid,
				classes, must_stay_correct, p.margin, &p.opt))
		{
			if (checkpoint_out != NULL)
				fclose(checkpoint_out);
			return (false);
		}
		fprintf(stderr, "  %s: %zu accepted, %zu rejected, %d evals, "
			"%.1fs\n", p.name, p.opt.accepted.size(), p.opt.rejected.size(),
			p.opt.evals_used, p.opt.search_seconds);
		out->push_back(std::move(p));
		li++;
	}
	if (checkpoint_out != NULL)
		fclose(checkpoint_out);
	return (true);
}

/* Item 4/9: writes every Pareto tier's final policy AND every individual
 * accept/reject candidate decision (layer, K/V, aggregate cosine/top1/top5,
 * rejection reason when applicable) to the machine-readable JSONL --
 * without this, layer-position distribution and candidate rejection
 * reasons (both explicitly required by item 4) would only exist as
 * stderr text, not in a format the cross-model analysis can consume. */
static void	emit_pareto_json(FILE *out, const char *model_name,
				const std::vector<pareto_point_t> &pareto, uint32_t n_layer)
{
	uint32_t	l;

	if (out == NULL)
		return ;
	for (const pareto_point_t &pp : pareto)
	{
		fprintf(out, "{\"record\":\"pareto_policy\",\"model\":\"%s\","
			"\"tier\":\"%s\",\"kbits\":[", model_name, pp.name);
		for (l = 0; l < n_layer; l++)
			fprintf(out, "%s%d", l ? "," : "", pp.opt.policy.kbits[l]);
		fprintf(out, "],\"vbits\":[");
		for (l = 0; l < n_layer; l++)
			fprintf(out, "%s%d", l ? "," : "", pp.opt.policy.vbits[l]);
		fprintf(out, "],\"evals_used\":%d,\"search_seconds\":%.3f,"
			"\"accepted\":%zu,\"rejected\":%zu}\n", pp.opt.evals_used,
			pp.opt.search_seconds, pp.opt.accepted.size(),
			pp.opt.rejected.size());
		for (const decision2_t &d : pp.opt.accepted)
			fprintf(out, "{\"record\":\"pareto_decision\",\"model\":\"%s\","
				"\"tier\":\"%s\",\"layer\":%d,\"kv\":\"%s\","
				"\"outcome\":\"accepted\",\"agg_cosine\":%.6f,"
				"\"agg_top1\":%.4f,\"agg_top5\":%.4f,\"ratio\":%.4f}\n",
				model_name, pp.name, d.slot.layer, d.slot.is_k ? "K" : "V",
				d.result.aggregate.cosine, d.result.aggregate.top1,
				d.result.aggregate.top5, d.ratio);
		for (const decision2_t &d : pp.opt.rejected)
			fprintf(out, "{\"record\":\"pareto_decision\",\"model\":\"%s\","
				"\"tier\":\"%s\",\"layer\":%d,\"kv\":\"%s\","
				"\"outcome\":\"rejected\",\"agg_cosine\":%.6f,"
				"\"agg_top1\":%.4f,\"agg_top5\":%.4f,\"reason\":\"%s\"}\n",
				model_name, pp.name, d.slot.layer, d.slot.is_k ? "K" : "V",
				d.result.aggregate.cosine, d.result.aggregate.top1,
				d.result.aggregate.top5, json_escape(d.reason).c_str());
	}
}

/* Item 5: re-runs the balanced-margin search at a reduced budget with the
 * valid prompt list reversed and (fixed-seed) shuffled, and compares the
 * resulting per-slot accept/reject decisions against the original order's
 * decisions over the same candidates -- per-prompt hard constraints are a
 * conjunction over the valid set, so accept/reject must be independent of
 * the order the set is iterated in; this proves that empirically rather
 * than only arguing it from the code. */
static bool	run_robustness_check(llama_model *model, const llama_vocab *vocab,
				const opts_t &o, const std::vector<baseline_t> &valid,
				const std::vector<prompt_class_t> &classes,
				const std::vector<bool> &must_stay_correct, int reduced_budget)
{
	std::vector<size_t>	orders[3];
	const char				*names[3] = {"original", "reversed", "shuffled"};
	size_t					n;
	size_t					i;
	int						mismatches;

	n = valid.size();
	orders[0].resize(n);
	orders[1].resize(n);
	orders[2] = {};
	for (i = 0; i < n; i++)
	{
		orders[0][i] = i;
		orders[1][n - 1 - i] = i;
	}
	orders[2] = {n > 3 ? 2u : 0u, 0, n > 2 ? 3u % n : 0, 1};
	while (orders[2].size() < n)
		orders[2].push_back(orders[2].size());
	orders[2].resize(n);
	fprintf(stderr, "\n=== Robustness: balanced policy under 3 prompt "
		"orderings (reduced budget=%d) ===\n", reduced_budget);
	std::vector<std::string>	decisions[3];
	for (int oi = 0; oi < 3; oi++)
	{
		std::vector<baseline_t>	permuted;
		std::vector<prompt_class_t>	pclasses;
		std::vector<bool>				pmust;

		for (size_t idx : orders[oi])
		{
			permuted.push_back(valid[idx]);
			pclasses.push_back(classes[idx]);
			pmust.push_back(must_stay_correct[idx]);
		}
		optimizer_result2_t	opt;
		kv_policy_t	start = all_q8_policy(permuted[0].idx.n_layer);
		margin_t	margin = o.has_margin_override ? o.balanced_margin_override
			: g_margin_balanced;
		if (!greedy_optimize_v2(model, vocab, o.n_tokens, o.gen_tokens,
				permuted, pclasses, pmust, margin, reduced_budget,
				start, &opt))
			return (false);
		for (const decision2_t &d : opt.accepted)
			decisions[oi].push_back(std::to_string(d.slot.layer) + (d.slot.is_k
					? "K" : "V") + "=accept");
		for (const decision2_t &d : opt.rejected)
			decisions[oi].push_back(std::to_string(d.slot.layer) + (d.slot.is_k
					? "K" : "V") + "=reject");
		std::sort(decisions[oi].begin(), decisions[oi].end());
		fprintf(stderr, "  %-10s: %zu accepted, %zu rejected (of %d "
			"considered)\n", names[oi], opt.accepted.size(),
			opt.rejected.size(), reduced_budget);
	}
	mismatches = 0;
	for (i = 0; i < decisions[0].size() && i < decisions[1].size(); i++)
		if (decisions[0][i] != decisions[1][i])
			mismatches++;
	for (i = 0; i < decisions[0].size() && i < decisions[2].size(); i++)
		if (decisions[0][i] != decisions[2][i])
			mismatches++;
	fprintf(stderr, "  stability: %s (%d mismatched decisions across the "
		"3 orderings, out of %zu candidates x2 comparisons)\n",
		mismatches == 0 ? "STABLE -- identical decisions regardless of "
			"prompt order" : "UNSTABLE", mismatches, decisions[0].size());
	return (true);
}

/* Item 5: loads a normalized layer-position policy learned on a DIFFERENT
 * model (e.g. the balanced policy exported from SmolLM2-135M), denormalizes
 * it onto this model's own layer count, and validates it against THIS
 * model's own per-prompt hard constraints at the balanced margin. A
 * transfer policy that fails even one prompt's threshold is reported
 * UNSAFE and is never accepted as a substitute for this model's own
 * independently-optimized policy, regardless of how well it performed on
 * the source model it was learned from. */
static bool	run_transfer_policy_check(llama_model *model,
				const llama_vocab *vocab, const opts_t &o,
				const std::vector<baseline_t> &valid,
				const std::vector<prompt_class_t> &classes,
				const std::vector<bool> &must_stay_correct,
				const std::vector<pareto_point_t> &pareto, FILE *json_out)
{
	normalized_policy_t		np;
	kv_policy_t				transferred;
	per_prompt_result_t		result;
	std::string				reason;
	margin_t				margin;
	const pareto_point_t	*own_balanced;
	bool					is_k;
	bool					safe;
	uint32_t				n_layer;
	uint32_t				l;
	uint32_t				own_q4;
	uint32_t				transfer_q4;
	size_t					pi;

	fprintf(stderr, "\n=== Transfer-policy check: '%s' on %s ===\n",
		o.transfer_from_path, o.model_name);
	if (!load_normalized_policy(o.transfer_from_path, &np))
	{
		fprintf(stderr, "  FAILED to load '%s' -- skipping transfer-policy "
			"check\n", o.transfer_from_path);
		return (true);
	}
	n_layer = valid[0].idx.n_layer;
	transferred = denormalize_policy(np, n_layer);
	is_k = false;
	l = 0;
	while (l < n_layer)
	{
		if (transferred.kbits[l] == 4)
			is_k = true;
		l++;
	}
	margin = o.has_margin_override ? o.balanced_margin_override
		: g_margin_balanced;
	if (!evaluate_policy_detailed(model, vocab, o.n_tokens, o.gen_tokens,
			valid, transferred, &result))
		return (false);
	reason = check_candidate_per_prompt(result, valid, classes, is_k, margin,
			must_stay_correct);
	safe = reason.empty();
	fprintf(stderr, "  denormalized: %d K-Q4 layers, %d V-Q4 layers (of %u)\n",
		(int)np.k_fractions.size(), (int)np.v_fractions.size(), n_layer);
	fprintf(stderr, "  aggregate cosine %.6f  top1 %.2f%%  top5 %.2f%%\n",
		result.aggregate.cosine, result.aggregate.top1,
		result.aggregate.top5);
	if (safe)
		fprintf(stderr, "  verdict: SAFE on %s -- passes every valid "
			"prompt's own hard constraint at the balanced margin\n",
			o.model_name);
	else
		fprintf(stderr, "  verdict: UNSAFE on %s -- %s (transfer policy is "
			"NOT accepted; this model's own optimized policy must be used "
			"instead)\n", o.model_name, reason.c_str());
	own_balanced = NULL;
	pi = 0;
	while (pi < pareto.size())
	{
		if (strcmp(pareto[pi].name, "balanced") == 0)
			own_balanced = &pareto[pi];
		pi++;
	}
	if (own_balanced != NULL)
	{
		own_q4 = 0;
		transfer_q4 = 0;
		l = 0;
		while (l < n_layer)
		{
			if (own_balanced->opt.policy.kbits[l] == 4)
				own_q4++;
			if (own_balanced->opt.policy.vbits[l] == 4)
				own_q4++;
			if (transferred.kbits[l] == 4)
				transfer_q4++;
			if (transferred.vbits[l] == 4)
				transfer_q4++;
			l++;
		}
		fprintf(stderr, "  vs. this model's own optimized balanced policy: "
			"%u/%u Q4 slots (own) vs %u/%u Q4 slots (transferred)\n",
			own_q4, n_layer * 2, transfer_q4, n_layer * 2);
	}
	if (json_out != NULL)
		fprintf(json_out, "{\"record\":\"transfer_policy\",\"model\":\"%s\","
			"\"source\":\"%s\",\"safe\":%s,\"reason\":\"%s\","
			"\"aggregate_cosine\":%.6f,\"aggregate_top1\":%.4f,"
			"\"aggregate_top5\":%.4f,\"k_q4_layers\":%d,\"v_q4_layers\":%d}\n",
			o.model_name, o.transfer_from_path, safe ? "true" : "false",
			json_escape(reason).c_str(), result.aggregate.cosine,
			result.aggregate.top1, result.aggregate.top5,
			(int)np.k_fractions.size(), (int)np.v_fractions.size());
	return (true);
}

/* Item 7: all-Q8 / Phase 3.3 policy / Phase 3.4 policy (reused) /
 * Phase 3.5 conservative / balanced / aggressive, for one prompt. */
static bool	run_risk_comparison(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens, const baseline_t &base,
				const std::vector<int> &layer_bits, bool age_override,
				const kv_policy_t &phase34_policy,
				const std::vector<pareto_point_t> &pareto, FILE *json_out,
				bool is_valid, bool skip_legacy)
{
	metrics_t	m_q8;
	metrics_t	m_q4;
	metrics_t	m_p33;
	metrics_t	m_p34;
	size_t		kv_q8;
	size_t		kv_q4;
	double		f16b_real;
	double		f16b_analytic;
	double		tps;
	std::vector<perturb_target_t>	pol33;
	std::vector<perturb_target_t>	pol34;
	bool		ok;

	f16b_real = (double)base.f16_state_bytes;
	f16b_analytic = full_f16_bytes(base.idx);
	ok = run_kv_combo(model, vocab, n_ctx, gen_tokens, base, GGML_TYPE_Q8_0,
			GGML_TYPE_Q8_0, &m_q8, &kv_q8);
	/* Item 6 success criteria need all-Q4's QUALITY (cosine/top1/top5/
	 * recall), not just its memory/speed (already covered by
	 * measure_native_perf) -- without this, "clearly better quality than
	 * all-Q4" can never actually be checked against a measured number. */
	if (ok)
		ok = run_kv_combo(model, vocab, n_ctx, gen_tokens, base,
				GGML_TYPE_Q4_0, GGML_TYPE_Q4_0, &m_q4, &kv_q4);
	if (!skip_legacy)
	{
		pol33 = build_policy_targets(base.idx, layer_bits, age_override, 4);
		if (ok)
			ok = run_experiment(model, vocab, n_ctx, gen_tokens, base, pol33,
					&m_p33);
		pol34 = policy_targets(base.idx, phase34_policy);
		if (ok)
			ok = run_experiment(model, vocab, n_ctx, gen_tokens, base, pol34,
					&m_p34);
	}
	if (!ok)
		return (false);
	fprintf(stderr, "\n--- prompt: %s (%s) ---\n", base.name.c_str(),
		is_valid ? "valid" : "EXCLUDED, informational only");
	print_row("all-Q8 (native)", m_q8, (double)kv_q8, f16b_real);
	print_row("all-Q4 (native)", m_q4, (double)kv_q4, f16b_real);
	emit_json_row(json_out, base.name.c_str(), "all_q8", m_q8, (double)kv_q8,
		f16b_real);
	emit_json_row(json_out, base.name.c_str(), "all_q4", m_q4, (double)kv_q4,
		f16b_real);
	if (!skip_legacy)
	{
		print_row("Phase 3.3 policy", m_p33, projected_kv_bytes(base.idx,
				pol33), f16b_analytic);
		print_row("Phase 3.4 policy", m_p34, projected_kv_bytes_kv(base.idx,
				phase34_policy), f16b_analytic);
		emit_json_row(json_out, base.name.c_str(), "phase33_policy", m_p33,
			projected_kv_bytes(base.idx, pol33), f16b_analytic);
		emit_json_row(json_out, base.name.c_str(), "phase34_policy", m_p34,
			projected_kv_bytes_kv(base.idx, phase34_policy), f16b_analytic);
	}
	for (const pareto_point_t &pp : pareto)
	{
		metrics_t	m;
		std::vector<perturb_target_t>	pol = policy_targets(base.idx,
				pp.opt.policy);

		if (!run_experiment(model, vocab, n_ctx, gen_tokens, base, pol, &m))
			return (false);
		if (!measure_speed(model, vocab, n_ctx, gen_tokens, base,
				pp.opt.policy, &tps))
			return (false);
		fprintf(stderr, "  %-24s top1 %6.2f%%  top5 %6.2f%%  cosine %.6f  "
			"KL %.6f  recall %-4s  KV %6.3fx  %.1f tok/s\n", pp.name,
			m.top1_pct, m.top5_pct, m.logit_cosine, m.kl_mean,
			m.recall_ok ? "OK" : "FAIL", f16b_analytic
				/ projected_kv_bytes_kv(base.idx, pp.opt.policy), tps);
		emit_json_row(json_out, base.name.c_str(),
			(std::string("phase35_") + pp.name).c_str(), m,
			projected_kv_bytes_kv(base.idx, pp.opt.policy), f16b_analytic);
	}
	return (true);
}

/* Top-level Phase 3.5 flow: baseline filtering + classification, all-Q8
 * recall reference, the Pareto frontier (item 6), bounded robustness
 * checking (item 5), and the final 6-way comparison (item 7) over every
 * prompt. */
static bool	run_risk_aware_mode(llama_model *model, const llama_vocab *vocab,
				const opts_t &o, FILE *out)
{
	std::vector<baseline_t>		all_bases;
	std::vector<baseline_t>		valid;
	std::vector<prompt_class_t>	valid_classes;
	std::vector<bool>				is_valid_flags;
	std::vector<prompt_class_t>	all_classes;
	agg_result_t					q8_ref;
	per_prompt_result_t			q8_ref_detail;
	kv_policy_t						q8_policy;
	kv_policy_t						phase34_policy;
	std::vector<pareto_point_t>		pareto;
	std::vector<layer_result_t>		layer_results;
	std::vector<age_result_t>			age_results;
	std::vector<int>					layer_bits;
	bool							age_override;
	size_t							i;
	std::chrono::steady_clock::time_point	t0;

	t0 = std::chrono::steady_clock::now();
	age_override = false;
	fprintf(stderr, "\n=== Phase 3.6 model: %s ===\n", o.model_name);
	if (out != NULL)
		fprintf(out, "{\"record\":\"model_info\",\"model\":\"%s\","
			"\"n_tokens\":%d,\"gen_tokens\":%d,\"search_budget\":%d}\n",
			o.model_name, o.n_tokens, o.gen_tokens, o.search_budget);
	fprintf(stderr, "\n=== Phase 3.5: baseline filtering + classification "
		"===\n");
	i = 0;
	while (i < o.prompts.size())
	{
		baseline_t	b;

		if (!capture_baseline(model, vocab, o.n_tokens, o.gen_tokens,
				o.prompts[i].path, o.prompts[i].answer, &b))
			return (false);
		prompt_class_t	c = classify_prompt(b);
		bool ok = (b.answer == NULL)
			|| (b.text.find(b.answer) != std::string::npos);
		fprintf(stderr, "  %-32s [%-16s] %s\n", o.prompts[i].path,
			class_name(c), ok ? "VALID" : "EXCLUDED (FP16 baseline itself "
				"does not answer correctly)");
		all_classes.push_back(c);
		if (ok)
		{
			valid.push_back(b);
			valid_classes.push_back(c);
		}
		is_valid_flags.push_back(ok);
		all_bases.push_back(std::move(b));
		i++;
	}
	if (valid.empty())
		return (die("no valid prompts -- cannot optimize"), false);
	fprintf(stderr, "valid evaluation set: %zu/%zu prompts\n", valid.size(),
		o.prompts.size());
	if (!self_test(model, vocab, o.n_tokens, o.gen_tokens, valid[0]))
		return (die("self-test failed -- aborting, results would not be "
				"trustworthy"), false);
	q8_policy = all_q8_policy(valid[0].idx.n_layer);
	if (!evaluate_policy(model, vocab, o.n_tokens, o.gen_tokens, valid,
			q8_policy, &q8_ref))
		return (false);
	if (!evaluate_policy_detailed(model, vocab, o.n_tokens, o.gen_tokens,
			valid, q8_policy, &q8_ref_detail))
		return (false);
	fprintf(stderr, "\nall-Q8 reference: aggregate cosine %.6f  top1 "
		"%.2f%%  top5 %.2f%%; per-prompt:\n", q8_ref.cosine, q8_ref.top1,
		q8_ref.top5);
	for (i = 0; i < valid.size(); i++)
		fprintf(stderr, "  %-32s [%-16s] cosine %.6f  top1 %.2f%%  "
			"recall %s\n", valid[i].name.c_str(), class_name(valid_classes[i]),
			q8_ref_detail.per_prompt[i].logit_cosine,
			q8_ref_detail.per_prompt[i].top1_pct,
			q8_ref_detail.per_prompt[i].recall_ok ? "OK" : "FAIL");
	if (o.has_margin_override)
		fprintf(stderr, "\n(--cosine-margin/--top1-margin/--top5-margin "
			"override the 'balanced' Pareto point's margins)\n");
	fprintf(stderr, "\n=== Item 7: real hardware TTFT/tok-s (native KV "
		"types only -- see measure_native_perf comment for why composed "
		"policies cannot be measured this way) ===\n");
	{
		const struct { const char *name; ggml_type tk; ggml_type tv; }
			native[] = {
				{"all-FP16", GGML_TYPE_F16, GGML_TYPE_F16},
				{"all-Q8", GGML_TYPE_Q8_0, GGML_TYPE_Q8_0},
				{"all-Q4", GGML_TYPE_Q4_0, GGML_TYPE_Q4_0},
			};
		size_t	ni;

		ni = 0;
		while (ni < sizeof(native) / sizeof(native[0]))
		{
			perf_result_t	perf;

			if (!measure_native_perf(model, vocab, o.n_tokens, o.gen_tokens,
					valid[0], native[ni].tk, native[ni].tv, &perf))
				return (false);
			fprintf(stderr, "  %-8s TTFT %8.1fms  %6.1f tok/s  KV %8zu "
				"bytes  peak RSS %6ldMB\n", native[ni].name, perf.ttft_ms,
				perf.tok_per_sec, perf.kv_bytes, perf.peak_rss_kb / 1024);
			if (out != NULL)
				fprintf(out, "{\"record\":\"native_perf\",\"model\":\"%s\","
					"\"config\":\"%s\",\"prompt\":\"%s\",\"ttft_ms\":%.3f,"
					"\"tok_per_sec\":%.3f,\"kv_bytes\":%zu,"
					"\"peak_rss_kb\":%ld}\n", o.model_name, native[ni].name,
					valid[0].name.c_str(), perf.ttft_ms, perf.tok_per_sec,
					perf.kv_bytes, perf.peak_rss_kb);
			ni++;
		}
	}
	if (!run_pareto(model, vocab, o, valid, valid_classes, q8_ref.recall_ok,
			&pareto))
		return (false);
	emit_pareto_json(out, o.model_name, pareto, valid[0].idx.n_layer);
	if (!run_robustness_check(model, vocab, o, valid, valid_classes,
			q8_ref.recall_ok, o.robustness_budget))
		return (false);
	if (o.skip_legacy_comparison)
		fprintf(stderr, "\n(--skip-legacy-comparison: Phase 3.3/3.4 "
			"per-layer/per-age sweeps and their policies are not "
			"re-derived or reported for this model -- item 3 of Phase "
			"3.6 only requires FP16/Q8/Q4/conservative/balanced/"
			"aggressive)\n");
	else
	{
		phase34_policy = phase34_reused_policy(valid[0].idx.n_layer);
		if (!run_layer_sweep(model, vocab, o.n_tokens, o.gen_tokens, valid[0],
				g_default_thresholds, &layer_results))
			return (false);
		if (!run_age_sweep(model, vocab, o.n_tokens, o.gen_tokens, valid[0],
				g_default_thresholds, &age_results))
			return (false);
		layer_bits.resize(layer_results.size());
		for (i = 0; i < layer_results.size(); i++)
			layer_bits[i] = layer_results[i].classification;
		age_override = !age_results.empty()
			&& age_results[0].classification == 4;
	}
	if (o.export_policy_path != NULL)
	{
		const pareto_point_t	*balanced;
		size_t					pi;

		balanced = NULL;
		pi = 0;
		while (pi < pareto.size())
		{
			if (strcmp(pareto[pi].name, "balanced") == 0)
				balanced = &pareto[pi];
			pi++;
		}
		if (balanced == NULL)
			fprintf(stderr, "\n(--export-policy: no 'balanced' Pareto "
				"point found, nothing exported)\n");
		else
		{
			normalized_policy_t	np = normalize_policy(balanced->opt.policy,
					valid[0].idx.n_layer);

			if (!save_normalized_policy(o.export_policy_path, np))
				fprintf(stderr, "\n(--export-policy: failed to write "
					"'%s')\n", o.export_policy_path);
			else
				fprintf(stderr, "\n(--export-policy: wrote normalized "
					"balanced policy -- %zu K fractions, %zu V fractions "
					"-- to '%s')\n", np.k_fractions.size(),
					np.v_fractions.size(), o.export_policy_path);
		}
	}
	if (o.transfer_from_path != NULL)
		if (!run_transfer_policy_check(model, vocab, o, valid, valid_classes,
				q8_ref.recall_ok, pareto, out))
			return (false);
	fprintf(stderr, "\n=== Final 6-way comparison across %zu prompts ===\n",
		all_bases.size());
	i = 0;
	while (i < all_bases.size())
	{
		if (!run_risk_comparison(model, vocab, o.n_tokens, o.gen_tokens,
				all_bases[i], layer_bits, age_override, phase34_policy,
				pareto, out, is_valid_flags[i], o.skip_legacy_comparison))
			return (false);
		i++;
	}
	fprintf(stderr, "\npolicy decision overhead (this whole run, wall "
		"clock): %.1fs\n", std::chrono::duration<double>(
			std::chrono::steady_clock::now() - t0).count());
	return (true);
}

static bool	run_sensitivity_mode(llama_model *model, const llama_vocab *vocab,
				const opts_t &o, FILE *out)
{
	std::vector<baseline_t>			bases;
	baseline_t							prof;
	std::vector<layer_result_t>		layer_results;
	std::vector<age_result_t>			age_results;
	std::vector<int>					layer_bits;
	bool								age_override;
	size_t								i;

	if (!capture_baseline(model, vocab, o.n_tokens, o.gen_tokens,
			o.prompts[(size_t)o.profiler_index].path,
			o.prompts[(size_t)o.profiler_index].answer, &prof))
		return (false);
	fprintf(stderr, "profiler prompt: %s (%u tokens, %u KV cells)\n",
		o.prompts[(size_t)o.profiler_index].path,
		(unsigned)prof.prompt_tokens.size(), prof.idx.cell_count);
	if (!self_test(model, vocab, o.n_tokens, o.gen_tokens, prof))
		return (die("self-test failed -- aborting, results would not be "
				"trustworthy"), false);
	if (!run_layer_sweep(model, vocab, o.n_tokens, o.gen_tokens, prof,
			g_default_thresholds, &layer_results))
		return (false);
	if (!run_age_sweep(model, vocab, o.n_tokens, o.gen_tokens, prof,
			g_default_thresholds, &age_results))
		return (false);
	layer_bits.resize(layer_results.size());
	for (i = 0; i < layer_results.size(); i++)
		layer_bits[i] = layer_results[i].classification;
	age_override = !age_results.empty() && age_results[0].classification == 4;
	fprintf(stderr, "\ntoken-age oldest-25%% Q4 override %s\n",
		age_override ? "ENABLED (oldest band cleared Q4 thresholds)"
			: "disabled (oldest band did not clear Q4 thresholds)");
	if (out != NULL)
	{
		fprintf(out, "{\"record\":\"policy\",\"layer_bits\":[");
		for (i = 0; i < layer_bits.size(); i++)
			fprintf(out, "%s%d", i ? "," : "", layer_bits[i]);
		fprintf(out, "],\"age_override\":%s}\n",
			age_override ? "true" : "false");
	}
	fprintf(stderr, "\n=== Comparison table across %zu prompts ===\n",
		o.prompts.size());
	for (i = 0; i < o.prompts.size(); i++)
	{
		baseline_t	b;
		if (!capture_baseline(model, vocab, o.n_tokens, o.gen_tokens,
				o.prompts[i].path, o.prompts[i].answer, &b))
			continue ;
		fprintf(stderr, "\nprompt file: %s\n", o.prompts[i].path);
		run_comparison_table(model, vocab, o.n_tokens, o.gen_tokens, b,
			layer_bits, age_override, o.prompts[i].path, out);
		if (i < 2)
			run_kv_combo_sweep(model, vocab, o.n_tokens, o.gen_tokens, b,
				o.prompts[i].path);
		bases.push_back(std::move(b));
	}
	return (true);
}

int	main(int argc, char **argv)
{
	opts_t				o;
	llama_model			*model;
	const llama_vocab	*vocab;
	FILE				*out;
	bool				ok;

	if (parse_args(argc, argv, &o) != 0)
		return (2);
	llama_backend_init();
	model = llama_model_load_from_file(o.model_path,
			llama_model_default_params());
	if (model == NULL)
		return (die("model load failed"), 2);
	vocab = llama_model_get_vocab(model);
	out = o.out_path != NULL ? fopen(o.out_path, "w") : NULL;
	if (strcmp(o.mode, "risk") == 0)
		ok = run_risk_aware_mode(model, vocab, o, out);
	else if (strcmp(o.mode, "optimize") == 0)
		ok = run_optimizer_mode(model, vocab, o, out);
	else
		ok = run_sensitivity_mode(model, vocab, o, out);
	fprintf(stderr, "\npeak RSS (whole process): %ld MB\n",
		peak_rss_kb() / 1024);
	if (out != NULL)
		fclose(out);
	llama_model_free(model);
	llama_backend_free();
	return (ok ? 0 : 1);
}
