/*
 * membrane-kv-trace-capture: runs a real llama.cpp decode and records
 * the REAL per-decode-step growth of the KV cache (measured via
 * llama_state_seq_get_size() deltas, not modeled) into a
 * membrane_kvtrace_t file (include/membrane/kvtrace.h) -- Phase 6.1's
 * "at least one real LLM KV access trace", used by membrane-cxl-sim
 * (tools/membrane-cxl-sim) both directly and as the source of the
 * real-measured per-token byte rate that trace's own parametric,
 * longer-context sweeps extrapolate from (see kvtrace.h's header
 * comment on MEMBRANE_KVTRACE_SOURCE_SYNTHETIC for why that
 * extrapolation is disclosed, not hidden).
 *
 * Next-token selection is real greedy argmax over the model's own
 * logits (not a replayed/repeated prompt) -- an actual autoregressive
 * decode, the same access pattern any real inference serves.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "llama.h"
#include "membrane/kvtrace.h"

typedef struct s_capture_opts
{
	const char	*model_path;
	const char	*prompt_path;
	const char	*out_path;
	int			n_tokens;
	int			gen_steps;
}	capture_opts_t;

static int	die(const char *msg)
{
	fprintf(stderr, "membrane-kv-trace-capture: %s\n", msg);
	return (-1);
}

static std::string	read_prompt(const char *path)
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

static std::vector<llama_token>	build_prompt_tokens(
		const llama_vocab *vocab, const std::string &text, int target)
{
	std::vector<llama_token>	base;
	std::vector<llama_token>	out;
	int							n;

	base.resize(text.size() + 8);
	n = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(),
			base.data(), (int32_t)base.size(), true, false);
	if (n < 0)
		return (out);
	base.resize(n);
	out = base;
	while ((int)out.size() < target)
		out.insert(out.end(), base.begin() + 1, base.end());
	if ((int)out.size() > target)
		out.resize(target);
	return (out);
}

static int	decode_prompt(llama_context *ctx,
				std::vector<llama_token> &tokens, int n_batch)
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
				llama_batch_get_one(tokens.data() + off, (int32_t)n)) != 0)
			return (die("llama_decode failed on prompt"));
		off += n;
	}
	return (0);
}

/* Real greedy argmax over the model's own logits for the last token. */
static llama_token	greedy_next(llama_context *ctx, int32_t n_vocab)
{
	const float	*logits;
	llama_token	best;
	float		best_v;
	int32_t		i;

	logits = llama_get_logits_ith(ctx, -1);
	best = 0;
	best_v = logits[0];
	i = 1;
	while (i < n_vocab)
	{
		if (logits[i] > best_v)
		{
			best_v = logits[i];
			best = i;
		}
		i++;
	}
	return (best);
}

static int	capture_steps(llama_context *ctx, int32_t n_vocab,
				int gen_steps, std::vector<uint32_t> &deltas)
{
	size_t		size_before;
	size_t		size_after;
	llama_token	tok;
	int			step;

	size_before = llama_state_seq_get_size(ctx, 0);
	step = 0;
	while (step < gen_steps)
	{
		tok = greedy_next(ctx, n_vocab);
		if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0)
			return (die("llama_decode failed during capture"));
		size_after = llama_state_seq_get_size(ctx, 0);
		if (size_after < size_before)
			return (die("KV state shrank -- unexpected, aborting capture"));
		deltas.push_back((uint32_t)(size_after - size_before));
		size_before = size_after;
		step++;
	}
	return (0);
}

static int	parse_args(int argc, char **argv, capture_opts_t *o)
{
	int	i;

	memset(o, 0, sizeof(*o));
	o->n_tokens = 512;
	o->gen_steps = 64;
	i = 1;
	while (i + 1 < argc)
	{
		if (strcmp(argv[i], "--model") == 0)
			o->model_path = argv[i + 1];
		else if (strcmp(argv[i], "--prompt-file") == 0)
			o->prompt_path = argv[i + 1];
		else if (strcmp(argv[i], "--out") == 0)
			o->out_path = argv[i + 1];
		else if (strcmp(argv[i], "--n-tokens") == 0)
			o->n_tokens = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "--gen-steps") == 0)
			o->gen_steps = atoi(argv[i + 1]);
		else
			return (die("unknown option"));
		i += 2;
	}
	if (o->model_path == NULL || o->prompt_path == NULL
			|| o->out_path == NULL || o->n_tokens < 16 || o->gen_steps < 1)
		return (die("usage: --model M --prompt-file P --out T "
				"[--n-tokens N] [--gen-steps S]"));
	return (0);
}

static llama_context	*make_context(llama_model *model, int n_tokens,
							int gen_steps)
{
	llama_context_params	cp;

	cp = llama_context_default_params();
	cp.n_ctx = (uint32_t)(n_tokens + gen_steps + 8);
	cp.n_batch = 256;
	cp.n_threads = 4;
	cp.n_threads_batch = 4;
	return (llama_init_from_model(model, cp));
}

static int	write_trace(const capture_opts_t &o, const char *model_desc,
				const std::vector<uint32_t> &deltas, uint32_t n_layer,
				uint32_t n_head_kv)
{
	membrane_kvtrace_header_t	h;
	FILE						*f;
	membrane_status_t			rc;

	memset(&h, 0, sizeof(h));
	snprintf(h.model, sizeof(h.model), "%s", model_desc);
	h.source = MEMBRANE_KVTRACE_SOURCE_REAL_CAPTURE;
	h.n_layer = n_layer;
	h.n_head_kv = n_head_kv;
	h.prompt_len = (uint32_t)o.n_tokens;
	h.step_count = (uint32_t)deltas.size();
	h.created_unix_time = (uint64_t)time(NULL);
	f = fopen(o.out_path, "wb");
	if (f == NULL)
		return (die("cannot open output trace file"));
	rc = membrane_kvtrace_write(f, &h, deltas.data());
	fclose(f);
	if (rc != MEMBRANE_OK)
		return (die("trace write failed"));
	fprintf(stderr,
		"membrane-kv-trace-capture: wrote %u real decode steps "
		"(model=%s n_layer=%u n_head_kv=%u prompt_len=%u) -> %s\n",
		h.step_count, h.model, h.n_layer, h.n_head_kv, h.prompt_len,
		o.out_path);
	return (0);
}

int	main(int argc, char **argv)
{
	capture_opts_t				o;
	llama_model					*model;
	llama_context				*ctx;
	const llama_vocab			*vocab;
	char						desc[64];
	std::vector<llama_token>	tokens;
	std::vector<uint32_t>		deltas;
	int							rc;

	if (parse_args(argc, argv, &o) != 0)
		return (2);
	llama_backend_init();
	model = llama_model_load_from_file(o.model_path,
			llama_model_default_params());
	if (model == NULL)
		return (die("model load failed"), 2);
	ctx = make_context(model, o.n_tokens, o.gen_steps);
	if (ctx == NULL)
		return (llama_model_free(model), die("context create failed"), 2);
	llama_model_desc(model, desc, sizeof(desc));
	vocab = llama_model_get_vocab(model);
	tokens = build_prompt_tokens(vocab, read_prompt(o.prompt_path),
			o.n_tokens);
	rc = -1;
	if (tokens.empty())
		die("tokenization failed");
	else if (decode_prompt(ctx, tokens, 256) == 0
			&& capture_steps(ctx, llama_vocab_n_tokens(vocab), o.gen_steps,
				deltas) == 0)
		rc = write_trace(o, desc, deltas,
				(uint32_t)llama_model_n_layer(model),
				(uint32_t)llama_model_n_head_kv(model));
	llama_free(ctx);
	llama_model_free(model);
	llama_backend_free();
	return (rc == 0 ? 0 : 1);
}
