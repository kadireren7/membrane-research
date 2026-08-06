/*
 * membrane-kv-quality: live model-quality validation of KV-cache
 * quantization (Phase 3.2).
 *
 * IMPORTANT SCOPE NOTE, established empirically while building this tool:
 * ggml's quantized KV cache types (Q8_0, Q4_0, Q5_0, Q5_1, Q4_1, IQ4_NL)
 * all use a FIXED block size of 32 -- baked into the on-disk/in-memory
 * format itself, not a runtime parameter. llama.cpp has no notion of
 * "Q8_0 with group_elems=128" or "affine int8 KV cache"; those are
 * properties of MEMBRANE's own codec (Phase 3.1,
 * src/codecs/q8block.c), which is not wired into any inference runtime.
 * So only ONE live config actually exists to test this way: GGML_TYPE_Q8_0
 * (block-32, symmetric) -- the live analogue of MEMBRANE's
 * "symmetric, group_elems=32" config specifically. The other three swept
 * configs from Phase 3.1 (symmetric/64, symmetric/128, symmetric/256,
 * affine/128) have no live equivalent in llama.cpp and are NOT
 * re-validated here; their numbers remain the Phase 3.1 offline
 * measurements on real captured KV tensors, cited in
 * docs/phase3-kv-q8-quality.md. This tool also runs GGML_TYPE_Q4_0 as a
 * bonus second live data point (also block-32, but 4-bit instead of
 * 8-bit) since it costs nothing extra in this design and gives one more
 * real quality/memory tradeoff point beyond what was strictly asked.
 *
 * Methodology per (prompt, live type, run): three decode passes, as in
 * Phase 3.1 --
 *   1. F16 baseline: greedy-decode gen_tokens steps, recording every
 *      chosen token, its full logit vector, and prefill (TTFT) time.
 *   2. Quantized, free-running: greedy-decode independently from the same
 *      prompt, to see whether generated TEXT diverges over time; the
 *      first position where its own greedy choice differs from the
 *      baseline's is recorded.
 *   3. Quantized, teacher-forced on pass 1's exact tokens: its logits are
 *      then directly comparable to pass 1's at matched positions,
 *      isolating the quantization effect from any confound of the two
 *      passes having walked different token sequences.
 * Every metric is collected over >= 10 repeated runs per (prompt, type)
 * and reported as mean/min/max/stddev, since llama.cpp's multi-threaded
 * CPU reduction order can introduce tiny run-to-run floating-point
 * nondeterminism even under greedy (seed-free) decoding -- that variance
 * is itself worth measuring honestly, not assumed to be zero.
 */

#include <sys/resource.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include "llama.h"

typedef struct s_quality_opts
{
	const char					*model_path;
	std::vector<const char *>	prompt_paths;
	int							n_tokens;
	int							gen_tokens;
	int							runs;
	const char					*out_path;
}	quality_opts_t;

typedef struct s_prompt_entry
{
	std::string					name;
	std::vector<llama_token>	tokens;
}	prompt_entry_t;

typedef struct s_live_type
{
	const char	*name;
	ggml_type	type;
}	live_type_t;

static const live_type_t	g_live_types[] = {
	{"q8_0", GGML_TYPE_Q8_0},
	{"q4_0", GGML_TYPE_Q4_0},
};
# define N_LIVE_TYPES 2

static int	die(const char *msg)
{
	fprintf(stderr, "membrane-kv-quality: %s\n", msg);
	return (-1);
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

static std::string	basename_no_ext(const char *path)
{
	std::string	s(path);
	size_t		slash;
	size_t		dot;

	slash = s.find_last_of('/');
	if (slash != std::string::npos)
		s = s.substr(slash + 1);
	dot = s.find_last_of('.');
	if (dot != std::string::npos)
		s = s.substr(0, dot);
	return (s);
}

static bool	tokenize_prompt(const llama_vocab *vocab, const char *path,
				prompt_entry_t *out)
{
	std::string	text;
	int			n;

	text = read_file(path);
	if (text.empty())
		return (die("empty or unreadable prompt file"), false);
	out->name = basename_no_ext(path);
	out->tokens.resize(text.size() + 8);
	n = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(),
			out->tokens.data(), (int32_t)out->tokens.size(), true, false);
	if (n < 0)
		return (die("tokenization failed"), false);
	out->tokens.resize(n);
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

typedef struct s_pass_result
{
	std::vector<llama_token>			tokens;
	std::vector<std::vector<float>>	logits;
	double								ttft_ms;
	double								tokens_per_sec;
	size_t								kv_state_bytes;
}	pass_result_t;

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

/*
 * Greedy-decodes gen_steps tokens. If `forced` is non-NULL, the token fed
 * at each step comes from it (teacher forcing) instead of this context's
 * own argmax; the logits recorded are always this context's own.
 */
static bool	run_pass(llama_context *ctx, const llama_vocab *vocab,
				const std::vector<llama_token> &prompt, int gen_steps,
				const std::vector<llama_token> *forced, pass_result_t *out)
{
	llama_token	tok;
	const float	*logits;
	int32_t		n_vocab;
	int			i;
	std::chrono::steady_clock::time_point	t0;
	std::chrono::steady_clock::time_point	t1;
	std::chrono::steady_clock::time_point	t2;

	t0 = std::chrono::steady_clock::now();
	if (!decode_prompt(ctx, prompt, 256))
		return (false);
	t1 = std::chrono::steady_clock::now();
	out->ttft_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	/* Phase 4.4 (docs/phase4-ggml-quant-parity.md item 7): measured
	 * right after the prompt decode, before any generated tokens grow
	 * the cache further -- matching membrane-kv-runtime,
	 * membrane-kv-runtime-optimizer, and membrane-kv-variance's shared
	 * convention (all three measure kv_bytes at this same point). This
	 * tool used to measure it after the full prompt+generation decode
	 * instead, which is why the SAME model/prompt/policy previously
	 * reported a different KV-byte figure here than in those tools;
	 * see docs/phase4-ggml-quant-parity.md for the real, measured
	 * before/after comparison. */
	out->kv_state_bytes = llama_state_seq_get_size(ctx, 0);
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
	t2 = std::chrono::steady_clock::now();
	out->tokens_per_sec = (double)gen_steps
		/ std::chrono::duration<double>(t2 - t1).count();
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

static long	peak_rss_kb(void)
{
	struct rusage	ru;

	getrusage(RUSAGE_SELF, &ru);
	return (ru.ru_maxrss);
}

/* Running mean/min/max/stddev over repeated-run values for one metric. */
typedef struct s_stat_acc
{
	double	sum;
	double	sumsq;
	double	mn;
	double	mx;
	int		n;
}	stat_acc_t;

static void	stat_init(stat_acc_t *a)
{
	a->sum = 0.0;
	a->sumsq = 0.0;
	a->mn = 1e300;
	a->mx = -1e300;
	a->n = 0;
}

static void	stat_add(stat_acc_t *a, double v)
{
	a->sum += v;
	a->sumsq += v * v;
	if (v < a->mn)
		a->mn = v;
	if (v > a->mx)
		a->mx = v;
	a->n += 1;
}

static double	stat_mean(const stat_acc_t *a)
{
	return (a->n ? a->sum / a->n : 0.0);
}

static double	stat_stddev(const stat_acc_t *a)
{
	double	m;
	double	var;

	if (a->n < 2)
		return (0.0);
	m = stat_mean(a);
	var = a->sumsq / a->n - m * m;
	return (var > 0.0 ? sqrt(var) : 0.0);
}

typedef struct s_agg
{
	stat_acc_t	top1_pct;
	stat_acc_t	top5_pct;
	stat_acc_t	logit_cosine;
	stat_acc_t	logit_rmse;
	stat_acc_t	kl_mean;
	stat_acc_t	first_divergence;	/* gen_steps if never diverged */
	stat_acc_t	ttft_ms_base;
	stat_acc_t	ttft_ms_q8;
	stat_acc_t	toks_base;
	stat_acc_t	toks_q8;
	stat_acc_t	kv_bytes_base;
	stat_acc_t	kv_bytes_q8;
	std::string	sample_base_text;
	std::string	sample_q8_text;
}	agg_t;

static void	agg_init(agg_t *a)
{
	stat_init(&a->top1_pct);
	stat_init(&a->top5_pct);
	stat_init(&a->logit_cosine);
	stat_init(&a->logit_rmse);
	stat_init(&a->kl_mean);
	stat_init(&a->first_divergence);
	stat_init(&a->ttft_ms_base);
	stat_init(&a->ttft_ms_q8);
	stat_init(&a->toks_base);
	stat_init(&a->toks_q8);
	stat_init(&a->kv_bytes_base);
	stat_init(&a->kv_bytes_q8);
}

/* True if `token` is among the 5 highest logits in `logits`. */
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

/* One matched-position comparison step: raw-logit cosine/RMSE, softmax KL
 * divergence, and top-1/top-5 agreement against the baseline's choice. */
static void	compare_step(const std::vector<float> &base,
				const std::vector<float> &q8, double *cos_sum,
				double *sumsq, uint64_t *n_elems, double *kl_sum,
				uint64_t *top1, uint64_t *top5)
{
	double	dot;
	double	na;
	double	nb;
	double	diff;
	double	mx;
	double	sum_pb;
	double	sum_pq;
	std::vector<double>	pb;
	std::vector<double>	pq;
	size_t	i;
	int		base_top1;

	dot = 0.0;
	na = 0.0;
	nb = 0.0;
	for (i = 0; i < base.size(); i++)
	{
		dot += (double)base[i] * (double)q8[i];
		na += (double)base[i] * (double)base[i];
		nb += (double)q8[i] * (double)q8[i];
		diff = (double)base[i] - (double)q8[i];
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
	pq.resize(base.size());
	sum_pb = 0.0;
	sum_pq = 0.0;
	for (i = 0; i < base.size(); i++)
	{
		pb[i] = exp((double)base[i] - mx);
		pq[i] = exp((double)q8[i] - mx);
		sum_pb += pb[i];
		sum_pq += pq[i];
	}
	for (i = 0; i < base.size(); i++)
	{
		pb[i] /= sum_pb;
		pq[i] /= sum_pq;
		if (pb[i] > 0.0)
			*kl_sum += pb[i] * log(pb[i] / (pq[i] > 1e-300 ? pq[i] : 1e-300));
	}
	base_top1 = argmax(base.data(), (int)base.size());
	*top1 += (base_top1 == argmax(q8.data(), (int)q8.size()));
	*top5 += in_top5(q8, base_top1);
}

/* One (prompt, live-type) run: baseline + quantized free-running +
 * quantized teacher-forced, folded into agg's running statistics. */
static bool	run_once(llama_model *model, const prompt_entry_t &prompt,
				ggml_type qtype, int n_ctx, int gen_tokens, bool keep_text,
				agg_t *agg)
{
	llama_context	*base_ctx;
	llama_context	*q8_ctx;
	llama_context	*q8_forced_ctx;
	const llama_vocab	*vocab;
	pass_result_t	base;
	pass_result_t	q8_free;
	pass_result_t	q8_forced;
	double			cos_sum;
	double			sumsq;
	uint64_t		n_elems;
	double			kl_sum;
	uint64_t		top1;
	uint64_t		top5;
	size_t			i;
	size_t			steps;
	long			first_div;
	bool			ok;

	vocab = llama_model_get_vocab(model);
	base_ctx = make_context(model, n_ctx, GGML_TYPE_F16, GGML_TYPE_F16);
	q8_ctx = make_context(model, n_ctx, qtype, qtype);
	q8_forced_ctx = make_context(model, n_ctx, qtype, qtype);
	ok = base_ctx != NULL && q8_ctx != NULL && q8_forced_ctx != NULL;
	if (ok)
		ok = run_pass(base_ctx, vocab, prompt.tokens, gen_tokens, NULL, &base);
	if (ok)
		ok = run_pass(q8_ctx, vocab, prompt.tokens, gen_tokens, NULL, &q8_free);
	if (ok)
		ok = run_pass(q8_forced_ctx, vocab, prompt.tokens, gen_tokens,
				&base.tokens, &q8_forced);
	if (ok)
	{
		cos_sum = 0.0;
		sumsq = 0.0;
		n_elems = 0;
		kl_sum = 0.0;
		top1 = 0;
		top5 = 0;
		steps = base.logits.size() < q8_forced.logits.size()
			? base.logits.size() : q8_forced.logits.size();
		for (i = 0; i < steps; i++)
			compare_step(base.logits[i], q8_forced.logits[i], &cos_sum,
				&sumsq, &n_elems, &kl_sum, &top1, &top5);
		first_div = (long)gen_tokens;
		for (i = 0; i < base.tokens.size() && i < q8_free.tokens.size(); i++)
			if (base.tokens[i] != q8_free.tokens[i])
			{
				first_div = (long)i;
				break ;
			}
		stat_add(&agg->top1_pct, steps ? 100.0 * (double)top1 / (double)steps
				: 0.0);
		stat_add(&agg->top5_pct, steps ? 100.0 * (double)top5 / (double)steps
				: 0.0);
		stat_add(&agg->logit_cosine, steps ? cos_sum / (double)steps : 0.0);
		stat_add(&agg->logit_rmse, n_elems ? sqrt(sumsq / (double)n_elems)
				: 0.0);
		stat_add(&agg->kl_mean, steps ? kl_sum / (double)steps : 0.0);
		stat_add(&agg->first_divergence, (double)first_div);
		stat_add(&agg->ttft_ms_base, base.ttft_ms);
		stat_add(&agg->ttft_ms_q8, q8_free.ttft_ms);
		stat_add(&agg->toks_base, base.tokens_per_sec);
		stat_add(&agg->toks_q8, q8_free.tokens_per_sec);
		stat_add(&agg->kv_bytes_base, (double)base.kv_state_bytes);
		stat_add(&agg->kv_bytes_q8, (double)q8_free.kv_state_bytes);
		if (keep_text)
		{
			agg->sample_base_text = tokens_to_text(vocab, base.tokens);
			agg->sample_q8_text = tokens_to_text(vocab, q8_free.tokens);
		}
	}
	llama_free(base_ctx);
	llama_free(q8_ctx);
	llama_free(q8_forced_ctx);
	return (ok);
}

static void	print_stat_line(const char *label, const stat_acc_t *a,
				const char *unit)
{
	fprintf(stderr, "    %-22s mean %10.4f  min %10.4f  max %10.4f  "
		"stddev %10.4f  %s\n", label, stat_mean(a), a->n ? a->mn : 0.0,
		a->n ? a->mx : 0.0, stat_stddev(a), unit);
}

static void	print_agg(const char *prompt_name, const char *type_name,
				int runs, int gen_tokens, const agg_t *a)
{
	fprintf(stderr, "\n[%s | %s | %d runs]\n", prompt_name, type_name, runs);
	print_stat_line("top1_agreement", &a->top1_pct, "%");
	print_stat_line("top5_agreement", &a->top5_pct, "%");
	print_stat_line("logit_cosine", &a->logit_cosine, "");
	print_stat_line("logit_rmse", &a->logit_rmse, "");
	print_stat_line("kl_divergence", &a->kl_mean, "nats");
	print_stat_line("first_divergence_pos", &a->first_divergence,
		gen_tokens == (int)a->first_divergence.mx ? "(gen_tokens = never)"
			: "tokens");
	print_stat_line("ttft_baseline", &a->ttft_ms_base, "ms");
	print_stat_line("ttft_quantized", &a->ttft_ms_q8, "ms");
	print_stat_line("tok/s_baseline", &a->toks_base, "tok/s");
	print_stat_line("tok/s_quantized", &a->toks_q8, "tok/s");
	print_stat_line("kv_bytes_baseline", &a->kv_bytes_base, "bytes");
	print_stat_line("kv_bytes_quantized", &a->kv_bytes_q8, "bytes");
	fprintf(stderr, "    KV memory reduction: %.3fx (%.1f%% smaller)\n",
		stat_mean(&a->kv_bytes_base) / stat_mean(&a->kv_bytes_q8),
		100.0 * (1.0 - stat_mean(&a->kv_bytes_q8)
			/ stat_mean(&a->kv_bytes_base)));
	fprintf(stderr, "    speed penalty: %.3fx (quantized tok/s / baseline "
		"tok/s)\n", stat_mean(&a->toks_q8) / stat_mean(&a->toks_base));
	fprintf(stderr, "    sample baseline text: %s\n",
		a->sample_base_text.c_str());
	fprintf(stderr, "    sample quantized text: %s\n",
		a->sample_q8_text.c_str());
}

static void	emit_json_line(FILE *f, const char *prompt_name,
				const char *type_name, int runs, const agg_t *a)
{
	fprintf(f, "{\"prompt\":\"%s\",\"type\":\"%s\",\"runs\":%d,",
		prompt_name, type_name, runs);
	fprintf(f, "\"top1_pct\":{\"mean\":%.6f,\"min\":%.6f,\"max\":%.6f,"
		"\"stddev\":%.6f},", stat_mean(&a->top1_pct), a->top1_pct.mn,
		a->top1_pct.mx, stat_stddev(&a->top1_pct));
	fprintf(f, "\"top5_pct\":{\"mean\":%.6f,\"min\":%.6f,\"max\":%.6f,"
		"\"stddev\":%.6f},", stat_mean(&a->top5_pct), a->top5_pct.mn,
		a->top5_pct.mx, stat_stddev(&a->top5_pct));
	fprintf(f, "\"logit_cosine\":{\"mean\":%.6f,\"min\":%.6f,\"max\":%.6f,"
		"\"stddev\":%.6f},", stat_mean(&a->logit_cosine), a->logit_cosine.mn,
		a->logit_cosine.mx, stat_stddev(&a->logit_cosine));
	fprintf(f, "\"logit_rmse\":{\"mean\":%.6f,\"min\":%.6f,\"max\":%.6f,"
		"\"stddev\":%.6f},", stat_mean(&a->logit_rmse), a->logit_rmse.mn,
		a->logit_rmse.mx, stat_stddev(&a->logit_rmse));
	fprintf(f, "\"kl_divergence\":{\"mean\":%.6f,\"min\":%.6f,\"max\":%.6f,"
		"\"stddev\":%.6f},", stat_mean(&a->kl_mean), a->kl_mean.mn,
		a->kl_mean.mx, stat_stddev(&a->kl_mean));
	fprintf(f, "\"first_divergence\":{\"mean\":%.6f,\"min\":%.6f,"
		"\"max\":%.6f,\"stddev\":%.6f},", stat_mean(&a->first_divergence),
		a->first_divergence.mn, a->first_divergence.mx,
		stat_stddev(&a->first_divergence));
	fprintf(f, "\"ttft_ms_baseline\":{\"mean\":%.6f,\"stddev\":%.6f},",
		stat_mean(&a->ttft_ms_base), stat_stddev(&a->ttft_ms_base));
	fprintf(f, "\"ttft_ms_quantized\":{\"mean\":%.6f,\"stddev\":%.6f},",
		stat_mean(&a->ttft_ms_q8), stat_stddev(&a->ttft_ms_q8));
	fprintf(f, "\"toks_baseline\":{\"mean\":%.6f,\"stddev\":%.6f},",
		stat_mean(&a->toks_base), stat_stddev(&a->toks_base));
	fprintf(f, "\"toks_quantized\":{\"mean\":%.6f,\"stddev\":%.6f},",
		stat_mean(&a->toks_q8), stat_stddev(&a->toks_q8));
	fprintf(f, "\"kv_bytes_baseline\":{\"mean\":%.6f},",
		stat_mean(&a->kv_bytes_base));
	fprintf(f, "\"kv_bytes_quantized\":{\"mean\":%.6f},",
		stat_mean(&a->kv_bytes_q8));
	fprintf(f, "\"kv_reduction_x\":%.6f}\n",
		stat_mean(&a->kv_bytes_base) / stat_mean(&a->kv_bytes_q8));
}

static int	parse_args(int argc, char **argv, quality_opts_t *o)
{
	int	i;

	o->model_path = NULL;
	o->prompt_paths.clear();
	o->n_tokens = 1024;
	o->gen_tokens = 32;
	o->runs = 10;
	o->out_path = NULL;
	i = 1;
	while (i + 1 < argc)
	{
		if (strcmp(argv[i], "--model") == 0)
			o->model_path = argv[i + 1];
		else if (strcmp(argv[i], "--prompt-file") == 0)
			o->prompt_paths.push_back(argv[i + 1]);
		else if (strcmp(argv[i], "--n-tokens") == 0)
			o->n_tokens = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "--gen-tokens") == 0)
			o->gen_tokens = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "--runs") == 0)
			o->runs = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "--out") == 0)
			o->out_path = argv[i + 1];
		else
			return (die("unknown option"));
		i += 2;
	}
	if (o->model_path == NULL || o->prompt_paths.empty() || o->runs < 1)
		return (die("usage: --model M --prompt-file P [--prompt-file P]... "
				"[--n-tokens N] [--gen-tokens G] [--runs R] [--out OUT.jsonl]"));
	return (0);
}

int	main(int argc, char **argv)
{
	quality_opts_t	o;
	llama_model		*model;
	const llama_vocab	*vocab;
	FILE			*out;
	prompt_entry_t	prompt;
	agg_t			agg;
	int				ti;
	size_t			pi;
	int				run;
	bool			ok;

	if (parse_args(argc, argv, &o) != 0)
		return (2);
	llama_backend_init();
	model = llama_model_load_from_file(o.model_path,
			llama_model_default_params());
	if (model == NULL)
		return (die("model load failed"), 2);
	vocab = llama_model_get_vocab(model);
	out = NULL;
	if (o.out_path != NULL)
		out = fopen(o.out_path, "w");
	fprintf(stderr, "model=%s  n_ctx=%d  gen_tokens=%d  runs=%d\n",
		o.model_path, o.n_tokens, o.gen_tokens, o.runs);
	for (pi = 0; pi < o.prompt_paths.size(); pi++)
	{
		if (!tokenize_prompt(vocab, o.prompt_paths[pi], &prompt))
			continue ;
		fprintf(stderr, "\n=== prompt: %s (%zu tokens) ===\n", prompt.name.c_str(),
			prompt.tokens.size());
		for (ti = 0; ti < N_LIVE_TYPES; ti++)
		{
			agg_init(&agg);
			run = 0;
			while (run < o.runs)
			{
				ok = run_once(model, prompt, g_live_types[ti].type,
						o.n_tokens, o.gen_tokens, run == 0, &agg);
				if (!ok)
					break ;
				run++;
			}
			if (run == 0)
			{
				fprintf(stderr, "  [%s] all runs failed, skipping\n",
					g_live_types[ti].name);
				continue ;
			}
			print_agg(prompt.name.c_str(), g_live_types[ti].name, run,
				o.gen_tokens, &agg);
			if (out != NULL)
				emit_json_line(out, prompt.name.c_str(),
					g_live_types[ti].name, run, &agg);
		}
	}
	fprintf(stderr, "\npeak RSS (whole process): %ld MB\n",
		peak_rss_kb() / 1024);
	if (out != NULL)
		fclose(out);
	llama_model_free(model);
	llama_backend_free();
	return (0);
}
