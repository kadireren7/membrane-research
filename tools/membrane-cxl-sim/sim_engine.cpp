#include <algorithm>
#include <cmath>
#include <deque>
#include <queue>
#include <vector>

#include "sim_engine.h"

namespace sim
{

k_server_resource_t::k_server_resource_t(int k_servers, std::string name,
		double dispatch_overhead_ns)
	: m_name(std::move(name)), m_dispatch_overhead_ns(dispatch_overhead_ns),
	  m_busy_ns(0.0), m_requests(0), m_queued_requests(0), m_total_wait_ns(0.0)
{
	m_busy_until.assign(k_servers > 0 ? (size_t)k_servers : 1, 0.0);
}

double	k_server_resource_t::submit(double now, double service_time_ns)
{
	size_t	best;
	size_t	i;
	double	start;
	double	total_service;

	best = 0;
	i = 1;
	while (i < m_busy_until.size())
	{
		if (m_busy_until[i] < m_busy_until[best])
			best = i;
		i++;
	}
	start = std::max(now, m_busy_until[best]);
	if (start > now)
	{
		m_queued_requests++;
		m_total_wait_ns += (start - now);
	}
	total_service = m_dispatch_overhead_ns + service_time_ns;
	m_busy_until[best] = start + total_service;
	m_busy_ns += total_service;
	m_requests++;
	return (m_busy_until[best]);
}

double	k_server_resource_t::utilization(double sim_end_ns) const
{
	double	capacity_ns;

	capacity_ns = sim_end_ns * (double)m_busy_until.size();
	if (capacity_ns <= 0.0)
		return (0.0);
	return (100.0 * m_busy_ns / capacity_ns);
}

double	transfer_ns(const link_params_t &p, double bytes)
{
	return (p.base_latency_ns + bytes / p.bandwidth_gbps);
}

/* ---- per-sequence tiered residency state ---- */
struct seq_runtime_t
{
	std::deque<uint32_t>	hot_q;
	std::deque<uint32_t>	warm_q;
	std::deque<uint32_t>	cold_q;
	uint64_t				hot_bytes = 0;
	uint64_t				warm_bytes = 0;
	uint64_t				cold_bytes = 0;
	uint64_t				hot_budget = 0;
	uint64_t				device_budget = 0;
	bool					exceeded = false;
	uint32_t				exceeded_at_step = 0;
	std::vector<double>	latencies_ns;
};

/* Resource bundle + per-baseline behavior flags, resolved once per
 * scenario run. See docs/phase6-cxl-near-memory.md section 4 for why
 * each baseline is wired the way it is -- summarized in sim_config.h's
 * per-constant comments. */
struct resources_t
{
	baseline_t			kind;
	k_server_resource_t	link;
	k_server_resource_t	device_dram;
	k_server_resource_t	quant_engine;
	bool				device_capable;	/* can demote beyond hot at all */
	bool				compress;			/* does demotion shrink bytes */
	bool				quant_on_device;	/* 1-hop (device) vs 2-hop (host round trip) */
	double				quant_ns_per_block;
	double				dequant_ns_per_block;
	double				link_bytes_total = 0.0;
	double				device_bytes_total = 0.0;
};

static resources_t	make_resources(baseline_t b, int pipeline_override)
{
	switch (b)
	{
	case baseline_t::HOST_ONLY:
		return (resources_t{ b,
			k_server_resource_t(1, "unused-link"),
			k_server_resource_t(1, "unused-dev"),
			k_server_resource_t(1, "unused-quant"),
			false, false, false, 0.0, 0.0 });
	case baseline_t::CPU_RAM_OFFLOAD:
		return (resources_t{ b,
			k_server_resource_t(4, "host-ram-link"),
			k_server_resource_t(4, "host-ram-link"), /* same fabric, no extra hop */
			k_server_resource_t(1, "unused-quant"),
			true, false, false, 0.0, 0.0 });
	case baseline_t::NVME_OFFLOAD:
		return (resources_t{ b,
			k_server_resource_t(16, "nvme-link"),
			k_server_resource_t(16, "nvme-link"),
			k_server_resource_t(1, "unused-quant"),
			true, false, false, 0.0, 0.0 });
	case baseline_t::PCIE_FPGA_ROUNDTRIP:
		return (resources_t{ b,
			k_server_resource_t(1, "pcie-link", PCIE_ROUNDTRIP_ASSUMED_NS),
			k_server_resource_t(1, "unused-dev"), /* data stays host-resident */
			k_server_resource_t(1, "pcie-fpga-quant"),
			false /* no capacity extension, Phase 5.4's own finding */,
			true, false /* 2-hop: host<->FPGA round trip */,
			PCIE_FPGA_NS_PER_BLOCK, PCIE_FPGA_NS_PER_BLOCK });
	case baseline_t::CXL_NO_PROCESSING:
		return (resources_t{ b,
			k_server_resource_t(4, "cxl-link", 0.0),
			k_server_resource_t(4, "device-dram"),
			k_server_resource_t(1, "unused-quant"),
			true, false /* no compression: literally "without processing" */,
			false, 0.0, 0.0 });
	case baseline_t::MEMBRANE_CXL_NEAR_MEMORY:
	{
		/* Default provisioning: 8 parallel wide (512-bit, Phase 5.3)
		 * quant pipelines and an 8-lane-class CXL link -- a disclosed
		 * DESIGN CHOICE for a memory-expansion appliance meant to serve
		 * hundreds of concurrent sequences, not a measurement. Section
		 * 7's pipeline-count sensitivity sweep (main.cpp) varies this
		 * explicitly via pipeline_override to show how latency/fit
		 * change with provisioning instead of asserting one number is
		 * "enough". */
		int	k = pipeline_override > 0 ? pipeline_override : 8;

		return (resources_t{ b,
			k_server_resource_t(k, "cxl-link", 0.0),
			k_server_resource_t(k, "device-dram"),
			k_server_resource_t(k, "nearmem-quant"),
			true, true, true /* 1-hop: quantize/dequantize happens en route */,
			BYTES_PER_BLOCK / NEARMEM_PIPELINE_BYTES_PER_NS,
			BYTES_PER_BLOCK / NEARMEM_PIPELINE_BYTES_PER_NS });
	}
	default:
		return (resources_t{ b,
			k_server_resource_t(1, "unused"),
			k_server_resource_t(1, "unused"),
			k_server_resource_t(1, "unused"),
			false, false, false, 0.0, 0.0 });
	}
}

static double	link_transfer(resources_t &r, double now, double bytes)
{
	link_params_t	lp;

	if (r.kind == baseline_t::CPU_RAM_OFFLOAD)
		lp = { HOST_RAM_LATENCY_NS, HOST_RAM_BANDWIDTH_GBPS };
	else if (r.kind == baseline_t::NVME_OFFLOAD)
		lp = { NVME_LATENCY_NS, NVME_BANDWIDTH_GBPS };
	else if (r.kind == baseline_t::PCIE_FPGA_ROUNDTRIP)
		lp = { 0.0, /* PCIe raw transfer at Gen4 x16 class rate */ 31.5 };
	else
		lp = { CXL_LINK_LATENCY_NS, CXL_LINK_BANDWIDTH_GBPS };
	r.link_bytes_total += bytes;
	return (r.link.submit(now, transfer_ns(lp, bytes)));
}

static double	device_access(resources_t &r, double now, double bytes)
{
	link_params_t	lp = { DEVICE_DRAM_LATENCY_NS, DEVICE_DRAM_BANDWIDTH_GBPS };

	if (r.kind == baseline_t::CPU_RAM_OFFLOAD || r.kind == baseline_t::NVME_OFFLOAD)
		return (now); /* folded into the link hop for these baselines */
	r.device_bytes_total += bytes;
	return (r.device_dram.submit(now, transfer_ns(lp, bytes)));
}

static double	quant_op(resources_t &r, double now, double bytes, bool encode)
{
	double	blocks;
	double	ns_per_block;

	if (!r.compress)
		return (now);
	blocks = bytes / BYTES_PER_BLOCK;
	ns_per_block = encode ? r.quant_ns_per_block : r.dequant_ns_per_block;
	return (r.quant_engine.submit(now, blocks * ns_per_block));
}

/* Demotes the oldest resident hot position to warm (compressing it if
 * this baseline compresses), paying quantize + transfer cost. Returns
 * the completion time of this background transfer (not necessarily on
 * the critical path -- see run_scenario's comment on why demotion
 * doesn't block the current step). */
static double	demote_hot_to_warm(resources_t &r, seq_runtime_t &s, double now,
					bool warm_compress)
{
	uint32_t	raw;
	double	t;
	uint64_t	compressed;
	bool		do_compress = r.compress && warm_compress;

	raw = s.hot_q.front();
	s.hot_q.pop_front();
	s.hot_bytes -= raw;
	t = now;
	if (r.quant_on_device)
	{
		/* 1 hop: raw bytes cross the link to reach the device, THEN
		 * are compressed on-device -- no separate return trip. */
		t = link_transfer(r, t, raw);
		if (do_compress)
			t = quant_op(r, t, raw, true);
	}
	else if (do_compress)
	{
		/* 2 hops: host must quantize first (host-side compute), THEN
		 * send the already-compressed bytes to wherever they live. */
		t = quant_op(r, t, raw, true);
		t = link_transfer(r, t, raw / Q8_COMPRESSION_RATIO);
	}
	else
		t = link_transfer(r, t, raw);
	compressed = do_compress
		? (uint64_t)((double)raw / Q8_COMPRESSION_RATIO) : raw;
	t = device_access(r, t, compressed);
	s.warm_q.push_back((uint32_t)compressed);
	s.warm_bytes += compressed;
	return (t);
}

static double	demote_warm_to_cold(resources_t &r, seq_runtime_t &s, double now,
					bool warm_compress, bool cold_compress)
{
	uint32_t	warm_item;
	double	t;
	uint64_t	cold_item;
	double	relative_shrink;
	/* A warm item that was never compressed (mix.warm_is_q8 == false)
	 * has nothing further to shrink by the Q8->Q4 relative ratio --
	 * only apply that step when the item actually arrived as Q8 AND
	 * this mix wants Q4 cold storage. */
	bool		do_compress = r.compress && warm_compress && cold_compress;

	warm_item = s.warm_q.front();
	s.warm_q.pop_front();
	s.warm_bytes -= warm_item;
	t = now;
	relative_shrink = Q4_COMPRESSION_RATIO / Q8_COMPRESSION_RATIO;
	cold_item = do_compress
		? (uint64_t)((double)warm_item / relative_shrink) : warm_item;
	if (do_compress)
		t = quant_op(r, t, warm_item, true);
	t = device_access(r, t, cold_item);
	s.cold_q.push_back((uint32_t)cold_item);
	s.cold_bytes += cold_item;
	return (t);
}

/* Makes room for `need_bytes` more in the hot tier by demoting the
 * oldest resident positions until it fits or nothing is left to
 * demote; then, if the device tier itself is over budget, further
 * demotes warm->cold. Marks the sequence capacity_exceeded if even a
 * fully-cold device tier can't make room (or this baseline has no
 * device at all). `mix` selects whether warm/cold demotion actually
 * compresses at all (the "fp16" precision mix keeps everything raw
 * even off the fast tier -- capacity-extension-only, no compression
 * efficiency -- while "all-q8"/"safe-mixed" apply Phase 5.4's real
 * measured ratios, see sim_config.h). */
static void	make_room(resources_t &r, seq_runtime_t &s, double now,
				uint64_t need_bytes, uint32_t step, const precision_mix_t &mix)
{
	/* Baselines with no device tier (HOST_ONLY, PCIE_FPGA_ROUNDTRIP --
	 * the latter's data is always host-resident, see make_resources'
	 * comment) have nowhere to demote to at all: skip the eviction
	 * attempt entirely rather than pushing into warm_q/warm_bytes,
	 * which nothing downstream ever checks a budget against for these
	 * baselines and would just corrupt bookkeeping for no purpose. */
	if (r.device_capable)
		while (s.hot_bytes + need_bytes > s.hot_budget && !s.hot_q.empty())
			demote_hot_to_warm(r, s, now, mix.warm_is_q8);
	if (s.hot_bytes + need_bytes > s.hot_budget)
	{
		/* Either this baseline has no device to spill to at all, or it
		 * does but a fully-drained hot tier (hot_q empty, hot_bytes
		 * back to 0) still can't fit this one write -- a single step's
		 * KV growth bigger than the entire per-sequence hot budget, an
		 * unrealistic edge case at any sane budget. Either way, this is
		 * the real, final capacity ceiling for this sequence. */
		s.exceeded = true;
		s.exceeded_at_step = step;
		return;
	}
	while (s.warm_bytes + s.cold_bytes > s.device_budget && !s.warm_q.empty())
		demote_warm_to_cold(r, s, now, mix.warm_is_q8, mix.cold_is_q4);
	if (s.warm_bytes + s.cold_bytes > s.device_budget && !s.cold_q.empty())
	{
		/* Device tier itself is full even fully compressed: this is
		 * the real, final capacity ceiling. */
		s.exceeded = true;
		s.exceeded_at_step = step;
	}
}

struct step_event_t
{
	double		time;
	uint32_t	seq;
	uint32_t	step;
};

struct event_cmp_t
{
	bool	operator()(const step_event_t &a, const step_event_t &b) const
	{
		return (a.time > b.time);
	}
};

scenario_result_t	run_scenario(baseline_t baseline,
						const capacity_scenario_t &capacity,
						const precision_mix_t &mix,
						const std::vector<sequence_trace_t> &traces,
						double prefetch_hit_rate,
						void (*heartbeat_cb)(uint64_t, uint64_t, void *),
						void *heartbeat_ctx,
						int pipeline_override)
{
	resources_t							r = make_resources(baseline, pipeline_override);
	std::vector<seq_runtime_t>				states(traces.size());
	std::priority_queue<step_event_t, std::vector<step_event_t>, event_cmp_t>	pq;
	scenario_result_t						out;
	uint64_t								total_steps = 0;
	uint64_t								completed_steps = 0;
	double									sim_end_ns = 0.0;
	uint32_t								hb_state = 0x9e3779b9u;

	for (size_t i = 0; i < traces.size(); i++)
	{
		states[i].hot_budget = capacity.host_bytes / std::max<size_t>(traces.size(), 1);
		states[i].device_budget = capacity.device_bytes / std::max<size_t>(traces.size(), 1);
		total_steps += traces[i].step_bytes.size();
		pq.push({ 0.0, (uint32_t)i, 0 });
	}
	while (!pq.empty())
	{
		step_event_t	ev = pq.top();
		pq.pop();
		seq_runtime_t	&s = states[ev.seq];
		const sequence_trace_t	&trace = traces[ev.seq];

		if (s.exceeded || ev.step >= trace.step_bytes.size())
			continue;
		uint32_t	new_bytes = trace.step_bytes[ev.step];
		double		t = ev.time;

		/* READ side: attention must re-touch every warm/cold byte this
		 * sequence currently holds. A monotonic-prefetch hit hides this
		 * behind the previous step's compute (still consumes real link/
		 * device bandwidth, tracked in resource stats, but doesn't gate
		 * this step's critical path); a miss blocks and additionally
		 * wastes bandwidth on the mispredicted fetch (section 5/6's
		 * "wrong prefetch cost"). */
		uint64_t	outstanding = s.warm_bytes + s.cold_bytes;
		double		read_complete = t;
		if (outstanding > 0 && r.device_capable)
		{
			hb_state ^= hb_state << 13; hb_state ^= hb_state >> 17; hb_state ^= hb_state << 5;
			bool	hit = (hb_state % 1000) < (uint32_t)(prefetch_hit_rate * 1000.0);
			double	rc = t;
			rc = device_access(r, rc, outstanding);
			rc = link_transfer(r, rc, outstanding);
			/* Dequant is only needed for the portion actually stored
			 * compressed under this mix -- the "fp16" mix never
			 * compresses (see make_room/demote_hot_to_warm), so nothing
			 * to dequantize even on a baseline that structurally could. */
			if (r.compress && mix.warm_is_q8)
				rc = quant_op(r, rc, outstanding, false);
			if (hit)
				read_complete = t; /* hidden behind prior step's compute */
			else
			{
				read_complete = rc;
				/* wrong-prefetch penalty: one extra step's worth of
				 * bytes wasted on the mispredicted early fetch. */
				link_transfer(r, t, (double)new_bytes);
			}
		}
		double	compute_start = std::max(t, read_complete);
		double	compute_complete = compute_start + trace.compute_ns_per_step;

		make_room(r, s, compute_complete, new_bytes, ev.step, mix);
		if (!s.exceeded)
		{
			s.hot_q.push_back(new_bytes);
			s.hot_bytes += new_bytes;
		}

		s.latencies_ns.push_back(compute_complete - t);
		completed_steps++;
		sim_end_ns = std::max(sim_end_ns, compute_complete);
		if (!s.exceeded)
			pq.push({ compute_complete, ev.seq, ev.step + 1 });
		if (heartbeat_cb != nullptr && (completed_steps % 4096) == 0)
			heartbeat_cb(completed_steps, total_steps, heartbeat_ctx);
	}

	std::vector<double>	all_latencies;
	uint64_t			fit_count = 0;
	for (size_t i = 0; i < traces.size(); i++)
	{
		if (!states[i].exceeded)
			fit_count++;
		for (double v : states[i].latencies_ns)
			all_latencies.push_back(v);
	}
	std::sort(all_latencies.begin(), all_latencies.end());
	auto	pct = [&](double p) -> double
	{
		if (all_latencies.empty())
			return (0.0);
		size_t	idx = (size_t)(p * (double)(all_latencies.size() - 1));
		return (all_latencies[idx]);
	};

	out.baseline = baseline;
	out.capacity_name = capacity.name;
	out.mix_name = mix.name;
	out.concurrency = (uint32_t)traces.size();
	out.context_len = traces.empty() ? 0
		: traces[0].prompt_len + (uint32_t)traces[0].step_bytes.size();
	out.sequences_fit = fit_count;
	out.p50_latency_ns = pct(0.50);
	out.p95_latency_ns = pct(0.95);
	out.p99_latency_ns = pct(0.99);
	/* When NO sequence fits within capacity, every recorded step is a
	 * brief burst on the way to an immediate hard failure, not a
	 * sustainable serving rate -- reporting it as "tokens/s" would read
	 * as a real throughput number when it is actually an artifact of
	 * how quickly the capacity ceiling was hit. Report 0 in that case
	 * (matches sequences_fit==0 already signaling "does not work here")
	 * rather than a misleadingly large burst rate. */
	out.total_tokens_per_sec = (sim_end_ns > 0.0 && fit_count > 0)
		? (double)completed_steps / (sim_end_ns / 1.0e9) : 0.0;
	{
		double	blended_ratio = 1.0;

		if (r.compress && mix.warm_is_q8 && mix.cold_is_q4)
			blended_ratio = (Q8_COMPRESSION_RATIO + Q4_COMPRESSION_RATIO) / 2.0;
		else if (r.compress && mix.warm_is_q8)
			blended_ratio = Q8_COMPRESSION_RATIO; /* all-q8: no further cold shrink */
		/* A baseline with no device tier (device_capable == false, e.g.
		 * PCIE_FPGA_ROUNDTRIP) never actually gets to use
		 * capacity.device_bytes no matter what compression ratio it
		 * could nominally apply -- reporting a large "effective
		 * capacity" for such a baseline would contradict its own
		 * sequences_fit == 0 result at any load exceeding the host
		 * budget. Report 0 (not capacity.device_bytes, and not a
		 * misleadingly-large compressed number) for those baselines. */
		out.effective_kv_capacity_bytes = r.device_capable
			? (uint64_t)((double)capacity.device_bytes * blended_ratio) : 0;
	}
	uint64_t	fp16_baseline_bytes = 0;
	for (auto &tr : traces)
	{
		uint64_t	s2 = 0;
		for (uint32_t b : tr.step_bytes)
			s2 += b;
		fp16_baseline_bytes += s2;
	}
	uint64_t	actually_on_host = 0;
	for (auto &st : states)
		actually_on_host += st.hot_bytes;
	out.host_bytes_saved = fp16_baseline_bytes > actually_on_host
		? fp16_baseline_bytes - actually_on_host : 0;
	out.device_utilization_pct = r.device_dram.utilization(sim_end_ns);
	out.link_utilization_pct = r.link.utilization(sim_end_ns);
	out.quant_engine_utilization_pct = r.quant_engine.utilization(sim_end_ns);

	double	max_util = out.link_utilization_pct;
	out.bottleneck = "link";
	if (out.device_utilization_pct > max_util)
	{
		max_util = out.device_utilization_pct;
		out.bottleneck = "device_dram";
	}
	if (out.quant_engine_utilization_pct > max_util)
	{
		max_util = out.quant_engine_utilization_pct;
		out.bottleneck = "quant_engine";
	}
	if (fit_count < traces.size())
		out.bottleneck = "capacity";
	else if (max_util < 5.0)
		out.bottleneck = "compute";

	/* Energy: explicitly assumption-based (sim_config.h has no measured
	 * power figures; no real hardware exists to measure). Point
	 * estimate: 4 pJ/bit moved over the link (industry-typical order-
	 * of-magnitude for a SerDes-class interconnect) plus 20 pJ/bit for
	 * DRAM access (industry-typical DDR order of magnitude), applied to
	 * the ACTUAL bytes this scenario moved over each resource
	 * (r.link_bytes_total / r.device_bytes_total, accumulated by
	 * link_transfer/device_access above), not a fixed workload proxy. */
	out.energy_estimate_joules = r.link_bytes_total * 8.0 * 4.0e-12
		+ r.device_bytes_total * 8.0 * 20.0e-12;

	return (out);
}

}	/* namespace sim */
