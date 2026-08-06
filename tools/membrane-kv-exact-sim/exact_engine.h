#ifndef MEMBRANE_EXACTSIM_ENGINE_H
#define MEMBRANE_EXACTSIM_ENGINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "calibrate.h"
#include "engine.h"

namespace exactsim
{

/*
 * Real, concurrent, capacity-bound exact-retrieval scenario -- reuses
 * tools/membrane-cxl-sim/sim_engine.h's k_server_resource_t for
 * genuine shared CXL-link/device-DRAM/quant-engine contention across
 * all `concurrency` sequences, processed in true global event-time
 * order (the same discrete-event design Phase 6.1 established),
 * replaying `profile`'s real calibrated per-step demand for each
 * sequence (small deterministic per-sequence jitter added so
 * concurrent sequences are not bit-identical clones -- same reasoning
 * as Phase 6.1's own synthetic-trace jitter).
 *
 * EXACT semantics, enforced structurally, not just asserted: no data
 * is ever dropped (device capacity is checked, never "solved" by
 * silently discarding a block); a sequence's step cannot complete
 * until its compulsory-miss fetch for that step (if any) actually
 * finishes -- see run_concurrent's implementation for exactly where
 * that dependency is enforced.
 */
struct concurrent_config_t
{
	uint32_t	concurrency;
	uint64_t	host_hot_cache_total_bytes;	/* reporting only -- the
			 * per-sequence split (host_hot_cache_total_bytes /
			 * concurrency) must already match what `profile` was
			 * calibrated with. */
	uint64_t	device_total_bytes;
	int			quant_pipelines;
	/* 0 = dispatch every compulsory-miss request immediately (no
	 * batching). >0 = real time-quantum micro-batching: requests
	 * arriving within the same `microbatch_max_wait_ns`-wide window
	 * are combined into one dispatch (split into
	 * `microbatch_max_batch_blocks`-sized chunks if the combined size
	 * would exceed that) -- section 8's max-wait/max-batch parameters,
	 * a disclosed simplification of true threshold-triggered batching
	 * (fixed time quanta instead of "whichever threshold hits first"),
	 * chosen because it composes cleanly with the discrete-event
	 * queue without a separate deferred-wakeup mechanism. */
	double		microbatch_max_wait_ns;
	uint32_t	microbatch_max_batch_blocks;
	/* Phase 6.4 item 9: hardware assumptions, now runtime-overridable
	 * (default_hardware_profile() reproduces the old hardcoded
	 * sim_config.h constants exactly). */
	wssim::hardware_profile_t	hw;
};

/* Phase 6.4 item 3: real capacity accounting, reported as its own
 * block, separate from latency/access metrics (spec's explicit
 * request -- "Capacity hesabi ile erisim/latency hesabini ayri
 * raporla"). */
struct capacity_report_t
{
	uint64_t	total_logical_kv_bytes;	/* uncompressed FP16, all
							 * `concurrency` sequences,
							 * full real context */
	uint64_t	physical_device_bytes;	/* after this scenario's real
							 * compression ratio */
	uint64_t	hot_cache_bytes;		/* host_hot_cache_total_bytes,
							 * unchanged, reported here
							 * for a single place to
							 * read every capacity
							 * number from */
	double		effective_capacity_ratio;	/* physical_device_bytes /
							 * total_logical_kv_bytes */
	uint64_t	sequences_fit;
	uint32_t	sequences_requested;
	std::string	capacity_failure_reason;	/* "none" | "device" |
							 * "host_cache_degraded" */
};

/* Phase 6.4 item 4: real per-request queue-wait percentiles, computed
 * analytically from each request's own (arrival, service_time,
 * completion) -- wait = completion - service_time - arrival -- no
 * change to k_server_resource_t's public API needed. Three separate
 * queues, matching Phase 6.1's own three-resource model (link,
 * device DRAM, quant/dequant engine), reintroduced here (Phase 6.3's
 * engine had collapsed device DRAM into the link for simplicity). */
struct queue_stats_t
{
	double		p50_wait_ns;
	double		p95_wait_ns;
	double		p99_wait_ns;
	double		utilization_pct;
	uint64_t	requests;
};

/* Phase 6.4 item 5: the worst N step-completions by exposed latency,
 * with enough detail to explain WHY each one was slow. Layer/head
 * identity is not available at this replay layer by design (the
 * calibrate-once-replay-many architecture, section 4/9 of the doc,
 * intentionally does not carry per-channel identity into the
 * replayed per-step scalar demand profile) -- per-layer/head detail
 * is reported separately from the single-sequence calibration pass
 * instead (task 25 / calibrate.h's layer_head_stats_t). */
struct tail_sample_t
{
	uint32_t	sequence_id;
	uint32_t	step_index;
	uint64_t	prefetch_bytes;
	uint64_t	compulsory_miss_bytes;
	double		link_wait_ns;
	double		device_wait_ns;
	double		quant_wait_ns;
	double		service_ns;
	double		total_latency_ns;
};

struct concurrent_result_t
{
	double		p50_latency_ns;
	double		p95_latency_ns;
	double		p99_latency_ns;
	double		tokens_per_sec;
	uint32_t	concurrency;
	double		mean_bytes_per_token;
	double		link_utilization_pct;
	double		quant_utilization_pct;
	std::string	bottleneck;
	/* Capacity-bound flags (kept for backward compatibility with
	 * Phase 6.3 call sites/CSV columns) -- capacity_report has the
	 * full real accounting these are derived from. */
	bool		host_capacity_bound;
	bool		device_capacity_bound;
	uint64_t	total_link_bytes;
	uint64_t	total_quant_bytes;

	capacity_report_t			capacity;
	queue_stats_t				link_queue;
	queue_stats_t				device_queue;
	queue_stats_t				quant_queue;
	uint64_t					max_simultaneous_fetches;
	double						mean_miss_burst_blocks;
	double						max_miss_burst_blocks;
	std::vector<tail_sample_t>	tail_samples;	/* worst N by total_latency_ns */

	/* Phase 6.5 item 13: compute-normalized latency -- p50/p95/p99_
	 * latency_ns above are the existing ABSOLUTE end-to-end figures
	 * the 10ms success bound is judged against (unchanged); these are
	 * an ADDITIONAL, purely explanatory breakdown of WHY, not a
	 * replacement bound. model_compute_floor_ns is the model's own
	 * single-thread decode compute floor (model.compute_ns_per_step,
	 * copied here so a CSV row is self-contained); incremental_kv_p99_ns
	 * is the p99 of max(0, total_latency_ns - model_compute_floor_ns)
	 * per completed step -- the KV-retrieval overhead EXCLUDING the
	 * compute floor, not total_p99_ns minus the floor (percentiles do
	 * not subtract linearly); hidden_under_compute_fraction is the
	 * fraction of completed steps where that value was exactly 0 (the
	 * step's real KV traffic fit entirely inside the previous step's
	 * compute-bound slack). */
	double						model_compute_floor_ns;
	double						incremental_kv_p99_ns;
	double						hidden_under_compute_fraction;
};

concurrent_result_t	run_concurrent(const calibrated_profile_t &profile,
							const wssim::model_calibration_t &model,
							uint32_t block_size_tokens,
							double compression_ratio,
							const concurrent_config_t &cfg,
							uint32_t tail_sample_count = 20);

}	/* namespace exactsim */

#endif
