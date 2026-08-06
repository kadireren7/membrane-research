/*
 * membrane-kv-attn-trace-capture: runs a real llama.cpp decode and
 * records the REAL per-decode-step, per-layer, per-head block-level
 * attention distribution (Phase 6.2's membrane_attntrace_t,
 * include/membrane/attntrace.h) by hooking llama.cpp's public
 * ggml_backend_sched_eval_callback (llama_context_params.cb_eval) to
 * read the actual "kq_soft_max-<layer>" tensor computed during a real
 * decode -- genuine post-softmax attention weights, not modeled.
 *
 * This does NOT modify third_party/llama.cpp: cb_eval is an existing
 * public API (the same one examples/eval-callback and common/debug.cpp
 * use), exercised here from our own tool code exactly like
 * membrane-kv-trace-capture already does for llama_state_seq_get_size().
 * Flash attention must be disabled for the capture run -- the fused
 * flash-attention kernel never materializes an explicit softmax
 * tensor, so this tool forces LLAMA_FLASH_ATTN_TYPE_DISABLED.
 *
 * Next-token selection is real greedy argmax over the model's own
 * logits (matching membrane-kv-trace-capture), so the captured
 * attention pattern is from an actual autoregressive decode, not a
 * replayed/repeated prompt.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "ggml.h"
#include "ggml-backend.h"
#include "llama.h"
#include "membrane/attntrace.h"
#include "membrane/attntrace2.h"
#include "membrane/hash.h"

typedef struct s_capture_opts
{
	const char	*model_path;
	const char	*prompt_path;
	const char	*out_path;
	int			n_tokens;
	int			gen_steps;
	int			block_size;
	int			top_k;
	int			v1_format;	/* 0 (default) = write the compact/
					 * optionally-compressed v2 format
					 * (membrane/attntrace2.h); 1 = write
					 * the legacy v1 raw format, kept for
					 * tools/tests that still want it. */
	int			no_compress;	/* v2 only: 1 disables DEFLATE. */
}	capture_opts_t;

static int	die(const char *msg)
{
	fprintf(stderr, "membrane-kv-attn-trace-capture: %s\n", msg);
	return (-1);
}

static std::string	read_prompt(const char *path)
{
	std::string	s;
	FILE		*f;
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

/*
 * Callback state. `entries` is pre-sized to
 * gen_steps * n_layer * n_head * top_k (attntrace.h's step-major,
 * layer, head, entry nesting) and written directly by layer index/
 * current step -- one llama_decode() call per generation step, one
 * "kq_soft_max-<il>" tensor delivered per layer within that call.
 * `active` gates capture to the generation phase only (prompt
 * processing produces its own batched kq_soft_max tensors with a
 * different token-batch shape that this trace format does not model).
 */
typedef struct s_cb_state
{
	std::vector<membrane_attntrace_entry_t>	entries;
	uint32_t	n_layer;
	uint32_t	n_head;
	uint32_t	top_k;
	uint32_t	block_size;
	int			current_step;
	bool		active;
	uint32_t	max_layer_seen;
	uint32_t	max_head_seen;
}	cb_state_t;

static float	tensor_f32_at(const uint8_t *data, const size_t *nb,
					size_t i0, size_t i1, size_t i2, size_t i3)
{
	size_t	off;

	off = i3 * nb[3] + i2 * nb[2] + i1 * nb[1] + i0 * nb[0];
	return (*(const float *)&data[off]);
}

static void	select_top_k(const std::vector<double> &block_scores,
				membrane_attntrace_entry_t *out, uint32_t top_k)
{
	std::vector<uint32_t>	idx;
	uint32_t				i;
	uint32_t				k;
	uint32_t				best;

	idx.resize(block_scores.size());
	i = 0;
	while (i < idx.size())
	{
		idx[i] = i;
		i++;
	}
	k = 0;
	while (k < top_k)
	{
		if (k >= idx.size())
		{
			out[k].block_id = UINT32_MAX;
			out[k].score = 0.0f;
			k++;
			continue ;
		}
		best = k;
		i = k + 1;
		while (i < idx.size())
		{
			if (block_scores[idx[i]] > block_scores[idx[best]])
				best = i;
			i++;
		}
		std::swap(idx[k], idx[best]);
		out[k].block_id = idx[k];
		out[k].score = (float)block_scores[idx[k]];
		k++;
	}
}

static void	process_kq_tensor(cb_state_t *st, ggml_tensor *t,
				const uint8_t *data)
{
	int			il;
	uint32_t	n_kv;
	uint32_t	n_head;
	uint32_t	n_blocks;
	uint32_t	head;
	uint32_t	i0;
	uint32_t	blk;
	size_t		base_off;

	if (sscanf(t->name, "kq_soft_max-%d", &il) != 1 || il < 0)
		return ;
	if ((uint32_t)il >= st->n_layer)
		return ;
	n_kv = (uint32_t)t->ne[0];
	n_head = (uint32_t)t->ne[2];
	if (n_head > st->n_head)
		n_head = st->n_head;
	if (n_kv == 0 || n_head == 0)
		return ;
	if ((uint32_t)il + 1 > st->max_layer_seen)
		st->max_layer_seen = (uint32_t)il + 1;
	if (n_head > st->max_head_seen)
		st->max_head_seen = n_head;
	n_blocks = (n_kv + st->block_size - 1) / st->block_size;
	std::vector<double>	block_scores(n_blocks, 0.0);
	head = 0;
	while (head < n_head)
	{
		std::fill(block_scores.begin(), block_scores.end(), 0.0);
		i0 = 0;
		while (i0 < n_kv)
		{
			blk = i0 / st->block_size;
			block_scores[blk] += (double)tensor_f32_at(data, t->nb,
					i0, 0, head, 0);
			i0++;
		}
		base_off = (((size_t)st->current_step * st->n_layer + (uint32_t)il)
				* st->n_head + head) * st->top_k;
		select_top_k(block_scores, &st->entries[base_off], st->top_k);
		head++;
	}
}

static bool	eval_cb(ggml_tensor *t, bool ask, void *user_data)
{
	cb_state_t				*st;
	bool					is_host;
	std::vector<uint8_t>	tmp;
	const uint8_t			*data;

	st = (cb_state_t *)user_data;
	if (ask)
		return (true);
	if (!st->active || t->type != GGML_TYPE_F32)
		return (true);
	if (strncmp(t->name, "kq_soft_max-", 12) != 0)
		return (true);
	is_host = ggml_backend_buffer_is_host(t->buffer);
	if (is_host)
		data = (const uint8_t *)t->data;
	else
	{
		tmp.resize(ggml_nbytes(t));
		ggml_backend_tensor_get(t, tmp.data(), 0, tmp.size());
		data = tmp.data();
	}
	process_kq_tensor(st, t, data);
	return (true);
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
				cb_state_t &st)
{
	llama_token	tok;
	int			step;

	step = 0;
	while (step < (int)(st.entries.size()
			/ (size_t)st.n_layer / st.n_head / st.top_k))
	{
		tok = greedy_next(ctx, n_vocab);
		st.current_step = step;
		st.active = true;
		if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0)
			return (die("llama_decode failed during capture"));
		st.active = false;
		step++;
	}
	return (0);
}

static int	parse_args(int argc, char **argv, capture_opts_t *o)
{
	int	i;

	memset(o, 0, sizeof(*o));
	o->n_tokens = 512;
	o->gen_steps = 128;
	o->block_size = 32;
	o->top_k = 8;
	i = 1;
	while (i < argc)
	{
		if (strcmp(argv[i], "--no-compress") == 0)
		{
			o->no_compress = 1;
			i += 1;
			continue ;
		}
		if (i + 1 >= argc)
			return (die("missing value for last option"));
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
		else if (strcmp(argv[i], "--block-size") == 0)
			o->block_size = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "--top-k") == 0)
			o->top_k = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "--format") == 0)
		{
			if (strcmp(argv[i + 1], "v1") == 0)
				o->v1_format = 1;
			else if (strcmp(argv[i + 1], "v2") == 0)
				o->v1_format = 0;
			else
				return (die("--format must be v1 or v2"));
		}
		else
			return (die("unknown option"));
		i += 2;
	}
	if (o->model_path == NULL || o->prompt_path == NULL
			|| o->out_path == NULL || o->n_tokens < 16 || o->gen_steps < 1
			|| o->block_size < 1
			|| o->top_k < 1 || o->top_k > (int)MEMBRANE_ATTNTRACE_MAX_TOPK)
		return (die("usage: --model M --prompt-file P --out T "
				"[--n-tokens N] [--gen-steps S] [--block-size B] "
				"[--top-k K] [--format v1|v2] [--no-compress]"));
	return (0);
}

static llama_context	*make_context(llama_model *model, int n_tokens,
							int gen_steps, cb_state_t *st)
{
	llama_context_params	cp;

	cp = llama_context_default_params();
	cp.n_ctx = (uint32_t)(n_tokens + gen_steps + 8);
	cp.n_batch = 256;
	cp.n_threads = 4;
	cp.n_threads_batch = 4;
	cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
	cp.cb_eval = eval_cb;
	cp.cb_eval_user_data = st;
	return (llama_init_from_model(model, cp));
}

/* Phase 6.4 item 6: a manifest committed alongside every trace, so a
 * future reader can verify integrity (SHA-256) and reproduce the
 * capture exactly (the real command line actually used) without
 * having to trust the binary trace file alone. */
static bool	write_manifest(const char *out_path, int argc, char **argv,
				uint64_t file_size)
{
	std::string	manifest_path = std::string(out_path) + ".manifest.json";
	char		hex[MEMBRANE_SHA256_HEX_LEN + 1];
	FILE		*mf;

	if (membrane_sha256_file(out_path, hex) != MEMBRANE_OK)
		return (false);
	mf = fopen(manifest_path.c_str(), "w");
	if (mf == NULL)
		return (false);
	fprintf(mf, "{\n  \"trace_file\": \"%s\",\n  \"sha256\": \"%s\",\n"
		"  \"file_size_bytes\": %llu,\n  \"reproduce_command\": \"",
		out_path, hex, (unsigned long long)file_size);
	for (int i = 0; i < argc; i++)
	{
		if (i > 0)
			fprintf(mf, " ");
		fprintf(mf, "%s", argv[i]);
	}
	fprintf(mf, "\"\n}\n");
	fclose(mf);
	return (true);
}

static int	write_trace(const capture_opts_t &o, const char *model_desc,
				const cb_state_t &st, int argc, char **argv)
{
	FILE							*f;
	membrane_status_t				rc;
	uint32_t						n_layer = st.n_layer;
	uint32_t						n_head = st.n_head;
	uint32_t						block_size = st.block_size;
	uint32_t						prompt_len = (uint32_t)o.n_tokens;
	uint32_t						step_count = (uint32_t)o.gen_steps;
	uint32_t						top_k = st.top_k;

	f = fopen(o.out_path, "wb");
	if (f == NULL)
		return (die("cannot open output trace file"));
	if (o.v1_format)
	{
		membrane_attntrace_header_t	h;

		memset(&h, 0, sizeof(h));
		snprintf(h.model, sizeof(h.model), "%s", model_desc);
		h.source = MEMBRANE_ATTNTRACE_SOURCE_REAL_CAPTURE;
		h.n_layer = n_layer;
		h.n_head = n_head;
		h.block_size_tokens = block_size;
		h.prompt_len = prompt_len;
		h.step_count = step_count;
		h.top_k = top_k;
		h.created_unix_time = (uint64_t)time(NULL);
		rc = membrane_attntrace_write(f, &h, st.entries.data());
	}
	else
	{
		membrane_attntrace2_header_t	h;

		memset(&h, 0, sizeof(h));
		snprintf(h.model, sizeof(h.model), "%s", model_desc);
		h.source = MEMBRANE_ATTNTRACE_SOURCE_REAL_CAPTURE;
		h.n_layer = n_layer;
		h.n_head = n_head;
		h.block_size_tokens = block_size;
		h.prompt_len = prompt_len;
		h.step_count = step_count;
		h.top_k = top_k;
		h.created_unix_time = (uint64_t)time(NULL);
		rc = membrane_attntrace2_write(f, &h, st.entries.data(),
			o.no_compress ? 0 : 1);
	}
	fclose(f);
	if (rc != MEMBRANE_OK)
		return (die("trace write failed"));

	FILE	*sf = fopen(o.out_path, "rb");
	uint64_t	file_size = 0;
	if (sf != NULL)
	{
		fseek(sf, 0, SEEK_END);
		file_size = (uint64_t)ftell(sf);
		fclose(sf);
	}
	if (!write_manifest(o.out_path, argc, argv, file_size))
		fprintf(stderr, "membrane-kv-attn-trace-capture: WARNING: "
			"failed to write manifest\n");
	fprintf(stderr,
		"membrane-kv-attn-trace-capture: wrote %u real decode steps "
		"(model=%s n_layer=%u n_head=%u block=%u top_k=%u, format=%s), "
		"tensors seen: max_layer=%u max_head=%u, file_size=%llu bytes "
		"-> %s (+ .manifest.json)\n",
		step_count, model_desc, n_layer, n_head, block_size, top_k,
		o.v1_format ? "v1" : "v2", st.max_layer_seen, st.max_head_seen,
		(unsigned long long)file_size, o.out_path);
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
	cb_state_t					st;
	int							rc;

	if (parse_args(argc, argv, &o) != 0)
		return (2);
	llama_backend_init();
	model = llama_model_load_from_file(o.model_path,
			llama_model_default_params());
	if (model == NULL)
		return (die("model load failed"), 2);
	st.n_layer = (uint32_t)llama_model_n_layer(model);
	st.n_head = (uint32_t)llama_model_n_head(model);
	st.top_k = (uint32_t)o.top_k;
	st.block_size = (uint32_t)o.block_size;
	st.current_step = 0;
	st.active = false;
	st.max_layer_seen = 0;
	st.max_head_seen = 0;
	if (st.n_layer > MEMBRANE_ATTNTRACE_MAX_LAYER
			|| st.n_head > MEMBRANE_ATTNTRACE_MAX_HEAD)
		return (llama_model_free(model),
			die("model geometry exceeds attntrace format caps"), 2);
	st.entries.assign((size_t)o.gen_steps * st.n_layer * st.n_head
			* st.top_k, membrane_attntrace_entry_t{UINT32_MAX, 0.0f});
	ctx = make_context(model, o.n_tokens, o.gen_steps, &st);
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
			&& capture_steps(ctx, llama_vocab_n_tokens(vocab), st) == 0)
		rc = write_trace(o, desc, st, argc, argv);
	llama_free(ctx);
	llama_model_free(model);
	llama_backend_free();
	return (rc == 0 ? 0 : 1);
}
