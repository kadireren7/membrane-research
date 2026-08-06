#ifndef MEMBRANE_WSSIM_ATTN_WORKLOAD_H
#define MEMBRANE_WSSIM_ATTN_WORKLOAD_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "membrane/attntrace.h"

namespace wssim
{

/*
 * In-memory view of a real (or synthetic) .attntrace file -- Phase 6.2's
 * ground truth for "what attention actually accessed" at every decode
 * step. Loaded once per (model, block_size) combination; block_size
 * here is the NATIVE granularity of the entries array (see
 * regroup_to_block_size for re-deriving other granularities).
 */
struct attn_trace_t
{
	std::string	model;
	bool		is_real_capture;
	uint32_t	n_layer;
	uint32_t	n_head;
	uint32_t	block_size_tokens;
	uint32_t	prompt_len;
	uint32_t	step_count;
	uint32_t	top_k;
	/* step-major, then layer, then head, then top_k entries -- same
	 * nesting as attntrace.h's on-disk payload. */
	std::vector<membrane_attntrace_entry_t>	entries;

	const membrane_attntrace_entry_t	*at(uint32_t step, uint32_t layer,
							uint32_t head) const
	{
		size_t	base;

		base = (((size_t)step * n_layer + layer) * n_head + head)
			* top_k;
		return (&entries[base]);
	}

	/*
	 * Real ground truth for one (layer, kv_head_group) CHANNEL at
	 * `step`: the union of every query head in that group's captured
	 * top-k blocks. This -- not a union across ALL heads/layers -- is
	 * the correct notion of "actually accessed" for a memory system,
	 * because GQA shares one physical K/V per kv-head group across
	 * `group_size` query heads: if any of those heads needs a block,
	 * the whole group's copy must be resident, but heads outside the
	 * group are irrelevant to this channel's fetch decision.
	 * `n_head_kv` (and therefore group_size = n_head / n_head_kv) is
	 * real model geometry supplied by the caller (not stored in the
	 * trace file -- see sim_config.h's SMOLLM2_*_N_HEAD_KV constants).
	 */
	std::vector<uint32_t>	ground_truth_blocks(uint32_t step,
						uint32_t layer, uint32_t kv_group,
						uint32_t group_size) const
	{
		std::vector<uint32_t>	out;

		for (uint32_t h = kv_group * group_size;
				h < (kv_group + 1) * group_size && h < n_head; h++)
		{
			const membrane_attntrace_entry_t	*e = at(step, layer, h);
			for (uint32_t k = 0; k < top_k; k++)
				if (e[k].block_id != UINT32_MAX)
					out.push_back(e[k].block_id);
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return (out);
	}
};

/* Loads a real .attntrace file written by membrane-kv-attn-trace-capture. */
bool	load_attn_trace(const std::string &path, attn_trace_t *out);

/*
 * Re-derives `src` at a different block granularity. For target sizes
 * that are an integer multiple of src.block_size_tokens, this is an
 * exact re-aggregation of the real captured top-k mass (summing
 * scores of native blocks that fall in the same coarser block) --
 * disclosed limitation: because the native capture already truncated
 * to top_k blocks, merged scores are a lower bound on the coarser
 * block's true attention mass, not the exact total (mass outside the
 * native top-k is invisible by construction). For target sizes finer
 * than src.block_size_tokens, each native block's score is split
 * uniformly across its sub-blocks -- an explicit, disclosed
 * approximation (uniform-within-block), not a real finer-grained
 * measurement, since capture did not observe below native
 * granularity.
 */
attn_trace_t	regroup_to_block_size(const attn_trace_t &src,
						uint32_t target_block_size_tokens);

/*
 * Extends a real trace to a longer synthetic context by cyclically
 * replaying its captured step-to-step attention SHAPE (recency
 * pattern, sink persistence, per-layer/head heavy-hitter identity)
 * while shifting block ids forward to match the new, longer position
 * range -- the real per-step KV byte rate this phase reuses from
 * kvtrace.h is a scalar so it extrapolates cleanly; an attention
 * PATTERN cannot be extrapolated the same way, so this is a real,
 * disclosed approximation (repeats the captured pattern's relative
 * structure, not a measurement at the new context length). Used only
 * for the block-granularity/hot-cache/policy sweeps at context
 * lengths beyond what a single real capture run covers.
 */
attn_trace_t	extend_synthetic(const attn_trace_t &src,
						uint32_t target_step_count, uint32_t seed);

/*
 * Phase 6.5: out-of-core sibling of extend_synthetic() -- generates the
 * exact same per-step content (same shared per-step generator, same
 * xorshift32 stream advanced in the same step/layer/head/k order, NOT
 * reset per chunk) but writes it straight to a .attntrace3 file one
 * chunk at a time, holding only `chunk_steps` decode-steps' worth of
 * entries resident at once instead of materializing all
 * `target_step_count` at once -- the whole point being that
 * extend_synthetic()'s output and this function's output are
 * byte-identical (see the parity test in test_attn_trace_reader.cpp),
 * just produced under a bounded, not context-length-proportional,
 * memory footprint.
 */
bool	extend_synthetic_to_file(const attn_trace_t &src,
						uint32_t target_step_count, uint32_t seed,
						const std::string &path, uint32_t chunk_steps,
						int compress = 1);

}	/* namespace wssim */

#endif
