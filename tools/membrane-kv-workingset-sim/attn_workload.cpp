#include <algorithm>
#include <cstdio>
#include <map>

#include "attn_workload.h"
#include "membrane/attntrace2.h"
#include "membrane/attntrace3.h"

namespace wssim
{

static uint32_t	xorshift32(uint32_t *state)
{
	uint32_t	x;

	x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return (x);
}

/* Phase 6.4: v2 (compact, optionally compressed -- membrane/attntrace2.h)
 * traces are auto-detected by magic number so every caller of
 * load_attn_trace() keeps working unchanged regardless of which
 * on-disk format a given file uses. */
static bool	load_attn_trace_v2(FILE *f, attn_trace_t *out)
{
	membrane_attntrace2_header_t	h;
	size_t							n;

	if (membrane_attntrace2_read_header(f, &h) != MEMBRANE_OK)
		return (false);
	n = membrane_attntrace2_entry_count(&h);
	out->entries.resize(n);
	if (membrane_attntrace2_read_entries(f, &h, out->entries.data())
			!= MEMBRANE_OK)
		return (false);
	out->model = h.model;
	out->is_real_capture = (h.source == MEMBRANE_ATTNTRACE_SOURCE_REAL_CAPTURE);
	out->n_layer = h.n_layer;
	out->n_head = h.n_head;
	out->block_size_tokens = h.block_size_tokens;
	out->prompt_len = h.prompt_len;
	out->step_count = h.step_count;
	out->top_k = h.top_k;
	return (true);
}

bool	load_attn_trace(const std::string &path, attn_trace_t *out)
{
	FILE						*f;
	membrane_attntrace_header_t	h;
	size_t						n;
	uint8_t						magic_buf[4];
	uint32_t					magic;

	f = fopen(path.c_str(), "rb");
	if (f == NULL)
		return (false);
	if (fread(magic_buf, 1, 4, f) != 4)
	{
		fclose(f);
		return (false);
	}
	magic = (uint32_t)magic_buf[0] | ((uint32_t)magic_buf[1] << 8)
		| ((uint32_t)magic_buf[2] << 16) | ((uint32_t)magic_buf[3] << 24);
	rewind(f);
	if (magic == MEMBRANE_ATTNTRACE2_MAGIC)
	{
		bool	ok = load_attn_trace_v2(f, out);
		fclose(f);
		return (ok);
	}
	if (membrane_attntrace_read_header(f, &h) != MEMBRANE_OK)
	{
		fclose(f);
		return (false);
	}
	n = membrane_attntrace_entry_count(&h);
	out->entries.resize(n);
	if (membrane_attntrace_read_entries(f, &h, out->entries.data())
			!= MEMBRANE_OK)
	{
		fclose(f);
		return (false);
	}
	fclose(f);
	out->model = h.model;
	out->is_real_capture = (h.source == MEMBRANE_ATTNTRACE_SOURCE_REAL_CAPTURE);
	out->n_layer = h.n_layer;
	out->n_head = h.n_head;
	out->block_size_tokens = h.block_size_tokens;
	out->prompt_len = h.prompt_len;
	out->step_count = h.step_count;
	out->top_k = h.top_k;
	return (true);
}

static void	top_k_from_map(const std::map<uint32_t, double> &acc,
				uint32_t top_k, std::vector<membrane_attntrace_entry_t> &dst,
				size_t base)
{
	std::vector<std::pair<uint32_t, double>>	v(acc.begin(), acc.end());

	std::sort(v.begin(), v.end(),
		[](const std::pair<uint32_t, double> &a,
			const std::pair<uint32_t, double> &b)
		{ return (a.second > b.second); });
	for (uint32_t k = 0; k < top_k; k++)
	{
		if (k < v.size())
		{
			dst[base + k].block_id = v[k].first;
			dst[base + k].score = (float)v[k].second;
		}
		else
		{
			dst[base + k].block_id = UINT32_MAX;
			dst[base + k].score = 0.0f;
		}
	}
}

attn_trace_t	regroup_to_block_size(const attn_trace_t &src,
						uint32_t target_block_size_tokens)
{
	attn_trace_t	out;
	uint32_t		b0;
	uint32_t		coarsen_ratio;
	uint32_t		split_ratio;

	out = src;
	out.block_size_tokens = target_block_size_tokens;
	if (target_block_size_tokens == src.block_size_tokens)
		return (out);
	b0 = src.block_size_tokens;
	out.entries.assign(src.entries.size(),
		membrane_attntrace_entry_t{UINT32_MAX, 0.0f});
	coarsen_ratio = 0;
	split_ratio = 0;
	if (target_block_size_tokens > b0
			&& target_block_size_tokens % b0 == 0)
		coarsen_ratio = target_block_size_tokens / b0;
	else if (b0 > target_block_size_tokens
			&& b0 % target_block_size_tokens == 0)
		split_ratio = b0 / target_block_size_tokens;
	for (uint32_t step = 0; step < src.step_count; step++)
	{
		for (uint32_t layer = 0; layer < src.n_layer; layer++)
		{
			for (uint32_t head = 0; head < src.n_head; head++)
			{
				const membrane_attntrace_entry_t	*native
					= src.at(step, layer, head);
				std::map<uint32_t, double>			acc;

				for (uint32_t k = 0; k < src.top_k; k++)
				{
					if (native[k].block_id == UINT32_MAX)
						continue ;
					if (coarsen_ratio > 0)
						acc[native[k].block_id / coarsen_ratio]
							+= native[k].score;
					else if (split_ratio > 0)
					{
						double	share = (double)native[k].score
							/ split_ratio;
						for (uint32_t j = 0; j < split_ratio; j++)
							acc[native[k].block_id * split_ratio + j]
								+= share;
					}
				}
				size_t	base = (((size_t)step * src.n_layer + layer)
						* src.n_head + head) * src.top_k;
				top_k_from_map(acc, src.top_k, out.entries, base);
			}
		}
	}
	return (out);
}

static uint32_t	synthetic_blocks_per_cycle(const attn_trace_t &src)
{
	return ((src.prompt_len + src.step_count + src.block_size_tokens - 1)
		/ src.block_size_tokens);
}

/*
 * Shared by extend_synthetic() (in-memory) and extend_synthetic_to_file()
 * (out-of-core, Phase 6.5) so their output is byte-identical -- writes
 * exactly one step's n_layer * n_head * top_k entries to `out` (base
 * offset 0), advancing the caller-owned RNG `state` by exactly the
 * same number of draws, in the same step/layer/head/k order, either
 * function uses. Callers MUST invoke this for steps 0..N-1 in
 * increasing order without resetting `state` in between (extend_
 * synthetic_to_file does this across chunk boundaries too, not just
 * within one chunk) -- the RNG stream, not just the per-step math, is
 * part of what must match for the two functions to agree.
 */
static void	generate_synthetic_step(const attn_trace_t &src, uint32_t step,
				uint32_t blocks_per_cycle, uint32_t *state,
				membrane_attntrace_entry_t *out)
{
	uint32_t	cycle = step / src.step_count;
	uint32_t	src_step = step % src.step_count;
	uint32_t	shift = cycle * blocks_per_cycle;

	for (uint32_t layer = 0; layer < src.n_layer; layer++)
	{
		for (uint32_t head = 0; head < src.n_head; head++)
		{
			const membrane_attntrace_entry_t	*native
				= src.at(src_step, layer, head);
			size_t	base = ((size_t)layer * src.n_head + head) * src.top_k;

			for (uint32_t k = 0; k < src.top_k; k++)
			{
				if (native[k].block_id == UINT32_MAX)
				{
					out[base + k].block_id = UINT32_MAX;
					out[base + k].score = 0.0f;
					continue ;
				}
				/* block 0 is the real, repeatedly-observed
				 * attention-sink block (see docs) -- kept fixed
				 * across cycles rather than shifted forward,
				 * matching the real captured behavior instead of
				 * diluting it away at longer synthetic contexts. */
				uint32_t	bid = native[k].block_id;
				if (bid != 0)
					bid += shift;
				double	jitter = 1.0 + (((double)(xorshift32(state)
						% 2001) - 1000.0) / 1000.0) * 0.03;
				double	jittered = (double)native[k].score * jitter;

				/* score represents a normalized attention weight
				 * everywhere else in this codebase (v1/v2/v3 on-disk
				 * encodings all clamp to [0,1] -- see e.g.
				 * attntrace3.c's writer); jitter alone can push a
				 * near-1.0 native score slightly past 1.0, which used
				 * to slip through uncaught in the in-memory
				 * representation while any serialization clamped it
				 * -- clamping here keeps extend_synthetic() and
				 * extend_synthetic_to_file() in exact agreement
				 * regardless of whether the result is ever written
				 * to disk. */
				if (jittered < 0.0)
					jittered = 0.0;
				else if (jittered > 1.0)
					jittered = 1.0;
				out[base + k].block_id = bid;
				out[base + k].score = (float)jittered;
			}
		}
	}
}

attn_trace_t	extend_synthetic(const attn_trace_t &src,
						uint32_t target_step_count, uint32_t seed)
{
	attn_trace_t	out;
	uint32_t		blocks_per_cycle;
	uint32_t		state;
	size_t			per_step;

	out = src;
	out.is_real_capture = false;
	out.step_count = target_step_count;
	blocks_per_cycle = synthetic_blocks_per_cycle(src);
	per_step = (size_t)src.n_layer * src.n_head * src.top_k;
	out.entries.assign((size_t)target_step_count * per_step,
		membrane_attntrace_entry_t{UINT32_MAX, 0.0f});
	state = seed | 1u;
	for (uint32_t step = 0; step < target_step_count; step++)
		generate_synthetic_step(src, step, blocks_per_cycle, &state,
			out.entries.data() + (size_t)step * per_step);
	return (out);
}

bool	extend_synthetic_to_file(const attn_trace_t &src,
						uint32_t target_step_count, uint32_t seed,
						const std::string &path, uint32_t chunk_steps,
						int compress)
{
	membrane_attntrace3_writer_t				w;
	FILE										*f;
	uint32_t									blocks_per_cycle;
	uint32_t									state;
	size_t										per_step;
	std::vector<membrane_attntrace_entry_t>	chunk_buf;

	f = fopen(path.c_str(), "w+b");
	if (f == NULL)
		return (false);
	if (membrane_attntrace3_writer_open(f, &w, src.model.c_str(),
			MEMBRANE_ATTNTRACE_SOURCE_SYNTHETIC, src.n_layer, src.n_head,
			src.block_size_tokens, src.prompt_len, target_step_count,
			src.top_k, chunk_steps) != MEMBRANE_OK)
	{
		fclose(f);
		return (false);
	}
	blocks_per_cycle = synthetic_blocks_per_cycle(src);
	per_step = (size_t)src.n_layer * src.n_head * src.top_k;
	state = seed | 1u;
	for (uint32_t c = 0; c < w.h.chunk_count; c++)
	{
		uint32_t	clen = membrane_attntrace3_writer_chunk_len(&w, c);
		uint32_t	step_lo = c * chunk_steps;

		chunk_buf.assign((size_t)clen * per_step,
			membrane_attntrace_entry_t{UINT32_MAX, 0.0f});
		for (uint32_t s = 0; s < clen; s++)
			generate_synthetic_step(src, step_lo + s, blocks_per_cycle,
				&state, chunk_buf.data() + (size_t)s * per_step);
		if (membrane_attntrace3_writer_put_chunk(f, &w, c, chunk_buf.data(),
				compress) != MEMBRANE_OK)
		{
			fclose(f);
			return (false);
		}
	}
	if (membrane_attntrace3_writer_close(f, &w) != MEMBRANE_OK)
	{
		fclose(f);
		return (false);
	}
	fclose(f);
	return (true);
}

}	/* namespace wssim */
