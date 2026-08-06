#include <algorithm>
#include <deque>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "wssim_config.h"

namespace wssim
{

static double	percentile(std::vector<double> v, double p)
{
	if (v.empty())
		return (0.0);
	std::sort(v.begin(), v.end());
	size_t	idx = (size_t)(p * (double)(v.size() - 1));
	return (v[idx]);
}

static double	transfer_ns(uint64_t bytes, const hardware_profile_t &hw)
{
	double	effective_bw = std::min(hw.cxl_link_bandwidth_gbps,
		hw.nearmem_pipeline_bytes_per_ns * hw.quant_pipelines);
	return (hw.cxl_link_latency_ns + (double)bytes / effective_bw);
}

static double	bytes_that_fit(double ns_budget, const hardware_profile_t &hw)
{
	double	effective_bw = std::min(hw.cxl_link_bandwidth_gbps,
		hw.nearmem_pipeline_bytes_per_ns * hw.quant_pipelines);
	double	remaining = ns_budget - hw.cxl_link_latency_ns;
	if (remaining <= 0.0)
		return (0.0);
	return (remaining * effective_bw);
}

static bool	vec_contains(const std::vector<uint32_t> &v, uint32_t x)
{
	return (std::find(v.begin(), v.end(), x) != v.end());
}

/*
 * Phase 6.5: the scalar trace fields run_scenario_calibration_impl
 * actually needs -- deliberately NOT attn_trace_t itself, so the same
 * template body below works whether the caller has a fully in-memory
 * attn_trace_t or an out-of-core attn_trace_reader_t (see
 * attn_trace_reader.h's trace_metadata_t, which this mirrors).
 */
struct trace_view_t
{
	uint32_t	n_layer;
	uint32_t	n_head;
	uint32_t	step_count;
	uint32_t	prompt_len;
	uint32_t	top_k;
};

/*
 * Same real ground-truth-union-per-channel logic as
 * attn_trace_t::ground_truth_blocks() (attn_workload.h) -- duplicated
 * here as a free template instead of reused directly because it needs
 * to run against `at`, a generic per-(step,layer,head) accessor, not
 * specifically an attn_trace_t. Both implementations must stay in
 * agreement; the cross-backend parity tests
 * (test_attn_trace_reader.cpp) are what actually enforces that.
 */
template <class AtFn>
static std::vector<uint32_t>	ground_truth_blocks_via(AtFn &&at,
					uint32_t top_k, uint32_t n_head, uint32_t step,
					uint32_t layer, uint32_t kv_group, uint32_t group_size)
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

/*
 * Phase 6.5: the one real implementation of the 8-stage per-decode-
 * step pipeline (see run_scenario's doc comment in engine.h) --
 * templated on `AtFn` (a (step,layer,head) -> const
 * membrane_attntrace_entry_t* accessor) so run_scenario_calibration()
 * and run_scenario_calibration_streamed() below are both thin
 * wrappers around this SAME code, not two maintained copies of it.
 * Everything below is unchanged from the pre-6.5 single-attn_trace_t
 * version except the two lines that used to call attn_trace_t's own
 * methods directly (`trace.ground_truth_blocks(...)` and
 * `trace.at(...)`), now routed through `at`/ground_truth_blocks_via so
 * they work against either backend.
 */
template <class AtFn>
static scenario_result_t	run_scenario_calibration_impl(
						const trace_view_t &trace, AtFn &&at,
						const model_calibration_t &model,
						const scenario_config_t &cfg,
						std::vector<per_step_calib_t> *out_steps,
						layer_head_stats_t *out_layer_head,
						coalescing_stats_t *out_coalescing,
						const hardware_profile_t *hw_in)
{
	scenario_result_t		res{};
	uint32_t				group_size;
	uint32_t				n_kv_group;
	double					compression;
	double					bytes_per_channel_per_token;
	hardware_profile_t		hw = hw_in != nullptr ? *hw_in
		: default_hardware_profile();

	group_size = model.n_head_kv > 0 ? (trace.n_head / model.n_head_kv) : 1;
	if (group_size == 0)
		group_size = 1;
	n_kv_group = model.n_head_kv;
	compression = cfg.no_compression ? 1.0
		: (cfg.warm_tier_is_q8
			? sim::Q8_COMPRESSION_RATIO : sim::Q4_COMPRESSION_RATIO);
	bytes_per_channel_per_token = (double)model.bytes_per_token_total
		/ (double)(model.n_layer * n_kv_group);

	hot_cache_t	cache(cfg.hot_cache_bytes, cfg.eviction);
	std::vector<channel_predictor_t>	predictors;
	for (uint32_t l = 0; l < trace.n_layer; l++)
		for (uint32_t g = 0; g < n_kv_group; g++)
			predictors.emplace_back(cfg.block_size_tokens, policy_params_t{});

	/* Phase 6.5 item 5: this set tracks every (layer, kv_group,
	 * block_id) tuple EVER fetched, across the whole trace, purely to
	 * detect a later redundant re-fetch of the same tuple -- at the
	 * unified sweep's 130560-step synthetic extension, block ids keep
	 * shifting forward every ~4096-step cycle (see extend_synthetic's
	 * "block 0 stays fixed, everything else shifts" comment), so the
	 * union of distinct tuples touched grows roughly LINEARLY with
	 * context length rather than staying bounded by working-set size
	 * the way the predictor/hot-cache state does -- a real
	 * unaccounted memory hotspot a --audit-memory run caught (~470
	 * MiB for one SmolLM2-135M calibrate() call). Bounded here to
	 * kEverFetchedCap tuples, FIFO-evicted -- a disclosed
	 * approximation: total_redundant_fetches becomes a LOWER BOUND
	 * once the cap is exceeded (a genuine redundant fetch of a tuple
	 * evicted from this tracking window looks like a fresh one), not
	 * an exact count. Every other metric (latency, capacity, bytes/
	 * token, hit rate, precision/recall) is unaffected -- none of
	 * them read `ever_fetched`. */
	constexpr size_t				kEverFetchedCap = 1000000;
	std::unordered_set<uint64_t>	ever_fetched;
	std::deque<uint64_t>			ever_fetched_order;
	auto	fetched_key = [](const cache_key_t &k) -> uint64_t
	{
		return (((uint64_t)k.layer << 40) | ((uint64_t)k.kv_group << 24)
			| (uint64_t)k.block_id);
	};
	/* Returns true if `key64` was already marked fetched (a real
	 * redundant fetch); always marks it fetched (bounded, FIFO) for
	 * future checks either way. */
	auto	mark_fetched = [&](uint64_t key64) -> bool
	{
		bool	redundant = ever_fetched.count(key64) != 0;

		if (!redundant)
		{
			if (ever_fetched.size() >= kEverFetchedCap)
			{
				ever_fetched.erase(ever_fetched_order.front());
				ever_fetched_order.pop_front();
			}
			ever_fetched.insert(key64);
			ever_fetched_order.push_back(key64);
		}
		return (redundant);
	};

	std::vector<uint64_t>	layer_hits;
	std::vector<uint64_t>	layer_checks;
	std::vector<uint64_t>	head_hits;
	std::vector<uint64_t>	head_checks;
	if (out_layer_head != nullptr)
	{
		layer_hits.assign(trace.n_layer, 0);
		layer_checks.assign(trace.n_layer, 0);
		head_hits.assign(trace.n_head, 0);
		head_checks.assign(trace.n_head, 0);
	}

	if (out_coalescing != nullptr)
		*out_coalescing = coalescing_stats_t{};

	std::vector<double>	step_latencies;
	uint64_t	total_transferred_bytes = 0;
	uint64_t	total_hit_checks = 0;
	uint64_t	total_hits = 0;
	uint64_t	total_redundant_fetches = 0;
	uint64_t	total_wasted_prefetch_bytes = 0;
	uint64_t	total_working_set_blocks = 0;
	uint64_t	total_precision_num = 0;
	uint64_t	total_precision_den = 0;
	uint64_t	total_recall_num = 0;
	uint64_t	total_recall_den = 0;
	uint64_t	total_late_fetches = 0;
	uint64_t	total_prefetch_attempts = 0;
	uint64_t	total_prefetched_ok = 0;

	step_latencies.reserve(trace.step_count);
	if (out_steps != nullptr)
		out_steps->reserve(trace.step_count);
	for (uint32_t step = 0; step < trace.step_count; step++)
	{
		uint32_t	current_num_blocks = (trace.prompt_len + step + 1
				+ cfg.block_size_tokens - 1) / cfg.block_size_tokens;
		double	slack_ns = model.compute_ns_per_step;
		double	slack_bytes = cfg.disable_prefetch
			? 0.0 : bytes_that_fit(slack_ns, hw);
		uint64_t	prefetch_budget_used = 0;
		uint64_t	metadata_checks = 0;
		uint64_t	exposed_bytes = 0;

		size_t	pred_idx = 0;
		for (uint32_t l = 0; l < trace.n_layer; l++)
		{
			for (uint32_t g = 0; g < n_kv_group; g++, pred_idx++)
			{
				channel_predictor_t	&pred = predictors[pred_idx];
				std::vector<uint32_t>	ground_truth
					= ground_truth_blocks_via(at, trace.top_k, trace.n_head,
						step, l, g, group_size);
				std::vector<uint32_t>	predicted = pred.predict(cfg.policy,
					step, current_num_blocks, ground_truth);

				total_working_set_blocks += predicted.size();

				/* Prefetch dispatch: ranking order from the policy,
				 * bounded by this step's slack bandwidth budget. */
				for (uint32_t b : predicted)
				{
					cache_key_t	key{l, g, b};
					metadata_checks++;
					if (cache.contains(key))
						continue ;
					uint64_t	blk_bytes = (uint64_t)(bytes_per_channel_per_token
						* cfg.block_size_tokens / compression);
					if (prefetch_budget_used + blk_bytes > (uint64_t)slack_bytes)
						continue ;
					prefetch_budget_used += blk_bytes;
					total_prefetch_attempts++;
					bool	is_useful = vec_contains(ground_truth, b);
					if (!is_useful)
						total_wasted_prefetch_bytes += blk_bytes;
					else
						total_prefetched_ok++;
					if (mark_fetched(fetched_key(key)))
						total_redundant_fetches++;
					cache.insert(key, blk_bytes, 1.0);
				}

				/* Real need this step: ground truth blocks, checked
				 * against the (now possibly-freshly-prefetched) hot
				 * cache. */
				uint64_t	channel_hits_this_step = 0;
				std::vector<uint32_t>	missed_this_channel;
				for (uint32_t b : ground_truth)
				{
					cache_key_t	key{l, g, b};
					metadata_checks++;
					total_hit_checks++;
					if (out_layer_head != nullptr)
						layer_checks[l]++;
					if (cache.contains(key))
					{
						cache.touch_hit(key, 1.0);
						total_hits++;
						channel_hits_this_step++;
						if (out_layer_head != nullptr)
							layer_hits[l]++;
						continue ;
					}
					uint64_t	blk_bytes = (uint64_t)(bytes_per_channel_per_token
						* cfg.block_size_tokens / compression);
					exposed_bytes += blk_bytes;
					total_late_fetches++;
					if (out_coalescing != nullptr || out_layer_head != nullptr)
						missed_this_channel.push_back(b);
					if (mark_fetched(fetched_key(key)))
						total_redundant_fetches++;
					cache.insert(key, blk_bytes, 1.0);
				}
				if (out_coalescing != nullptr && !missed_this_channel.empty())
				{
					uint64_t	blk_bytes = (uint64_t)(bytes_per_channel_per_token
						* cfg.block_size_tokens / compression);
					out_coalescing->naive_request_count
						+= missed_this_channel.size();
					out_coalescing->real_needed_bytes
						+= missed_this_channel.size() * blk_bytes;
					/* Sorted (ground_truth already is): greedily group
					 * consecutive missed ids into one request whenever
					 * the gap to the next missed id is within the
					 * coalescing window -- the request then spans
					 * [group_min, group_max], paying for any non-missed
					 * blocks caught inside that span as real padding. */
					size_t	i = 0;
					while (i < missed_this_channel.size())
					{
						uint32_t	group_min = missed_this_channel[i];
						uint32_t	group_max = group_min;
						size_t	j = i + 1;
						while (j < missed_this_channel.size()
								&& missed_this_channel[j] - group_max
									<= cfg.coalescing_window)
						{
							group_max = missed_this_channel[j];
							j++;
						}
						out_coalescing->coalesced_request_count++;
						out_coalescing->transferred_bytes_with_padding
							+= (uint64_t)(group_max - group_min + 1) * blk_bytes;
						i = j;
					}
				}
				/* Per-head resolution: a head's individual block is
				 * "hit" only if it was ALREADY resident before this
				 * step's misses were serviced -- checked against
				 * `missed_this_channel` (captured during the
				 * ground_truth loop above, BEFORE any of this step's
				 * misses were inserted), not by re-querying the cache
				 * now, which would trivially read back 100% (every
				 * ground-truth block, hit or miss, is resident by the
				 * time this point in the loop is reached -- a real
				 * bug caught during this phase's own development: an
				 * earlier version re-queried post-insertion and
				 * reported every head at exactly 100% hit rate,
				 * always, which was the tell). */
				if (out_layer_head != nullptr)
				{
					for (uint32_t h = g * group_size;
							h < (g + 1) * group_size && h < trace.n_head; h++)
					{
						const membrane_attntrace_entry_t	*he
							= at(step, l, h);
						for (uint32_t k = 0; k < trace.top_k; k++)
						{
							if (he[k].block_id == UINT32_MAX)
								continue ;
							head_checks[h]++;
							if (!vec_contains(missed_this_channel,
									he[k].block_id))
								head_hits[h]++;
						}
					}
				}

				size_t	inter = 0;
				for (uint32_t b : predicted)
					if (vec_contains(ground_truth, b))
						inter++;
				total_precision_num += inter;
				total_precision_den += predicted.size();
				total_recall_num += inter;
				total_recall_den += ground_truth.size();

				pred.observe(step, ground_truth);
			}
		}

		total_transferred_bytes += prefetch_budget_used + exposed_bytes;
		if (out_steps != nullptr)
			out_steps->push_back({prefetch_budget_used, exposed_bytes});

		double	metadata_ns = metadata_checks * METADATA_LOOKUP_NS_PER_BLOCK
			+ metadata_checks * HOTCACHE_LOOKUP_NS_PER_BLOCK;
		/* NOTE: decompression time intentionally uses the single-
		 * pipeline rate here, matching the pre-Phase-6.4 hardcoded
		 * behavior exactly (transfer_ns's effective bandwidth above
		 * DOES scale with pipeline count; this term historically did
		 * not -- preserved as-is rather than silently changed, since
		 * changing it would shift every previously-reported Phase
		 * 6.2/6.3 number retroactively without a fresh measurement). */
		double	exposed_ns = exposed_bytes > 0
			? transfer_ns(exposed_bytes, hw)
			+ (double)exposed_bytes / hw.nearmem_pipeline_bytes_per_ns
			: 0.0;
		double	memory_ns = metadata_ns + exposed_ns;
		double	step_latency = std::max(model.compute_ns_per_step, memory_ns);
		step_latencies.push_back(step_latency);
	}

	res.policy_name = policy_name(cfg.policy);
	res.eviction_name = eviction_policy_name(cfg.eviction);
	res.block_size_tokens = cfg.block_size_tokens;
	res.hot_cache_bytes = cfg.hot_cache_bytes;
	res.p50_latency_ns = percentile(step_latencies, 0.50);
	res.p95_latency_ns = percentile(step_latencies, 0.95);
	res.p99_latency_ns = percentile(step_latencies, 0.99);
	res.mean_transferred_bytes_per_token = trace.step_count
		? (double)total_transferred_bytes / trace.step_count : 0.0;
	res.metadata_overhead_ns_per_token = trace.step_count
		? (double)(total_hit_checks) * (METADATA_LOOKUP_NS_PER_BLOCK
			+ HOTCACHE_LOOKUP_NS_PER_BLOCK) / trace.step_count : 0.0;
	res.hot_cache_hit_rate = total_hit_checks
		? (double)total_hits / total_hit_checks : 0.0;
	res.redundant_fetches = total_redundant_fetches;
	res.wasted_prefetch_bytes = total_wasted_prefetch_bytes;
	res.mean_working_set_blocks = trace.step_count
		? (double)total_working_set_blocks
			/ (trace.step_count * trace.n_layer * n_kv_group) : 0.0;
	res.precision = total_precision_den
		? (double)total_precision_num / total_precision_den : 0.0;
	res.recall = total_recall_den
		? (double)total_recall_num / total_recall_den : 0.0;
	res.prefetch_hit_rate = total_hit_checks
		? (double)(total_hit_checks - total_late_fetches) / total_hit_checks
		: 0.0;
	res.late_fetch_rate = total_hit_checks
		? (double)total_late_fetches / total_hit_checks : 0.0;
	res.false_prefetch_rate = total_prefetch_attempts
		? (double)(total_prefetch_attempts - total_prefetched_ok)
			/ total_prefetch_attempts : 0.0;
	res.additional_link_traffic_bytes = total_wasted_prefetch_bytes;

	if (out_layer_head != nullptr)
	{
		out_layer_head->per_layer_hit_rate.resize(trace.n_layer);
		for (uint32_t l = 0; l < trace.n_layer; l++)
			out_layer_head->per_layer_hit_rate[l] = layer_checks[l]
				? (double)layer_hits[l] / layer_checks[l] : 0.0;
		out_layer_head->per_head_hit_rate.resize(trace.n_head);
		for (uint32_t h = 0; h < trace.n_head; h++)
			out_layer_head->per_head_hit_rate[h] = head_checks[h]
				? (double)head_hits[h] / head_checks[h] : 0.0;
	}
	return (res);
}

static trace_view_t	view_of(const attn_trace_t &trace)
{
	return {trace.n_layer, trace.n_head, trace.step_count, trace.prompt_len,
		trace.top_k};
}

static trace_view_t	view_of(const trace_metadata_t &md)
{
	return {md.n_layer, md.n_head, md.step_count, md.prompt_len, md.top_k};
}

scenario_result_t	run_scenario_calibration(const attn_trace_t &trace,
						const model_calibration_t &model,
						const scenario_config_t &cfg,
						std::vector<per_step_calib_t> *out_steps,
						layer_head_stats_t *out_layer_head,
						coalescing_stats_t *out_coalescing,
						const hardware_profile_t *hw_in)
{
	return (run_scenario_calibration_impl(view_of(trace),
		[&trace](uint32_t step, uint32_t layer, uint32_t head)
		{ return (trace.at(step, layer, head)); },
		model, cfg, out_steps, out_layer_head, out_coalescing, hw_in));
}

scenario_result_t	run_scenario_calibration_streamed(
						attn_trace_reader_t &trace,
						const model_calibration_t &model,
						const scenario_config_t &cfg,
						std::vector<per_step_calib_t> *out_steps,
						layer_head_stats_t *out_layer_head,
						coalescing_stats_t *out_coalescing,
						const hardware_profile_t *hw_in)
{
	return (run_scenario_calibration_impl(view_of(trace.get_metadata()),
		[&trace](uint32_t step, uint32_t layer, uint32_t head)
		{ return (trace.at(step, layer, head)); },
		model, cfg, out_steps, out_layer_head, out_coalescing, hw_in));
}

scenario_result_t	run_scenario(const attn_trace_t &trace,
						const model_calibration_t &model,
						const scenario_config_t &cfg)
{
	return (run_scenario_calibration(trace, model, cfg, nullptr, nullptr));
}

}	/* namespace wssim */
