/*
 * Phase 6.5 items 9/10: mem_guard_t reads REAL /proc data and must
 * drive REAL escalating action (cache shrink -> worker reduction ->
 * checkpoint-and-exit) as the declared budget shrinks relative to
 * this process's actual current RSS -- not simulated numbers.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "mem_guard.h"

#define TEST_ASSERT(cond, msg) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			abort(); \
		} \
	} while (0)

using namespace exactsim;

/* Rounds the budget UP to whole MiB so the resulting rss/budget ratio
 * never overshoots `target_ratio` (coarse 1 MiB granularity relative
 * to a small test-process RSS otherwise made the earlier version of
 * this test flaky -- rounding down could push the ratio into the
 * NEXT band up). */
static uint64_t	budget_mib_for_ratio(uint64_t rss_kb, double target_ratio)
{
	double	budget_kib = (double)rss_kb / target_ratio;

	return ((uint64_t)std::ceil(budget_kib / 1024.0));
}

static void	test_sample_process_memory_reads_real_proc(void)
{
	mem_sample_t	s = sample_process_memory();

	TEST_ASSERT(s.ok, "sample_process_memory reads /proc successfully "
		"on Linux");
	TEST_ASSERT(s.rss_kb > 0, "this process's own RSS is real and nonzero");
	TEST_ASSERT(s.mem_total_kb > 0, "MemTotal is real and nonzero");
	printf("PASS test_sample_process_memory_reads_real_proc "
		"(rss=%llu kB, available=%llu kB, majflt=%llu)\n",
		(unsigned long long)s.rss_kb,
		(unsigned long long)s.mem_available_kb,
		(unsigned long long)s.majflt);
}

static void	test_huge_budget_takes_no_action(void)
{
	mem_guard_t	g(1ull << 20 /* 1 TiB */, 4, 1);
	auto		action = g.tick(nullptr);

	TEST_ASSERT(action == mem_action_t::NONE,
		"a budget far above real RSS takes no action");
	TEST_ASSERT(g.active_worker_limit() == 4,
		"worker limit untouched when comfortably under budget");
	TEST_ASSERT(!g.should_exit(), "no exit requested when under budget");
	printf("PASS test_huge_budget_takes_no_action\n");
}

static void	test_tiny_budget_forces_checkpoint_and_exit(void)
{
	/* 1 MiB is guaranteed far below any real process's actual RSS. */
	mem_guard_t	g(1, 4, 1);
	auto		action = g.tick(nullptr);

	TEST_ASSERT(action == mem_action_t::CHECKPOINT_AND_EXIT,
		"a budget far below real RSS forces CHECKPOINT_AND_EXIT");
	TEST_ASSERT(g.should_exit(),
		"should_exit() reflects the CHECKPOINT_AND_EXIT decision");
	printf("PASS test_tiny_budget_forces_checkpoint_and_exit\n");
}

static int	severity(mem_action_t a)
{
	switch (a)
	{
	case mem_action_t::NONE: return (0);
	case mem_action_t::SHRINK_CACHE: return (1);
	case mem_action_t::REDUCE_WORKERS: return (2);
	case mem_action_t::CHECKPOINT_AND_EXIT: return (3);
	}
	return (-1);
}

/*
 * Real process RSS for a tiny test binary is only a few MiB, and
 * mem_guard_t's budget is declared in whole MiB (the real CLI unit,
 * --memory-budget-mib) -- that 1 MiB granularity is too coarse to
 * reliably land in one specific narrow ratio band (a fixed-point
 * test tried that first and was flaky). Instead, sweep budgets from
 * far-above to far-below this process's real RSS and assert action
 * severity is monotonically non-decreasing as the budget shrinks,
 * starts at NONE, and ends at CHECKPOINT_AND_EXIT -- a real property
 * of mem_guard_t's escalation policy that doesn't depend on exactly
 * where the band edges fall relative to MiB rounding.
 */
static void	test_action_escalates_monotonically_as_budget_shrinks(void)
{
	mem_sample_t			s0 = sample_process_memory();
	uint64_t				rss_mib = s0.rss_kb / 1024 + 1;
	std::vector<double>	factors = {100.0, 20.0, 5.0, 2.0, 1.3, 1.1, 1.0,
		0.9, 0.7, 0.5, 0.2, 0.05, 0.01};
	int						prev_sev = 0;
	bool					saw_none = false;
	bool					saw_exit = false;

	for (double factor : factors)
	{
		uint64_t	budget_mib = (uint64_t)((double)rss_mib * factor);

		if (budget_mib == 0)
			budget_mib = 1;
		mem_guard_t	g(budget_mib, 4, 1);
		auto		cache
			= std::make_shared<wssim::attn_trace_chunk_cache_t>(1ull << 30);
		auto		action = g.tick(cache);
		int			sev = severity(action);

		TEST_ASSERT(sev >= prev_sev,
			"action severity is monotonically non-decreasing as the "
			"budget/RSS factor shrinks");
		prev_sev = sev;
		if (action == mem_action_t::NONE)
			saw_none = true;
		if (action == mem_action_t::CHECKPOINT_AND_EXIT)
		{
			saw_exit = true;
			TEST_ASSERT(g.should_exit(), "should_exit() set alongside "
				"CHECKPOINT_AND_EXIT");
		}
		if (sev >= 1)
			TEST_ASSERT(cache->stats().budget_bytes < (1ull << 30),
				"cache budget is real shrunk whenever action >= "
				"SHRINK_CACHE");
		if (action == mem_action_t::REDUCE_WORKERS)
			TEST_ASSERT(g.active_worker_limit() < 4,
				"REDUCE_WORKERS actually lowers the worker limit");
	}
	TEST_ASSERT(saw_none, "a large-enough factor produced NONE");
	TEST_ASSERT(saw_exit, "a small-enough factor produced "
		"CHECKPOINT_AND_EXIT");
	printf("PASS test_action_escalates_monotonically_as_budget_shrinks\n");
}

/* budget_mib_for_ratio's 1 MiB rounding is negligible once real RSS
 * is inflated to ~300 MiB (touching every page, not just reserving
 * address space, so it actually counts toward VmRSS) -- lets this
 * test reliably target the REDUCE_WORKERS band specifically (unlike
 * the tiny baseline RSS the other tests work with) to prove ticking
 * it repeatedly never drops the limit below min_workers. */
static void	test_reduce_workers_floors_at_min(void)
{
	std::vector<char>	inflate(300ull << 20, 0x5a);

	mem_sample_t	s0 = sample_process_memory();
	uint64_t		budget_mib = budget_mib_for_ratio(s0.rss_kb, 0.95);
	mem_guard_t		g(budget_mib, 5, 2);

	for (int i = 0; i < 20; i++)
	{
		auto	action = g.tick(nullptr);

		TEST_ASSERT(action != mem_action_t::CHECKPOINT_AND_EXIT,
			"budget targeted at the REDUCE_WORKERS band should not "
			"immediately escalate to CHECKPOINT_AND_EXIT");
	}
	TEST_ASSERT(g.active_worker_limit() == 2,
		"20 REDUCE_WORKERS ticks from limit 5 with min_workers 2 must "
		"floor at exactly 2, not below");
	(void)inflate;
	printf("PASS test_reduce_workers_floors_at_min "
		"(final limit=%u)\n", g.active_worker_limit());
}

int	main(void)
{
	test_sample_process_memory_reads_real_proc();
	test_huge_budget_takes_no_action();
	test_tiny_budget_forces_checkpoint_and_exit();
	test_action_escalates_monotonically_as_budget_shrinks();
	test_reduce_workers_floors_at_min();
	return (0);
}
