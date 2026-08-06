/*
 * Phase 6.5 item 5: bounded_quantile_accumulator_t must return the
 * EXACT same p50/p95/p99 a plain std::sort over all samples would --
 * whether it stayed under the in-memory cap or spilled to disk.
 * Specifically covers the shape docs/phase6-unified-stress.md
 * documents as the common case at full unified scale: the vast
 * majority of samples pinned to one exact repeated value (the model's
 * compute floor) -- the case that would defeat a naive histogram
 * approach but must resolve correctly (and, by construction, cheaply)
 * via this accumulator's equal-bucket short-circuit.
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "bounded_quantile.h"

#define TEST_ASSERT(cond, msg) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			abort(); \
		} \
	} while (0)

using namespace exactsim;

static void	reference_percentiles(std::vector<double> v, double *p50,
				double *p95, double *p99)
{
	std::sort(v.begin(), v.end());
	auto	pct = [&](double p) -> double
	{
		size_t	idx = (size_t)(p * (double)(v.size() - 1));
		return (v[idx]);
	};
	*p50 = pct(0.50);
	*p95 = pct(0.95);
	*p99 = pct(0.99);
}

static void	check_against_reference(const std::vector<double> &samples,
				size_t in_memory_cap, const char *label)
{
	bounded_quantile_accumulator_t	acc(in_memory_cap);

	for (double v : samples)
		acc.add(v);
	double	p50, p95, p99;
	acc.finish(&p50, &p95, &p99);

	double	rp50, rp95, rp99;
	reference_percentiles(samples, &rp50, &rp95, &rp99);

	TEST_ASSERT(p50 == rp50, label);
	TEST_ASSERT(p95 == rp95, label);
	TEST_ASSERT(p99 == rp99, label);
}

static void	test_small_stays_in_memory(void)
{
	std::vector<double>	v = {5.0, 1.0, 3.0, 2.0, 4.0, 9.0, 7.0, 6.0, 8.0,
		10.0};

	bounded_quantile_accumulator_t	acc(1000);

	for (double x : v)
		acc.add(x);
	TEST_ASSERT(!acc.spilled(), "10 samples under a 1000 cap never spills");
	check_against_reference(v, 1000, "small in-memory case matches sort");
	printf("PASS test_small_stays_in_memory\n");
}

static void	test_spilled_uniform_random(void)
{
	std::mt19937					rng(12345);
	std::uniform_real_distribution<double>	dist(0.0, 1.0e9);
	std::vector<double>			v;

	for (int i = 0; i < 50000; i++)
		v.push_back(dist(rng));

	bounded_quantile_accumulator_t	acc(1000);	/* tiny cap forces spill */

	for (double x : v)
		acc.add(x);
	TEST_ASSERT(acc.spilled(), "50000 samples over a 1000 cap must spill");
	check_against_reference(v, 1000, "spilled uniform-random matches sort");
	printf("PASS test_spilled_uniform_random\n");
}

/* The documented common shape at full unified scale: ~99% of samples
 * are bit-identical (pinned to a "compute floor"), with a small tail
 * of distinct miss-driven outliers -- this is exactly the case a
 * naive fixed-bucket histogram would fail to bound (nearly everything
 * lands in one bucket) but the equal-bucket short-circuit handles in
 * O(1) extra rounds. */
static void	test_spilled_compute_floor_pinned(void)
{
	std::mt19937					rng(777);
	std::uniform_real_distribution<double>	tail(1.0e7, 5.0e7);
	std::vector<double>			v;

	for (int i = 0; i < 200000; i++)
		v.push_back(1.567e7);	/* the "compute floor" */
	for (int i = 0; i < 500; i++)
		v.push_back(tail(rng));	/* rare real misses */
	std::shuffle(v.begin(), v.end(), rng);

	bounded_quantile_accumulator_t	acc(1000);

	for (double x : v)
		acc.add(x);
	TEST_ASSERT(acc.spilled(), "200500 samples over a 1000 cap must spill");
	check_against_reference(v, 1000,
		"spilled compute-floor-pinned distribution matches sort");
	printf("PASS test_spilled_compute_floor_pinned\n");
}

static void	test_all_identical(void)
{
	std::vector<double>	v(10000, 42.0);

	bounded_quantile_accumulator_t	acc(100);

	for (double x : v)
		acc.add(x);
	double	p50, p95, p99;
	acc.finish(&p50, &p95, &p99);
	TEST_ASSERT(p50 == 42.0 && p95 == 42.0 && p99 == 42.0,
		"all-identical samples resolve to that value at every percentile");
	printf("PASS test_all_identical\n");
}

static void	test_empty(void)
{
	bounded_quantile_accumulator_t	acc(100);
	double							p50, p95, p99;

	acc.finish(&p50, &p95, &p99);
	TEST_ASSERT(p50 == 0.0 && p95 == 0.0 && p99 == 0.0,
		"empty accumulator returns all-zero percentiles");
	printf("PASS test_empty\n");
}

/* Cap boundary: exactly at the cap must NOT spill (only strictly over
 * it should), and results must still match. */
static void	test_exactly_at_cap_does_not_spill(void)
{
	std::vector<double>	v;

	for (int i = 0; i < 500; i++)
		v.push_back((double)(i * 37 % 500));

	bounded_quantile_accumulator_t	acc(500);

	for (double x : v)
		acc.add(x);
	TEST_ASSERT(!acc.spilled(), "exactly-at-cap must not spill");
	check_against_reference(v, 500, "exactly-at-cap matches sort");
	printf("PASS test_exactly_at_cap_does_not_spill\n");
}

int	main(void)
{
	test_small_stays_in_memory();
	test_spilled_uniform_random();
	test_spilled_compute_floor_pinned();
	test_all_identical();
	test_empty();
	test_exactly_at_cap_does_not_spill();
	return (0);
}
