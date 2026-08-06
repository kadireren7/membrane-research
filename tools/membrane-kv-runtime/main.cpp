/*
 * Phase 4.1: the real runtime path. Everything in Phase 3 (blob-splicing,
 * membrane-kv-sensitivity) is offline research tooling -- it captures a
 * KV-cache state blob, numerically perturbs specific byte ranges to
 * SIMULATE quantization, and reloads it; the underlying ggml tensors stay
 * F16 the whole time, so it can never measure real speed or real memory.
 * This tool does the opposite: it loads a membrane_policy and builds an
 * ACTUAL llama_context whose per-layer K/V ggml tensors are allocated at
 * the policy's chosen precision (via the kv_type_override patch, see
 * patches/llama.cpp-membrane-kv-type-override.patch), so every number
 * measured here -- KV bytes, tok/s, TTFT -- is real, not simulated.
 */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/resource.h>
#include <vector>

#include "llama.h"

#include "membrane/hash.h"
#include "membrane/llama_commit.h"
#include "membrane/policy.h"
#include "membrane/quant_backend.h"

static int	die(const char *msg)
{
	fprintf(stderr, "membrane-kv-runtime: %s\n", msg);
	return (0);
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

/*
 * Bundles the policy pointer with lookup-overhead accounting so the
 * measurement lives in a stack-allocated struct threaded through
 * kv_type_override_ud, not a global -- membrane_policy_query itself
 * already takes no global state either (see membrane/policy.h).
 */
typedef struct s_policy_lookup_ctx
{
	const membrane_policy_t	*policy;
	uint64_t					calls;
	double						total_ns;
}	policy_lookup_ctx_t;

static ggml_type	precision_to_ggml(membrane_precision_t p)
{
	if (p == MEMBRANE_PRECISION_Q8)
		return (GGML_TYPE_Q8_0);
	if (p == MEMBRANE_PRECISION_Q4)
		return (GGML_TYPE_Q4_0);
	return (GGML_TYPE_F16);
}

static ggml_type	policy_type_cb(int32_t il, bool is_v, void *user_data)
{
	policy_lookup_ctx_t					*pc;
	membrane_precision_t					prec;
	std::chrono::steady_clock::time_point	t0;
	std::chrono::steady_clock::time_point	t1;
	membrane_status_t						st;

	pc = (policy_lookup_ctx_t *)user_data;
	t0 = std::chrono::steady_clock::now();
	st = membrane_policy_query(pc->policy, (uint32_t)il, is_v ? 1 : 0, &prec);
	t1 = std::chrono::steady_clock::now();
	pc->calls++;
	pc->total_ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
	if (st != MEMBRANE_OK)
		return (GGML_TYPE_F16);
	return (precision_to_ggml(prec));
}

static llama_context	*make_context(llama_model *model, int n_ctx,
						ggml_type type_k, ggml_type type_v,
						ggml_type (*override_cb)(int32_t, bool, void *),
						void *override_ud)
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
				double *kl_sum, uint64_t *top1, uint64_t *top5)
{
	double	dot;
	double	na;
	double	nb;
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
	}
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

typedef struct s_metrics
{
	double	top1_pct;
	double	top5_pct;
	double	logit_cosine;
	double	kl_mean;
	long	first_divergence;
	bool	recall_ok;
	std::string	text;
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

	cos_sum = 0.0;
	kl_sum = 0.0;
	top1 = 0;
	top5 = 0;
	steps = ref_free.logits.size() < forced_run.logits.size()
		? ref_free.logits.size() : forced_run.logits.size();
	for (i = 0; i < steps; i++)
		compare_step(ref_free.logits[i], forced_run.logits[i], &cos_sum,
			&kl_sum, &top1, &top5);
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

static long	peak_rss_kb(void)
{
	struct rusage	ru;

	getrusage(RUSAGE_SELF, &ru);
	return (ru.ru_maxrss);
}

typedef struct s_run_result
{
	metrics_t	m;
	double		ttft_ms;
	double		tok_per_sec;
	size_t		kv_bytes;
	long		peak_rss_kb;
}	run_result_t;

/*
 * Runs ONE configuration through a REAL llama_context (no splicing): a
 * fresh prompt decode (timed for TTFT), then a free-running pass and a
 * teacher-forced pass, to fill quality metrics exactly like Phase 3's
 * run_kv_combo -- but every byte here is genuinely stored at the
 * configured precision, not numerically perturbed after the fact.
 * For the FP16 reference call itself (is_reference=true), the forced
 * pass replays the free pass's OWN tokens (comparing FP16 against
 * itself is the self-test: expected to come out perfect) and `ref_free`
 * is ignored. Every other call forces against and compares to the
 * reference's free-run pass, passed in as `ref_free`. Always fills
 * `*out_free_run` with this call's own free-run pass so the caller can
 * use it as `ref_free` for later calls.
 */
static bool	run_config(llama_model *model, const llama_vocab *vocab,
				int n_ctx, int gen_tokens,
				const std::vector<llama_token> &prompt_tokens,
				const char *answer, ggml_type type_k, ggml_type type_v,
				ggml_type (*override_cb)(int32_t, bool, void *),
				void *override_ud, const pass_result_t &ref_free,
				bool is_reference, run_result_t *out,
				pass_result_t *out_free_run)
{
	llama_context							*free_ctx;
	llama_context							*forced_ctx;
	pass_result_t							forced_run;
	const std::vector<llama_token>			*force_against;
	std::chrono::steady_clock::time_point	t0;
	std::chrono::steady_clock::time_point	t_first;
	std::chrono::steady_clock::time_point	t_gen_done;
	bool									ok;

	free_ctx = make_context(model, n_ctx, type_k, type_v, override_cb,
			override_ud);
	forced_ctx = make_context(model, n_ctx, type_k, type_v, override_cb,
			override_ud);
	ok = free_ctx != NULL && forced_ctx != NULL;
	if (ok)
	{
		t0 = std::chrono::steady_clock::now();
		ok = decode_prompt(free_ctx, prompt_tokens, 256);
	}
	if (ok)
	{
		out->kv_bytes = llama_state_seq_get_size(free_ctx, 0);
		ok = run_gen(free_ctx, vocab, 1, NULL, out_free_run);
		t_first = std::chrono::steady_clock::now();
	}
	if (ok && gen_tokens > 1)
	{
		pass_result_t	rest;

		ok = run_gen(free_ctx, vocab, gen_tokens - 1, NULL, &rest);
		out_free_run->tokens.insert(out_free_run->tokens.end(),
			rest.tokens.begin(), rest.tokens.end());
		out_free_run->logits.insert(out_free_run->logits.end(),
			rest.logits.begin(), rest.logits.end());
	}
	t_gen_done = std::chrono::steady_clock::now();
	force_against = is_reference ? &out_free_run->tokens : &ref_free.tokens;
	if (ok)
		ok = decode_prompt(forced_ctx, prompt_tokens, 256)
			&& run_gen(forced_ctx, vocab, gen_tokens, force_against,
				&forced_run);
	if (ok)
	{
		out->ttft_ms = std::chrono::duration<double, std::milli>(
				t_first - t0).count();
		out->tok_per_sec = gen_tokens > 1 ? (double)(gen_tokens - 1)
			/ std::chrono::duration<double>(t_gen_done - t_first).count()
			: 0.0;
		out->peak_rss_kb = peak_rss_kb();
		fill_metrics(vocab, gen_tokens, is_reference ? *out_free_run
				: ref_free, *out_free_run, forced_run, answer, &out->m);
	}
	llama_free(free_ctx);
	llama_free(forced_ctx);
	return (ok);
}

typedef struct s_opts
{
	const char	*model_path;
	const char	*policy_path;
	const char	*prompt_path;
	const char	*answer;
	int			n_tokens;
	int			gen_tokens;
	const char	*model_name;
	const char	*out_path;
	membrane_quant_backend_t	quant_backend;
}	opts_t;

static int	parse_args(int argc, char **argv, opts_t *o)
{
	int	i;

	memset(o, 0, sizeof(*o));
	o->n_tokens = 1024;
	o->gen_tokens = 32;
	o->model_name = "model";
	o->quant_backend = MEMBRANE_QUANT_BACKEND_CPU;
	i = 1;
	while (i < argc)
	{
		if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
			o->model_path = argv[++i];
		else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc)
			o->policy_path = argv[++i];
		else if (strcmp(argv[i], "--prompt") == 0 && i + 2 < argc)
		{
			o->prompt_path = argv[++i];
			o->answer = strcmp(argv[i + 1], "-") == 0 ? NULL : argv[i + 1];
			i++;
		}
		else if (strcmp(argv[i], "--n-tokens") == 0 && i + 1 < argc)
			o->n_tokens = atoi(argv[++i]);
		else if (strcmp(argv[i], "--gen-tokens") == 0 && i + 1 < argc)
			o->gen_tokens = atoi(argv[++i]);
		else if (strcmp(argv[i], "--model-name") == 0 && i + 1 < argc)
			o->model_name = argv[++i];
		else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
			o->out_path = argv[++i];
		else if (strcmp(argv[i], "--quant-backend") == 0 && i + 1 < argc)
		{
			const char	*v = argv[++i];

			/*
			 * Additive-only flag (Phase 5.4 task 116): default "cpu"
			 * reproduces this tool's exact pre-existing behavior --
			 * K/V tensors are quantized by ggml's own internal Q8_0/
			 * Q4_0 kernels via the kv_type_override mechanism, same
			 * as before this flag existed. "fpga"/"auto" do NOT
			 * redirect that live, in-inference quantization (ggml's
			 * internal dispatch is not patched, see this flag's
			 * report_backend_choice() call site for why) -- they
			 * only affect the ADDITIONAL "quant_backend" field this
			 * tool now reports, resolved via the same
			 * membrane_choose_quant_backend() this phase's
			 * membrane-fpga-runtime tool uses for its own AUTO
			 * decisions (membrane/quant_backend.h), so the choice
			 * that would apply to a batch OFFLINE quantize/dequantize
			 * call (as opposed to live inference) is genuinely
			 * computed, not just echoed back.
			 */
			if (strcmp(v, "cpu") == 0)
				o->quant_backend = MEMBRANE_QUANT_BACKEND_CPU;
			else if (strcmp(v, "fpga") == 0)
				o->quant_backend = MEMBRANE_QUANT_BACKEND_FPGA;
			else if (strcmp(v, "auto") == 0)
				o->quant_backend = MEMBRANE_QUANT_BACKEND_AUTO;
			else
				return (die("--quant-backend must be cpu|fpga|auto"), 1);
		}
		else
			return (die("unknown or malformed option"), 1);
		i++;
	}
	if (o->model_path == NULL || o->prompt_path == NULL)
		return (die("usage: --model PATH --prompt PATH ANSWER|- "
				"[--policy POLICY] [--n-tokens N] [--gen-tokens G] "
				"[--model-name LABEL] [--out JSONL] "
				"[--quant-backend cpu|fpga|auto]"), 1);
	return (0);
}

static void	print_and_emit(FILE *out, const char *model_name,
				const char *config, const run_result_t &r, double kv_ratio)
{
	fprintf(stderr, "  %-18s top1 %6.2f%%  top5 %6.2f%%  cosine %.6f  "
		"KL %.6f  recall %-4s  KV %10zu bytes (%.3fx)  TTFT %8.1fms  "
		"%6.1f tok/s  peakRSS %6ldMB\n", config, r.m.top1_pct, r.m.top5_pct,
		r.m.logit_cosine, r.m.kl_mean, r.m.recall_ok ? "OK" : "FAIL",
		r.kv_bytes, kv_ratio, r.ttft_ms, r.tok_per_sec,
		r.peak_rss_kb / 1024);
	if (out == NULL)
		return ;
	fprintf(out, "{\"record\":\"runtime_row\",\"model\":\"%s\","
		"\"config\":\"%s\",\"top1_pct\":%.6f,\"top5_pct\":%.6f,"
		"\"logit_cosine\":%.6f,\"kl_divergence\":%.6f,\"recall_ok\":%s,"
		"\"kv_bytes\":%zu,\"kv_reduction_x\":%.6f,\"ttft_ms\":%.3f,"
		"\"tok_per_sec\":%.3f,\"peak_rss_kb\":%ld,"
		"\"first_divergence\":%ld}\n", model_name, config, r.m.top1_pct,
		r.m.top5_pct, r.m.logit_cosine, r.m.kl_mean,
		r.m.recall_ok ? "true" : "false", r.kv_bytes, kv_ratio, r.ttft_ms,
		r.tok_per_sec, r.peak_rss_kb, r.m.first_divergence);
}

int	main(int argc, char **argv)
{
	opts_t						o;
	llama_model					*model;
	const llama_vocab			*vocab;
	std::vector<llama_token>	prompt_tokens;
	membrane_policy_t			*policy;
	policy_lookup_ctx_t			plc;
	char						model_hex[MEMBRANE_SHA256_HEX_LEN + 1];
	uint8_t						model_digest[MEMBRANE_SHA256_DIGEST_BYTES];
	membrane_policy_context_t	pctx;
	char						reason[256];
	run_result_t				ref;
	run_result_t				r_q8;
	run_result_t				r_q4;
	run_result_t				r_policy;
	pass_result_t				ref_free_run;
	pass_result_t				discard_free_run;
	FILE						*out;
	int							ok;

	if (parse_args(argc, argv, &o) != 0)
		return (1);
	{
		/*
		 * Phase 5.4 task 116: report which backend an OFFLINE batch
		 * quantize/dequantize call (not this run's own live
		 * inference, which always uses ggml's internal kernels, see
		 * --quant-backend's help text above) would resolve to for a
		 * representative batch -- computed via the same shared
		 * membrane_choose_quant_backend() this phase's
		 * membrane-fpga-runtime tool uses, not a placeholder.
		 * cpu_cores_available is approximated from the context's own
		 * n_threads (4, see make_context) since that's the thread
		 * budget this run itself is already using.
		 */
		membrane_quant_backend_t	resolved = membrane_choose_quant_backend(
			o.quant_backend, /* batch_blocks (representative) */ 64,
			/* fpga_queue_used */ 0, /* fpga_queue_depth */ 16,
			/* cpu_cores_available */ 4);
		const char					*names[3] = {"cpu", "fpga", "auto"};

		fprintf(stderr,
			"membrane-kv-runtime: quant_backend requested=%s resolved=%s "
			"(live in-inference K/V quantization always uses ggml's own "
			"CPU kernels -- see --quant-backend's source comment)\n",
			names[o.quant_backend], names[resolved]);
	}
	llama_backend_init();
	model = llama_model_load_from_file(o.model_path,
			llama_model_default_params());
	if (model == NULL)
		return (die("model load failed"), 1);
	vocab = llama_model_get_vocab(model);
	if (!tokenize_prompt(vocab, o.prompt_path, &prompt_tokens))
		return (llama_model_free(model), 1);
	policy = NULL;
	if (o.policy_path != NULL)
	{
		if (membrane_policy_load(o.policy_path, &policy) != MEMBRANE_OK)
			return (llama_model_free(model),
				die("membrane_policy_load failed -- refusing to run "
					"with an unreadable/corrupt policy"), 1);
		if (membrane_sha256_file(o.model_path, model_hex) != MEMBRANE_OK)
			return (llama_model_free(model), membrane_policy_destroy(policy),
				die("could not hash --model for policy validation"), 1);
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
		memcpy(pctx.model_sha256, model_digest, MEMBRANE_SHA256_DIGEST_BYTES);
		pctx.llama_cpp_commit = MEMBRANE_LLAMA_CPP_COMMIT;
		pctx.layer_count = (uint32_t)llama_model_n_layer(model);
		if (membrane_policy_validate(policy, &pctx, reason, sizeof(reason))
				!= MEMBRANE_OK)
		{
			fprintf(stderr, "membrane-kv-runtime: policy REJECTED: %s\n",
				reason);
			llama_model_free(model);
			membrane_policy_destroy(policy);
			return (1);
		}
		fprintf(stderr, "policy accepted: %u layers, model hash and "
			"llama.cpp commit match this build\n", pctx.layer_count);
	}
	out = o.out_path != NULL ? fopen(o.out_path, "a") : NULL;
	fprintf(stderr, "\n--- prompt: %s ---\n", o.prompt_path);
	ok = run_config(model, vocab, o.n_tokens, o.gen_tokens, prompt_tokens,
			o.answer, GGML_TYPE_F16, GGML_TYPE_F16, NULL, NULL,
			discard_free_run, true, &ref, &ref_free_run);
	if (ok)
	{
		print_and_emit(out, o.model_name, "all-FP16", ref, 1.0);
		ok = run_config(model, vocab, o.n_tokens, o.gen_tokens, prompt_tokens,
				o.answer, GGML_TYPE_Q8_0, GGML_TYPE_Q8_0, NULL, NULL,
				ref_free_run, false, &r_q8, &discard_free_run);
	}
	if (ok)
	{
		print_and_emit(out, o.model_name, "all-Q8",
			r_q8, (double)ref.kv_bytes / (double)r_q8.kv_bytes);
		ok = run_config(model, vocab, o.n_tokens, o.gen_tokens, prompt_tokens,
				o.answer, GGML_TYPE_Q4_0, GGML_TYPE_Q4_0, NULL, NULL,
				ref_free_run, false, &r_q4, &discard_free_run);
	}
	if (ok)
		print_and_emit(out, o.model_name, "all-Q4",
			r_q4, (double)ref.kv_bytes / (double)r_q4.kv_bytes);
	if (ok && policy != NULL)
	{
		plc.policy = policy;
		plc.calls = 0;
		plc.total_ns = 0.0;
		ok = run_config(model, vocab, o.n_tokens, o.gen_tokens, prompt_tokens,
				o.answer, GGML_TYPE_F16, GGML_TYPE_F16, policy_type_cb, &plc,
				ref_free_run, false, &r_policy, &discard_free_run);
		if (ok)
		{
			print_and_emit(out, o.model_name, "MEMBRANE-policy", r_policy,
				(double)ref.kv_bytes / (double)r_policy.kv_bytes);
			fprintf(stderr, "  policy lookup overhead: %llu calls, "
				"%.1fns total (%.2fns/call)\n",
				(unsigned long long)plc.calls, plc.total_ns,
				plc.calls ? plc.total_ns / (double)plc.calls : 0.0);
			if (out != NULL)
				fprintf(out, "{\"record\":\"policy_lookup_overhead\","
					"\"model\":\"%s\",\"calls\":%llu,\"total_ns\":%.1f,"
					"\"ns_per_call\":%.3f}\n", o.model_name,
					(unsigned long long)plc.calls, plc.total_ns,
					plc.calls ? plc.total_ns / (double)plc.calls : 0.0);
		}
	}
	if (out != NULL)
		fclose(out);
	membrane_policy_destroy(policy);
	llama_model_free(model);
	return (ok ? 0 : 1);
}
