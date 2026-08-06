/*
 * Phase 6.3 item 16/18: exact_engine.cpp correctness tests -- device
 * capacity enforcement, real contention under concurrency, micro-
 * batching sanity, and deterministic replay. Hand-built
 * calibrated_profile_t fixtures (bypassing calibrate.cpp) so these
 * run in milliseconds and don't depend on any real trace file --
 * same "fast, model-free" philosophy as
 * tools/membrane-kv-runtime-optimizer/test_checkpoint.cpp.
 */

#include <cstdio>
#include <cstdlib>

#include "exact_engine.h"

#define TEST_ASSERT(cond, msg) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			abort(); \
		} \
	} while (0)

using namespace exactsim;
using namespace wssim;

static calibrated_profile_t	make_profile(uint32_t steps,
		uint64_t miss_bytes_per_step, uint32_t prompt_len = 512)
{
	calibrated_profile_t	p;

	p.policy_name = "test";
	p.context_tokens = prompt_len + steps;
	p.hit_rate = 0.99;
	p.precision = 0.7;
	p.recall = 0.9;
	p.mean_working_set_blocks = 16.0;
	p.source_is_real_capture = false;
	p.steps.assign(steps, {0, miss_bytes_per_step});
	return (p);
}

static model_calibration_t	tiny_model(void)
{
	return {"tiny", 4, 2, 4096, 1.0e7};	/* 10ms/step compute floor */
}

static void	test_device_capacity_enforced(void)
{
	calibrated_profile_t	profile = make_profile(50, 1000);
	concurrent_config_t	cfg{};

	cfg.concurrency = 4;
	cfg.host_hot_cache_total_bytes = 1ull << 30;
	cfg.device_total_bytes = 1000;	/* far too small for any sequence */
	cfg.quant_pipelines = 4;
	cfg.microbatch_max_wait_ns = 0.0;
	cfg.microbatch_max_batch_blocks = 0;

	concurrent_result_t	r = run_concurrent(profile, tiny_model(), 32,
		1.0, cfg);
	TEST_ASSERT(r.capacity.sequences_fit < cfg.concurrency,
		"tiny device budget must not let every sequence fit");
	TEST_ASSERT(r.device_capacity_bound, "device_capacity_bound must be set");
	printf("PASS test_device_capacity_enforced\n");
}

static void	test_device_capacity_generous_all_fit(void)
{
	calibrated_profile_t	profile = make_profile(50, 1000);
	concurrent_config_t	cfg{};

	cfg.concurrency = 4;
	cfg.host_hot_cache_total_bytes = 1ull << 30;
	cfg.device_total_bytes = 1ull << 40;	/* 1 TiB, ample */
	cfg.quant_pipelines = 4;
	cfg.microbatch_max_wait_ns = 0.0;
	cfg.microbatch_max_batch_blocks = 0;

	concurrent_result_t	r = run_concurrent(profile, tiny_model(), 32,
		1.0, cfg);
	TEST_ASSERT(r.capacity.sequences_fit == cfg.concurrency,
		"ample device budget must let every sequence fit");
	TEST_ASSERT(!r.device_capacity_bound, "must not be device-capacity-bound");
	printf("PASS test_device_capacity_generous_all_fit\n");
}

static void	test_higher_concurrency_increases_contention(void)
{
	calibrated_profile_t	profile = make_profile(200, 50000);
	concurrent_config_t	cfg_lo{};

	cfg_lo.concurrency = 1;
	cfg_lo.host_hot_cache_total_bytes = 1ull << 30;
	cfg_lo.device_total_bytes = 1ull << 40;
	cfg_lo.quant_pipelines = 2;
	cfg_lo.microbatch_max_wait_ns = 0.0;
	cfg_lo.microbatch_max_batch_blocks = 0;

	concurrent_config_t	cfg_hi = cfg_lo;
	cfg_hi.concurrency = 64;

	concurrent_result_t	r_lo = run_concurrent(profile, tiny_model(), 32,
		1.0, cfg_lo);
	concurrent_result_t	r_hi = run_concurrent(profile, tiny_model(), 32,
		1.0, cfg_hi);
	TEST_ASSERT(r_hi.p99_latency_ns >= r_lo.p99_latency_ns,
		"more concurrent sequences sharing one link must not lower p99");
	TEST_ASSERT(r_hi.link_utilization_pct >= r_lo.link_utilization_pct,
		"higher concurrency must not lower link utilization");
	printf("PASS test_higher_concurrency_increases_contention\n");
}

static void	test_microbatch_runs_and_completes_all_steps(void)
{
	calibrated_profile_t	profile = make_profile(100, 20000);
	concurrent_config_t	cfg{};

	cfg.concurrency = 16;
	cfg.host_hot_cache_total_bytes = 1ull << 30;
	cfg.device_total_bytes = 1ull << 40;
	cfg.quant_pipelines = 2;
	cfg.microbatch_max_wait_ns = 500.0;
	cfg.microbatch_max_batch_blocks = 4;

	concurrent_result_t	r = run_concurrent(profile, tiny_model(), 32,
		1.0, cfg);
	TEST_ASSERT(r.capacity.sequences_fit == cfg.concurrency,
		"ample capacity: every sequence should fit under microbatching too");
	TEST_ASSERT(r.tokens_per_sec > 0.0, "microbatched run must make progress");
	printf("PASS test_microbatch_runs_and_completes_all_steps\n");
}

static void	test_deterministic_replay(void)
{
	calibrated_profile_t	profile = make_profile(150, 30000);
	concurrent_config_t	cfg{};

	cfg.concurrency = 32;
	cfg.host_hot_cache_total_bytes = 1ull << 30;
	cfg.device_total_bytes = 1ull << 40;
	cfg.quant_pipelines = 4;
	cfg.microbatch_max_wait_ns = 500.0;
	cfg.microbatch_max_batch_blocks = 8;

	concurrent_result_t	a = run_concurrent(profile, tiny_model(), 32,
		1.0, cfg);
	concurrent_result_t	b = run_concurrent(profile, tiny_model(), 32,
		1.0, cfg);
	TEST_ASSERT(a.p50_latency_ns == b.p50_latency_ns, "p50 must be deterministic");
	TEST_ASSERT(a.p99_latency_ns == b.p99_latency_ns, "p99 must be deterministic");
	TEST_ASSERT(a.mean_bytes_per_token == b.mean_bytes_per_token,
		"bytes/token must be deterministic");
	TEST_ASSERT(a.capacity.sequences_fit == b.capacity.sequences_fit,
		"sequences_fit must be deterministic");
	printf("PASS test_deterministic_replay\n");
}

static void	test_zero_miss_bytes_hits_compute_floor(void)
{
	calibrated_profile_t	profile = make_profile(20, 0);
	concurrent_config_t	cfg{};

	cfg.concurrency = 2;
	cfg.host_hot_cache_total_bytes = 1ull << 30;
	cfg.device_total_bytes = 1ull << 40;
	cfg.quant_pipelines = 2;
	cfg.microbatch_max_wait_ns = 0.0;
	cfg.microbatch_max_batch_blocks = 0;

	model_calibration_t	m = tiny_model();
	concurrent_result_t	r = run_concurrent(profile, m, 32, 1.0, cfg);
	TEST_ASSERT(r.p50_latency_ns == m.compute_ns_per_step,
		"no misses at all: latency must equal the compute-bound floor exactly");
	printf("PASS test_zero_miss_bytes_hits_compute_floor\n");
}

int	main(void)
{
	test_device_capacity_enforced();
	test_device_capacity_generous_all_fit();
	test_higher_concurrency_increases_contention();
	test_microbatch_runs_and_completes_all_steps();
	test_deterministic_replay();
	test_zero_miss_bytes_hits_compute_floor();
	return (0);
}
