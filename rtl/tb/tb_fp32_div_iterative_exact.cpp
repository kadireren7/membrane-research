// EXP-FPGA-DIV-001 Phase B2: component-level differential test between
// the real, general-purpose rtl/membrane_fp_divider.sv (the reference:
// fixed DELAY=1, no handshake beyond valid_in/valid_out) and the new
// rtl/experimental/fp_div/fp32_div_iterative_exact.sv (the candidate: a
// real in_valid/in_ready/out_valid/out_ready multi-cycle handshake,
// single transaction in flight). Both DUTs are Verilated from their own
// real RTL (two separate `verilator --Mdir` invocations, see
// scripts/run-exp-fp-divider-001.sh) -- this is a genuine RTL-vs-RTL
// bit-exactness check, not RTL-vs-idealized-math, exactly like Phase
// B1's own tb_fp32_scale_neg_pow2.cpp.
//
// The exact operation this experiment cares about is Q4_0's
// `id = (d!=0) ? 1.0f/d : 0.0f` -- so the bulk of cases hold the
// candidate/baseline's "numerator" input at the F32 constant 1.0f
// (0x3F800000) and vary the denominator, matching
// rtl/q4_scale.sv's own u_div_id instantiation
// (`membrane_fp_divider(.a_in(32'h3F800000), .b_in(d_f32_raw))`)
// exactly. Because fp32_div_iterative_exact.sv is deliberately kept
// general-purpose (numerator is a real input, not hardwired), a smaller
// supplementary set of cases also varies BOTH operands, including a
// full specials x specials cross product -- with the numerator pinned
// at 1.0f, membrane_fp_divider.sv's a_is_nan/a_is_inf/a_is_zero branches
// (and this module's identical copies of them) would never actually be
// exercised, which would leave real dead logic unverified.
//
// Since the two DUTs have genuinely different latency conventions
// (baseline: fixed 1 cycle, no ready port; candidate: variable,
// multi-cycle, single-in-flight, real backpressure), this test drives
// them ONE CASE AT A TIME (not continuously pipelined every cycle like
// tb_fp32_scale_neg_pow2.cpp could, since that Phase B1 pair shared an
// identical fixed DELAY) -- fully draining the candidate's result
// before issuing the next case. Ordering is therefore preserved
// trivially by construction at this component level (there is never
// more than one transaction outstanding to reorder); genuine concurrent-
// transaction ordering under the shared top-level FIFO is exercised
// separately by rtl/experimental/fp_div/tb_top_verilator_b2.cpp (the
// full-datapath test), not here.
//
// Report: total cases, exact matches, mismatch count (by category),
// accepted-input count, output count, min/mean/max candidate latency
// (cycles from issue to retire), initiation interval (measured, single
// in-flight so II == latency for back-to-back full-throughput
// candidate-only issuance -- see this file's own throughput stage), and
// separate reset-recovery / backpressure / timeout-deadlock checks.
// Target: 0 mismatches. If that is not achieved, this test says so
// loudly (nonzero exit code, mismatch examples printed) -- per this
// experiment's own rule, Q4_0 integration must not proceed if this ever
// fails.
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
#include "verilated.h"
#include "membrane/f16convert.h"

static const uint32_t	ONE_F32 = 0x3F800000u;
static const uint64_t	MAX_CYCLES_PER_CASE = 10000;

static Vmembrane_fp_divider			*g_baseline;
static Vfp32_div_iterative_exact	*g_cand;

static uint64_t	g_cycle = 0;
static std::chrono::steady_clock::time_point	g_start_time, g_last_heartbeat;
static std::mt19937	g_rng(0xC0FFEEu);

struct Result
{
	uint64_t	total = 0;
	uint64_t	matches = 0;
	uint64_t	mismatches = 0;
	uint64_t	accepted = 0;
	uint64_t	retired = 0;
	uint64_t	latency_min = UINT64_MAX;
	uint64_t	latency_max = 0;
	long double	latency_sum = 0.0L;
	uint64_t	latency_nobp_min = UINT64_MAX;
	uint64_t	latency_nobp_max = 0;
	long double	latency_nobp_sum = 0.0L;
	uint64_t	latency_nobp_count = 0;
	std::map<std::string, uint64_t>	mismatch_by_category;
	std::vector<std::string>	first_mismatches;
};

static Result	g_result;
static uint64_t	g_planned = 0;

static std::string	category_of(uint32_t num, uint32_t den, const char *tag)
{
	if (tag)
		return std::string(tag);

	uint32_t	exp = (den >> 23) & 0xFFu;
	uint32_t	mant = den & 0x7FFFFFu;
	uint32_t	sign = (den >> 31) & 1u;
	const char	*sgn = sign ? "neg" : "pos";

	(void)num;
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
			"[heartbeat] cases=%lu/%lu mismatches=%lu accepted=%lu retired=%lu elapsed=%.0fs eta=%.0fs cycle=%lu\n",
			(unsigned long)g_result.total, (unsigned long)g_planned,
			(unsigned long)g_result.mismatches, (unsigned long)g_result.accepted,
			(unsigned long)g_result.retired, elapsed, eta, (unsigned long)g_cycle);
		g_last_heartbeat = now;
	}
}

// One clk 0->1 toggle for BOTH DUTs (shared clock domain). Since this
// test drives one case at a time (not a continuous every-cycle
// pipeline), outputs are read directly after step() returns -- by then
// both DUTs' registers have already latched this edge's new values and
// verilated eval() has re-settled all downstream combinational logic,
// so there is no "stale one-cycle-behind" reading subtlety to manage
// here (unlike tb_top_verilator.cpp's continuous per-cycle pipeline,
// which reads pre-edge for a different, unrelated reason -- see that
// file's own header comment).
static void	step(void)
{
	g_baseline->clk = 0;
	g_cand->clk = 0;
	g_baseline->eval();
	g_cand->eval();

	g_baseline->clk = 1;
	g_cand->clk = 1;
	g_baseline->eval();
	g_cand->eval();
	g_cycle++;
}

static void	reset_both(int cycles)
{
	g_baseline->rst_n = 0;
	g_cand->rst_n = 0;
	g_baseline->valid_in = 0;
	g_baseline->a_in = 0;
	g_baseline->b_in = 0;
	g_cand->in_valid = 0;
	g_cand->numerator = 0;
	g_cand->denominator = 0;
	g_cand->out_ready = 1;
	for (int i = 0; i < cycles; i++)
		step();
	g_baseline->rst_n = 1;
	g_cand->rst_n = 1;
	step();
}

// Drives one case through both DUTs, fully draining the candidate
// before returning. `allow_bp`: randomize out_ready each cycle instead
// of holding it high (exercises real backpressure -- the result must
// not change and out_valid must not drop until consumed). Returns false
// on protocol violation/timeout (printed already); on success, fills in
// baseline_bits/cand_bits/latency_cycles and updates g_result's
// accepted/retired/latency stats (but NOT match/mismatch bookkeeping --
// callers do that themselves so they can attach a category label).
static bool	run_case(uint32_t num, uint32_t den, uint32_t &baseline_bits,
		uint32_t &cand_bits, uint64_t &latency_cycles, bool allow_bp)
{
	if (!g_cand->in_ready)
	{
		fprintf(stderr, "PROTOCOL ERROR: candidate not in_ready at case start (num=0x%08X den=0x%08X) -- previous case not fully drained\n",
			num, den);
		return false;
	}

	g_baseline->valid_in = 1;
	g_baseline->a_in = num;
	g_baseline->b_in = den;
	g_cand->in_valid = 1;
	g_cand->numerator = num;
	g_cand->denominator = den;
	g_cand->out_ready = 1;

	bool	pre_in_ready = g_cand->in_ready;
	bool	pre_in_valid = g_cand->in_valid;

	step();	// issue edge: baseline latches into its 1-stage pipe; candidate transitions IDLE -> ITER/ROUND
	g_result.accepted += (pre_in_ready && pre_in_valid) ? 1 : 0;
	g_baseline->valid_in = 0;
	g_cand->in_valid = 0;

	if (!g_baseline->valid_out)
	{
		fprintf(stderr, "PROTOCOL ERROR: baseline valid_out not asserted 1 cycle after issue (DELAY=1 violated)\n");
		return false;
	}
	baseline_bits = g_baseline->result_out;

	uint64_t	cyc = 1;
	bool	got = false;

	while (cyc < MAX_CYCLES_PER_CASE)
	{
		bool	this_ready = allow_bp ? (bool)(g_rng() & 1u) : true;

		g_cand->out_ready = this_ready;
		if (g_cand->out_valid && this_ready)
		{
			cand_bits = g_cand->quotient;
			got = true;
			step();
			cyc++;
			break;
		}
		step();
		cyc++;
	}
	if (!got)
	{
		fprintf(stderr, "TIMEOUT/DEADLOCK: candidate never retired within %lu cycles (num=0x%08X den=0x%08X)\n",
			(unsigned long)MAX_CYCLES_PER_CASE, num, den);
		return false;
	}
	g_result.retired++;
	latency_cycles = cyc;
	g_result.latency_min = std::min(g_result.latency_min, cyc);
	g_result.latency_max = std::max(g_result.latency_max, cyc);
	g_result.latency_sum += (long double)cyc;
	if (!allow_bp)
	{
		g_result.latency_nobp_min = std::min(g_result.latency_nobp_min, cyc);
		g_result.latency_nobp_max = std::max(g_result.latency_nobp_max, cyc);
		g_result.latency_nobp_sum += (long double)cyc;
		g_result.latency_nobp_count++;
	}
	return true;
}

static bool	check_case(uint32_t num, uint32_t den, const char *tag, bool allow_bp)
{
	uint32_t	bo, co;
	uint64_t	lat;

	if (!run_case(num, den, bo, co, lat, allow_bp))
		exit(1);
	g_result.total++;
	if (bo == co)
		g_result.matches++;
	else
	{
		g_result.mismatches++;
		std::string	cat = category_of(num, den, tag);

		g_result.mismatch_by_category[cat]++;
		if (g_result.first_mismatches.size() < 25)
		{
			char	buf[256];

			snprintf(buf, sizeof(buf),
				"num=0x%08X den=0x%08X category=%s baseline=0x%08X candidate=0x%08X",
				num, den, cat.c_str(), bo, co);
			g_result.first_mismatches.push_back(buf);
		}
	}
	heartbeat();
	return (bo == co);
}

// ---- reset-mid-computation recovery test ----
// Issues a real case, lets it run partway into ITER (a fixed few
// cycles -- guaranteed to still be mid-iteration given
// MANT_ITER_WIDTH=26), asserts rst_n for a few cycles, then verifies
// the FSM comes back to a clean idle state (in_ready high, busy/
// out_valid low) and can correctly process a FRESH case afterward
// (checked bit-exact against baseline, same as every other case). The
// interrupted case's own result is legitimately discarded -- that is
// what reset-mid-computation is supposed to do.
static void	reset_recovery_test(uint64_t &fails)
{
	fprintf(stderr, "[stage] reset-mid-computation recovery test\n");
	if (!g_cand->in_ready)
	{
		fprintf(stderr, "FAIL: candidate not idle before reset-recovery test\n");
		fails++;
		return;
	}
	g_cand->numerator = ONE_F32;
	g_cand->denominator = 0x40490FDBu;	// pi, an arbitrary general-path denominator
	g_cand->in_valid = 1;
	g_cand->out_ready = 1;
	step();
	g_cand->in_valid = 0;
	if (!g_cand->busy)
	{
		fprintf(stderr, "FAIL: candidate not busy right after accepting a general-path case\n");
		fails++;
	}
	for (int i = 0; i < 5; i++)
		step();	// definitely mid-ITER now (MANT_ITER_WIDTH=26 cycles of iteration)
	if (g_cand->out_valid)
	{
		fprintf(stderr, "FAIL: candidate asserted out_valid too early (5 cycles in) for a general-path case -- test assumption invalid or FSM bug\n");
		fails++;
	}

	g_cand->rst_n = 0;
	for (int i = 0; i < 3; i++)
	{
		step();
		if (g_cand->out_valid)
		{
			fprintf(stderr, "FAIL: out_valid asserted during reset\n");
			fails++;
		}
	}
	g_cand->rst_n = 1;
	step();

	if (!g_cand->in_ready)
	{
		fprintf(stderr, "FAIL: candidate not in_ready one cycle after reset deassertion\n");
		fails++;
	}
	if (g_cand->busy)
	{
		fprintf(stderr, "FAIL: candidate still busy one cycle after reset deassertion\n");
		fails++;
	}

	// The baseline divider was mid-pipe too (a 1-stage register) --
	// reset it back to a clean state the same way before resuming the
	// shared-DUT differential loop, otherwise its very next real case
	// would spuriously fail (stale valid_out from a reset-corrupted
	// pipe register, not a real candidate bug).
	g_baseline->rst_n = 0;
	g_baseline->valid_in = 0;
	step();
	g_baseline->rst_n = 1;
	step();

	// Now a fresh, real case must still be bit-exact.
	if (!check_case(ONE_F32, 0x3F000000u, "reset_recovery_followup", false))	// 1/0.5 = 2.0
	{
		fprintf(stderr, "FAIL: post-reset-recovery case mismatched baseline\n");
		fails++;
	}
	fprintf(stderr, "[stage] reset-mid-computation recovery test done, fails so far: %lu\n",
		(unsigned long)fails);
}

// ---- throughput measurement: back-to-back general-path cases, no
// backpressure, to measure initiation interval directly (single
// in-flight, so II should equal the general-path latency measured
// elsewhere in this same run) ----
static void	throughput_test(uint64_t n, uint64_t &out_total_cycles)
{
	uint64_t	cyc0 = g_cycle;
	std::uniform_int_distribution<uint32_t>	dist(0x00800000u, 0x7F7FFFFFu);	// normal, finite, positive

	fprintf(stderr, "[stage] throughput measurement: %lu back-to-back general-path cases\n", (unsigned long)n);
	for (uint64_t i = 0; i < n; i++)
	{
		uint32_t	bo, co;
		uint64_t	lat;

		if (!run_case(ONE_F32, dist(g_rng), bo, co, lat, false))
			exit(1);
		g_result.total++;
		g_result.matches += (bo == co) ? 1 : 0;
		if (bo != co)
		{
			g_result.mismatches++;
			g_result.mismatch_by_category["throughput_general"]++;
		}
	}
	out_total_cycles = g_cycle - cyc0;
	fprintf(stderr, "[stage] throughput measurement done: %lu cycles for %lu transactions\n",
		(unsigned long)out_total_cycles, (unsigned long)n);
}

int	main(int argc, char **argv)
{
	uint64_t	random_count = 2000000;
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
	g_cand = new Vfp32_div_iterative_exact;

	g_start_time = std::chrono::steady_clock::now();
	g_last_heartbeat = g_start_time;

	reset_both(5);

	std::vector<uint32_t>	mantissas = {
		0x000000u, 0x000001u, 0x000002u, 0x3FFFFFu,
		0x400000u, 0x400001u, 0x7FFFFEu, 0x7FFFFFu,
	};
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
		0x3F800000u, 0xBF800000u,			// +-1.0 (exact quotient case)
		0x40000000u, 0xC0000000u,			// +-2.0 (power of two)
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

	fprintf(stderr, "=== EXP-FPGA-DIV-001 Phase B2 differential test ===\n");
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
	fprintf(stderr, "[stage] boundary sweep done, total=%lu mismatches=%lu\n",
		(unsigned long)g_result.total, (unsigned long)g_result.mismatches);

	fprintf(stderr, "[stage] specials x specials cross product (both operands vary): %lu cases\n",
		(unsigned long)cross_special_count);
	for (uint32_t num : specials)
		for (uint32_t den : specials)
			check_case(num, den, "cross_special", false);
	fprintf(stderr, "[stage] cross-special done, total=%lu mismatches=%lu\n",
		(unsigned long)g_result.total, (unsigned long)g_result.mismatches);

	fprintf(stderr, "[stage] powers of two (numerator=1.0f): %lu cases\n", (unsigned long)pow2.size());
	for (uint32_t den : pow2)
		check_case(ONE_F32, den, "pow2", false);

	fprintf(stderr, "[stage] uniform random denominator, numerator=1.0f (the real Q4_0 op): %lu cases, random out_ready backpressure\n",
		(unsigned long)random_count);
	{
		std::uniform_int_distribution<uint32_t>	dist(0, 0xFFFFFFFFu);

		for (uint64_t i = 0; i < random_count; i++)
			check_case(ONE_F32, dist(g_rng), "random_num1_denrand", (i % 4) == 0);
	}
	fprintf(stderr, "[stage] random (numerator=1.0f) done, total=%lu mismatches=%lu\n",
		(unsigned long)g_result.total, (unsigned long)g_result.mismatches);

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
	fprintf(stderr, "[stage] random (both operands) done, total=%lu mismatches=%lu\n",
		(unsigned long)g_result.total, (unsigned long)g_result.mismatches);

	fprintf(stderr, "[stage] q4 runtime d-distribution sample (numerator=1.0f, denominator=real mx/-8.0f values): %lu cases\n",
		(unsigned long)q4_dist_count);
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
	fprintf(stderr, "[stage] q4 d-distribution done, total=%lu mismatches=%lu\n",
		(unsigned long)g_result.total, (unsigned long)g_result.mismatches);

	uint64_t	reset_fails = 0;

	reset_recovery_test(reset_fails);

	uint64_t	throughput_cycles = 0;
	uint64_t	throughput_n = 2000;

	throughput_test(throughput_n, throughput_cycles);

	double	elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - g_start_time).count();
	double	mean_latency = g_result.retired ? (double)(g_result.latency_sum / (long double)g_result.retired) : 0.0;
	double	ii = throughput_n ? (double)throughput_cycles / (double)throughput_n : 0.0;
	double	tput = ii > 0.0 ? 1.0 / ii : 0.0;

	printf("=== EXP-FPGA-DIV-001 Phase B2 differential test report ===\n");
	printf("total cases:           %lu\n", (unsigned long)g_result.total);
	printf("exact matches:         %lu\n", (unsigned long)g_result.matches);
	printf("mismatches:            %lu\n", (unsigned long)g_result.mismatches);
	printf("accepted inputs:       %lu\n", (unsigned long)g_result.accepted);
	printf("retired outputs:       %lu\n", (unsigned long)g_result.retired);
	printf("latency_cycles_min (all, incl. backpressure stalls):  %lu\n", (unsigned long)(g_result.latency_min == UINT64_MAX ? 0 : g_result.latency_min));
	printf("latency_cycles_mean (all, incl. backpressure stalls): %.3f\n", mean_latency);
	printf("latency_cycles_max (all, incl. backpressure stalls):  %lu\n", (unsigned long)g_result.latency_max);
	{
		double	mean_nobp = g_result.latency_nobp_count
			? (double)(g_result.latency_nobp_sum / (long double)g_result.latency_nobp_count) : 0.0;

		printf("latency_cycles_min (no backpressure):  %lu\n", (unsigned long)(g_result.latency_nobp_min == UINT64_MAX ? 0 : g_result.latency_nobp_min));
		printf("latency_cycles_mean (no backpressure): %.3f (n=%lu)\n", mean_nobp, (unsigned long)g_result.latency_nobp_count);
		printf("latency_cycles_max (no backpressure):  %lu\n", (unsigned long)g_result.latency_nobp_max);
	}
	printf("initiation_interval:   %.3f (measured, %lu back-to-back general-path transactions, no backpressure)\n",
		ii, (unsigned long)throughput_n);
	printf("throughput_txn_per_cycle: %.6f\n", tput);
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

	bool	pass = (g_result.mismatches == 0) && (reset_fails == 0)
		&& (g_result.total == g_planned + 1 /* reset_recovery_followup */ + throughput_n);

	if (pass)
	{
		printf("PASS: fp32_div_iterative_exact vs membrane_fp_divider, %lu cases, 0 mismatches, 0 reset-recovery fails\n",
			(unsigned long)g_result.total);
		delete g_baseline;
		delete g_cand;
		return 0;
	}
	printf("FAIL: fp32_div_iterative_exact vs membrane_fp_divider -- %lu mismatches, %lu reset-recovery fails -- do not integrate\n",
		(unsigned long)g_result.mismatches, (unsigned long)reset_fails);
	delete g_baseline;
	delete g_cand;
	return 1;
}
