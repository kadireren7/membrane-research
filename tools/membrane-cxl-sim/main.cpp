/*
 * membrane-cxl-sim: Phase 6.1 discrete-event simulator modeling a
 * near-memory/CXL KV appliance -- see docs/phase6-cxl-near-memory.md
 * for the full write-up. This file drives three sweeps
 * (concurrency x context, capacity x precision-mix, micro-batching)
 * plus a deterministic-replay self-check, all reusing sim_engine.h's
 * discrete-event core, then prints break-even and success-criteria
 * analysis derived from the results (no thresholds are adjusted after
 * the fact to force a particular conclusion -- see main()'s closing
 * section and the doc's "success criteria" section for the honest
 * result either way).
 */

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "sim_config.h"
#include "sim_engine.h"
#include "workload.h"

using namespace sim;

static const uint32_t	CONCURRENCY_LEVELS[] = { 1, 8, 32, 128, 512 };
static const uint32_t	CONTEXT_LEVELS[] = { 4096, 16384, 32768, 131072 };
/* Disclosed simulation-scale cap: contexts beyond this many decode
 * steps per sequence are truncated to keep sweep wall-clock time
 * tractable. Set high enough (>= the largest swept context, 131072)
 * that it never actually truncates anything in this phase's sweeps --
 * capacity is a CUMULATIVE-bytes-over-the-full-context question, so an
 * earlier, tighter cap (8192) silently hid the capacity ceiling at
 * every concurrency level except the most extreme one, which would
 * have understated exactly the effect this simulator exists to show.
 * Measured throughput is ~14M events/sec on this machine (see
 * docs/phase6-cxl-near-memory.md's "simulation granularity" section),
 * so the full uncapped sweep still completes in well under a minute. */
static const uint32_t	MAX_SIM_STEPS = 200000;

static const baseline_t	ALL_BASELINES[] = {
	baseline_t::HOST_ONLY, baseline_t::CPU_RAM_OFFLOAD,
	baseline_t::NVME_OFFLOAD, baseline_t::PCIE_FPGA_ROUNDTRIP,
	baseline_t::CXL_NO_PROCESSING, baseline_t::MEMBRANE_CXL_NEAR_MEMORY
};

static const uint64_t	GB = 1000ull * 1000ull * 1000ull;
static const uint64_t	HOST_FAST_BYTES = 80ull * GB;

static const capacity_scenario_t	CAPACITY_SCENARIOS[] = {
	{ "512GB-cxl", HOST_FAST_BYTES, 512ull * GB },
	{ "1TB-cxl",   HOST_FAST_BYTES, 1000ull * GB },
	{ "2TB-cxl",   HOST_FAST_BYTES, 2000ull * GB },
};

static const precision_mix_t	PRECISION_MIXES[] = {
	{ "fp16", false, false },
	{ "all-q8", true, false },
	{ "safe-mixed-q8-q4", true, true },
};

struct heartbeat_state_t
{
	std::chrono::steady_clock::time_point	last_print;
	std::chrono::steady_clock::time_point	start;
	const char								*scenario_label;
	uint64_t								scenario_idx;
	uint64_t								scenario_total;
};

static void	heartbeat_cb(uint64_t completed, uint64_t total, void *ctx)
{
	heartbeat_state_t	*hb = (heartbeat_state_t *)ctx;
	auto				now = std::chrono::steady_clock::now();
	double				since_last = std::chrono::duration<double>(
							now - hb->last_print).count();

	if (since_last < 60.0)
		return;
	hb->last_print = now;
	double	wall_elapsed = std::chrono::duration<double>(now - hb->start).count();
	double	frac = total > 0 ? (double)completed / (double)total : 0.0;
	double	eta = (frac > 0.0) ? wall_elapsed * (1.0 - frac) / frac : 0.0;
	fprintf(stderr,
		"[heartbeat] scenario %" PRIu64 "/%" PRIu64 " (%s): "
		"steps %" PRIu64 "/%" PRIu64 " (%.1f%%) wall=%.0fs ETA=%.0fs\n",
		hb->scenario_idx, hb->scenario_total, hb->scenario_label,
		completed, total, 100.0 * frac, wall_elapsed, eta);
}

static sequence_trace_t	pick_base_trace(const std::string &trace_path,
								double tok_per_sec)
{
	sequence_trace_t	base;

	if (!load_real_trace(trace_path, 1.0e9 / tok_per_sec, &base))
	{
		fprintf(stderr, "membrane-cxl-sim: failed to load trace '%s'\n",
			trace_path.c_str());
		exit(2);
	}
	return (base);
}

static void	print_csv_header(FILE *f)
{
	fprintf(f,
		"sweep,baseline,capacity,mix,concurrency,context_len,"
		"sequences_fit,p50_us,p95_us,p99_us,tokens_per_sec,"
		"effective_kv_gb,host_bytes_saved_gb,device_util_pct,"
		"link_util_pct,quant_util_pct,bottleneck,energy_mJ\n");
}

static void	print_csv_row(FILE *f, const char *sweep, const scenario_result_t &r)
{
	fprintf(f,
		"%s,%s,%s,%s,%u,%u,%" PRIu64 ",%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,"
		"%.1f,%.1f,%.1f,%s,%.4f\n",
		sweep, baseline_name(r.baseline), r.capacity_name.c_str(),
		r.mix_name.c_str(), r.concurrency, r.context_len, r.sequences_fit,
		r.p50_latency_ns / 1000.0, r.p95_latency_ns / 1000.0,
		r.p99_latency_ns / 1000.0, r.total_tokens_per_sec,
		(double)r.effective_kv_capacity_bytes / (double)GB,
		(double)r.host_bytes_saved / (double)GB,
		r.device_utilization_pct, r.link_utilization_pct,
		r.quant_engine_utilization_pct, r.bottleneck.c_str(),
		r.energy_estimate_joules * 1000.0);
}

static std::vector<scenario_result_t>	sweep_a(const sequence_trace_t &base,
									FILE *csv, uint32_t seed)
{
	std::vector<scenario_result_t>	results;
	uint64_t						idx = 0;
	uint64_t						total = 5 * 4 * 6;

	for (uint32_t conc : CONCURRENCY_LEVELS)
	{
		for (uint32_t ctx : CONTEXT_LEVELS)
		{
			uint32_t	capped_ctx = ctx;
			if (ctx > base.prompt_len + MAX_SIM_STEPS)
				capped_ctx = base.prompt_len + MAX_SIM_STEPS;
			std::vector<sequence_trace_t>	traces =
				generate_workload(base, conc, capped_ctx, seed);
			for (baseline_t b : ALL_BASELINES)
			{
				idx++;
				heartbeat_state_t	hb{ std::chrono::steady_clock::now(),
					std::chrono::steady_clock::now(), baseline_name(b),
					idx, total };
				scenario_result_t	r = run_scenario(b,
					CAPACITY_SCENARIOS[1] /* 1TB representative */,
					PRECISION_MIXES[2] /* safe-mixed representative */,
					traces, 0.9, heartbeat_cb, &hb);
				r.context_len = ctx; /* report the REQUESTED context,
					not the truncated one, so the sweep table stays
					keyed by what was asked for; capped_ctx only bounds
					simulated event count. */
				print_csv_row(csv, "sweep_a_concurrency_context", r);
				results.push_back(r);
			}
		}
	}
	return (results);
}

static std::vector<scenario_result_t>	sweep_b(const sequence_trace_t &base,
									FILE *csv, uint32_t seed)
{
	std::vector<scenario_result_t>	results;
	/* 512/32768: sweep_a shows host-only's 80GB ceiling binds starting
	 * around concurrency=128 at this context length, but 128 alone
	 * still left every DEVICE-having baseline comfortably under even
	 * the smallest (512GB) capacity scenario regardless of precision
	 * mix (found by running at 128/32768 first: cxl-no-processing --
	 * no compression benefit at all -- still fit all 128 sequences at
	 * 512GB, so capacity scenario and mix made no visible difference).
	 * 512 concurrency is where capacity actually becomes the
	 * differentiator between mixes/capacities while context=32768 (not
	 * 131072) keeps per-step latency in a realistic range (sweep_a:
	 * ~15ms p99 at 512/32768 vs ~74s at 512/131072 -- see the
	 * "simulation granularity"/full-reread discussion in the doc for
	 * why very long context at very high concurrency is a genuinely
	 * separate, much harsher regime, not a bug). */
	const uint32_t					conc = 512;
	const uint32_t					ctx = 32768;
	uint32_t						capped_ctx = std::min(ctx,
										base.prompt_len + MAX_SIM_STEPS);

	std::vector<sequence_trace_t>	traces =
		generate_workload(base, conc, capped_ctx, seed + 777);
	for (const capacity_scenario_t &cap : CAPACITY_SCENARIOS)
	{
		for (const precision_mix_t &mix : PRECISION_MIXES)
		{
			for (baseline_t b : ALL_BASELINES)
			{
				scenario_result_t	r = run_scenario(b, cap, mix, traces,
					0.9, nullptr, nullptr);
				r.context_len = ctx;
				print_csv_row(csv, "sweep_b_capacity_mix", r);
				results.push_back(r);
			}
		}
	}
	return (results);
}

/* Micro-batching study (section 6): the near-memory quant engine can
 * either dispatch each small demotion/promotion immediately (max_wait
 * = 0), or wait up to max_wait_ns to accumulate several concurrent
 * sequences' independently-arriving pending requests into one larger
 * batch, amortizing per-request dispatch overhead against a max batch
 * size cap. Arrivals are an independent random stream (deterministic
 * seeded exponential inter-arrival, mean 300ns -- representative of
 * many concurrent sequences' demotion requests interleaving, not tied
 * to one specific sweep_a scenario) so p50/p95/p99 reflect a genuine
 * latency DISTRIBUTION (early arrivals in a window wait nearly the
 * full max_wait; late arrivals wait less; queueing on the shared
 * engine adds further spread) rather than one fixed number. */
static void	microbatch_study(FILE *csv)
{
	static const double	WAIT_OPTIONS_NS[] = { 0.0, 500.0, 2000.0, 8000.0 };
	static const int		MAX_BATCH_SIZES[] = { 1, 4, 16, 64 };
	const double			dispatch_overhead_ns = 200.0; /* per-invocation
		issue cost, disclosed assumption -- distinct from per-block
		compute, representing e.g. a pipeline-transaction setup cost */
	const int				n_requests = 20000;
	/* 100ns mean: below the ~205ns unbatched floor (200ns dispatch
	 * overhead + ~5ns compute for one 32-element block at the
	 * near-memory pipeline's real rate), so batch=1/wait=0 is
	 * genuinely overloaded and falls behind -- deliberately chosen to
	 * show batching's actual amortization benefit, not a load level
	 * where the engine is idle regardless of configuration. */
	const double			mean_interarrival_ns = 100.0;

	fprintf(csv, "\n# micro-batching study (near-memory quant engine)\n");
	fprintf(csv, "max_wait_ns,max_batch_size,p50_us,p95_us,p99_us,"
		"throughput_req_per_sec\n");
	for (double wait_ns : WAIT_OPTIONS_NS)
	{
		for (int max_batch : MAX_BATCH_SIZES)
		{
			k_server_resource_t	engine(1, "microbatch-quant",
				dispatch_overhead_ns);
			std::vector<double>	arrivals(n_requests);
			std::vector<double>	latencies;
			uint32_t				rng = 0x2545f491u
										^ (uint32_t)wait_ns
										^ (uint32_t)(max_batch * 7919);
			double					t = 0.0;
			double					last_complete = 0.0;
			int						i;

			for (i = 0; i < n_requests; i++)
			{
				rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
				double	u = ((double)(rng % 1000000) + 1.0) / 1000001.0;
				t += -mean_interarrival_ns * log(u); /* exponential */
				arrivals[i] = t;
			}
			i = 0;
			while (i < n_requests)
			{
				double	window_start = arrivals[i];
				int		count = 1;
				while (i + count < n_requests
						&& arrivals[i + count] <= window_start + wait_ns
						&& count < max_batch)
					count++;
				double	dispatch_time = window_start + wait_ns;
				if (dispatch_time < arrivals[i])
					dispatch_time = arrivals[i];
				double	service_ns = (double)count
					* (BYTES_PER_BLOCK / NEARMEM_PIPELINE_BYTES_PER_NS);
				double	complete = engine.submit(dispatch_time, service_ns);
				int		k;
				for (k = 0; k < count; k++)
					latencies.push_back(complete - arrivals[i + k]);
				last_complete = complete;
				i += count;
			}
			std::sort(latencies.begin(), latencies.end());
			auto	pct = [&](double p) {
				size_t	idx = (size_t)(p * (double)(latencies.size() - 1));
				return (latencies[idx]);
			};
			/* Achieved throughput over the actual processing span (first
			 * arrival to last completion) -- if a config can't keep up
			 * with the offered load, this span grows beyond the raw
			 * arrival span and throughput drops below the nominal
			 * 1/mean_interarrival input rate, visibly showing overload. */
			double	span_ns = last_complete - arrivals[0];
			double	throughput = span_ns > 0.0
				? (double)n_requests / (span_ns / 1.0e9) : 0.0;
			fprintf(csv, "%.0f,%d,%.3f,%.3f,%.3f,%.1f\n",
				wait_ns, max_batch, pct(0.50) / 1000.0, pct(0.95) / 1000.0,
				pct(0.99) / 1000.0, throughput);
		}
	}
}

static void	deterministic_replay_check(const sequence_trace_t &base)
{
	std::vector<sequence_trace_t>	t1 = generate_workload(base, 8, 4096, 42);
	std::vector<sequence_trace_t>	t2 = generate_workload(base, 8, 4096, 42);
	scenario_result_t	r1 = run_scenario(baseline_t::MEMBRANE_CXL_NEAR_MEMORY,
		CAPACITY_SCENARIOS[1], PRECISION_MIXES[2], t1, 0.9, nullptr, nullptr);
	scenario_result_t	r2 = run_scenario(baseline_t::MEMBRANE_CXL_NEAR_MEMORY,
		CAPACITY_SCENARIOS[1], PRECISION_MIXES[2], t2, 0.9, nullptr, nullptr);
	bool	identical = (r1.sequences_fit == r2.sequences_fit)
		&& (r1.p50_latency_ns == r2.p50_latency_ns)
		&& (r1.p99_latency_ns == r2.p99_latency_ns)
		&& (r1.total_tokens_per_sec == r2.total_tokens_per_sec);
	fprintf(stderr,
		"[verify] deterministic replay (same seed, same trace): %s\n",
		identical ? "PASS -- identical results" : "FAIL -- results diverged");
	if (!identical)
	{
		fprintf(stderr, "  run1: fit=%" PRIu64 " p50=%.3f p99=%.3f tok/s=%.3f\n",
			r1.sequences_fit, r1.p50_latency_ns, r1.p99_latency_ns,
			r1.total_tokens_per_sec);
		fprintf(stderr, "  run2: fit=%" PRIu64 " p50=%.3f p99=%.3f tok/s=%.3f\n",
			r2.sequences_fit, r2.p50_latency_ns, r2.p99_latency_ns,
			r2.total_tokens_per_sec);
		exit(1);
	}
}

static const scenario_result_t	*find(const std::vector<scenario_result_t> &v,
							baseline_t b, uint32_t conc, uint32_t ctx)
{
	for (const auto &r : v)
		if (r.baseline == b && r.concurrency == conc && r.context_len == ctx)
			return (&r);
	return (nullptr);
}

static void	break_even_and_success_criteria(
					const std::vector<scenario_result_t> &a)
{
	fprintf(stderr, "\n=== break-even analysis (from sweep_a) ===\n");
	for (uint32_t ctx : CONTEXT_LEVELS)
	{
		bool	found = false;
		for (uint32_t conc : CONCURRENCY_LEVELS)
		{
			const scenario_result_t	*host = find(a, baseline_t::HOST_ONLY,
				conc, ctx);
			const scenario_result_t	*mem = find(a,
				baseline_t::MEMBRANE_CXL_NEAR_MEMORY, conc, ctx);
			if (host == nullptr || mem == nullptr)
				continue;
			if (mem->sequences_fit > host->sequences_fit && !found)
			{
				fprintf(stderr,
					"  context=%u: near-memory fits MORE sequences than "
					"host-only starting at concurrency=%u "
					"(host_fit=%" PRIu64 " vs nearmem_fit=%" PRIu64 ")\n",
					ctx, conc, host->sequences_fit, mem->sequences_fit);
				found = true;
			}
		}
		if (!found)
			fprintf(stderr,
				"  context=%u: no concurrency level in this sweep showed "
				"near-memory fitting more sequences than host-only "
				"(likely means host-only's 80GB budget never binds at "
				"this context length within the swept concurrency range)\n",
				ctx);
	}

	fprintf(stderr, "\n=== success criteria evaluation (target, not guarantee) ===\n");
	/* For throughput/latency criteria (1/2/4), the fairest point in
	 * this sweep is the LARGEST load (by concurrency x context) at
	 * which near-memory does not itself exceed capacity (sequences_fit
	 * == concurrency) -- comparing at a point where near-memory has
	 * already collapsed under its own capacity ceiling (like the most
	 * extreme 512/128K corner, where NO baseline's sequences fit) would
	 * make every throughput/latency number an artifact of instant
	 * failure rather than a real reading of sustained behavior. This is
	 * found programmatically, not hand-picked. Capacity criterion 3
	 * still legitimately uses the most extreme corner, since that one
	 * is testing the ceiling itself. */
	uint32_t	best_conc = 0;
	uint32_t	best_ctx = 0;
	for (uint32_t conc : CONCURRENCY_LEVELS)
		for (uint32_t ctx : CONTEXT_LEVELS)
		{
			const scenario_result_t	*m = find(a,
				baseline_t::MEMBRANE_CXL_NEAR_MEMORY, conc, ctx);
			if (m != nullptr && m->sequences_fit == conc
					&& (uint64_t)conc * ctx > (uint64_t)best_conc * best_ctx)
			{
				best_conc = conc;
				best_ctx = ctx;
			}
		}
	if (best_conc == 0)
	{
		fprintf(stderr,
			"  no swept scenario had near-memory fit ALL concurrent "
			"sequences -- criteria 1/2/4 cannot be fairly evaluated from "
			"this sweep as configured (reported as-is, not adjusted)\n");
	}
	else
	{
		fprintf(stderr,
			"  (criteria 1/2/4 evaluated at concurrency=%u context=%u: "
			"the largest load in this sweep near-memory sustains without "
			"itself hitting its capacity ceiling)\n", best_conc, best_ctx);
		const scenario_result_t	*host = find(a, baseline_t::HOST_ONLY,
			best_conc, best_ctx);
		const scenario_result_t	*mem = find(a,
			baseline_t::MEMBRANE_CXL_NEAR_MEMORY, best_conc, best_ctx);
		const scenario_result_t	*pcie = find(a,
			baseline_t::PCIE_FPGA_ROUNDTRIP, best_conc, best_ctx);
		if (host != nullptr && mem != nullptr)
		{
			fprintf(stderr,
				"  1. more sequences/throughput than host-only: %s "
				"(host_fit=%" PRIu64 " tok/s=%.1f vs "
				"nearmem_fit=%" PRIu64 " tok/s=%.1f)\n",
				(mem->sequences_fit > host->sequences_fit
					|| mem->total_tokens_per_sec > host->total_tokens_per_sec)
					? "MET" : "NOT MET",
				host->sequences_fit, host->total_tokens_per_sec,
				mem->sequences_fit, mem->total_tokens_per_sec);
			fprintf(stderr,
				"  2. p99 latency within an explicit bound (<10ms/step "
				"used here as the illustrative bound): %s (p99=%.1fus)\n",
				mem->p99_latency_ns < 10.0e6 ? "MET" : "NOT MET",
				mem->p99_latency_ns / 1000.0);
		}
		if (pcie != nullptr && mem != nullptr)
			fprintf(stderr,
				"  4. clearly better than PCIe-FPGA-roundtrip: %s "
				"(nearmem tok/s=%.1f fit=%" PRIu64 " vs "
				"pcie-fpga tok/s=%.1f fit=%" PRIu64 ")\n",
				(mem->total_tokens_per_sec >= pcie->total_tokens_per_sec
					&& mem->sequences_fit >= pcie->sequences_fit)
					? "MET" : "NOT MET",
				mem->total_tokens_per_sec, mem->sequences_fit,
				pcie->total_tokens_per_sec, pcie->sequences_fit);
	}
	const scenario_result_t	*ceiling_host = find(a, baseline_t::HOST_ONLY,
		512, 131072);
	const scenario_result_t	*ceiling_mem = find(a,
		baseline_t::MEMBRANE_CXL_NEAR_MEMORY, 512, 131072);
	if (ceiling_host != nullptr && ceiling_mem != nullptr)
		fprintf(stderr,
			"  3. >= 2x effective KV capacity vs an 80GB host-only ceiling: "
			"%s (effective=%.1f GB vs host-only ceiling=%.1f GB)\n",
			((double)ceiling_mem->effective_kv_capacity_bytes
				>= 2.0 * (double)HOST_FAST_BYTES) ? "MET" : "NOT MET",
			(double)ceiling_mem->effective_kv_capacity_bytes / (double)GB,
			(double)HOST_FAST_BYTES / (double)GB);
	fprintf(stderr,
		"  (per the spec: if these do not come out favorably, that is "
		"reported as-is -- see docs/phase6-cxl-near-memory.md section 12, "
		"thresholds and workload are not adjusted after the fact)\n");
}

/* Pipeline-count sensitivity sweep (section 7's "required pipeline
 * count"): fixed high load (concurrency=128, context=32768), varying
 * only the near-memory quant engine's parallel pipeline count, to show
 * how provisioning trades off against latency/fit -- directly answers
 * "how many pipelines does this appliance need" from simulated
 * evidence rather than asserting a number. */
static void	pipeline_sensitivity_sweep(const sequence_trace_t &base,
						FILE *csv, uint32_t seed)
{
	static const int	PIPELINE_COUNTS[] = { 1, 2, 4, 8, 16, 32 };
	const uint32_t		conc = 128;
	/* 131072, not a smaller context: sweep_a shows the near-memory
	 * quant engine only actually saturates (88.9% at the default 8
	 * pipelines) at this concurrency/context combination -- a smaller
	 * context never triggers enough tiering to be a meaningful
	 * pipeline-count sensitivity test (see docs/phase6-cxl-near-memory.md). */
	const uint32_t		ctx = 131072;
	uint32_t			capped_ctx = std::min(ctx,
							base.prompt_len + MAX_SIM_STEPS);

	std::vector<sequence_trace_t>	traces =
		generate_workload(base, conc, capped_ctx, seed + 999);
	fprintf(csv, "\n# pipeline-count sensitivity (concurrency=%u context=%u)\n",
		conc, ctx);
	fprintf(csv, "pipelines,sequences_fit,p50_us,p95_us,p99_us,tokens_per_sec,"
		"link_util_pct,quant_util_pct\n");
	for (int k : PIPELINE_COUNTS)
	{
		scenario_result_t	r = run_scenario(baseline_t::MEMBRANE_CXL_NEAR_MEMORY,
			CAPACITY_SCENARIOS[1], PRECISION_MIXES[2], traces, 0.9,
			nullptr, nullptr, k);
		fprintf(csv, "%d,%" PRIu64 ",%.2f,%.2f,%.2f,%.2f,%.1f,%.1f\n",
			k, r.sequences_fit, r.p50_latency_ns / 1000.0,
			r.p95_latency_ns / 1000.0, r.p99_latency_ns / 1000.0,
			r.total_tokens_per_sec, r.link_utilization_pct,
			r.quant_engine_utilization_pct);
	}
}

int	main(int argc, char **argv)
{
	std::string	trace_path =
		"benchmarks/cxl-sim/traces/SmolLM2-135M-Instruct-f16.kvtrace";
	std::string	out_path = "/tmp/membrane-cxl-sim-report.csv";
	uint32_t	seed = 12345;
	int			i;

	i = 1;
	while (i + 1 < argc)
	{
		if (strcmp(argv[i], "--trace") == 0)
			trace_path = argv[i + 1];
		else if (strcmp(argv[i], "--out") == 0)
			out_path = argv[i + 1];
		else if (strcmp(argv[i], "--seed") == 0)
			seed = (uint32_t)atoi(argv[i + 1]);
		i += 2;
	}

	sequence_trace_t	base = pick_base_trace(trace_path,
		SMOLLM2_135M_TOK_PER_SEC);
	fprintf(stderr,
		"membrane-cxl-sim: base trace '%s' model=%s real_capture=%s "
		"prompt_len=%u steps=%zu\n", trace_path.c_str(),
		base.source_model.c_str(), base.is_real_capture ? "yes" : "no",
		base.prompt_len, base.step_bytes.size());

	deterministic_replay_check(base);

	FILE	*csv = fopen(out_path.c_str(), "w");
	if (csv == NULL)
	{
		fprintf(stderr, "membrane-cxl-sim: cannot open %s\n", out_path.c_str());
		return (1);
	}
	print_csv_header(csv);
	auto	t0 = std::chrono::steady_clock::now();
	std::vector<scenario_result_t>	a = sweep_a(base, csv, seed);
	sweep_b(base, csv, seed);
	microbatch_study(csv);
	pipeline_sensitivity_sweep(base, csv, seed);
	fclose(csv);
	auto	t1 = std::chrono::steady_clock::now();
	fprintf(stderr,
		"membrane-cxl-sim: sweep complete in %.1fs wall time, report -> %s\n",
		std::chrono::duration<double>(t1 - t0).count(), out_path.c_str());

	break_even_and_success_criteria(a);
	return (0);
}
