/*
 * membrane-kv-workingset-sim: Phase 6.2's discrete-event working-set/
 * hot-cache/prefetch simulator. Consumes real .attntrace files
 * (membrane-kv-attn-trace-capture's output) and sweeps
 * (working-set policy x eviction policy x block granularity x
 * hot-cache size) for both SmolLM2 models, reusing Phase 6.1's real
 * calibration constants (tools/membrane-cxl-sim/sim_config.h)
 * unmodified for link/quant-engine costs. See
 * docs/phase6-attention-working-set.md for the full writeup.
 */

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "attn_workload.h"
#include "checkpoint.h"
#include "engine.h"
#include "policy.h"
#include "sim_config.h"

using namespace wssim;

static std::vector<policy_t>	all_policies()
{
	return {
		policy_t::FULL, policy_t::SLIDING_WINDOW, policy_t::RECENCY_SINKS,
		policy_t::TOPK_LAG1, policy_t::HEAVY_HITTER,
		policy_t::RECENCY_FREQUENCY_HYBRID, policy_t::ORACLE,
		policy_t::MEMBRANE_PREDICTIVE
	};
}

static std::vector<eviction_policy_t>	all_evictions()
{
	return {
		eviction_policy_t::LRU, eviction_policy_t::LFU,
		eviction_policy_t::ATTENTION_SCORE_AWARE,
		eviction_policy_t::SEGMENTED_LRU
	};
}

struct model_entry_t
{
	std::string				name;
	std::string				trace_path;
	model_calibration_t	calib;
};

static std::string	csv_row(const std::string &model,
					const scenario_result_t &r)
{
	char	buf[1024];

	snprintf(buf, sizeof(buf),
		"%s,%s,%s,%u,%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%llu,%llu,%.3f,"
		"%.4f,%.4f,%.4f,%.4f,%.4f,%llu",
		model.c_str(), r.policy_name.c_str(), r.eviction_name.c_str(),
		r.block_size_tokens, (unsigned long long)r.hot_cache_bytes,
		r.p50_latency_ns, r.p95_latency_ns, r.p99_latency_ns,
		r.mean_transferred_bytes_per_token, r.metadata_overhead_ns_per_token,
		r.hot_cache_hit_rate, (unsigned long long)r.redundant_fetches,
		(unsigned long long)r.wasted_prefetch_bytes,
		r.mean_working_set_blocks, r.precision, r.recall,
		r.prefetch_hit_rate, r.late_fetch_rate, r.false_prefetch_rate,
		(unsigned long long)r.additional_link_traffic_bytes);
	return (std::string(buf));
}

static const char	*CSV_HEADER =
	"model,policy,eviction,block_size_tokens,hot_cache_bytes,"
	"p50_latency_ns,p95_latency_ns,p99_latency_ns,"
	"mean_bytes_per_token,metadata_ns_per_token,hot_cache_hit_rate,"
	"redundant_fetches,wasted_prefetch_bytes,mean_working_set_blocks,"
	"precision,recall,prefetch_hit_rate,late_fetch_rate,"
	"false_prefetch_rate,additional_link_traffic_bytes\n";

struct heartbeat_t
{
	std::chrono::steady_clock::time_point	last;
	std::chrono::steady_clock::time_point	start;

	heartbeat_t()
	{
		last = std::chrono::steady_clock::now();
		start = last;
	}

	void	maybe_print(size_t done, size_t total, const std::string &tag)
	{
		auto	now = std::chrono::steady_clock::now();
		double	since_last = std::chrono::duration<double>(now - last).count();
		if (since_last < 60.0 && done < total)
			return ;
		last = now;
		double	elapsed = std::chrono::duration<double>(now - start).count();
		double	rate = done > 0 ? elapsed / done : 0.0;
		double	eta = rate * (double)(total - done);
		fprintf(stderr, "[heartbeat] %s %zu/%zu wall=%.1fs eta=%.1fs\n",
			tag.c_str(), done, total, elapsed, eta);
	}
};

static void	run_sweep_a(const std::vector<model_entry_t> &models,
				const std::string &out_csv, const std::string &ckpt_path,
				const std::string &config_hash)
{
	std::vector<uint32_t>	block_sizes = {16, 32, 64, 128, 256};
	std::vector<uint64_t>	cache_sizes = {
		64ull << 20, 256ull << 20, 1ull << 30, 4ull << 30, 8ull << 30};
	std::vector<policy_t>			policies = all_policies();
	std::vector<eviction_policy_t>	evictions = all_evictions();

	std::string	trace_hash;
	for (const auto &m : models)
		trace_hash += sha256_hex_of_file(m.trace_path);
	trace_hash = sha256_hex_of_string(trace_hash);

	checkpoint_state_t	prior = load_checkpoint(ckpt_path, trace_hash,
		config_hash);
	if (prior.header_present && !prior.header_matches)
	{
		fprintf(stderr, "membrane-kv-workingset-sim: checkpoint %s is "
			"STALE (%s) -- refusing to resume, starting fresh\n",
			ckpt_path.c_str(), prior.mismatch_reason.c_str());
		prior = checkpoint_state_t{};
	}

	FILE	*csv = fopen(out_csv.c_str(), "w");
	fprintf(csv, "%s", CSV_HEADER);
	for (const std::string &row : prior.completed_rows)
		fprintf(csv, "%s\n", row.c_str());

	checkpoint_writer_t	ckpt;
	ckpt.open(ckpt_path, trace_hash, config_hash, prior.header_present);

	size_t	total = models.size() * block_sizes.size() * cache_sizes.size()
		* evictions.size() * policies.size();
	size_t	done = prior.completed_ids.size();
	heartbeat_t	hb;

	for (const auto &m : models)
	{
		attn_trace_t	native;
		if (!load_attn_trace(m.trace_path, &native))
		{
			fprintf(stderr, "membrane-kv-workingset-sim: failed to load "
				"%s\n", m.trace_path.c_str());
			continue ;
		}
		for (uint32_t bs : block_sizes)
		{
			attn_trace_t	regrouped = regroup_to_block_size(native, bs);
			for (uint64_t cache_bytes : cache_sizes)
			{
				for (eviction_policy_t ev : evictions)
				{
					for (policy_t p : policies)
					{
						char	id[256];
						snprintf(id, sizeof(id), "%s|%s|%s|%u|%llu",
							m.name.c_str(), policy_name(p),
							eviction_policy_name(ev), bs,
							(unsigned long long)cache_bytes);
						if (prior.completed_ids.count(id))
							continue ;
						scenario_config_t	cfg{};
						cfg.policy = p;
						cfg.eviction = ev;
						cfg.block_size_tokens = bs;
						cfg.hot_cache_bytes = cache_bytes;
						cfg.warm_tier_is_q8 = true;
						scenario_result_t	r = run_scenario(regrouped,
							m.calib, cfg);
						std::string	row = csv_row(m.name, r);
						fprintf(csv, "%s\n", row.c_str());
						fflush(csv);
						ckpt.write_scenario(id, row);
						done++;
						hb.maybe_print(done, total, "sweep-a");
					}
				}
			}
		}
	}
	ckpt.write_complete();
	fclose(csv);
	fprintf(stderr, "membrane-kv-workingset-sim: sweep A done, %zu/%zu "
		"scenarios -> %s\n", done, total, out_csv.c_str());
}

static void	run_sweep_b(const std::vector<model_entry_t> &models,
				const std::string &out_csv)
{
	/*
	 * Capped well below the spec's up-to-128K-token target: FULL
	 * policy's cost to literally SIMULATE (not just model) is
	 * O(steps^2) here, since a full re-read predicts current_num_blocks
	 * (itself O(steps)) at every one of O(steps) decode steps -- the
	 * same real full-reread cost Phase 6.1 found made the actual
	 * system slow at large scale (docs/phase6-cxl-near-memory.md
	 * section 9/12) also makes it expensive to SIMULATE at that scale.
	 * A real, disclosed tractability reduction (see
	 * docs/phase6-attention-working-set.md), not a silent one --
	 * ORACLE/MEMBRANE_PREDICTIVE alone are far cheaper and would
	 * happily run further.
	 */
	std::vector<uint32_t>	context_steps = {512, 1024, 2048, 4096};
	std::vector<policy_t>	policies = {
		policy_t::FULL, policy_t::ORACLE, policy_t::MEMBRANE_PREDICTIVE};

	FILE	*csv = fopen(out_csv.c_str(), "w");
	fprintf(csv, "context_steps,%s", CSV_HEADER);
	for (const auto &m : models)
	{
		attn_trace_t	native;
		if (!load_attn_trace(m.trace_path, &native))
			continue ;
		attn_trace_t	regrouped = regroup_to_block_size(native, 32);
		for (uint32_t steps : context_steps)
		{
			attn_trace_t	ext = extend_synthetic(regrouped, steps, 12345u);
			for (policy_t p : policies)
			{
				scenario_config_t	cfg{};
				cfg.policy = p;
				cfg.eviction = eviction_policy_t::SEGMENTED_LRU;
				cfg.block_size_tokens = 32;
				cfg.hot_cache_bytes = 256ull << 20;
				cfg.warm_tier_is_q8 = true;
				scenario_result_t	r = run_scenario(ext, m.calib, cfg);
				fprintf(csv, "%u,%s\n", steps, csv_row(m.name, r).c_str());
				fflush(csv);
				fprintf(stderr, "[sweep-b] model=%s context_steps=%u "
					"policy=%s bytes/token=%.1f p99_ns=%.1f\n",
					m.name.c_str(), steps, policy_name(p),
					r.mean_transferred_bytes_per_token, r.p99_latency_ns);
			}
		}
	}
	fclose(csv);
	fprintf(stderr, "membrane-kv-workingset-sim: sweep B done -> %s\n",
		out_csv.c_str());
}

int	main(int argc, char **argv)
{
	std::string	trace_135m;
	std::string	trace_360m;
	std::string	out_a = "benchmarks/cxl-sim/workingset-sweep.csv";
	std::string	out_b = "benchmarks/cxl-sim/workingset-context-sweep.csv";
	std::string	ckpt = "benchmarks/cxl-sim/workingset-sweep.ckpt";
	bool		do_sweep_b = true;

	for (int i = 1; i + 1 <= argc; i++)
	{
		if (strcmp(argv[i], "--trace-135m") == 0 && i + 1 < argc)
			trace_135m = argv[++i];
		else if (strcmp(argv[i], "--trace-360m") == 0 && i + 1 < argc)
			trace_360m = argv[++i];
		else if (strcmp(argv[i], "--out-a") == 0 && i + 1 < argc)
			out_a = argv[++i];
		else if (strcmp(argv[i], "--out-b") == 0 && i + 1 < argc)
			out_b = argv[++i];
		else if (strcmp(argv[i], "--checkpoint") == 0 && i + 1 < argc)
			ckpt = argv[++i];
		else if (strcmp(argv[i], "--no-sweep-b") == 0)
			do_sweep_b = false;
	}
	if (trace_135m.empty() || trace_360m.empty())
	{
		fprintf(stderr, "usage: membrane-kv-workingset-sim --trace-135m P "
			"--trace-360m P [--out-a CSV] [--out-b CSV] "
			"[--checkpoint PATH] [--no-sweep-b]\n");
		return (2);
	}

	std::vector<model_entry_t>	models;
	model_entry_t	m135;
	m135.name = "SmolLM2-135M";
	m135.trace_path = trace_135m;
	m135.calib = {"SmolLM2-135M", 30, 3, sim::SMOLLM2_135M_BYTES_PER_TOKEN,
		1.0e9 / sim::SMOLLM2_135M_TOK_PER_SEC};
	models.push_back(m135);
	model_entry_t	m360;
	m360.name = "SmolLM2-360M";
	m360.trace_path = trace_360m;
	m360.calib = {"SmolLM2-360M", 32, 5, sim::SMOLLM2_360M_BYTES_PER_TOKEN,
		1.0e9 / sim::SMOLLM2_360M_TOK_PER_SEC};
	models.push_back(m360);

	std::string	config_hash = sha256_hex_of_string(
		"block_sizes=16,32,64,128,256;"
		"cache_sizes=64Mi,256Mi,1Gi,4Gi,8Gi;"
		"evictions=lru,lfu,attn,segmented;"
		"policies=full,sliding,recency_sinks,topk_lag1,heavy_hitter,"
		"hybrid,oracle,membrane_predictive;version=1");

	run_sweep_a(models, out_a, ckpt, config_hash);
	if (do_sweep_b)
		run_sweep_b(models, out_b);
	return (0);
}
