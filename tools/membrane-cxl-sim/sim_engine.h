#ifndef MEMBRANE_CXL_SIM_ENGINE_H
#define MEMBRANE_CXL_SIM_ENGINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "sim_config.h"
#include "workload.h"

namespace sim
{

/*
 * A K-server FCFS queueing resource (link, device DRAM, quant engine,
 * ...). Requests are submitted in true global simulated-time order by
 * the caller (sim_engine's event loop processes ALL sequences'
 * events through one time-ordered priority queue, so this always
 * holds) -- given that, "earliest-free-server" is an exact, standard
 * closed-form way to reproduce M/M/K-style FCFS queueing dynamics
 * without a nested per-resource event loop: this is a real queueing
 * simulation, not a flat/instantaneous bandwidth formula. Backpressure
 * shows up honestly as rising completion latency under contention
 * (never a silent drop), matching this project's established
 * disclosure/behavior discipline from Phase 5.4's DMA bridge.
 */
class k_server_resource_t
{
public:
	k_server_resource_t(int k_servers, std::string name,
		double dispatch_overhead_ns = 0.0);

	/* Submits one request of `service_time_ns` duration, arriving at
	 * `now`. Returns the completion time. Updates internal server
	 * state and stats. */
	double	submit(double now, double service_time_ns);

	double	busy_ns() const { return (m_busy_ns); }
	uint64_t	requests() const { return (m_requests); }
	uint64_t	queued_requests() const { return (m_queued_requests); }
	double	total_wait_ns() const { return (m_total_wait_ns); }
	double	utilization(double sim_end_ns) const;
	const std::string	&name() const { return (m_name); }

private:
	std::string				m_name;
	double					m_dispatch_overhead_ns;
	std::vector<double>	m_busy_until;
	double					m_busy_ns;
	uint64_t				m_requests;
	uint64_t				m_queued_requests;
	double					m_total_wait_ns;
};

/* Bytes/latency service-time helper: base link/device latency plus
 * bytes/bandwidth (bandwidth in GB/s == bytes/ns, see sim_config.h). */
double	transfer_ns(const link_params_t &p, double bytes);

struct capacity_scenario_t
{
	std::string	name;
	uint64_t	host_bytes;
	uint64_t	device_bytes;	/* 0 for baselines with no device tier */
};

struct precision_mix_t
{
	std::string	name;		/* "fp16", "all-q8", "safe-mixed" */
	bool	warm_is_q8;			/* true unless mix forces something else */
	bool	cold_is_q4;
};

struct scenario_result_t
{
	baseline_t	baseline;
	std::string	capacity_name;
	std::string	mix_name;
	uint32_t	concurrency;
	uint32_t	context_len;

	uint64_t	sequences_fit;		/* how many of `concurrency` completed
									 * without exceeding capacity */
	double		p50_latency_ns;
	double		p95_latency_ns;
	double		p99_latency_ns;
	double		total_tokens_per_sec;
	uint64_t	effective_kv_capacity_bytes;	/* device_bytes, accounting
									 * for the tier's compression ratio */
	uint64_t	host_bytes_saved;	/* vs. an all-FP16-host baseline at the
									 * same concurrency/context */
	double		device_utilization_pct;
	double		link_utilization_pct;
	double		quant_engine_utilization_pct;
	std::string	bottleneck;			/* "compute" | "link" | "device_dram"
									 * | "quant_engine" | "capacity" */
	double		energy_estimate_joules;	/* explicitly assumption-based,
									 * see sim_config.h/doc */
};

/*
 * Runs one (baseline, capacity, mix, concurrency, context_len)
 * scenario to completion (every sequence either finishes its trace or
 * is marked capacity-exceeded) and returns aggregate metrics.
 * `heartbeat_cb`, if non-null, is invoked periodically (roughly every
 * simulated 60 real seconds of wall-clock progress, driven by the
 * caller's own wall-clock timer -- see main.cpp) with
 * (completed_steps, total_steps, queue_occupancy_estimate).
 */
/* `pipeline_override`, if > 0, overrides the near-memory quant engine's
 * default parallel-pipeline count (and, symmetrically, the CXL link's
 * server count, since a wider link is what a real deployment would add
 * alongside more pipelines) -- used by the pipeline-count sensitivity
 * sweep (task 128 / section 7's "required pipeline count") to show how
 * provisioning changes latency/throughput at fixed load. -1 (default)
 * uses each baseline's built-in default server count. */
scenario_result_t	run_scenario(baseline_t baseline,
						const capacity_scenario_t &capacity,
						const precision_mix_t &mix,
						const std::vector<sequence_trace_t> &traces,
						double prefetch_hit_rate,
						void (*heartbeat_cb)(uint64_t, uint64_t, void *),
						void *heartbeat_ctx,
						int pipeline_override = -1);

}	/* namespace sim */

#endif
