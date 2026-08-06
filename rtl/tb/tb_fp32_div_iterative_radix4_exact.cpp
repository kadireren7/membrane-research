// EXP-FPGA-DIV-001 Phase B4: three-way component-level differential test --
// the real, general-purpose rtl/membrane_fp_divider.sv (the reference),
// Phase B2's rtl/experimental/fp_div/fp32_div_iterative_exact.sv (radix-2,
// one quotient bit/cycle, 26-cycle main iteration), and this phase's
// rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv (radix-4, two
// quotient bits/cycle, 13-cycle main iteration). All three are Verilated
// from their own real RTL -- a genuine 3-way RTL-vs-RTL bit-exactness
// check, not RTL-vs-idealized-math, structurally identical in method to
// Phase B2's own tb_fp32_div_iterative_exact.cpp (this file is closely
// modeled on it, extended from 2 DUTs to 3).
//
// Every case is driven through all three DUTs simultaneously, one case at
// a time (fully drained before the next, since latencies differ across
// all three -- baseline: fixed 1 cycle; B2: variable, ~3-29+ cycles; B4:
// variable, ~3-16+ cycles, roughly half of B2's given the same MSB-first
// restoring-division construction run 2 bits/cycle instead of 1).
// Mismatches are tracked and reported for all three pairs (baseline-vs-B2,
// baseline-vs-B4, B2-vs-B4) so a B4-only bug cannot hide behind an
// unrelated baseline/B2 agreement (or vice versa).
//
// Scope (task item 4): reproduces every one of Phase B2's own 2,456,685
// differential cases (same category structure: boundary sweep, specials
// cross product, powers of two, random num=1, random general, real Q4
// runtime d-distribution, reset-recovery, throughput) PLUS >=2,000,000
// additional random-denominator cases (folded into the same random-num=1
// pool, see main()'s random_count default) -- random reset, random
// out_ready backpressure, back-to-back throughput, and a per-case
// timeout/deadlock bound are all exercised, same as Phase B2's own test.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <random>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include "Vmembrane_fp_divider.h"
#include "Vfp32_div_iterative_exact.h"
#include "Vfp32_div_iterative_radix4_exact.h"
#include "verilated.h"
#include "membrane/f16convert.h"

static const uint32_t	ONE_F32 = 0x3F800000u;
static const uint64_t	MAX_CYCLES_PER_CASE = 10000;

static Vmembrane_fp_divider			*g_baseline;
static Vfp32_div_iterative_exact		*g_b2;
static Vfp32_div_iterative_radix4_exact	*g_b4;

static uint64_t	g_cycle = 0;
static std::chrono::steady_clock::time_point	g_start_time, g_last_heartbeat;
static std::mt19937	g_rng(0xC0FFEEu);

struct LatStats
{
	uint64_t	min_c = UINT64_MAX;
	uint64_t	max_c = 0;
	long double	sum = 0.0L;
	uint64_t	count = 0;
	uint64_t	nobp_min = UINT64_MAX;
	uint64_t	nobp_max = 0;
	long double	nobp_sum = 0.0L;
	uint64_t	nobp_count = 0;

	void	record(uint64_t cyc, bool allow_bp)
	{
		min_c = std::min(min_c, cyc);
		max_c = std::max(max_c, cyc);
		sum += (long double)cyc;
		count++;
		if (!allow_bp)
		{
			nobp_min = std::min(nobp_min, cyc);
			nobp_max = std::max(nobp_max, cyc);
			nobp_sum += (long double)cyc;
			nobp_count++;
		}
	}
};

struct Result
{
	uint64_t	total = 0;
	uint64_t	accepted = 0;
	uint64_t	retired_b2 = 0, retired_b4 = 0;
	uint64_t	mismatch_baseline_b2 = 0;
	uint64_t	mismatch_baseline_b4 = 0;
	uint64_t	mismatch_b2_b4 = 0;
	std::map<std::string, uint64_t>	mismatch_by_category;
	std::vector<std::string>	first_mismatches;
	LatStats	lat_b2, lat_b4;
};

static Result	g_result;
static uint64_t	g_planned = 0;

static std::string	category_of(uint32_t den, const char *tag)
{
	if (tag)
		return std::string(tag);

	uint32_t	exp = (den >> 23) & 0xFFu;
	uint32_t	mant = den & 0x7FFFFFu;
	uint32_t	sign = (den >> 31) & 1u;
	const char	*sgn = sign ? "neg" : "pos";

	if (exp == 0xFFu && mant == 0u)
		return std::string("den_inf_") + sgn;
	if (exp == 0xFFu && mant != 0u)
		return std::string(((mant & 0x400000u) ? "den_nan_quiet_" : "den_nan_signaling_")) + sgn;
	if (exp == 0u && mant == 0u)
		return std::string("den_zero_") + sgn;
	if (exp == 0u && mant != 0u)
		return std::string("den_subnormal_") + sgn;
	return std::string("den_general_") + sgn;
}

static void	heartbeat(void)
{
	auto	now = std::chrono::steady_clock::now();
	double	since = std::chrono::duration<double>(now - g_last_heartbeat).count();

	if (since >= 5.0)
	{
		double	elapsed = std::chrono::duration<double>(now - g_start_time).count();
		double	frac = g_planned ? (double)g_result.total / (double)g_planned : 0.0;
		double	eta = (frac > 0.0) ? (elapsed / frac - elapsed) : 0.0;

		fprintf(stderr,
			"[heartbeat] cases=%lu/%lu mismatches(base-b2/base-b4/b2-b4)=%lu/%lu/%lu elapsed=%.0fs eta=%.0fs cycle=%lu\n",
			(unsigned long)g_result.total, (unsigned long)g_planned,
			(unsigned long)g_result.mismatch_baseline_b2, (unsigned long)g_result.mismatch_baseline_b4,
			(unsigned long)g_result.mismatch_b2_b4, elapsed, eta, (unsigned long)g_cycle);
		g_last_heartbeat = now;
	}
}

static void	step(void)
{
	g_baseline->clk = 0;
	g_b2->clk = 0;
	g_b4->clk = 0;
	g_baseline->eval();
	g_b2->eval();
	g_b4->eval();

	g_baseline->clk = 1;
	g_b2->clk = 1;
	g_b4->clk = 1;
	g_baseline->eval();
	g_b2->eval();
	g_b4->eval();
	g_cycle++;
}

static void	reset_all(int cycles)
{
	g_baseline->rst_n = 0;
	g_b2->rst_n = 0;
	g_b4->rst_n = 0;
	g_baseline->valid_in = 0;
	g_baseline->a_in = 0;
	g_baseline->b_in = 0;
	g_b2->in_valid = 0;
	g_b2->numerator = 0;
	g_b2->denominator = 0;
	g_b2->out_ready = 1;
	g_b4->in_valid = 0;
	g_b4->numerator = 0;
	g_b4->denominator = 0;
	g_b4->out_ready = 1;
	for (int i = 0; i < cycles; i++)
		step();
	g_baseline->rst_n = 1;
	g_b2->rst_n = 1;
	g_b4->rst_n = 1;
	step();
}

// Drives one case through all three DUTs, fully draining B2 AND B4 (they
// generally finish at different cycles) before returning.
static bool	run_case(uint32_t num, uint32_t den, uint32_t &bo, uint32_t &b2o,
		uint32_t &b4o, uint64_t &lat_b2, uint64_t &lat_b4, bool allow_bp)
{
	if (!g_b2->in_ready || !g_b4->in_ready)
	{
		fprintf(stderr, "PROTOCOL ERROR: a candidate not in_ready at case start (num=0x%08X den=0x%08X)\n",
			num, den);
		return false;
	}

	g_baseline->valid_in = 1;
	g_baseline->a_in = num;
	g_baseline->b_in = den;
	g_b2->in_valid = 1;
	g_b2->numerator = num;
	g_b2->denominator = den;
	g_b2->out_ready = 1;
	g_b4->in_valid = 1;
	g_b4->numerator = num;
	g_b4->denominator = den;
	g_b4->out_ready = 1;

	bool	pre_ready = g_b2->in_ready && g_b4->in_ready;
	bool	pre_valid = g_b2->in_valid && g_b4->in_valid;

	step();
	g_result.accepted += (pre_ready && pre_valid) ? 1 : 0;
	g_baseline->valid_in = 0;
	g_b2->in_valid = 0;
	g_b4->in_valid = 0;

	if (!g_baseline->valid_out)
	{
		fprintf(stderr, "PROTOCOL ERROR: baseline valid_out not asserted 1 cycle after issue (DELAY=1 violated)\n");
		return false;
	}
	bo = g_baseline->result_out;

	uint64_t	cyc = 1;
	bool	got_b2 = false, got_b4 = false;

	while (cyc < MAX_CYCLES_PER_CASE && (!got_b2 || !got_b4))
	{
		bool	ready_b2 = allow_bp ? (bool)(g_rng() & 1u) : true;
		bool	ready_b4 = allow_bp ? (bool)(g_rng() & 1u) : true;

		g_b2->out_ready = ready_b2;
		g_b4->out_ready = ready_b4;
		if (!got_b2 && g_b2->out_valid && ready_b2)
		{
			b2o = g_b2->quotient;
			lat_b2 = cyc;
			got_b2 = true;
		}
		if (!got_b4 && g_b4->out_valid && ready_b4)
		{
			b4o = g_b4->quotient;
			lat_b4 = cyc;
			got_b4 = true;
		}
		step();
		cyc++;
	}
	if (!got_b2 || !got_b4)
	{
		fprintf(stderr, "TIMEOUT/DEADLOCK: %s never retired within %lu cycles (num=0x%08X den=0x%08X)\n",
			!got_b2 ? "B2" : "B4", (unsigned long)MAX_CYCLES_PER_CASE, num, den);
		return false;
	}
	g_result.retired_b2++;
	g_result.retired_b4++;
	g_result.lat_b2.record(lat_b2, allow_bp);
	g_result.lat_b4.record(lat_b4, allow_bp);
	return true;
}

static bool	check_case(uint32_t num, uint32_t den, const char *tag, bool allow_bp)
{
	uint32_t	bo, b2o, b4o;
	uint64_t	lat_b2, lat_b4;

	if (!run_case(num, den, bo, b2o, b4o, lat_b2, lat_b4, allow_bp))
		exit(1);
	g_result.total++;

	bool	ok = true;

	if (bo != b2o)
	{
		g_result.mismatch_baseline_b2++;
		ok = false;
	}
	if (bo != b4o)
	{
		g_result.mismatch_baseline_b4++;
		ok = false;
	}
	if (b2o != b4o)
	{
		g_result.mismatch_b2_b4++;
		ok = false;
	}
	if (!ok)
	{
		std::string	cat = category_of(den, tag);

		g_result.mismatch_by_category[cat]++;
		if (g_result.first_mismatches.size() < 25)
		{
			char	buf[320];

			snprintf(buf, sizeof(buf),
				"num=0x%08X den=0x%08X category=%s baseline=0x%08X b2=0x%08X b4=0x%08X",
				num, den, cat.c_str(), bo, b2o, b4o);
			g_result.first_mismatches.push_back(buf);
		}
	}
	heartbeat();
	return ok;
}

static void	reset_recovery_test(uint64_t &fails)
{
	fprintf(stderr, "[stage] reset-mid-computation recovery test (random reset timing on B4)\n");
	if (!g_b4->in_ready)
	{
		fprintf(stderr, "FAIL: B4 not idle before reset-recovery test\n");
		fails++;
		return;
	}
	g_b4->numerator = ONE_F32;
	g_b4->denominator = 0x40490FDBu;	// pi, arbitrary general-path denominator
	g_b4->in_valid = 1;
	g_b4->out_ready = 1;
	step();
	g_b4->in_valid = 0;
	if (!g_b4->busy)
	{
		fprintf(stderr, "FAIL: B4 not busy right after accepting a general-path case\n");
		fails++;
	}
	// Random reset timing within the (shorter, ~13-cycle) B4 iteration --
	// exercises reset at different points relative to B4's own faster
	// iteration, not just a fixed offset copied from B2's own test.
	std::uniform_int_distribution<int>	offset_dist(1, 6);
	int	off = offset_dist(g_rng);

	for (int i = 0; i < off; i++)
		step();	// mid-ITER for B4's own (shorter) iteration count
	if (g_b4->out_valid)
	{
		fprintf(stderr, "FAIL: B4 asserted out_valid too early (%d cycles in) for a general-path case\n", off);
		fails++;
	}

	g_b4->rst_n = 0;
	for (int i = 0; i < 3; i++)
	{
		step();
		if (g_b4->out_valid)
		{
			fprintf(stderr, "FAIL: out_valid asserted during reset\n");
			fails++;
		}
	}
	g_b4->rst_n = 1;
	step();

	if (!g_b4->in_ready)
	{
		fprintf(stderr, "FAIL: B4 not in_ready one cycle after reset deassertion\n");
		fails++;
	}
	if (g_b4->busy)
	{
		fprintf(stderr, "FAIL: B4 still busy one cycle after reset deassertion\n");
		fails++;
	}

	// baseline/B2 were not exercised in this stage -- reset them back to
	// a clean idle state too before resuming the shared 3-way loop.
	g_baseline->rst_n = 0;
	g_baseline->valid_in = 0;
	g_b2->rst_n = 0;
	g_b2->in_valid = 0;
	step();
	g_baseline->rst_n = 1;
	g_b2->rst_n = 1;
	step();

	if (!check_case(ONE_F32, 0x3F000000u, "reset_recovery_followup", false))	// 1/0.5 = 2.0
	{
		fprintf(stderr, "FAIL: post-reset-recovery case mismatched\n");
		fails++;
	}
	fprintf(stderr, "[stage] reset-mid-computation recovery test done, fails so far: %lu\n",
		(unsigned long)fails);
}

static void	throughput_test(uint64_t n, uint64_t &out_total_cycles)
{
	uint64_t	cyc0 = g_cycle;
	std::uniform_int_distribution<uint32_t>	dist(0x00800000u, 0x7F7FFFFFu);

	fprintf(stderr, "[stage] throughput measurement: %lu back-to-back general-path cases\n", (unsigned long)n);
	for (uint64_t i = 0; i < n; i++)
	{
		if (!check_case(ONE_F32, dist(g_rng), "throughput_general", false))
			exit(1);
	}
	out_total_cycles = g_cycle - cyc0;
	fprintf(stderr, "[stage] throughput measurement done: %lu cycles for %lu transactions\n",
		(unsigned long)out_total_cycles, (unsigned long)n);
}

int	main(int argc, char **argv)
{
	// Default random_count (4,200,000) reproduces Phase B2's own random
	// (num=1) case count (2,200,000) PLUS the >=2,000,000 additional
	// cases this phase's task explicitly asks for, in one combined pool
	// (same category, same purpose: real Q4_0 runtime denominator
	// distribution stress) -- not a separate stage.
	uint64_t	random_count = 4200000;
	uint64_t	general_random_count = 200000;
	uint64_t	q4_dist_count = 50000;

	Verilated::commandArgs(argc, argv);
	if (argc > 1)
		random_count = strtoull(argv[1], nullptr, 10);
	if (argc > 2)
		general_random_count = strtoull(argv[2], nullptr, 10);
	if (argc > 3)
		q4_dist_count = strtoull(argv[3], nullptr, 10);

	g_baseline = new Vmembrane_fp_divider;
	g_b2 = new Vfp32_div_iterative_exact;
	g_b4 = new Vfp32_div_iterative_radix4_exact;

	g_start_time = std::chrono::steady_clock::now();
	g_last_heartbeat = g_start_time;

	reset_all(5);

	std::vector<uint32_t>	mantissas = {
		0x000000u, 0x000001u, 0x000002u, 0x3FFFFFu,
		0x400000u, 0x400001u, 0x7FFFFEu, 0x7FFFFFu,
	};
	std::vector<uint32_t>	specials = {
		0x00000000u, 0x80000000u,
		0x00000001u, 0x80000001u,
		0x007FFFFFu, 0x807FFFFFu,
		0x00800000u, 0x80800000u,
		0x7F7FFFFFu, 0xFF7FFFFFu,
		0x7F800000u, 0xFF800000u,
		0x7FC00000u, 0xFFC00000u,
		0x7FA00000u, 0xFFA00000u,
		0x7FFFFFFFu, 0xFFFFFFFFu,
		0x7F800001u, 0xFF800001u,
		0x3F800000u, 0xBF800000u,
		0x40000000u, 0xC0000000u,
	};
	std::vector<uint32_t>	pow2 = {
		0x00800000u, 0x01000000u, 0x3F000000u, 0x3F800000u,
		0x40000000u, 0x40800000u, 0x7E800000u, 0x7F000000u,
		0x80800000u, 0xBF800000u, 0xC0000000u, 0xFE800000u,
	};

	uint64_t	boundary_count = 256ull * 2ull * mantissas.size();
	uint64_t	cross_special_count = (uint64_t)specials.size() * (uint64_t)specials.size();

	g_planned = boundary_count + cross_special_count + pow2.size()
		+ random_count + general_random_count + q4_dist_count;

	fprintf(stderr, "=== EXP-FPGA-DIV-001 Phase B4 differential test (baseline vs B2 radix-2 vs B4 radix-4) ===\n");
	fprintf(stderr, "planned total cases: %lu\n", (unsigned long)g_planned);

	fprintf(stderr, "[stage] denominator exponent/mantissa boundary sweep (numerator=1.0f): %lu cases\n",
		(unsigned long)boundary_count);
	for (uint32_t exp = 0; exp <= 255; exp++)
		for (uint32_t sign = 0; sign <= 1; sign++)
			for (uint32_t m : mantissas)
			{
				uint32_t	den = (sign << 31) | (exp << 23) | m;

				check_case(ONE_F32, den, nullptr, false);
			}
	fprintf(stderr, "[stage] boundary sweep done, total=%lu\n", (unsigned long)g_result.total);

	fprintf(stderr, "[stage] specials x specials cross product (both operands vary): %lu cases\n",
		(unsigned long)cross_special_count);
	for (uint32_t num : specials)
		for (uint32_t den : specials)
			check_case(num, den, "cross_special", false);
	fprintf(stderr, "[stage] cross-special done, total=%lu\n", (unsigned long)g_result.total);

	fprintf(stderr, "[stage] powers of two (numerator=1.0f): %lu cases\n", (unsigned long)pow2.size());
	for (uint32_t den : pow2)
		check_case(ONE_F32, den, "pow2", false);

	fprintf(stderr, "[stage] uniform random denominator, numerator=1.0f (reproduces B2's own case count + >=2,000,000 additional): %lu cases, random out_ready backpressure\n",
		(unsigned long)random_count);
	{
		std::uniform_int_distribution<uint32_t>	dist(0, 0xFFFFFFFFu);

		for (uint64_t i = 0; i < random_count; i++)
			check_case(ONE_F32, dist(g_rng), "random_num1_denrand", (i % 4) == 0);
	}
	fprintf(stderr, "[stage] random (numerator=1.0f) done, total=%lu\n", (unsigned long)g_result.total);

	fprintf(stderr, "[stage] uniform random BOTH operands (general-purpose check): %lu cases\n",
		(unsigned long)general_random_count);
	{
		std::uniform_int_distribution<uint32_t>	dist(0, 0xFFFFFFFFu);

		for (uint64_t i = 0; i < general_random_count; i++)
		{
			uint32_t	num = dist(g_rng);
			uint32_t	den = dist(g_rng);

			check_case(num, den, "random_general", (i % 5) == 0);
		}
	}
	fprintf(stderr, "[stage] random (both operands) done, total=%lu\n", (unsigned long)g_result.total);

	fprintf(stderr, "[stage] q4 runtime d-distribution sample: %lu cases\n", (unsigned long)q4_dist_count);
	{
		std::mt19937	block_rng(0xB10CDu);
		std::uniform_int_distribution<uint32_t>	half_dist(0, 0xFFFFu);

		for (uint64_t i = 0; i < q4_dist_count; i++)
		{
			float	amax = 0.0f;
			float	mx = 0.0f;

			for (int j = 0; j < 32; j++)
			{
				uint16_t	h = (uint16_t)half_dist(block_rng);
				float	v = membrane_f16_to_f32(h);

				if (fabsf(v) > amax)
				{
					amax = fabsf(v);
					mx = v;
				}
			}

			float	d = mx / -8.0f;
			uint32_t	d_bits;

			memcpy(&d_bits, &d, sizeof(d_bits));
			check_case(ONE_F32, d_bits, "q4_runtime_d_dist", (i % 3) == 0);
		}
	}
	fprintf(stderr, "[stage] q4 d-distribution done, total=%lu\n", (unsigned long)g_result.total);

	uint64_t	reset_fails = 0;

	reset_recovery_test(reset_fails);

	uint64_t	throughput_cycles = 0;
	uint64_t	throughput_n = 2000;

	throughput_test(throughput_n, throughput_cycles);

	double	elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - g_start_time).count();
	double	ii_b2 = throughput_n ? (double)throughput_cycles / (double)throughput_n : 0.0;
	uint64_t	total_mismatches = g_result.mismatch_baseline_b2
		+ g_result.mismatch_baseline_b4 + g_result.mismatch_b2_b4;

	printf("=== EXP-FPGA-DIV-001 Phase B4 differential test report ===\n");
	printf("total cases:              %lu\n", (unsigned long)g_result.total);
	printf("mismatches baseline-vs-B2: %lu\n", (unsigned long)g_result.mismatch_baseline_b2);
	printf("mismatches baseline-vs-B4: %lu\n", (unsigned long)g_result.mismatch_baseline_b4);
	printf("mismatches B2-vs-B4:       %lu\n", (unsigned long)g_result.mismatch_b2_b4);
	printf("accepted inputs:          %lu\n", (unsigned long)g_result.accepted);
	printf("retired outputs (B2/B4):  %lu / %lu\n", (unsigned long)g_result.retired_b2, (unsigned long)g_result.retired_b4);
	printf("B2  latency cycles: min=%lu mean=%.3f max=%lu (no-bp: min=%lu mean=%.3f max=%lu, n=%lu)\n",
		(unsigned long)(g_result.lat_b2.min_c == UINT64_MAX ? 0 : g_result.lat_b2.min_c),
		g_result.lat_b2.count ? (double)(g_result.lat_b2.sum / (long double)g_result.lat_b2.count) : 0.0,
		(unsigned long)g_result.lat_b2.max_c,
		(unsigned long)(g_result.lat_b2.nobp_min == UINT64_MAX ? 0 : g_result.lat_b2.nobp_min),
		g_result.lat_b2.nobp_count ? (double)(g_result.lat_b2.nobp_sum / (long double)g_result.lat_b2.nobp_count) : 0.0,
		(unsigned long)g_result.lat_b2.nobp_max, (unsigned long)g_result.lat_b2.nobp_count);
	printf("B4  latency cycles: min=%lu mean=%.3f max=%lu (no-bp: min=%lu mean=%.3f max=%lu, n=%lu)\n",
		(unsigned long)(g_result.lat_b4.min_c == UINT64_MAX ? 0 : g_result.lat_b4.min_c),
		g_result.lat_b4.count ? (double)(g_result.lat_b4.sum / (long double)g_result.lat_b4.count) : 0.0,
		(unsigned long)g_result.lat_b4.max_c,
		(unsigned long)(g_result.lat_b4.nobp_min == UINT64_MAX ? 0 : g_result.lat_b4.nobp_min),
		g_result.lat_b4.nobp_count ? (double)(g_result.lat_b4.nobp_sum / (long double)g_result.lat_b4.nobp_count) : 0.0,
		(unsigned long)g_result.lat_b4.nobp_max, (unsigned long)g_result.lat_b4.nobp_count);
	printf("initiation_interval (B2, measured, %lu back-to-back, no backpressure): %.3f\n", (unsigned long)throughput_n, ii_b2);
	printf("reset_recovery_fails:  %lu\n", (unsigned long)reset_fails);
	printf("wall_time_s:           %.1f\n", elapsed);
	if (!g_result.mismatch_by_category.empty())
	{
		printf("mismatch categories:\n");
		for (auto &kv : g_result.mismatch_by_category)
			printf("  %-28s %lu\n", kv.first.c_str(), (unsigned long)kv.second);
		printf("first %zu mismatch examples:\n", g_result.first_mismatches.size());
		for (auto &s : g_result.first_mismatches)
			printf("  %s\n", s.c_str());
	}

	bool	pass = (total_mismatches == 0) && (reset_fails == 0)
		&& (g_result.total == g_planned + 1 + throughput_n);

	if (pass)
	{
		printf("PASS: fp32_div_iterative_radix4_exact vs membrane_fp_divider AND vs fp32_div_iterative_exact, %lu cases, 0 mismatches, 0 reset-recovery fails\n",
			(unsigned long)g_result.total);
		delete g_baseline;
		delete g_b2;
		delete g_b4;
		return 0;
	}
	printf("FAIL: %lu total mismatches, %lu reset-recovery fails -- do not integrate\n",
		(unsigned long)total_mismatches, (unsigned long)reset_fails);
	delete g_baseline;
	delete g_b2;
	delete g_b4;
	return 1;
}
