#include <algorithm>
#include <map>
#include <queue>
#include <vector>

#include "bounded_quantile.h"
#include "exact_engine.h"
#include "sim_config.h"
#include "sim_engine.h"

namespace exactsim
{

enum class event_kind_t { STEP, FLUSH, RELEASE };

struct event_t
{
	double			time;
	event_kind_t	kind;
	uint32_t		seq;	/* STEP only */
	uint32_t		step;	/* STEP only */
	uint64_t		quantum_id;	/* FLUSH only */
};

struct event_cmp_t
{
	bool	operator()(const event_t &a, const event_t &b) const
	{
		return (a.time > b.time);
	}
};

struct pending_req_t
{
	uint32_t	seq;
	uint32_t	step;
	uint64_t	bytes;
	double		compute_ready_ns;	/* this step's compute-bound floor */
};

static uint32_t	xorshift32(uint32_t *state)
{
	uint32_t	x = *state;

	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	*state = x;
	return (x);
}

/* Bounded (size `cap`) min-heap of the worst (largest total_latency_ns)
 * samples seen so far -- O(log cap) per candidate, O(cap) memory,
 * regardless of how many total steps are processed (up to tens of
 * millions at the full unified 128K x 512 scale, section 5's real
 * tractability constraint on this memory-limited machine). */
class tail_tracker_t
{
public:
	explicit tail_tracker_t(uint32_t cap) : m_cap(cap) {}

	void	offer(const tail_sample_t &s)
	{
		if (m_cap == 0)
			return ;
		if (m_heap.size() < m_cap)
		{
			m_heap.push_back(s);
			std::push_heap(m_heap.begin(), m_heap.end(), cmp);
			return ;
		}
		if (s.total_latency_ns > m_heap.front().total_latency_ns)
		{
			std::pop_heap(m_heap.begin(), m_heap.end(), cmp);
			m_heap.back() = s;
			std::push_heap(m_heap.begin(), m_heap.end(), cmp);
		}
	}

	std::vector<tail_sample_t>	sorted_desc() const
	{
		std::vector<tail_sample_t>	out = m_heap;
		std::sort(out.begin(), out.end(),
			[](const tail_sample_t &a, const tail_sample_t &b)
			{ return (a.total_latency_ns > b.total_latency_ns); });
		return (out);
	}

private:
	static bool	cmp(const tail_sample_t &a, const tail_sample_t &b)
	{
		return (a.total_latency_ns > b.total_latency_ns);	/* min-heap */
	}

	uint32_t					m_cap;
	std::vector<tail_sample_t>	m_heap;
};

/* Wraps a k_server_resource_t to additionally record each request's
 * real queueing wait time -- wait = completion - service_ns - arrival,
 * derivable without touching k_server_resource_t's own API/ABI (which
 * tools/membrane-cxl-sim also depends on). Wait samples are only ever
 * recorded for requests that actually dispatch (misses/prefetches),
 * not every decode step, so memory stays bounded by real traffic
 * volume, not total step count -- but at the full unified 128K x 512
 * scale, real traffic volume is itself tens of millions of dispatches
 * (prefetch alone can fire nearly every step, for every one of 512
 * sequences), so this used to be a plain std::vector<double> per
 * resource -- THREE of them (link/device_dram/quant_engine) -- which
 * is exactly what a real Phase 6.5 smoke test caught overshooting a
 * declared --memory-budget-mib well past what
 * bounded_quantile_accumulator_t's fix to seq_latencies/all_latencies
 * alone accounted for (see docs/phase6-out-of-core-simulator.md).
 * Phase 6.5: same bounded (spill-if-needed) exact accumulator as
 * total/kv-overhead latency now backs this too. */
struct tracked_resource_t
{
	sim::k_server_resource_t		res;
	bounded_quantile_accumulator_t	wait_acc;

	explicit tracked_resource_t(int k, const char *name)
		: res(k, name, 0.0) {}

	double	submit(double now, double service_ns)
	{
		double	completion = res.submit(now, service_ns);
		double	wait = completion - service_ns - now;
		wait_acc.add(wait > 0.0 ? wait : 0.0);
		return (completion);
	}

	queue_stats_t	stats(double sim_end_ns)
	{
		queue_stats_t	out{};

		wait_acc.finish(&out.p50_wait_ns, &out.p95_wait_ns, &out.p99_wait_ns);
		out.utilization_pct = res.utilization(sim_end_ns);
		out.requests = res.requests();
		return (out);
	}
};

concurrent_result_t	run_concurrent(const calibrated_profile_t &profile,
						const wssim::model_calibration_t &model,
						uint32_t block_size_tokens,
						double compression_ratio,
						const concurrent_config_t &cfg,
						uint32_t tail_sample_count)
{
	concurrent_result_t	out{};
	uint32_t				N = cfg.concurrency > 0 ? cfg.concurrency : 1;
	const wssim::hardware_profile_t	&hw = cfg.hw;

	int	pipelines = cfg.quant_pipelines > 0 ? cfg.quant_pipelines : 1;
	tracked_resource_t	link(pipelines, "exact-cxl-link");
	tracked_resource_t	device_dram(pipelines, "exact-device-dram");
	tracked_resource_t	quant_engine(pipelines, "exact-quant-engine");

	uint64_t	per_seq_device_budget = cfg.device_total_bytes / N;
	uint64_t	bytes_per_channel_per_token = (model.n_layer && model.n_head_kv)
		? model.bytes_per_token_total / (model.n_layer * model.n_head_kv) : 0;
	uint64_t	bytes_per_block = bytes_per_channel_per_token
		* block_size_tokens / (compression_ratio > 0.0 ? compression_ratio : 1.0);

	/* Phase 6.5 item 5/13: bounded (spill-if-needed) exact quantile
	 * accumulators replace what used to be a
	 * std::vector<std::vector<double>> holding EVERY completed step's
	 * latency (up to ~536 MiB at the full unified 128K x 512 scale) --
	 * see bounded_quantile.h. record_latency() below is the single
	 * place every completion path (zero-miss, immediate-dispatch
	 * miss, and flush_quantum's micro-batched completions) reports
	 * through, so both the existing total-latency percentiles and the
	 * new compute-normalized KV-overhead percentile (item 13) come
	 * from the exact same real per-step samples. */
	bounded_quantile_accumulator_t		total_latency_acc;
	bounded_quantile_accumulator_t		kv_overhead_acc;
	uint64_t							hidden_under_compute_count = 0;
	auto	record_latency = [&](double total_latency_ns)
	{
		double	kv_overhead = total_latency_ns - model.compute_ns_per_step;

		if (kv_overhead < 0.0)
			kv_overhead = 0.0;
		if (kv_overhead == 0.0)
			hidden_under_compute_count++;
		total_latency_acc.add(total_latency_ns);
		kv_overhead_acc.add(kv_overhead);
	};
	std::vector<bool>					seq_capacity_exceeded(N, false);

	std::priority_queue<event_t, std::vector<event_t>, event_cmp_t>	pq;
	std::map<uint64_t, std::vector<pending_req_t>>						pending;
	uint64_t	total_link_bytes = 0;
	uint64_t	total_transferred_bytes = 0;
	uint64_t	completed_steps = 0;
	uint64_t	total_steps_all_seqs = 0;
	double		sim_end_ns = 0.0;
	uint32_t	jitter_state = 0xabcdef01u;

	uint64_t	active_fetches = 0;
	uint64_t	max_simultaneous_fetches = 0;
	uint64_t	burst_step_count = 0;
	double		sum_burst_blocks = 0.0;
	double		max_burst_blocks = 0.0;
	tail_tracker_t	tail(tail_sample_count);

	double	quantum = cfg.microbatch_max_wait_ns > 0.0
		? cfg.microbatch_max_wait_ns : 1.0;
	uint64_t	max_batch_bytes = cfg.microbatch_max_batch_blocks > 0
		? (uint64_t)cfg.microbatch_max_batch_blocks * bytes_per_block
		: UINT64_MAX;

	for (uint32_t s = 0; s < N; s++)
	{
		pq.push({0.0, event_kind_t::STEP, s, 0, 0});
		total_steps_all_seqs += profile.steps.size();
	}

	auto	do_fetch = [&](double now, uint64_t bytes,
			double *link_wait, double *dev_wait, double *quant_wait)
			-> double
	{
		active_fetches++;
		max_simultaneous_fetches = std::max(max_simultaneous_fetches,
			active_fetches);
		double	link_service = hw.cxl_link_latency_ns
			+ (double)bytes / hw.cxl_link_bandwidth_gbps;
		double	t1 = link.submit(now, link_service);
		if (link_wait != nullptr)
			*link_wait = t1 - link_service - now;
		double	dev_service = sim::DEVICE_DRAM_LATENCY_NS
			+ (double)bytes / sim::DEVICE_DRAM_BANDWIDTH_GBPS;
		double	t2 = device_dram.submit(t1, dev_service);
		if (dev_wait != nullptr)
			*dev_wait = t2 - dev_service - t1;
		double	quant_service = (double)bytes
			/ (hw.nearmem_pipeline_bytes_per_ns * hw.quant_pipelines);
		double	t3 = quant_engine.submit(t2, quant_service);
		if (quant_wait != nullptr)
			*quant_wait = t3 - quant_service - t2;
		active_fetches--;
		return (t3);
	};

	/* Flushes every request that landed in quantum `qid`, dispatching
	 * either one combined request (if it fits under max_batch_bytes)
	 * or several chunked ones -- either way through the SAME shared
	 * resources, so genuine cross-sequence contention (including
	 * contention this microbatching itself creates) is reflected in
	 * every request's real completion time. */
	auto	flush_quantum = [&](uint64_t qid, double now)
	{
		auto	it = pending.find(qid);
		if (it == pending.end())
			return ;
		std::vector<pending_req_t>	&reqs = it->second;
		size_t	i = 0;
		while (i < reqs.size())
		{
			uint64_t	batch_bytes = 0;
			size_t	j = i;
			while (j < reqs.size() && batch_bytes + reqs[j].bytes
					<= max_batch_bytes)
			{
				batch_bytes += reqs[j].bytes;
				j++;
			}
			if (j == i)
			{
				batch_bytes = reqs[i].bytes;
				j = i + 1;
			}
			total_link_bytes += batch_bytes;
			double	lw = 0.0, dw = 0.0, qw = 0.0;
			double	completion = do_fetch(now, batch_bytes, &lw, &dw, &qw);
			for (size_t k = i; k < j; k++)
			{
				double	step_complete = std::max(reqs[k].compute_ready_ns,
					completion);
				record_latency(step_complete - reqs[k].compute_ready_ns
					+ model.compute_ns_per_step);
				sim_end_ns = std::max(sim_end_ns, step_complete);
				completed_steps++;
				pq.push({step_complete, event_kind_t::STEP, reqs[k].seq,
					reqs[k].step + 1, 0});
			}
			i = j;
		}
		pending.erase(it);
	};

	while (!pq.empty())
	{
		event_t	ev = pq.top();
		pq.pop();
		if (ev.kind == event_kind_t::FLUSH)
		{
			flush_quantum(ev.quantum_id, ev.time);
			continue ;
		}
		uint32_t	s = ev.seq;
		if (seq_capacity_exceeded[s] || ev.step >= profile.steps.size())
			continue ;

		uint64_t	device_needed = (uint64_t)((double)model.bytes_per_token_total
			* (double)(profile.context_tokens - profile.steps.size() + ev.step + 1)
			/ compression_ratio);
		if (device_needed > per_seq_device_budget)
		{
			seq_capacity_exceeded[s] = true;
			continue ;
		}

		const wssim::per_step_calib_t	&step = profile.steps[ev.step];
		xorshift32(&jitter_state);
		double	jitter = 1.0 + (((double)(jitter_state % 2001) - 1000.0)
			/ 1000.0) * 0.03;
		uint64_t	prefetch_bytes = (uint64_t)((double)step.prefetch_bytes
			* jitter);
		uint64_t	miss_bytes = (uint64_t)((double)step.compulsory_miss_bytes
			* jitter);
		total_transferred_bytes += prefetch_bytes + miss_bytes;

		if (miss_bytes > 0 && bytes_per_block > 0)
		{
			double	blocks = (double)miss_bytes / (double)bytes_per_block;
			burst_step_count++;
			sum_burst_blocks += blocks;
			max_burst_blocks = std::max(max_burst_blocks, blocks);
		}

		/* Prefetch: real link/device/quant traffic, contends for
		 * resources, but does NOT gate this step's own completion (it
		 * was dispatched during the previous step's slack -- exactly
		 * what "prefetch" means; see wssim::engine.cpp for where that
		 * slack accounting happens during calibration). */
		if (prefetch_bytes > 0)
		{
			total_link_bytes += prefetch_bytes;
			do_fetch(ev.time, prefetch_bytes, nullptr, nullptr, nullptr);
		}

		double	compute_ready = ev.time;
		if (miss_bytes == 0)
		{
			double	step_complete = compute_ready + model.compute_ns_per_step;
			record_latency(step_complete - compute_ready);
			sim_end_ns = std::max(sim_end_ns, step_complete);
			completed_steps++;
			pq.push({step_complete, event_kind_t::STEP, s, ev.step + 1, 0});
			continue ;
		}
		if (cfg.microbatch_max_wait_ns <= 0.0)
		{
			/* No micro-batching: dispatched immediately, still
			 * through the SHARED resources (real contention), and
			 * attention cannot complete until this fetch's real
			 * completion -- the exact-mode dependency item 9
			 * requires. */
			total_link_bytes += miss_bytes;
			double	lw = 0.0, dw = 0.0, qw = 0.0;
			double	completion = do_fetch(ev.time, miss_bytes, &lw, &dw, &qw);
			double	step_complete = std::max(compute_ready
				+ model.compute_ns_per_step, completion);
			double	total_lat = step_complete - compute_ready;
			record_latency(total_lat);
			sim_end_ns = std::max(sim_end_ns, step_complete);
			completed_steps++;
			tail.offer({s, ev.step, prefetch_bytes, miss_bytes, lw, dw, qw,
				completion - ev.time, total_lat});
			pq.push({step_complete, event_kind_t::STEP, s, ev.step + 1, 0});
			continue ;
		}
		/* Micro-batching: enqueue into this time quantum's pending
		 * list; schedule (once per quantum) a FLUSH event at the
		 * quantum's end boundary. */
		uint64_t	qid = (uint64_t)(ev.time / quantum);
		bool	first_in_quantum = pending.find(qid) == pending.end();
		pending[qid].push_back({s, ev.step, miss_bytes,
			compute_ready + model.compute_ns_per_step});
		if (first_in_quantum)
			pq.push({(double)(qid + 1) * quantum, event_kind_t::FLUSH,
				0, 0, qid});
	}

	uint64_t	fit_count = 0;
	for (uint32_t s = 0; s < N; s++)
		if (!seq_capacity_exceeded[s])
			fit_count++;

	total_latency_acc.finish(&out.p50_latency_ns, &out.p95_latency_ns,
		&out.p99_latency_ns);
	{
		double	unused_p50;
		double	unused_p95;

		kv_overhead_acc.finish(&unused_p50, &unused_p95,
			&out.incremental_kv_p99_ns);
	}
	out.model_compute_floor_ns = model.compute_ns_per_step;
	out.hidden_under_compute_fraction = completed_steps
		? (double)hidden_under_compute_count / (double)completed_steps : 0.0;

	out.tokens_per_sec = (sim_end_ns > 0.0 && fit_count > 0)
		? (double)completed_steps / (sim_end_ns / 1.0e9) : 0.0;
	out.concurrency = N;
	out.mean_bytes_per_token = total_steps_all_seqs
		? (double)total_transferred_bytes / total_steps_all_seqs : 0.0;
	out.link_utilization_pct = link.res.utilization(sim_end_ns);
	out.quant_utilization_pct = quant_engine.res.utilization(sim_end_ns);
	out.total_link_bytes = total_link_bytes;
	out.total_quant_bytes = total_link_bytes;
	out.host_capacity_bound = profile.hit_rate < 0.999;
	out.device_capacity_bound = fit_count < N;

	out.link_queue = link.stats(sim_end_ns);
	out.device_queue = device_dram.stats(sim_end_ns);
	out.quant_queue = quant_engine.stats(sim_end_ns);
	out.max_simultaneous_fetches = max_simultaneous_fetches;
	out.mean_miss_burst_blocks = burst_step_count
		? sum_burst_blocks / (double)burst_step_count : 0.0;
	out.max_miss_burst_blocks = max_burst_blocks;
	out.tail_samples = tail.sorted_desc();

	/* Capacity accounting, section 3/23: reported separately from the
	 * latency/queueing metrics above. */
	out.capacity.sequences_requested = N;
	out.capacity.sequences_fit = fit_count;
	out.capacity.hot_cache_bytes = cfg.host_hot_cache_total_bytes;
	out.capacity.total_logical_kv_bytes = (uint64_t)model.bytes_per_token_total
		* (uint64_t)profile.context_tokens * (uint64_t)N;
	out.capacity.physical_device_bytes = (uint64_t)((double)
		out.capacity.total_logical_kv_bytes / compression_ratio);
	out.capacity.effective_capacity_ratio = out.capacity.total_logical_kv_bytes
		? (double)cfg.device_total_bytes * compression_ratio
			/ (double)out.capacity.total_logical_kv_bytes
		: 0.0;
	if (fit_count < N)
		out.capacity.capacity_failure_reason = "device";
	else if (profile.hit_rate < 0.999)
		out.capacity.capacity_failure_reason = "host_cache_degraded";
	else
		out.capacity.capacity_failure_reason = "none";

	double	max_util = out.link_utilization_pct;
	out.bottleneck = "link";
	if (out.device_queue.utilization_pct > max_util)
	{
		max_util = out.device_queue.utilization_pct;
		out.bottleneck = "device_dram";
	}
	if (out.quant_utilization_pct > max_util)
	{
		max_util = out.quant_utilization_pct;
		out.bottleneck = "quant_engine";
	}
	if (fit_count < N)
		out.bottleneck = "device_capacity";
	else if (out.host_capacity_bound && max_util < 5.0)
		out.bottleneck = "host_cache_capacity";
	else if (max_util < 5.0)
		out.bottleneck = "compute";
	return (out);
}

}	/* namespace exactsim */
