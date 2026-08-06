/*
 * membrane-kv-capture: runs a prompt through llama.cpp and exports the
 * resulting KV-cache tensors into MEMBRANE's kvdump format.
 *
 * The exporter parses the blob produced by llama_state_seq_get_data().
 * That layout is internal to llama.cpp, so this tool targets exactly the
 * commit pinned in third_party/llama.cpp (see docs/phase2-kv-analysis.md
 * for the layout description); every read is bounds-checked and a layout
 * mismatch aborts with an error rather than exporting garbage.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "llama.h"
#include "membrane/block.h"
#include "membrane/kvdump.h"

# define SEQ_STATE_MAGIC 0xaf143cd8u

typedef struct s_cursor
{
	const uint8_t	*p;
	size_t			left;
}	cursor_t;

typedef struct s_capture_opts
{
	const char	*model_path;
	const char	*prompt_path;
	const char	*out_path;
	int			n_tokens;
}	capture_opts_t;

static int	cur_read(cursor_t *c, void *out, size_t n)
{
	if (c->left < n)
		return (-1);
	memcpy(out, c->p, n);
	c->p += n;
	c->left -= n;
	return (0);
}

static int	cur_skip(cursor_t *c, size_t n)
{
	if (c->left < n)
		return (-1);
	c->p += n;
	c->left -= n;
	return (0);
}

static int	die(const char *msg)
{
	fprintf(stderr, "membrane-kv-capture: %s\n", msg);
	return (-1);
}

/* Skips the per-cell metadata (pos, n_seq_id, seq ids). */
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

static void	fill_header(membrane_kv_header_t *h, const char *model,
				uint32_t layer, uint32_t type, uint32_t cell_count)
{
	memset(h, 0, sizeof(*h));
	snprintf(h->model, sizeof(h->model), "%s", model);
	h->layer = layer;
	h->tensor_type = type;
	h->token_start = 0;
	h->token_end = cell_count;
}

/* Row-major tensor record: [i32 type][u64 row_size][cells x row bytes].
 * Used for K always, and for V when llama stores it non-transposed. */
static int	export_rows(cursor_t *c, FILE *out, const char *model,
				uint32_t layer, uint32_t type, uint32_t cell_count)
{
	membrane_kv_header_t	h;
	int32_t					elem_type;
	uint64_t				size_row;
	uint64_t				total;

	if (cur_read(c, &elem_type, 4) != 0 || cur_read(c, &size_row, 8) != 0)
		return (die("truncated tensor header"));
	if (size_row == 0 || size_row > (1u << 20))
		return (die("implausible row size: layout mismatch?"));
	total = size_row * cell_count;
	if (c->left < total)
		return (die("truncated tensor payload"));
	fill_header(&h, model, layer, type, cell_count);
	h.dtype = (uint32_t)elem_type;
	h.n_dims = 2;
	h.dims[0] = size_row;
	h.dims[1] = cell_count;
	h.payload_size = total;
	h.checksum = membrane_block_checksum(c->p, total);
	if (membrane_kvdump_write(out, &h, c->p) != MEMBRANE_OK)
		return (die("dump write failed"));
	return (cur_skip(c, total));
}

static int	export_v(cursor_t *c, FILE *out, const char *model,
				uint32_t layer, uint32_t cell_count)
{
	membrane_kv_header_t	h;
	int32_t					v_type;
	uint32_t				v_size_el;
	uint32_t				n_embd;
	uint64_t				total;

	if (cur_read(c, &v_type, 4) != 0 || cur_read(c, &v_size_el, 4) != 0
		|| cur_read(c, &n_embd, 4) != 0)
		return (die("truncated V header"));
	if (v_size_el == 0 || v_size_el > 64 || n_embd == 0 || n_embd > (1u << 20))
		return (die("implausible V geometry: layout mismatch?"));
	total = (uint64_t)v_size_el * n_embd * cell_count;
	if (c->left < total)
		return (die("truncated V payload"));
	fill_header(&h, model, layer, MEMBRANE_KV_TENSOR_V, cell_count);
	h.dtype = (uint32_t)v_type;
	h.n_dims = 3;
	h.dims[0] = cell_count;
	h.dims[1] = n_embd;
	h.dims[2] = v_size_el;
	h.payload_size = total;
	h.checksum = membrane_block_checksum(c->p, total);
	if (membrane_kvdump_write(out, &h, c->p) != MEMBRANE_OK)
		return (die("dump write failed"));
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
		return (die("multi-stream KV not supported by this exporter"));
	if (cur_read(c, cell_count, 4) != 0 || *cell_count == 0)
		return (die("empty KV cache"));
	if (*cell_count > (1u << 24))
		return (die("implausible cell count: layout mismatch?"));
	return (skip_cell_meta(c, *cell_count));
}

static int	export_v_layers(cursor_t *c, FILE *out, const char *model,
				uint32_t v_trans, uint32_t n_layer, uint32_t cell_count)
{
	uint32_t	il;
	int			rc;

	il = 0;
	while (il < n_layer)
	{
		if (v_trans == 0)
			rc = export_rows(c, out, model, il, MEMBRANE_KV_TENSOR_V,
					cell_count);
		else
			rc = export_v(c, out, model, il, cell_count);
		if (rc != 0)
			return (-1);
		il++;
	}
	return (0);
}

static int	export_layers(cursor_t *c, FILE *out, const char *model,
				uint32_t cell_count)
{
	uint32_t	v_trans;
	uint32_t	n_layer;
	uint32_t	il;

	if (cur_read(c, &v_trans, 4) != 0 || cur_read(c, &n_layer, 4) != 0)
		return (die("truncated data prologue"));
	if (v_trans > 1 || n_layer == 0 || n_layer > 512)
		return (die("implausible data prologue: layout mismatch?"));
	il = 0;
	while (il < n_layer)
	{
		if (export_rows(c, out, model, il, MEMBRANE_KV_TENSOR_K,
				cell_count) != 0)
			return (-1);
		il++;
	}
	if (export_v_layers(c, out, model, v_trans, n_layer, cell_count) != 0)
		return (-1);
	fprintf(stderr, "exported %u layers x {K,V}, %u cells, v_trans=%u\n",
		n_layer, cell_count, v_trans);
	return (0);
}

static std::vector<llama_token>	build_tokens(const llama_vocab *vocab,
									const std::string &text, int target)
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

static int	decode_tokens(llama_context *ctx,
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
			return (die("llama_decode failed"));
		off += n;
	}
	return (0);
}

static int	export_state(llama_context *ctx, const char *model_desc,
				const char *out_path)
{
	std::vector<uint8_t>	blob;
	cursor_t				cur;
	uint32_t				cell_count;
	FILE					*out;
	int						rc;

	blob.resize(llama_state_seq_get_size(ctx, 0));
	if (blob.empty()
		|| llama_state_seq_get_data(ctx, blob.data(), blob.size(), 0)
			!= blob.size())
		return (die("llama_state_seq_get_data failed"));
	cur.p = blob.data();
	cur.left = blob.size();
	out = fopen(out_path, "wb");
	if (out == NULL)
		return (die("cannot open output dump"));
	rc = parse_prologue(&cur, &cell_count);
	if (rc == 0)
		rc = export_layers(&cur, out, model_desc, cell_count);
	fclose(out);
	return (rc);
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

static int	parse_args(int argc, char **argv, capture_opts_t *o)
{
	int	i;

	memset(o, 0, sizeof(*o));
	o->n_tokens = 1024;
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
		else
			return (die("unknown option"));
		i += 2;
	}
	if (o->model_path == NULL || o->prompt_path == NULL
		|| o->out_path == NULL || o->n_tokens < 16)
		return (die("usage: --model M --prompt-file P --out D "
				"[--n-tokens N]"));
	return (0);
}

static llama_context	*make_context(llama_model *model, int n_tokens)
{
	llama_context_params	cp;

	cp = llama_context_default_params();
	cp.n_ctx = (uint32_t)n_tokens;
	cp.n_batch = 256;
	cp.n_threads = 4;
	cp.n_threads_batch = 4;
	return (llama_init_from_model(model, cp));
}

int	main(int argc, char **argv)
{
	capture_opts_t	o;
	llama_model		*model;
	llama_context	*ctx;
	char			desc[64];
	int				rc;

	if (parse_args(argc, argv, &o) != 0)
		return (2);
	llama_backend_init();
	model = llama_model_load_from_file(o.model_path,
			llama_model_default_params());
	if (model == NULL)
		return (die("model load failed"), 2);
	ctx = make_context(model, o.n_tokens);
	if (ctx == NULL)
		return (die("context create failed"), 2);
	llama_model_desc(model, desc, sizeof(desc));
	std::vector<llama_token> tokens = build_tokens(llama_model_get_vocab(model),
			read_prompt(o.prompt_path), o.n_tokens);
	rc = -1;
	if (tokens.empty())
		die("tokenization failed");
	else if (decode_tokens(ctx, tokens, 256) == 0)
		rc = export_state(ctx, desc, o.out_path);
	llama_free(ctx);
	llama_model_free(model);
	llama_backend_free();
	return (rc == 0 ? 0 : 1);
}
