// EXP-FPGA-DIV-001 Phase B1: differential test between the real,
// general-purpose rtl/membrane_fp_divider.sv (b_in held at the exact
// F32 constant -8.0, 0xC1000000 -- the actual constant
// rtl/q4_scale.sv's `u_div_d` instance uses) and the new specialized
// rtl/experimental/fp_div/fp32_scale_neg_pow2.sv (SHIFT=3), both built
// from the SAME source files (not two independently maintained C++
// re-implementations of FP32 division) so this test is a genuine
// RTL-vs-RTL bit-exactness check, not RTL-vs-idealized-math.
//
// Verilates two SEPARATE top-level designs (Vmembrane_fp_divider,
// Vfp32_scale_neg_pow2 -- built via two separate `verilator --Mdir`
// invocations, see scripts/run-exp-fp-divider-001.sh) and drives both
// from the same a_in sequence every cycle, comparing their registered
// outputs one DELAY-cycle later. Both modules use the same DELAY=1
// convention (see membrane_fp_divider.sv/fp32_scale_neg_pow2.sv
// headers), so both retire the same cycle for the same input -- no
// backpressure or handshake divergence to account for (neither module
// has a `ready` port; this is a fixed-latency pipe on both sides).
//
// Case set (see this project's CLAUDE-facing task spec for the exact
// minimums):
//   1. An exponent/mantissa boundary sweep: every one of the 256
//      possible exponent field values, both signs, crossed with a
//      small curated mantissa set (0, 1, 2, a "mostly zero" pattern,
//      the quiet-bit-only pattern, a "mostly one" pattern, and
//      all-ones) -- 256 * 2 * 8 = 4,096 cases, deliberately including
//      every exponent transition (0/1 subnormal<->normal, 3/4 this
//      operation's own underflow-flush threshold, 254/255
//      normal<->Inf/NaN) with multiple mantissas each.
//   2. A curated list of named IEEE-754 special values (+-0,
//      smallest/largest subnormal, smallest/largest normal, +-Inf,
//      quiet/signaling/max-payload/min-payload NaN in both signs, and
//      explicit exponent=3-vs-4 transition values with nonzero
//      mantissa).
//   3. >= 2,000,000 (--full) or a smaller (--quick) count of uniformly
//      random 32-bit patterns (a std::mt19937, fixed seed -- fully
//      reproducible), which naturally also re-hits every category
//      above stochastically in addition to the deliberate sweep.
//
// Report: total cases, exact matches, mismatch count, mismatch counts
// broken down by category, and the first N mismatching examples in
// full (input hex, category, baseline result, candidate result).
// Target: 0 mismatches. If that is not achieved, this test says so
// loudly (nonzero exit code, mismatch examples printed) rather than
// hiding it -- per this experiment's own rule, integration in
// q4_scale.sv must not proceed if this ever fails.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <random>
#include <string>
#include <map>
#include <vector>
#include "Vmembrane_fp_divider.h"
#include "Vfp32_scale_neg_pow2.h"
#include "verilated.h"

static const uint32_t	NEG_EIGHT_F32 = 0xC1000000u;	// -8.0f

static Vmembrane_fp_divider	*g_baseline;
static Vfp32_scale_neg_pow2	*g_candidate;

static uint64_t	g_cycle = 0;
static std::chrono::steady_clock::time_point	g_start_time, g_last_heartbeat;

static std::string	category_of(uint32_t bits)
{
	uint32_t	sign = (bits >> 31) & 1u;
	uint32_t	exp = (bits >> 23) & 0xFFu;
	uint32_t	mant = bits & 0x7FFFFFu;
	const char	*sgn = sign ? "neg" : "pos";

	if (exp == 0xFFu && mant == 0u)
		return std::string("inf_") + sgn;
	if (exp == 0xFFu && mant != 0u)
		return std::string(((mant & 0x400000u) ? "nan_quiet_" : "nan_signaling_")) + sgn;
	if (exp == 0u && mant == 0u)
		return std::string("zero_") + sgn;
	if (exp == 0u && mant != 0u)
		return std::string("subnormal_") + sgn;
	if (exp <= 3u)
		return std::string("normal_underflow_") + sgn;	// exp_result <= 0 for SHIFT=3
	return std::string("normal_general_") + sgn;
}

// clk=0 settle -> sample the CURRENTLY registered (pre-edge) output,
// which is the result for whatever was presented DELAY cycles ago ->
// clk=1 settle, matching rtl/tb/tb_top_verilator.cpp's own sampling
// convention (step_cycle's header comment there explains why pre-edge
// sampling is the correct point for a synchronous design).
static void	step(uint32_t a_in, uint32_t &baseline_out, uint32_t &candidate_out,
	bool &baseline_valid, bool &candidate_valid)
{
	g_baseline->clk = 0;
	g_candidate->clk = 0;
	g_baseline->eval();
	g_candidate->eval();

	baseline_valid = g_baseline->valid_out;
	candidate_valid = g_candidate->valid_out;
	baseline_out = g_baseline->result_out;
	candidate_out = g_candidate->result_out;

	g_baseline->a_in = a_in;
	g_baseline->b_in = NEG_EIGHT_F32;
	g_baseline->valid_in = 1;
	g_candidate->a_in = a_in;
	g_candidate->valid_in = 1;

	g_baseline->clk = 1;
	g_candidate->clk = 1;
	g_baseline->eval();
	g_candidate->eval();

	g_cycle++;
}

static void	do_reset(int cycles)
{
	uint32_t	bo, co;
	bool		bv, cv;

	g_baseline->rst_n = 0;
	g_candidate->rst_n = 0;
	g_baseline->valid_in = 0;
	g_candidate->valid_in = 0;
	for (int i = 0; i < cycles; i++)
		step(0, bo, co, bv, cv);
	g_baseline->rst_n = 1;
	g_candidate->rst_n = 1;
	step(0, bo, co, bv, cv);
}

struct Result
{
	uint64_t	total = 0;
	uint64_t	matches = 0;
	uint64_t	mismatches = 0;
	std::map<std::string, uint64_t>	mismatch_by_category;
	std::vector<std::string>	first_mismatches;
};

static Result	g_result;

static void	heartbeat(uint64_t planned)
{
	auto	now = std::chrono::steady_clock::now();
	double	since = std::chrono::duration<double>(now - g_last_heartbeat).count();

	if (since >= 60.0)
	{
		double	elapsed = std::chrono::duration<double>(now - g_start_time).count();
		double	frac = planned ? (double)g_result.total / (double)planned : 0.0;
		double	eta = (frac > 0.0) ? (elapsed / frac - elapsed) : 0.0;

		fprintf(stderr, "[heartbeat] %lu/%lu cases, %lu mismatches, elapsed=%.0fs eta=%.0fs\n",
			(unsigned long)g_result.total, (unsigned long)planned,
			(unsigned long)g_result.mismatches, elapsed, eta);
		g_last_heartbeat = now;
	}
}

// Feeds `bits`, DELAY(=1) cycles later reads back and compares. Since
// both DUTs are fixed-latency, fully-pipelined (II=1) designs with no
// `ready` port, this drives one case per cycle continuously -- callers
// pass a sentinel run of trailing dummy inputs (see drain()) to flush
// the last real case through the pipe.
static void	feed(uint32_t bits, uint64_t planned)
{
	uint32_t	bo, co;
	bool		bv, cv;
	static uint32_t	pending_bits = 0;
	static bool	have_pending = false;

	step(bits, bo, co, bv, cv);

	if (have_pending)
	{
		if (!bv || !cv)
		{
			fprintf(stderr, "PROTOCOL ERROR: expected valid_out on both DUTs every cycle post-reset (baseline=%d candidate=%d) at cycle=%lu\n",
				(int)bv, (int)cv, (unsigned long)g_cycle);
			exit(1);
		}
		g_result.total++;
		if (bo == co)
			g_result.matches++;
		else
		{
			g_result.mismatches++;
			std::string	cat = category_of(pending_bits);
			g_result.mismatch_by_category[cat]++;
			if (g_result.first_mismatches.size() < 25)
			{
				char	buf[256];

				snprintf(buf, sizeof(buf),
					"input=0x%08X category=%s baseline=0x%08X candidate=0x%08X",
					pending_bits, cat.c_str(), bo, co);
				g_result.first_mismatches.push_back(buf);
			}
		}
		heartbeat(planned);
	}
	pending_bits = bits;
	have_pending = true;
}

// One more cycle to flush and compare the final pending case through
// DELAY=1 -- reuses feed()'s own compare-the-pending-case logic rather
// than duplicating it, then leaves this dummy 0x0 input's own result
// uncompared (there is nothing after it to flush it in turn).
static void	drain(uint64_t planned)
{
	feed(0, planned);
}

int	main(int argc, char **argv)
{
	uint64_t	random_count = 2200000;

	Verilated::commandArgs(argc, argv);
	if (argc > 1)
		random_count = strtoull(argv[1], nullptr, 10);

	g_baseline = new Vmembrane_fp_divider;
	g_candidate = new Vfp32_scale_neg_pow2;

	g_start_time = std::chrono::steady_clock::now();
	g_last_heartbeat = g_start_time;

	do_reset(5);

	std::vector<uint32_t>	mantissas = {
		0x000000u, 0x000001u, 0x000002u, 0x3FFFFFu,
		0x400000u, 0x400001u, 0x7FFFFEu, 0x7FFFFFu,
	};
	uint64_t	boundary_count = 256ull * 2ull * mantissas.size();

	std::vector<uint32_t>	specials = {
		0x00000000u, 0x80000000u,			// +-0
		0x00000001u, 0x80000001u,			// smallest +-subnormal
		0x007FFFFFu, 0x807FFFFFu,			// largest +-subnormal
		0x00800000u, 0x80800000u,			// smallest +-normal
		0x7F7FFFFFu, 0xFF7FFFFFu,			// largest +-normal
		0x7F800000u, 0xFF800000u,			// +-Inf
		0x7FC00000u, 0xFFC00000u,			// +-quiet NaN
		0x7FA00000u, 0xFFA00000u,			// +-signaling NaN
		0x7FFFFFFFu, 0xFFFFFFFFu,			// max-payload NaN
		0x7F800001u, 0xFF800001u,			// min-payload signaling NaN
		0x01800000u, 0x81800000u,			// exp=3, mant=0 (underflow boundary)
		0x018001FFu, 0x818001FFu,			// exp=3, mant!=0 (underflow boundary)
		0x02000000u, 0x82000000u,			// exp=4, mant=0 (first surviving exponent)
		0x020001FFu, 0x820001FFu,			// exp=4, mant!=0
		0x00800001u, 0x80800001u,			// exp=1, mant=1 (subnormal/normal boundary+1)
		0x7F000000u, 0xFF000000u,			// exp=254 boundary neighbor
	};

	uint64_t	planned = boundary_count + specials.size() + random_count;

	fprintf(stderr, "[stage] boundary sweep: %lu cases\n", (unsigned long)boundary_count);
	for (uint32_t exp = 0; exp <= 255; exp++)
	{
		for (uint32_t sign = 0; sign <= 1; sign++)
		{
			for (uint32_t m : mantissas)
			{
				uint32_t	bits = (sign << 31) | (exp << 23) | m;

				feed(bits, planned);
			}
		}
	}
	fprintf(stderr, "[stage] boundary sweep done, total=%lu mismatches=%lu\n",
		(unsigned long)g_result.total, (unsigned long)g_result.mismatches);

	fprintf(stderr, "[stage] named specials: %lu cases\n", (unsigned long)specials.size());
	for (uint32_t bits : specials)
		feed(bits, planned);
	fprintf(stderr, "[stage] named specials done, total=%lu mismatches=%lu\n",
		(unsigned long)g_result.total, (unsigned long)g_result.mismatches);

	fprintf(stderr, "[stage] random: %lu cases\n", (unsigned long)random_count);
	{
		std::mt19937	rng(0xC0FFEEu);
		std::uniform_int_distribution<uint32_t>	dist(0, 0xFFFFFFFFu);

		for (uint64_t i = 0; i < random_count; i++)
			feed(dist(rng), planned);
	}
	fprintf(stderr, "[stage] random done, total=%lu mismatches=%lu\n",
		(unsigned long)g_result.total, (unsigned long)g_result.mismatches);

	drain(planned);

	printf("=== EXP-FPGA-DIV-001 Phase B1 differential test report ===\n");
	printf("total cases:      %lu\n", (unsigned long)g_result.total);
	printf("exact matches:    %lu\n", (unsigned long)g_result.matches);
	printf("mismatches:       %lu\n", (unsigned long)g_result.mismatches);
	if (!g_result.mismatch_by_category.empty())
	{
		printf("mismatch categories:\n");
		for (auto &kv : g_result.mismatch_by_category)
			printf("  %-24s %lu\n", kv.first.c_str(), (unsigned long)kv.second);
		printf("first %zu mismatch examples:\n", g_result.first_mismatches.size());
		for (auto &s : g_result.first_mismatches)
			printf("  %s\n", s.c_str());
	}

	if (g_result.mismatches == 0 && g_result.total == planned)
	{
		printf("PASS: fp32_scale_neg_pow2 vs membrane_fp_divider, %lu cases, 0 mismatches\n",
			(unsigned long)g_result.total);
		return 0;
	}
	if (g_result.total != planned)
	{
		printf("FAIL: expected %lu total cases, got %lu (protocol/accounting bug in this testbench)\n",
			(unsigned long)planned, (unsigned long)g_result.total);
		return 1;
	}
	printf("FAIL: %lu mismatches against the real membrane_fp_divider RTL -- do not integrate\n",
		(unsigned long)g_result.mismatches);
	return 1;
}
