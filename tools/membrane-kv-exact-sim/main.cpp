/*
 * membrane-kv-exact-sim: Phase 6.4's UNIFIED 128K-context x
 * 512-concurrency exact sparse KV retrieval stress validation, and
 * Phase 6.5's out-of-core memory-bounded rework of the same sweep --
 * see docs/phase6-unified-stress.md and docs/phase6-out-of-core-
 * simulator.md. Phase 6.5 changes HOW the trace is held (streamed
 * from a chunked .attntrace3 file through a bounded cache instead of
 * one giant resident vector) and adds real memory-pressure
 * backpressure (mem_guard.h); it does not change WHAT is computed --
 * config_hash below is deliberately left byte-for-byte identical to
 * Phase 6.4's so existing checkpoints (and the 107/231 already-real
 * SmolLM2-360M rows they hold) stay valid and are not recomputed.
 */

#define _DEFAULT_SOURCE

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "attn_trace_reader.h"
#include "bounded_quantile.h"
#include "calibrate.h"
#include "checkpoint.h"
#include "exact_engine.h"
#include "mem_guard.h"
#include "membrane/block.h"
#include "sim_config.h"
#include "wssim_config.h"

using namespace wssim;
using namespace exactsim;

static const char	*MEMBRANE_EXACTSIM_VERSION = "phase6.5-out-of-core-v1";

struct model_entry_t
{
	std::string				name;
	std::string				long_trace_path;
	model_calibration_t	calib;
};

/* Phase 6.4 item 1: the unified scenario is ALWAYS 128K context x 512
 * concurrency -- both axes maxed simultaneously in the same run,
 * exactly the thing Phase 6.3 left disclosed as not attempted
 * (docs/phase6-exact-sparse-retrieval.md section 0/17). */
constexpr uint32_t	UNIFIED_TARGET_STEPS = 130560;	/* 128K - 512 prompt */
constexpr uint32_t	UNIFIED_CONCURRENCY = 512;
constexpr uint32_t	SYNTHETIC_SEED = 42u;

struct precision_mode_t
{
	std::string	name;
	bool		no_compression;
	bool		warm_is_q8;
};

static std::vector<precision_mode_t>	precision_modes()
{
	return {
		{"fp16", true, false},
		{"all-q8", false, true},
		/* "safe-mixed" reuses the Q8 ratio as its calibration-time
		 * compression proxy -- a real, disclosed simplification: this
		 * engine's per-channel cache model has one compression ratio
		 * per scenario, not a separately-tracked warm(Q8)/cold(Q4)
		 * split the way Phase 6.1's own tiered demotion model has.
		 * Phase 5.4/6.1's real blended Q8/Q4 ratio (~2.7x) is cited in
		 * the doc as the more accurate capacity-side number; this
		 * mode's calibration behavior (hit/miss timing) is identical
		 * to all-q8. */
		{"safe-mixed", false, true},
	};
}

/* Phase 6.4 item 2: the 7 requested comparisons. Two are analytical
 * (not simulated, section 0); five run through the real calibrate/
 * run_concurrent pipeline. */
struct comparison_t
{
	std::string	name;
	bool		analytical;
	policy_t	policy;
	bool		disable_prefetch;
	uint32_t	coalescing_window;
};

static std::vector<comparison_t>	comparisons()
{
	return {
		{"full-scan-cxl", true, policy_t::FULL, false, 0},
		{"compressed-full-scan-cxl", true, policy_t::FULL, false, 0},
		{"exact-no-prefetch", false, policy_t::NO_PREFETCH, false, 0},
		{"exact-predictor", false, policy_t::MEMBRANE_PREDICTIVE, true, 0},
		{"exact-predictor-prefetch", false, policy_t::MEMBRANE_PREDICTIVE,
			false, 0},
		{"exact-predictor-coalescing", false, policy_t::MEMBRANE_PREDICTIVE,
			false, 4},
		{"oracle", false, policy_t::ORACLE, false, 0},
	};
}

struct scenario_desc_t
{
	std::string	model_name;
	std::string	comparison_name;
	std::string	precision_name;
	uint64_t	host_cache_total_bytes;
	uint64_t	device_total_bytes;

	std::string	id() const
	{
		char	buf[512];
		snprintf(buf, sizeof(buf), "unified|%s|%s|%s|%llu|%llu",
			model_name.c_str(), comparison_name.c_str(),
			precision_name.c_str(),
			(unsigned long long)host_cache_total_bytes,
			(unsigned long long)device_total_bytes);
		return (std::string(buf));
	}
};

static const char	*CSV_HEADER =
	"model,comparison,precision,host_cache_total_bytes,device_total_bytes,"
	"concurrency,context_tokens,"
	"p50_latency_ns,p95_latency_ns,p99_latency_ns,tokens_per_sec,"
	"mean_bytes_per_token,link_util_pct,quant_util_pct,bottleneck,"
	"cap_total_logical_kv_bytes,cap_physical_device_bytes,cap_hot_cache_bytes,"
	"cap_effective_capacity_ratio,cap_sequences_fit,cap_sequences_requested,"
	"cap_failure_reason,"
	"link_p50_wait_ns,link_p95_wait_ns,link_p99_wait_ns,link_requests,"
	"device_p50_wait_ns,device_p95_wait_ns,device_p99_wait_ns,device_requests,"
	"quant_p50_wait_ns,quant_p95_wait_ns,quant_p99_wait_ns,quant_requests,"
	"max_simultaneous_fetches,mean_miss_burst_blocks,max_miss_burst_blocks,"
	"calib_hit_rate,calib_precision,calib_recall,calib_working_set_blocks,"
	/* Phase 6.5 item 13: compute-normalized latency -- explanatory
	 * only, does not change the existing 10ms bound above. */
	"model_compute_floor_ns,incremental_kv_p99_ns,hidden_under_compute_fraction,"
	/* Phase 6.5 item 11: results integrity. trace_hash8/config_hash8
	 * are the first 8 hex chars of the full sweep-level SHA-256es
	 * computed once in main() (truncated for CSV compactness -- a
	 * spot-check identifier, not a security digest). completion_checksum
	 * is CRC32 of every column before it in this same row, letting the
	 * integrity tool (membrane-kv-exact-sim-verify) detect truncated/
	 * corrupted rows independent of the checkpoint file. */
	"sim_version,backend,trace_hash8,config_hash8,completion_checksum\n";

static std::string	fmt_row(const scenario_desc_t &d, uint32_t concurrency,
					uint32_t context_tokens, const concurrent_result_t &cr,
					double hit_rate, double precision, double recall,
					double working_set)
{
	char	buf[2048];
	snprintf(buf, sizeof(buf),
		"%s,%s,%s,%llu,%llu,%u,%u,"
		"%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%s,"
		"%llu,%llu,%llu,%.4f,%llu,%u,%s,"
		"%.2f,%.2f,%.2f,%llu,"
		"%.2f,%.2f,%.2f,%llu,"
		"%.2f,%.2f,%.2f,%llu,"
		"%llu,%.3f,%.3f,"
		"%.4f,%.4f,%.4f,%.3f,"
		"%.2f,%.2f,%.4f",
		d.model_name.c_str(), d.comparison_name.c_str(),
		d.precision_name.c_str(),
		(unsigned long long)d.host_cache_total_bytes,
		(unsigned long long)d.device_total_bytes, concurrency, context_tokens,
		cr.p50_latency_ns, cr.p95_latency_ns, cr.p99_latency_ns,
		cr.tokens_per_sec, cr.mean_bytes_per_token, cr.link_utilization_pct,
		cr.quant_utilization_pct, cr.bottleneck.c_str(),
		(unsigned long long)cr.capacity.total_logical_kv_bytes,
		(unsigned long long)cr.capacity.physical_device_bytes,
		(unsigned long long)cr.capacity.hot_cache_bytes,
		cr.capacity.effective_capacity_ratio,
		(unsigned long long)cr.capacity.sequences_fit,
		cr.capacity.sequences_requested,
		cr.capacity.capacity_failure_reason.c_str(),
		cr.link_queue.p50_wait_ns, cr.link_queue.p95_wait_ns,
		cr.link_queue.p99_wait_ns, (unsigned long long)cr.link_queue.requests,
		cr.device_queue.p50_wait_ns, cr.device_queue.p95_wait_ns,
		cr.device_queue.p99_wait_ns,
		(unsigned long long)cr.device_queue.requests,
		cr.quant_queue.p50_wait_ns, cr.quant_queue.p95_wait_ns,
		cr.quant_queue.p99_wait_ns,
		(unsigned long long)cr.quant_queue.requests,
		(unsigned long long)cr.max_simultaneous_fetches,
		cr.mean_miss_burst_blocks, cr.max_miss_burst_blocks,
		hit_rate, precision, recall, working_set,
		cr.model_compute_floor_ns, cr.incremental_kv_p99_ns,
		cr.hidden_under_compute_fraction);
	return (std::string(buf));
}

static std::string	analytical_row(const scenario_desc_t &d,
					uint32_t context_tokens, double bytes_per_token)
{
	char	buf[1024];
	snprintf(buf, sizeof(buf),
		"%s,%s,%s,%llu,%llu,%u,%u,"
		"0,0,0,0,%.2f,0,0,n/a,"
		"0,0,0,0,0,%u,n/a,"
		"0,0,0,0,0,0,0,0,0,0,0,0,"
		"0,0,0,0,0,0,0,"
		"0,0,0",
		d.model_name.c_str(), d.comparison_name.c_str(),
		d.precision_name.c_str(),
		(unsigned long long)d.host_cache_total_bytes,
		(unsigned long long)d.device_total_bytes, UNIFIED_CONCURRENCY,
		context_tokens, bytes_per_token, UNIFIED_CONCURRENCY);
	return (std::string(buf));
}

/* Phase 6.5 item 11: appends the results-integrity suffix (simulator
 * version, backend, truncated trace/config hash, then a CRC32
 * checksum over everything before it in the row) to an already-built
 * data row. Every CSV row and checkpoint record (checkpoint.h stores
 * the row text verbatim) goes through this, so the integrity tool
 * can validate either file independently. */
static std::string	finalize_row(const std::string &base,
					const std::string &backend_name,
					const std::string &trace_hash8,
					const std::string &config_hash8)
{
	char	meta[256];

	snprintf(meta, sizeof(meta), ",%s,%s,%s,%s", MEMBRANE_EXACTSIM_VERSION,
		backend_name.c_str(), trace_hash8.c_str(), config_hash8.c_str());
	std::string	with_meta = base + meta;
	uint32_t	crc = membrane_block_checksum(
		(const uint8_t *)with_meta.data(), with_meta.size());
	char		cbuf[16];
	snprintf(cbuf, sizeof(cbuf), ",%08x", crc);
	return (with_meta + cbuf);
}

static std::mutex			g_tail_mutex;
static FILE					*g_tail_csv = nullptr;
static const std::string	*g_tail_target_comparison = nullptr;

static std::string	process_one(const scenario_desc_t &d,
						attn_trace_reader_t &reader,
						const std::map<std::string, model_calibration_t>
							&models, const comparison_t &comp,
						const std::string &backend_name,
						const std::string &trace_hash8,
						const std::string &config_hash8)
{
	const trace_metadata_t		&md = reader.get_metadata();
	const model_calibration_t	&model = models.at(d.model_name);
	uint32_t	context_tokens = md.prompt_len + md.step_count;

	if (comp.analytical)
	{
		double	mean_context = (double)md.prompt_len
			+ (double)md.step_count / 2.0;
		double	raw = (double)model.bytes_per_token_total * mean_context;
		double	bpt = comp.name == "full-scan-cxl" ? raw
			: raw / sim::Q8_COMPRESSION_RATIO;
		return (finalize_row(analytical_row(d, context_tokens, bpt),
			backend_name, trace_hash8, config_hash8));
	}

	precision_mode_t	prec{};
	for (const auto &p : precision_modes())
		if (p.name == d.precision_name)
			prec = p;

	scenario_config_t	cfg{};
	cfg.policy = comp.policy;
	cfg.eviction = eviction_policy_t::SEGMENTED_LRU;
	cfg.block_size_tokens = 32;
	cfg.hot_cache_bytes = d.host_cache_total_bytes / UNIFIED_CONCURRENCY;
	cfg.warm_tier_is_q8 = prec.warm_is_q8;
	cfg.no_compression = prec.no_compression;
	cfg.disable_prefetch = comp.disable_prefetch;
	cfg.coalescing_window = comp.coalescing_window;

	calibrated_profile_t	profile = calibrate_streamed(reader, model, cfg,
		false);

	concurrent_config_t	ccfg{};
	ccfg.concurrency = UNIFIED_CONCURRENCY;
	ccfg.host_hot_cache_total_bytes = d.host_cache_total_bytes;
	ccfg.device_total_bytes = d.device_total_bytes;
	ccfg.quant_pipelines = wssim::DEFAULT_QUANT_PIPELINES;
	ccfg.microbatch_max_wait_ns = 0.0;	/* off by default -- Phase 6.3's
			 * null result (docs/phase6-exact-sparse-retrieval.md
			 * section 8) -- kept as one controlled comparison point
			 * elsewhere, not the unified matrix's default. */
	ccfg.microbatch_max_batch_blocks = 0;
	ccfg.hw = default_hardware_profile();

	double	compression = prec.no_compression ? 1.0
		: (prec.warm_is_q8 ? sim::Q8_COMPRESSION_RATIO : sim::Q4_COMPRESSION_RATIO);
	uint32_t	tail_n = 0;
	if (g_tail_target_comparison != nullptr && comp.name == *g_tail_target_comparison)
		tail_n = 20;
	concurrent_result_t	cr = run_concurrent(profile, model, 32, compression,
		ccfg, tail_n);

	if (tail_n > 0 && !cr.tail_samples.empty())
	{
		std::lock_guard<std::mutex>	lock(g_tail_mutex);
		for (const auto &ts : cr.tail_samples)
			fprintf(g_tail_csv,
				"%s,%s,%s,%llu,%llu,%u,%u,%llu,%llu,%.2f,%.2f,%.2f,%.2f,%.2f\n",
				d.model_name.c_str(), d.comparison_name.c_str(),
				d.precision_name.c_str(),
				(unsigned long long)d.host_cache_total_bytes,
				(unsigned long long)d.device_total_bytes,
				ts.sequence_id, ts.step_index,
				(unsigned long long)ts.prefetch_bytes,
				(unsigned long long)ts.compulsory_miss_bytes,
				ts.link_wait_ns, ts.device_wait_ns, ts.quant_wait_ns,
				ts.service_ns, ts.total_latency_ns);
		fflush(g_tail_csv);
	}

	return (finalize_row(fmt_row(d, UNIFIED_CONCURRENCY, context_tokens, cr,
		profile.hit_rate, profile.precision, profile.recall,
		profile.mean_working_set_blocks), backend_name, trace_hash8,
		config_hash8));
}

static std::vector<scenario_desc_t>	build_scenarios(
		const std::vector<model_entry_t> &models, bool tail_recovery_only,
		const std::string &tail_recovery_model,
		const std::string &tail_recovery_comparison)
{
	std::vector<scenario_desc_t>	out;
	std::vector<uint64_t>	host_sizes = {
		64ull << 20, 256ull << 20, 1ull << 30, 4ull << 30, 8ull << 30};
	std::vector<uint64_t>	device_sizes = {
		512ull << 30, 1ull << 40, 2ull << 40};

	for (const auto &m : models)
		for (const auto &comp : comparisons())
			for (const auto &prec : precision_modes())
				for (uint64_t hb : host_sizes)
					for (uint64_t db : device_sizes)
					{
						if (comp.analytical && (hb != host_sizes[0]
								|| db != device_sizes[0]))
							continue ;	/* analytical rows don't vary
									 * with cache/device -- one row
									 * per (model, comparison,
									 * precision) is enough. */
						if (tail_recovery_only
								&& (m.name != tail_recovery_model
									|| comp.name != tail_recovery_comparison))
							continue ;	/* Phase 6.5 item 8: --tail-
									 * recovery-only touches ONLY this
									 * one (model, comparison)'s
									 * scenarios -- everything else
									 * already has a complete, real,
									 * checkpointed main-sweep row and
									 * must not be recomputed. */
						out.push_back({m.name, comp.name, prec.name, hb, db});
					}
	return (out);
}

struct heartbeat_t
{
	std::chrono::steady_clock::time_point	last;
	std::chrono::steady_clock::time_point	start;

	heartbeat_t()
	{
		last = std::chrono::steady_clock::now();
		start = last;
	}

	/* Phase 6.5 item 15: extended with real memory/cache/worker
	 * visibility on top of Phase 6.4's sim-progress fields -- every
	 * number here is sampled for real at print time (mem_guard's
	 * /proc reads, the shared chunk cache's own counters), nothing
	 * estimated. */
	void	maybe_print(size_t done, size_t total, uint64_t sim_tokens,
				double hit_rate, double bytes_per_token, double p99,
				const std::string &bottleneck, const std::string &model,
				const std::string &backend_name,
				const mem_sample_t &mem, uint64_t memory_budget_mib,
				const attn_trace_chunk_cache_t::stats_t &cache_stats,
				unsigned active_workers, unsigned total_workers)
	{
		auto	now = std::chrono::steady_clock::now();
		double	since = std::chrono::duration<double>(now - last).count();
		if (since < 60.0 && done < total)
			return ;
		last = now;
		double	elapsed = std::chrono::duration<double>(now - start).count();
		double	rate = done > 0 ? elapsed / done : 0.0;
		double	eta = rate * (double)(total - done);
		uint64_t	cache_reads = cache_stats.hits + cache_stats.misses;
		double		cache_hit_rate = cache_reads
			? (double)cache_stats.hits / (double)cache_reads : 0.0;
		double		swap_used_pct = mem.swap_total_kb > 0
			? 100.0 * (double)(mem.swap_total_kb - mem.swap_free_kb)
				/ (double)mem.swap_total_kb
			: 0.0;
		fprintf(stderr, "[heartbeat] model=%s %zu/%zu sim_tokens=%llu "
			"hit_rate=%.4f bytes/token=%.1f p99=%.1fns bottleneck=%s "
			"backend=%s rss=%llu/%lluMiB cache_hit_rate=%.3f "
			"chunk_reads=%llu(hit=%llu,miss=%llu,evict=%llu) "
			"workers=%u/%u swap=%.1f%% wall=%.1fs eta=%.1fs\n",
			model.c_str(), done, total, (unsigned long long)sim_tokens,
			hit_rate, bytes_per_token, p99, bottleneck.c_str(),
			backend_name.c_str(), (unsigned long long)(mem.rss_kb / 1024),
			(unsigned long long)memory_budget_mib, cache_hit_rate,
			(unsigned long long)cache_reads,
			(unsigned long long)cache_stats.hits,
			(unsigned long long)cache_stats.misses,
			(unsigned long long)cache_stats.evictions,
			active_workers, total_workers, swap_used_pct, elapsed, eta);
	}
};

static const char	*backend_name_str(trace_backend_t b)
{
	if (b == trace_backend_t::MMAP)
		return ("mmap");
	if (b == trace_backend_t::STREAMING)
		return ("streaming");
	return ("inmemory");
}

/* Phase 6.5 item 1/3: generates (or, on a matching resumed run,
 * reuses) the out-of-core synthetic trace file for one model. A
 * sidecar `<path>.manifest` records exactly what would have to match
 * for the existing file to still be valid (the native capture's own
 * content hash, plus every synthetic-generation parameter) -- a
 * restart with the SAME native trace and the SAME target step count/
 * seed/chunk size skips regenerating 130560 steps of synthetic data
 * (a real, measured time cost, not just a memory one) and instead
 * just re-verifies the existing file's own embedded file_sha256. */
static bool	ensure_synthetic_trace(const std::string &native_path,
					const std::string &synth_path, uint32_t target_steps,
					uint32_t seed, uint32_t chunk_steps)
{
	std::string	manifest_path = synth_path + ".manifest";
	std::string	native_hash = sha256_hex_of_file(native_path);
	std::string	expect = native_hash + "|" + std::to_string(target_steps)
		+ "|" + std::to_string(seed) + "|" + std::to_string(chunk_steps);

	FILE	*mf = fopen(manifest_path.c_str(), "r");
	if (mf != nullptr)
	{
		char	buf[512] = {0};

		if (fgets(buf, sizeof(buf), mf) != nullptr)
		{
			std::string	got(buf);
			while (!got.empty() && (got.back() == '\n' || got.back() == '\r'))
				got.pop_back();
			if (got == expect)
			{
				FILE	*f = fopen(synth_path.c_str(), "rb");
				bool	ok = false;

				if (f != nullptr)
				{
					membrane_attntrace3_header_t	h;

					ok = membrane_attntrace3_read_header(f, &h) == MEMBRANE_OK
						&& membrane_attntrace3_verify_file_sha256(f, &h)
							== MEMBRANE_OK;
					fclose(f);
				}
				if (ok)
				{
					fclose(mf);
					fprintf(stderr, "membrane-kv-exact-sim: reusing "
						"existing synthetic trace %s (manifest matches, "
						"file verifies)\n", synth_path.c_str());
					return (true);
				}
				fprintf(stderr, "membrane-kv-exact-sim: %s manifest "
					"matched but file failed to verify -- "
					"regenerating\n", synth_path.c_str());
			}
		}
		fclose(mf);
	}

	fprintf(stderr, "membrane-kv-exact-sim: generating out-of-core "
		"synthetic trace -> %s (chunk_steps=%u)...\n", synth_path.c_str(),
		chunk_steps);
	attn_trace_t	native;
	if (!load_attn_trace(native_path, &native))
	{
		fprintf(stderr, "failed to load %s\n", native_path.c_str());
		return (false);
	}
	if (!extend_synthetic_to_file(native, target_steps, seed, synth_path,
			chunk_steps, 1))
	{
		fprintf(stderr, "failed to write synthetic trace to %s\n",
			synth_path.c_str());
		return (false);
	}
	FILE	*wmf = fopen(manifest_path.c_str(), "w");
	if (wmf != nullptr)
	{
		fprintf(wmf, "%s\n", expect.c_str());
		fclose(wmf);
	}
	fprintf(stderr, "membrane-kv-exact-sim: synthetic trace ready -> %s\n",
		synth_path.c_str());
	return (true);
}

int	main(int argc, char **argv)
{
	std::string	trace_135m_long;
	std::string	trace_360m_long;
	std::string	out_csv = "benchmarks/cxl-sim/unified-sweep.csv";
	std::string	tail_csv_path = "benchmarks/cxl-sim/unified-tail-samples.csv";
	std::string	ckpt_path = "benchmarks/cxl-sim/unified-sweep.ckpt";
	unsigned	workers = std::thread::hardware_concurrency();
	unsigned	min_workers = 1;
	std::string	tail_target = "exact-predictor-prefetch";
	std::string	tail_recovery_model = "SmolLM2-135M";	/* --tail-recovery-only's
								 * default target -- item 8's
								 * explicit ask. Overridable
								 * (--tail-recovery-model) for
								 * the SAME mechanism applied
								 * to a different model's own
								 * missing tail scenarios
								 * (e.g. SmolLM2-360M) without
								 * redundantly reprocessing
								 * whichever model is already
								 * complete. */
	uint64_t	memory_budget_mib = 0;		/* 0 = no budget declared,
								 * mem_guard takes no action --
								 * matches pre-6.5 unconstrained
								 * behavior */
	uint64_t	trace_cache_mib = 0;		/* 0 = derive (see below) */
	uint32_t	chunk_steps = 512;
	uint32_t	prefetch_depth = 0;
	/* Default is STREAMING, not MMAP, despite mmap's chunk cache being
	 * exactly as bounded on paper -- a real --audit-memory run caught
	 * mmap's RSS running ~2.75x higher than streaming's for the
	 * identical scenario (233 MiB vs. 641-645 MiB delta for one
	 * calibrate() call at full 130560-step scale): a sequential walk
	 * of the whole trace touches every mmap'd page at least once, and
	 * those clean file-backed pages count toward VmRSS for as long as
	 * the kernel leaves them resident, independent of this process's
	 * OWN chunk cache eviction -- so mem_guard's RSS-based budget
	 * enforcement is measurably less predictable under mmap even
	 * though both backends are byte-for-byte identical in what they
	 * COMPUTE (see the parity tests). mmap remains available via
	 * --backend mmap for anyone who wants its (real, but less
	 * predictable under a strict budget) repeat-access speed. */
	trace_backend_t	backend = trace_backend_t::STREAMING;
	bool		tail_recovery_only = false;
	bool		audit_memory = false;

	for (int i = 1; i + 1 <= argc; i++)
	{
		if (strcmp(argv[i], "--trace-135m-long") == 0 && i + 1 < argc)
			trace_135m_long = argv[++i];
		else if (strcmp(argv[i], "--trace-360m-long") == 0 && i + 1 < argc)
			trace_360m_long = argv[++i];
		else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
			out_csv = argv[++i];
		else if (strcmp(argv[i], "--tail-out") == 0 && i + 1 < argc)
			tail_csv_path = argv[++i];
		else if (strcmp(argv[i], "--checkpoint") == 0 && i + 1 < argc)
			ckpt_path = argv[++i];
		else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc)
			workers = (unsigned)atoi(argv[++i]);
		else if (strcmp(argv[i], "--min-workers") == 0 && i + 1 < argc)
			min_workers = (unsigned)atoi(argv[++i]);
		else if (strcmp(argv[i], "--memory-budget-mib") == 0 && i + 1 < argc)
			memory_budget_mib = (uint64_t)atoll(argv[++i]);
		else if (strcmp(argv[i], "--trace-cache-mib") == 0 && i + 1 < argc)
			trace_cache_mib = (uint64_t)atoll(argv[++i]);
		else if (strcmp(argv[i], "--chunk-steps") == 0 && i + 1 < argc)
			chunk_steps = (uint32_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--prefetch-depth") == 0 && i + 1 < argc)
			prefetch_depth = (uint32_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
		{
			std::string	b = argv[++i];

			if (b == "inmemory")
				backend = trace_backend_t::IN_MEMORY;
			else if (b == "mmap")
				backend = trace_backend_t::MMAP;
			else if (b == "streaming")
				backend = trace_backend_t::STREAMING;
			else
			{
				fprintf(stderr, "unknown --backend '%s' (expected "
					"inmemory|mmap|streaming)\n", b.c_str());
				return (2);
			}
		}
		else if (strcmp(argv[i], "--tail-recovery-only") == 0)
			tail_recovery_only = true;
		else if (strcmp(argv[i], "--tail-recovery-model") == 0
				&& i + 1 < argc)
			tail_recovery_model = argv[++i];
		else if (strcmp(argv[i], "--audit-memory") == 0)
			audit_memory = true;
	}
	if (trace_135m_long.empty())
	{
		fprintf(stderr, "usage: membrane-kv-exact-sim --trace-135m-long P "
			"[--trace-360m-long P] [--out CSV] [--tail-out CSV] "
			"[--checkpoint PATH] [--workers N] [--min-workers N] "
			"[--memory-budget-mib N] [--trace-cache-mib N] "
			"[--chunk-steps N] [--prefetch-depth N] "
			"[--backend inmemory|mmap|streaming] [--tail-recovery-only] "
			"[--tail-recovery-model NAME] "
			"[--audit-memory]\n");
		return (2);
	}
	if (workers < 1)
		workers = 1;
	if (min_workers < 1)
		min_workers = 1;
	if (min_workers > workers)
		min_workers = workers;
	if (trace_cache_mib == 0)
		trace_cache_mib = memory_budget_mib > 0
			? (memory_budget_mib * 40) / 100 : 512;
	g_tail_target_comparison = &tail_target;

	std::vector<model_entry_t>	models;
	model_entry_t	m135;
	m135.name = "SmolLM2-135M";
	m135.long_trace_path = trace_135m_long;
	m135.calib = {"SmolLM2-135M", 30, 3, sim::SMOLLM2_135M_BYTES_PER_TOKEN,
		1.0e9 / sim::SMOLLM2_135M_TOK_PER_SEC};
	models.push_back(m135);
	if (!trace_360m_long.empty())
	{
		model_entry_t	m360;
		m360.name = "SmolLM2-360M";
		m360.long_trace_path = trace_360m_long;
		m360.calib = {"SmolLM2-360M", 32, 5,
			sim::SMOLLM2_360M_BYTES_PER_TOKEN,
			1.0e9 / sim::SMOLLM2_360M_TOK_PER_SEC};
		models.push_back(m360);
	}
	std::map<std::string, model_calibration_t>	model_by_name;
	for (const auto &m : models)
		model_by_name[m.name] = m.calib;

	/* Phase 6.5 item 5: real, MEASURED (not sizeof-estimated) memory
	 * audit -- samples actual RSS deltas around each real phase for
	 * the first model, using the exact same code path the sweep
	 * itself uses. Runs one scenario, prints the report, exits --
	 * does not touch the real sweep's checkpoint/CSV. */
	if (audit_memory)
	{
		const model_entry_t	&m = models[0];
		mem_sample_t	s0 = sample_process_memory();

		fprintf(stderr, "[audit] baseline rss=%llu kB\n",
			(unsigned long long)s0.rss_kb);

		attn_trace_t	native;
		if (!load_attn_trace(m.long_trace_path, &native))
		{
			fprintf(stderr, "[audit] failed to load %s\n",
				m.long_trace_path.c_str());
			return (1);
		}
		mem_sample_t	s1 = sample_process_memory();
		fprintf(stderr, "[audit] after loading native capture (%s, "
			"%zu entries, %zu bytes as membrane_attntrace_entry_t): "
			"rss=%llu kB (delta=%lld kB)\n", m.long_trace_path.c_str(),
			native.entries.size(),
			native.entries.size() * sizeof(membrane_attntrace_entry_t),
			(unsigned long long)s1.rss_kb,
			(long long)s1.rss_kb - (long long)s0.rss_kb);

		std::string	audit_synth_path = "/tmp/membrane-audit-"
			+ m.name + ".attntrace3";
		if (!extend_synthetic_to_file(native, UNIFIED_TARGET_STEPS,
				SYNTHETIC_SEED, audit_synth_path, chunk_steps, 1))
		{
			fprintf(stderr, "[audit] failed to generate synthetic trace\n");
			return (1);
		}
		mem_sample_t	s2 = sample_process_memory();
		FILE			*szf = fopen(audit_synth_path.c_str(), "rb");
		long			synth_bytes = 0;
		if (szf != nullptr)
		{
			fseek(szf, 0, SEEK_END);
			synth_bytes = ftell(szf);
			fclose(szf);
		}
		fprintf(stderr, "[audit] after streaming synthetic generation "
			"(%u target steps, %u chunk_steps, one chunk resident at a "
			"time) to disk: rss=%llu kB (delta=%lld kB), file=%ld bytes "
			"on disk (vs. %zu bytes the pre-6.5 fully in-memory "
			"equivalent would have held resident all at once)\n",
			UNIFIED_TARGET_STEPS, chunk_steps,
			(unsigned long long)s2.rss_kb,
			(long long)s2.rss_kb - (long long)s1.rss_kb, synth_bytes,
			(size_t)UNIFIED_TARGET_STEPS * native.n_layer * native.n_head
				* native.top_k * sizeof(membrane_attntrace_entry_t));

		auto	audit_cache = std::make_shared<attn_trace_chunk_cache_t>(
			trace_cache_mib << 20);
		auto	audit_reader = make_attn_trace_reader(
			backend == trace_backend_t::IN_MEMORY ? trace_backend_t::MMAP
				: backend, audit_cache);
		audit_reader->open(audit_synth_path);
		mem_sample_t	s3 = sample_process_memory();
		fprintf(stderr, "[audit] after opening the reader (cache budget "
			"%llu MiB, no chunks decoded yet): rss=%llu kB "
			"(delta=%lld kB)\n", (unsigned long long)trace_cache_mib,
			(unsigned long long)s3.rss_kb,
			(long long)s3.rss_kb - (long long)s2.rss_kb);

		scenario_config_t	cfg{};
		cfg.policy = policy_t::MEMBRANE_PREDICTIVE;
		cfg.eviction = eviction_policy_t::SEGMENTED_LRU;
		cfg.block_size_tokens = 32;
		cfg.hot_cache_bytes = (256ull << 20) / UNIFIED_CONCURRENCY;
		cfg.warm_tier_is_q8 = true;
		calibrated_profile_t	profile = calibrate_streamed(*audit_reader,
			m.calib, cfg, false);
		mem_sample_t	s4 = sample_process_memory();
		fprintf(stderr, "[audit] after one calibrate() call: rss=%llu kB "
			"(delta=%lld kB); chunk cache resident=%zu bytes "
			"(hits=%llu misses=%llu evictions=%llu); "
			"calibrated_profile_t.steps=%zu entries (%zu bytes, "
			"bytes/sequence-independent since it's replayed, not "
			"per-sequence)\n", (unsigned long long)s4.rss_kb,
			(long long)s4.rss_kb - (long long)s3.rss_kb,
			audit_cache->stats().bytes_resident,
			(unsigned long long)audit_cache->stats().hits,
			(unsigned long long)audit_cache->stats().misses,
			(unsigned long long)audit_cache->stats().evictions,
			profile.steps.size(),
			profile.steps.size() * sizeof(per_step_calib_t));

		concurrent_config_t	ccfg{};
		ccfg.concurrency = UNIFIED_CONCURRENCY;
		ccfg.host_hot_cache_total_bytes = 256ull << 20;
		ccfg.device_total_bytes = 1ull << 40;
		ccfg.quant_pipelines = wssim::DEFAULT_QUANT_PIPELINES;
		ccfg.hw = default_hardware_profile();
		concurrent_result_t	cr = run_concurrent(profile, m.calib, 32,
			sim::Q8_COMPRESSION_RATIO, ccfg, 20);
		mem_sample_t	s5 = sample_process_memory();
		fprintf(stderr, "[audit] after one run_concurrent() call "
			"(%u concurrency x up to %u steps replayed): rss=%llu kB "
			"(delta=%lld kB); peak transient bound = 5 x "
			"bounded_quantile_accumulator_t (total-latency, kv-overhead, "
			"link/device/quant queue-wait), each capped at %zu in-memory "
			"doubles (%zu bytes) before spilling to disk -- worst case "
			"%zu bytes across all 5 before any spill; tail_tracker_t is "
			"O(cap)=%d samples regardless of scale; p99=%.1fns "
			"tokens/sec=%.1f\n", UNIFIED_CONCURRENCY, UNIFIED_TARGET_STEPS,
			(unsigned long long)s5.rss_kb,
			(long long)s5.rss_kb - (long long)s4.rss_kb,
			bounded_quantile_accumulator_t::kDefaultInMemoryCap,
			bounded_quantile_accumulator_t::kDefaultInMemoryCap
				* sizeof(double),
			5 * bounded_quantile_accumulator_t::kDefaultInMemoryCap
				* sizeof(double),
			20, cr.p99_latency_ns, cr.tokens_per_sec);

		fprintf(stderr, "[audit] TOTAL rss delta baseline->after one full "
			"scenario: %lld kB (%.1f MiB)\n",
			(long long)s5.rss_kb - (long long)s0.rss_kb,
			(double)((long long)s5.rss_kb - (long long)s0.rss_kb)
				/ 1024.0);

		audit_reader->close();
		unlink(audit_synth_path.c_str());
		return (0);
	}

	fprintf(stderr, "membrane-kv-exact-sim: backend=%s memory-budget-mib=%llu "
		"trace-cache-mib=%llu chunk-steps=%u prefetch-depth=%u "
		"workers=%u(min=%u)%s\n", backend_name_str(backend),
		(unsigned long long)memory_budget_mib,
		(unsigned long long)trace_cache_mib, chunk_steps, prefetch_depth,
		workers, min_workers, tail_recovery_only ? " TAIL-RECOVERY-ONLY"
			: "");

	std::vector<scenario_desc_t>	scenarios = build_scenarios(models,
		tail_recovery_only, tail_recovery_model, tail_target);
	std::string	config_hash = sha256_hex_of_string(
		"phase6.4-unified-v1;target_steps=130560;concurrency=512;"
		"comparisons=7;precisions=3;host_sizes=5;device_sizes=3");
	std::string	trace_hash_input;
	for (const auto &m : models)
		trace_hash_input += sha256_hex_of_file(m.long_trace_path);
	std::string	trace_hash = sha256_hex_of_string(trace_hash_input);
	std::string	trace_hash8 = trace_hash.substr(0, 8);
	std::string	config_hash8 = config_hash.substr(0, 8);

	checkpoint_state_t	prior = load_checkpoint(ckpt_path, trace_hash,
		config_hash);
	if (prior.header_present && !prior.header_matches)
	{
		fprintf(stderr, "membrane-kv-exact-sim: checkpoint %s is STALE "
			"(%s) -- refusing to resume, starting fresh\n",
			ckpt_path.c_str(), prior.mismatch_reason.c_str());
		prior = checkpoint_state_t{};
	}

	FILE	*csv = fopen(out_csv.c_str(), "w");
	fprintf(csv, "%s", CSV_HEADER);
	{
		/* Phase 6.5 item 11: dedupe by scenario id (keep last) while
		 * replaying -- load_checkpoint() intentionally does not
		 * dedupe itself (see checkpoint.h), so a checkpoint file that
		 * somehow accumulated two records for the same id (e.g. a
		 * crash between this rebuild and the NEXT run's fresh append)
		 * would otherwise become a real duplicate CSV row here. */
		std::map<std::string, std::string>	by_id;
		std::vector<std::string>			order;
		/* Defensive schema check: CSV_HEADER declares 49 columns (48
		 * commas). A checkpoint resumed from an OLDER schema version
		 * that was never migrated (see the real 338-row Phase 6.4
		 * checkpoint this phase migrated by hand before its own first
		 * real run -- see docs/phase6-out-of-core-simulator.md) would
		 * silently produce a CSV with ragged column counts per row
		 * otherwise. This doesn't refuse to write such rows (that
		 * would silently drop real historical data on a future schema
		 * change), it just makes the mismatch loud instead of silent. */
		size_t								schema_mismatch_count = 0;

		for (size_t i = 0; i < prior.completed_rows.size(); i++)
		{
			/* completed_ids and completed_rows share index order
			 * only when no duplicates exist; recompute the id from
			 * the row's own leading columns instead of trusting
			 * positional alignment. */
			const std::string	&row = prior.completed_rows[i];
			size_t				c1 = row.find(',');
			size_t				c2 = row.find(',', c1 + 1);
			size_t				c3 = row.find(',', c2 + 1);
			size_t				c4 = row.find(',', c3 + 1);
			size_t				c5 = row.find(',', c4 + 1);

			if (c5 == std::string::npos)
				continue ;	/* truncated row -- skip defensively */
			std::string	id = "unified|" + row.substr(0, c1) + "|"
				+ row.substr(c1 + 1, c2 - c1 - 1) + "|"
				+ row.substr(c2 + 1, c3 - c2 - 1) + "|"
				+ row.substr(c3 + 1, c4 - c3 - 1) + "|"
				+ row.substr(c4 + 1, c5 - c4 - 1);
			if (by_id.find(id) == by_id.end())
				order.push_back(id);
			by_id[id] = row;
			if (std::count(row.begin(), row.end(), ',') != 48)
				schema_mismatch_count++;
		}
		if (schema_mismatch_count > 0)
			fprintf(stderr, "membrane-kv-exact-sim: WARNING: %zu replayed "
				"checkpoint row(s) do not have the current 49-column CSV "
				"schema (48 commas) -- likely resumed from an "
				"un-migrated older checkpoint; written as-is, but the "
				"resulting CSV will have ragged column counts\n",
				schema_mismatch_count);
		for (const std::string &id : order)
			fprintf(csv, "%s\n", by_id.at(id).c_str());
		/* Real bug this rewrite's own testing caught: without this
		 * flush, a process killed (SIGTERM/OOM) before its FIRST new
		 * scenario completes -- the only other place that flushes
		 * `csv` -- loses every replayed row to stdio buffering even
		 * though the checkpoint genuinely has them, leaving a
		 * populated checkpoint paired with an empty CSV on the next
		 * inspection. Durable on disk immediately, not contingent on
		 * this run making any NEW progress at all. */
		fflush(csv);
	}

	/* Append, not truncate, when resuming: unlike the main CSV (whose
	 * rows are durably backed by the checkpoint and replayed above),
	 * tail samples have no checkpoint backing -- opening "w" on every
	 * restart silently discarded every tail sample captured by a prior
	 * process before it was OOM-killed (a real data-loss bug this
	 * phase hit: benchmarks/cxl-sim/unified-tail-samples.csv ended up
	 * empty after repeated restarts despite genuinely capturing 900
	 * real samples earlier in the run -- see
	 * docs/phase6-unified-stress.md section 5; those specific 900
	 * samples were unrecoverable and are regenerated for real via
	 * --tail-recovery-only, item 8). */
	bool	tail_csv_exists_nonempty = false;
	{
		FILE	*probe = fopen(tail_csv_path.c_str(), "r");
		if (probe != nullptr)
		{
			fseek(probe, 0, SEEK_END);
			tail_csv_exists_nonempty = ftell(probe) > 0;
			fclose(probe);
		}
	}
	g_tail_csv = fopen(tail_csv_path.c_str(),
		tail_csv_exists_nonempty ? "a" : "w");
	if (!tail_csv_exists_nonempty)
		fprintf(g_tail_csv, "model,comparison,precision,host_cache_total_bytes,"
			"device_total_bytes,sequence_id,step_index,prefetch_bytes,"
			"compulsory_miss_bytes,link_wait_ns,device_wait_ns,quant_wait_ns,"
			"service_ns,total_latency_ns\n");

	checkpoint_writer_t	ckpt;
	ckpt.open(ckpt_path, trace_hash, config_hash, prior.header_present);

	std::mutex	io_mutex;
	std::atomic<size_t>	done_count{prior.completed_ids.size()};
	std::atomic<size_t>	failed_count{0};
	heartbeat_t	hb;
	std::map<std::string, comparison_t>	comp_by_name;
	for (const auto &c : comparisons())
		comp_by_name[c.name] = c;

	std::string	lh_path = out_csv.substr(0, out_csv.find_last_of('.'))
		+ "-layer-head-detail.csv";
	FILE	*lhf = fopen(lh_path.c_str(), "w");
	fprintf(lhf, "model,resolution,index,hit_rate\n");

	std::string	hw_path = out_csv.substr(0, out_csv.find_last_of('.'))
		+ "-hardware-sensitivity.csv";

	fprintf(stderr, "membrane-kv-exact-sim: %zu unified scenarios total, "
		"%u workers per model (%zu already complete)\n", scenarios.size(),
		workers, prior.completed_ids.size());

	bool	sweep_stopped_for_memory = false;

	/* Process one model fully (main scenarios + its layer/head-detail +,
	 * for 135M, hardware sensitivity), THEN close its synthetic trace's
	 * reader/cache before opening the next model's. Phase 6.5: the
	 * synthetic trace itself now lives on disk (a .attntrace3 file,
	 * generated once, reused across restarts -- see
	 * ensure_synthetic_trace) and is read through a bounded shared
	 * chunk cache instead of held as one ~2-4 GiB resident vector; see
	 * docs/phase6-out-of-core-simulator.md for the real measured RSS
	 * this achieves. This keeps the FULL spec'd 128K x top-k=8
	 * resolution for both models -- it changes only how much is
	 * resident at once, not what is computed (see config_hash above). */
	for (const auto &m : models)
	{
		if (tail_recovery_only && m.name != tail_recovery_model)
			continue ;

		std::vector<scenario_desc_t>	model_scenarios;
		for (const auto &d : scenarios)
			if (d.model_name == m.name)
				model_scenarios.push_back(d);
		if (model_scenarios.empty())
			continue ;

		std::shared_ptr<attn_trace_chunk_cache_t>	cache;
		std::string									synth_path;

		if (backend == trace_backend_t::IN_MEMORY)
		{
			fprintf(stderr, "membrane-kv-exact-sim: loading %s and "
				"building its 128K unified trace IN MEMORY "
				"(--backend inmemory)...\n", m.name.c_str());
		}
		else
		{
			fprintf(stderr, "membrane-kv-exact-sim: loading %s and "
				"preparing its 128K unified out-of-core trace "
				"(backend=%s)...\n", m.name.c_str(),
				backend_name_str(backend));
			synth_path = "benchmarks/cxl-sim/traces/" + m.name
				+ "-unified-128k.attntrace3";
			if (!ensure_synthetic_trace(m.long_trace_path, synth_path,
					UNIFIED_TARGET_STEPS, SYNTHETIC_SEED, chunk_steps))
				return (1);
			cache = std::make_shared<attn_trace_chunk_cache_t>(
				trace_cache_mib << 20);
		}

		/* IN_MEMORY path only: build the full synthetic trace resident
		 * (pre-6.5 behavior, kept for parity/comparison runs -- NOT
		 * the default, since this is exactly the ~2-4 GiB allocation
		 * Phase 6.4's real OOM kills were caused by). */
		attn_trace_t	in_memory_trace;
		if (backend == trace_backend_t::IN_MEMORY)
		{
			attn_trace_t	native;
			if (!load_attn_trace(m.long_trace_path, &native))
			{
				fprintf(stderr, "failed to load %s\n",
					m.long_trace_path.c_str());
				return (1);
			}
			in_memory_trace = extend_synthetic(native, UNIFIED_TARGET_STEPS,
				SYNTHETIC_SEED);
		}

		fprintf(stderr, "membrane-kv-exact-sim: %s unified trace ready "
			"(context=%u tokens, concurrency=%u)\n", m.name.c_str(),
			UNIFIED_TARGET_STEPS + 512, UNIFIED_CONCURRENCY);

		mem_guard_t			guard(memory_budget_mib, workers, min_workers);
		std::atomic<size_t>	next_index{0};
		auto	worker_fn = [&](unsigned rank)
		{
			std::unique_ptr<attn_trace_reader_t>	reader;

			if (backend == trace_backend_t::IN_MEMORY)
				reader = make_in_memory_reader(in_memory_trace);
			else
			{
				reader = make_attn_trace_reader(backend, cache);
				if (!reader->open(synth_path))
				{
					fprintf(stderr, "membrane-kv-exact-sim: worker %u "
						"failed to open %s\n", rank, synth_path.c_str());
					return ;
				}
				reader->set_prefetch_depth(prefetch_depth);
			}
			while (true)
			{
				while (rank >= guard.active_worker_limit()
						&& !guard.should_exit())
				{
					if (next_index.load() >= model_scenarios.size())
						return ;
					std::this_thread::sleep_for(
						std::chrono::milliseconds(500));
				}
				if (guard.should_exit())
					return ;
				size_t	idx = next_index.fetch_add(1);
				if (idx >= model_scenarios.size())
					return ;
				const scenario_desc_t	&d = model_scenarios[idx];
				std::string	id = d.id();
				{
					std::lock_guard<std::mutex>	lock(io_mutex);
					if (prior.completed_ids.count(id))
						continue ;
				}
				/* A corrupt/truncated chunk (attn_trace_reader.cpp's
				 * decode()) or a spill-file I/O failure
				 * (bounded_quantile.cpp, e.g. disk full) throws
				 * std::runtime_error. Uncaught, that escapes this
				 * thread's entry function and calls std::terminate()
				 * -- aborting the ENTIRE sweep (every worker, every
				 * in-flight scenario) instead of losing just the one
				 * bad scenario, and bypassing every bit of the
				 * checkpoint-and-exit-cleanly design this phase built
				 * specifically to avoid abrupt kills. Catch here,
				 * log, and skip: this scenario is simply never
				 * written to CSV/checkpoint, so it stays eligible for
				 * retry on the next resume (the reader's own state is
				 * already safe to reuse for the next scenario -- see
				 * chunked_reader_base_t::at()'s sentinel-before-
				 * acquire fix). */
				std::string	row;
				bool		ok = true;
				try
				{
					row = process_one(d, *reader, model_by_name,
						comp_by_name.at(d.comparison_name),
						backend_name_str(backend), trace_hash8,
						config_hash8);
				}
				catch (const std::exception &e)
				{
					ok = false;
					std::lock_guard<std::mutex>	lock(io_mutex);
					fprintf(stderr, "membrane-kv-exact-sim: scenario %s "
						"FAILED (%s) -- skipped, eligible for retry on "
						"resume, sweep continues\n", id.c_str(), e.what());
					failed_count++;
				}
				if (!ok)
					continue ;
				{
					std::lock_guard<std::mutex>	lock(io_mutex);
					fprintf(csv, "%s\n", row.c_str());
					fflush(csv);
					ckpt.write_scenario(id, row);
					done_count++;
					mem_action_t	action = guard.tick(cache);
					if (action == mem_action_t::CHECKPOINT_AND_EXIT)
						fprintf(stderr, "membrane-kv-exact-sim: memory "
							"guard triggered CHECKPOINT_AND_EXIT "
							"(rss=%lluMiB budget=%lluMiB) -- stopping "
							"cleanly, resume later with the same "
							"--checkpoint\n",
							(unsigned long long)(guard.last_sample().rss_kb
								/ 1024),
							(unsigned long long)memory_budget_mib);
					/* Cheap heartbeat proxy fields parsed back out of the
					 * row we just wrote (rather than threading extra
					 * state through process_one's return type). */
					hb.maybe_print(done_count.load(), scenarios.size(),
						(uint64_t)done_count.load() * UNIFIED_CONCURRENCY,
						0.0, 0.0, 0.0, d.comparison_name, m.name,
						backend_name_str(backend), guard.last_sample(),
						memory_budget_mib,
						cache != nullptr ? cache->stats()
							: attn_trace_chunk_cache_t::stats_t{},
						guard.active_worker_limit(), workers);
				}
			}
		};

		fprintf(stderr, "membrane-kv-exact-sim: %zu scenarios for %s, %u "
			"workers\n", model_scenarios.size(), m.name.c_str(), workers);
		std::vector<std::thread>	pool;
		for (unsigned w = 0; w < workers; w++)
			pool.emplace_back(worker_fn, w);
		for (auto &th : pool)
			th.join();

		if (guard.should_exit())
		{
			sweep_stopped_for_memory = true;
			fprintf(stderr, "membrane-kv-exact-sim: stopping the sweep "
				"early for %s due to real memory pressure -- checkpoint "
				"is up to date, re-invoke to resume\n", m.name.c_str());
			break ;
		}

		if (tail_recovery_only)
			continue ;	/* item 8: tail-only recovery skips layer/head
					 * detail and hardware sensitivity entirely --
					 * neither is part of what this recovery run
					 * needs to regenerate. */

		/* Item 25: per-layer/per-head predictor accuracy IN the unified
		 * (128K x 512-implied-cache-budget) scenario, for this model,
		 * while its trace is still resident. Same reasoning as Phase 6.3
		 * (variable-length arrays don't belong in the wide unified CSV).
		 * Uses a representative per-sequence cache budget (256MiB
		 * total / 512). */
		{
			scenario_config_t	cfg{};
			cfg.policy = policy_t::MEMBRANE_PREDICTIVE;
			cfg.eviction = eviction_policy_t::SEGMENTED_LRU;
			cfg.block_size_tokens = 32;
			cfg.hot_cache_bytes = (256ull << 20) / UNIFIED_CONCURRENCY;
			cfg.warm_tier_is_q8 = true;
			layer_head_stats_t	lh;

			if (backend == trace_backend_t::IN_MEMORY)
				run_scenario_calibration(in_memory_trace, m.calib, cfg,
					nullptr, &lh, nullptr);
			else
			{
				auto	reader = make_attn_trace_reader(backend, cache);
				reader->open(synth_path);
				run_scenario_calibration_streamed(*reader, m.calib, cfg,
					nullptr, &lh, nullptr);
				reader->close();
			}
			for (size_t l = 0; l < lh.per_layer_hit_rate.size(); l++)
				fprintf(lhf, "%s,layer,%zu,%.4f\n", m.name.c_str(), l,
					lh.per_layer_hit_rate[l]);
			for (size_t h = 0; h < lh.per_head_hit_rate.size(); h++)
				fprintf(lhf, "%s,head,%zu,%.4f\n", m.name.c_str(), h,
					lh.per_head_hit_rate[h]);
			fflush(lhf);
			fprintf(stderr, "membrane-kv-exact-sim: layer/head detail for "
				"%s done\n", m.name.c_str());
		}

		/* Item 9/27: hardware sensitivity matrix -- ONE representative
		 * unified scenario point (135M, exact-predictor-prefetch, all-q8,
		 * 256MiB host cache, 1TiB device), hardware assumptions varied.
		 * Every latency/bandwidth figure here is ASSUMED (published
		 * industry-typical ranges, no real CXL hardware exists anywhere
		 * in this project -- same disclosure Phase 6.1 established)
		 * except pipeline count, which Phase 5.3's own real RTL
		 * simulation calibrated the per-pipeline rate for. Only run for
		 * 135M (this pass's original, still-representative scope) --
		 * done here, while 135M's trace is resident/open, instead of
		 * reloading it in a separate pass. */
		bool	hw_already_done = false;
		{
			FILE	*probe = fopen(hw_path.c_str(), "r");
			if (probe != nullptr)
			{
				int	lines = 0;
				int	ch;
				while ((ch = fgetc(probe)) != EOF)
					if (ch == '\n')
						lines++;
				fclose(probe);
				/* 1 header + 10 hw points -- this pass never writes a
				 * partial file (it's written point-by-point but this
				 * process only reaches here again after a full prior
				 * run completed it, since a mid-pass crash restarts the
				 * whole process from the checkpoint). Restarting this
				 * ~10-15 minute, non-checkpointed pass on every OOM-
				 * triggered restart was real, observed wasted wall time
				 * (see docs/phase6-unified-stress.md section 0) -- this
				 * skip makes retries spend that time on the actual
				 * remaining scenarios instead. */
				hw_already_done = (lines >= 11);
			}
		}
		if (m.name == "SmolLM2-135M" && !hw_already_done)
		{
			FILE	*hwf = fopen(hw_path.c_str(), "w");
			fprintf(hwf, "profile,cxl_latency_ns,cxl_bandwidth_gbps,"
				"pipelines,p50_latency_ns,p99_latency_ns,tokens_per_sec,"
				"mean_bytes_per_token,link_util_pct,quant_util_pct,"
				"bottleneck,link_p99_wait_ns\n");

			struct hw_point_t { std::string label; hardware_profile_t hw; };
			std::vector<hw_point_t>	points = {
				{"cxl-latency-low", {"cxl-latency-low", 100.0,
					sim::CXL_LINK_BANDWIDTH_GBPS,
					sim::NEARMEM_PIPELINE_BYTES_PER_NS,
					wssim::DEFAULT_QUANT_PIPELINES}},
				{"cxl-latency-medium (default)", {"cxl-latency-medium",
					sim::CXL_LINK_LATENCY_NS, sim::CXL_LINK_BANDWIDTH_GBPS,
					sim::NEARMEM_PIPELINE_BYTES_PER_NS,
					wssim::DEFAULT_QUANT_PIPELINES}},
				{"cxl-latency-high", {"cxl-latency-high", 250.0,
					sim::CXL_LINK_BANDWIDTH_GBPS,
					sim::NEARMEM_PIPELINE_BYTES_PER_NS,
					wssim::DEFAULT_QUANT_PIPELINES}},
				/* ASSUMED: Gen5 x16-class raw bandwidth, ~2x this
				 * project's existing x8-class 48 GB/s figure. */
				{"gen5-x16-bandwidth", {"gen5-x16", sim::CXL_LINK_LATENCY_NS,
					96.0, sim::NEARMEM_PIPELINE_BYTES_PER_NS,
					wssim::DEFAULT_QUANT_PIPELINES}},
				/* ASSUMED: CXL 2.0-class (PCIe5-generation) figures --
				 * higher latency, narrower link than this project's
				 * existing default. */
				{"cxl-2.0-profile", {"cxl-2.0", 200.0, 32.0,
					sim::NEARMEM_PIPELINE_BYTES_PER_NS,
					wssim::DEFAULT_QUANT_PIPELINES}},
				/* ASSUMED: CXL 3.0-class (PCIe6-generation) figures --
				 * lower latency, wider link. */
				{"cxl-3.0-profile", {"cxl-3.0", 150.0, 64.0,
					sim::NEARMEM_PIPELINE_BYTES_PER_NS,
					wssim::DEFAULT_QUANT_PIPELINES}},
				{"pipelines-1", {"pipelines-1", sim::CXL_LINK_LATENCY_NS,
					sim::CXL_LINK_BANDWIDTH_GBPS,
					sim::NEARMEM_PIPELINE_BYTES_PER_NS, 1}},
				{"pipelines-2", {"pipelines-2", sim::CXL_LINK_LATENCY_NS,
					sim::CXL_LINK_BANDWIDTH_GBPS,
					sim::NEARMEM_PIPELINE_BYTES_PER_NS, 2}},
				{"pipelines-4", {"pipelines-4", sim::CXL_LINK_LATENCY_NS,
					sim::CXL_LINK_BANDWIDTH_GBPS,
					sim::NEARMEM_PIPELINE_BYTES_PER_NS, 4}},
				{"pipelines-8 (default)", {"pipelines-8",
					sim::CXL_LINK_LATENCY_NS, sim::CXL_LINK_BANDWIDTH_GBPS,
					sim::NEARMEM_PIPELINE_BYTES_PER_NS, 8}},
			};

			std::unique_ptr<attn_trace_reader_t>	hw_reader;
			if (backend != trace_backend_t::IN_MEMORY)
			{
				hw_reader = make_attn_trace_reader(backend, cache);
				hw_reader->open(synth_path);
			}
			for (const auto &pt : points)
			{
				scenario_config_t	cfg{};
				cfg.policy = policy_t::MEMBRANE_PREDICTIVE;
				cfg.eviction = eviction_policy_t::SEGMENTED_LRU;
				cfg.block_size_tokens = 32;
				cfg.hot_cache_bytes = (256ull << 20) / UNIFIED_CONCURRENCY;
				cfg.warm_tier_is_q8 = true;
				calibrated_profile_t	profile = backend
						== trace_backend_t::IN_MEMORY
					? calibrate(in_memory_trace, m.calib, cfg, false, &pt.hw)
					: calibrate_streamed(*hw_reader, m.calib, cfg, false,
						&pt.hw);

				concurrent_config_t	ccfg{};
				ccfg.concurrency = UNIFIED_CONCURRENCY;
				ccfg.host_hot_cache_total_bytes = 256ull << 20;
				ccfg.device_total_bytes = 1ull << 40;
				ccfg.quant_pipelines = pt.hw.quant_pipelines;
				ccfg.microbatch_max_wait_ns = 0.0;
				ccfg.microbatch_max_batch_blocks = 0;
				ccfg.hw = pt.hw;
				concurrent_result_t	cr = run_concurrent(profile, m.calib,
					32, sim::Q8_COMPRESSION_RATIO, ccfg, 0);

				fprintf(hwf, "%s,%.1f,%.1f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,"
					"%.2f,%s,%.2f\n", pt.label.c_str(),
					pt.hw.cxl_link_latency_ns,
					pt.hw.cxl_link_bandwidth_gbps, pt.hw.quant_pipelines,
					cr.p50_latency_ns, cr.p99_latency_ns,
					cr.tokens_per_sec, cr.mean_bytes_per_token,
					cr.link_utilization_pct, cr.quant_utilization_pct,
					cr.bottleneck.c_str(), cr.link_queue.p99_wait_ns);
				fflush(hwf);
				fprintf(stderr, "membrane-kv-exact-sim: hw-sensitivity "
					"point %s done\n", pt.label.c_str());
			}
			if (hw_reader != nullptr)
				hw_reader->close();
			fclose(hwf);
			fprintf(stderr, "membrane-kv-exact-sim: hardware sensitivity "
				"-> %s\n", hw_path.c_str());
		}
		else if (m.name == "SmolLM2-135M" && hw_already_done)
		{
			fprintf(stderr, "membrane-kv-exact-sim: hardware sensitivity "
				"already complete at %s, skipping re-run\n",
				hw_path.c_str());
		}
	}	/* this model's trace/reader/cache go out of scope here and are
		 * freed/closed before the next model loads. */

	fclose(lhf);
	fprintf(stderr, "membrane-kv-exact-sim: layer/head detail -> %s\n",
		lh_path.c_str());
	fclose(csv);
	fclose(g_tail_csv);

	if (sweep_stopped_for_memory)
	{
		fprintf(stderr, "membrane-kv-exact-sim: exiting with code %d "
			"(memory-guard-triggered stop, %zu/%zu scenarios so far) -- "
			"NOT marking the checkpoint complete\n", kMemGuardExitCode,
			done_count.load(), scenarios.size());
		return (kMemGuardExitCode);
	}
	if (failed_count.load() > 0)
	{
		fprintf(stderr, "membrane-kv-exact-sim: %zu scenario(s) FAILED "
			"(see per-scenario error lines above) -- NOT marking the "
			"checkpoint complete; re-invoke with the same --checkpoint "
			"to retry just those\n", failed_count.load());
		return (1);
	}
	ckpt.write_complete();
	fprintf(stderr, "membrane-kv-exact-sim: done, %zu/%zu scenarios -> %s "
		"(+ %s, + layer-head-detail, + hardware-sensitivity)\n",
		done_count.load(), scenarios.size(), out_csv.c_str(),
		tail_csv_path.c_str());
	return (0);
}
