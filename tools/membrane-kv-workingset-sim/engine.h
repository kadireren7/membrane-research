#ifndef MEMBRANE_WSSIM_ENGINE_H
#define MEMBRANE_WSSIM_ENGINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "attn_trace_reader.h"
#include "attn_workload.h"
#include "hotcache.h"
#include "policy.h"
#include "wssim_config.h"

namespace wssim
{

/*
 * Real model geometry + real per-token byte rate needed to run a
 * scenario -- reused, not re-derived, from Phase 6.1's own real
 * measurements (docs/phase6-cxl-near-memory.md section 3 /
 * tools/membrane-cxl-sim/sim_config.h's SMOLLM2_* constants).
 */
struct model_calibration_t
{
	std::string	name;
	uint32_t	n_layer;
	uint32_t	n_head_kv;
	uint32_t	bytes_per_token_total;	/* REAL, all layers/heads, FP16 */
	double		compute_ns_per_step;	/* REAL end-to-end tok/s floor */
};

struct scenario_config_t
{
	policy_t			policy;
	eviction_policy_t	eviction;
	uint32_t			block_size_tokens;
	uint64_t			hot_cache_bytes;
	bool				warm_tier_is_q8;	/* fetched-tier compression */
	/* Phase 6.3: 0 disables coalescing bookkeeping (default,
	 * zero-overhead); >=1 groups a channel's compulsory-miss block
	 * ids that fall within this many block-slots of each other into
	 * one wider request spanning [min_id, max_id] -- real payload
	 * padding for any non-missed blocks caught inside that span is
	 * tracked as "wasted bytes" (see coalescing_stats_t). */
	uint32_t			coalescing_window = 0;
	/* Phase 6.4: FP16 ("no compression") precision mode -- when true,
	 * blk_bytes is computed with compression ratio 1.0 instead of
	 * Q8/Q4, matching the spec's 3-way precision sweep (FP16/all-Q8/
	 * safe-mixed). */
	bool				no_compression = false;
	/* Phase 6.4: separates "exact predictor" (ranking used only to
	 * decide what to KEEP once something is already being fetched;
	 * true miss-fetches happen but nothing is proactively fetched
	 * ahead of need) from "exact predictor + prefetch" (this policy's
	 * normal behavior) -- when true, the per-step prefetch bandwidth
	 * budget is forced to 0 regardless of compute-floor slack, so
	 * predicted-but-not-yet-needed blocks are never dispatched early. */
	bool				disable_prefetch = false;
};

struct scenario_result_t
{
	std::string	policy_name;
	std::string	eviction_name;
	uint32_t	block_size_tokens;
	uint64_t	hot_cache_bytes;

	double		p50_latency_ns;
	double		p95_latency_ns;
	double		p99_latency_ns;
	double		mean_transferred_bytes_per_token;
	double		metadata_overhead_ns_per_token;
	double		hot_cache_hit_rate;
	uint64_t	redundant_fetches;
	uint64_t	wasted_prefetch_bytes;
	double		mean_working_set_blocks;

	/* Prefetch-predictor quality (section 5). */
	double		precision;
	double		recall;
	double		prefetch_hit_rate;
	double		late_fetch_rate;
	double		false_prefetch_rate;
	uint64_t	additional_link_traffic_bytes;
};

/*
 * Runs one scenario end to end over `trace` (already regrouped to
 * cfg.block_size_tokens by the caller -- see
 * attn_workload.h's regroup_to_block_size) and returns aggregate
 * metrics. Implements the 8-stage per-decode-step pipeline: working-
 * set selection -> metadata lookup -> hot-cache lookup -> prefetch ->
 * CXL miss fetch -> decompression -> attention consumption ->
 * eviction (see docs/phase6-attention-working-set.md section on
 * simulator integration for the exact cost model and what it does
 * NOT model).
 */
scenario_result_t	run_scenario(const attn_trace_t &trace,
						const model_calibration_t &model,
						const scenario_config_t &cfg);

/* Phase 6.3: one decode step's real, causally-predicted demand on the
 * CXL link -- `prefetch_bytes` is dispatched ahead (hidden behind the
 * previous step's compute if bandwidth allows), `compulsory_miss_bytes`
 * is exposed synchronously (ground-truth blocks neither already hot
 * nor successfully prefetched in time). Used by
 * tools/membrane-kv-exact-sim to calibrate a REAL per-step miss-byte
 * trace from this single-sequence engine, then replay it inside a
 * genuine multi-sequence discrete-event simulation with real shared
 * CXL-link/quant-engine contention (reusing
 * tools/membrane-cxl-sim/sim_engine.h's k_server_resource_t) --
 * concurrent sequences share this same real calibrated per-step
 * demand profile rather than each independently re-running the full
 * per-channel predictor (see docs/phase6-exact-sparse-retrieval.md for
 * why: predicted working-set size for every non-FULL policy is
 * bounded, not context-dependent, so the profile itself is real and
 * representative, but re-deriving it independently per concurrent
 * sequence at 128K context x 512 concurrency is not tractable in this
 * session -- the same kind of calibrate-once-replay-many reduction
 * Phase 6.1 itself used for its own synthetic sweep). */
struct per_step_calib_t
{
	uint64_t	prefetch_bytes;
	uint64_t	compulsory_miss_bytes;
};

/* Per-layer and per-(query)-head hit rate -- layer resolution is a
 * real per-channel-group average; head resolution additionally checks
 * each individual query head's OWN real ground truth against whatever
 * its kv-head-group channel actually had cached (the cache decision
 * itself stays at channel/kv-group granularity, matching real GQA
 * physical KV storage -- see attn_workload.h -- but this metric
 * reports true per-head outcome, not just its group's average). */
struct layer_head_stats_t
{
	std::vector<double>	per_layer_hit_rate;
	std::vector<double>	per_head_hit_rate;
};

/* Real, block-id-level fetch-coalescing measurement (section 7 of the
 * Phase 6.3 spec) -- computed from the actual sorted compulsory-miss
 * block ids within each channel each step (not a byte-count
 * approximation), only when cfg.coalescing_window > 0. */
struct coalescing_stats_t
{
	uint64_t	naive_request_count;	/* one request per missed block */
	uint64_t	coalesced_request_count;	/* after grouping */
	uint64_t	real_needed_bytes;		/* == compulsory-miss bytes */
	uint64_t	transferred_bytes_with_padding;	/* includes span gaps */
};

/* Same engine as run_scenario, with optional additional outputs
 * (any may be nullptr to skip that bookkeeping). `hw`, if nullptr
 * (the default), uses default_hardware_profile() -- exactly the old
 * hardcoded sim_config.h/wssim_config.h constants, so every call site
 * that predates Phase 6.4's hardware-sensitivity matrix keeps behaving
 * identically. */
scenario_result_t	run_scenario_calibration(const attn_trace_t &trace,
						const model_calibration_t &model,
						const scenario_config_t &cfg,
						std::vector<per_step_calib_t> *out_steps,
						layer_head_stats_t *out_layer_head,
						coalescing_stats_t *out_coalescing = nullptr,
						const hardware_profile_t *hw = nullptr);

/*
 * Phase 6.5: identical algorithm to run_scenario_calibration() above --
 * both are thin wrappers around the SAME templated implementation in
 * engine.cpp (run_scenario_calibration_impl), so an out-of-core
 * (attn_trace_reader_t-backed) trace and a fully in-memory attn_trace_t
 * are guaranteed to run the exact same calibration code, not two
 * maintained copies of it. Only the trace access differs: this
 * overload reads through `trace.at()`, which pulls from the reader's
 * bounded chunk cache instead of one giant resident vector.
 */
scenario_result_t	run_scenario_calibration_streamed(
						attn_trace_reader_t &trace,
						const model_calibration_t &model,
						const scenario_config_t &cfg,
						std::vector<per_step_calib_t> *out_steps,
						layer_head_stats_t *out_layer_head,
						coalescing_stats_t *out_coalescing = nullptr,
						const hardware_profile_t *hw = nullptr);

}	/* namespace wssim */

#endif
