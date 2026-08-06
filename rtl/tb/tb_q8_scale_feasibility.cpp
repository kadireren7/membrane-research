// EXP-FPGA-DIV-002 Phase A: differential FEASIBILITY study for q8_scale's
// two membrane_fp_divider instances (d = amax/127.0, id = 127.0/amax).
// This is NOT a pass/fail correctness gate for any production RTL --
// q8_scale.sv is unmodified by this experiment (rtl/q8_scale.sv's own
// header comment already states the current, direct-division form is
// intentional; see experiments/EXP-FPGA-DIV-002/results/baseline-dataflow.md
// section 2 for the full derivation). This tool MEASURES how often each
// candidate reconstruction/shortcut would agree bit-exactly with the real
// RTL's current behavior, across a large, realistic + adversarial sample,
// and reports the result -- it does not assert 0 mismatches anywhere.
//
// Every candidate is computed either by re-driving the REAL, Verilated
// production RTL (rtl/membrane_fp_divider.sv, rtl/membrane_fp_multiplier.sv)
// with different operands, or a plain host double-precision computation
// used only to derive a compile-time ROUNDING CONSTANT (the nearest-float
// approximation of 1/127) -- never a hand-written approximation of the
// divider's own rounding behavior. No new divider variant is written or
// synthesized anywhere in this file, per this experiment's own scope.
//
// Candidates measured (see experiments/EXP-FPGA-DIV-002/baseline.md
// section 4 for the full analysis each maps to):
//   A. current_d  = RTL membrane_fp_divider(amax, 127.0)          [[the real d]]
//   A. current_id = RTL membrane_fp_divider(127.0, amax)          [[the real id]]
//   B. recip_from_d  = RTL membrane_fp_divider(1.0, current_d)    [[candidate 1/d]]
//   B. recip_from_id = RTL membrane_fp_divider(1.0, current_id)   [[candidate 1/id]]
//   C. const_recip_d = RTL membrane_fp_multiplier(amax, INV127)   [[candidate d via amax * (1/127), vs. current_d -- this
//      replaces the amax/127 divider, NOT the 127/amax one; amax*(1/127) computes amax/127's own float value, not its
//      reciprocal, so the correct comparison target is current_d, not current_id]]
// Candidate D/E (shared or dual small dividers) and F (algebraic) are
// architectural, not bit-value, questions -- not applicable to this
// differential harness; addressed in baseline.md's analysis sections only.
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
#include "Vmembrane_fp_multiplier.h"
#include "verilated.h"
#include "membrane/f16convert.h"

static const uint32_t	F32_127 = 0x42FE0000u;	// 127.0f
static const uint32_t	F32_ONE = 0x3F800000u;	// 1.0f

// (float)(1.0/127.0), the nearest-float constant-reciprocal candidate.
// Computed once, at startup, from a plain host double division -- this is
// a ROUNDING CONSTANT derivation (what the closest float to the exact
// mathematical value 1/127 is), not a stand-in for the divider's own
// runtime rounding of a variable operand; the codebase's own existing
// vector generators (rtl/tb/gen_fp_divider_vectors.c) already use plain
// host `float` arithmetic as their reference/golden values for this
// divider, so this follows established convention.
static uint32_t	g_inv127_bits;

static Vmembrane_fp_divider		*g_div;
static Vmembrane_fp_multiplier	*g_mul;

static uint64_t	g_cycle = 0;
static std::chrono::steady_clock::time_point	g_start_time, g_last_heartbeat;
static std::mt19937	g_rng(0xD1D2D3u);

static uint32_t	f32_bits(float f)
{
	uint32_t	bits;

	memcpy(&bits, &f, sizeof(bits));
	return (bits);
}

static void	div_step_clk0(void) { g_div->clk = 0; g_div->eval(); }
static void	div_step_clk1(void) { g_div->clk = 1; g_div->eval(); }
static void	mul_step_clk0(void) { g_mul->clk = 0; g_mul->eval(); }
static void	mul_step_clk1(void) { g_mul->clk = 1; g_mul->eval(); }

// membrane_fp_divider and membrane_fp_multiplier are both fixed DELAY=1
// (rtl/q8_scale.sv/q8_quantize_pack.sv's own instantiation parameters),
// single-cycle-latency, no handshake -- feed one operand pair, step the
// clock ONE posedge (the same result_pipe[0]<=result_comb register that
// makes DELAY=1 latches on that same edge), then valid_out/result_out
// already reflect it -- matches rtl/tb/tb_membrane_fp_divider_radix4.cpp's
// own baseline-divider driving pattern (one step() per case, check
// immediately after).
static uint32_t	div_once(uint32_t a, uint32_t b)
{
	g_div->valid_in = 1;
	g_div->a_in = a;
	g_div->b_in = b;
	div_step_clk0(); div_step_clk1(); g_cycle++;
	g_div->valid_in = 0;
	if (!g_div->valid_out)
	{
		fprintf(stderr, "PROTOCOL ERROR: membrane_fp_divider valid_out not asserted 1 cycle after issue\n");
		exit(1);
	}
	return (g_div->result_out);
}

static uint32_t	mul_once(uint32_t a, uint32_t b)
{
	g_mul->valid_in = 1;
	g_mul->a_in = a;
	g_mul->b_in = b;
	mul_step_clk0(); mul_step_clk1(); g_cycle++;
	g_mul->valid_in = 0;
	if (!g_mul->valid_out)
	{
		fprintf(stderr, "PROTOCOL ERROR: membrane_fp_multiplier valid_out not asserted 1 cycle after issue\n");
		exit(1);
	}
	return (g_mul->result_out);
}

static void	reset_all(int cycles)
{
	g_div->rst_n = 0;
	g_div->valid_in = 0;
	g_div->a_in = 0;
	g_div->b_in = 0;
	g_mul->rst_n = 0;
	g_mul->valid_in = 0;
	g_mul->a_in = 0;
	g_mul->b_in = 0;
	for (int i = 0; i < cycles; i++)
	{
		div_step_clk0(); div_step_clk1();
		mul_step_clk0(); mul_step_clk1();
		g_cycle++;
	}
	g_div->rst_n = 1;
	g_mul->rst_n = 1;
	div_step_clk0(); div_step_clk1();
	mul_step_clk0(); mul_step_clk1();
	g_cycle++;
}

static uint32_t	ulp_distance(uint32_t a, uint32_t b)
{
	// Standard signed-magnitude-to-biased-integer ULP distance, defined
	// only when neither operand is NaN. Both inputs here are expected to
	// be finite/Inf-class values from this datapath's own domain; NaN
	// pairs are never fed to this function (mismatch categories below
	// report NaN cases separately, without an ULP number).
	int64_t	ai = (a & 0x80000000u) ? -(int64_t)(a & 0x7FFFFFFFu) : (int64_t)a;
	int64_t	bi = (b & 0x80000000u) ? -(int64_t)(b & 0x7FFFFFFFu) : (int64_t)b;

	return (uint32_t)std::llabs(ai - bi);
}

static bool	is_nan_bits(uint32_t x)
{
	return ((x & 0x7F800000u) == 0x7F800000u) && ((x & 0x007FFFFFu) != 0u);
}

struct CandidateStats
{
	std::string	name;
	uint64_t	total = 0;
	uint64_t	exact_match = 0;
	uint64_t	mismatch = 0;
	uint32_t	max_ulp = 0;
	std::map<std::string, uint64_t>	mismatch_by_category;
	std::vector<std::string>	first_mismatches;

	void	record(uint32_t amax_bits, uint32_t reference, uint32_t candidate,
			const char *category)
	{
		total++;
		bool	ref_nan = is_nan_bits(reference);
		bool	cand_nan = is_nan_bits(candidate);
		bool	ok;

		if (ref_nan || cand_nan)
			ok = ref_nan && cand_nan;	// NaN-vs-NaN: payload not compared, matches this project's own multiplier/divider NaN-propagation convention
		else
			ok = (reference == candidate);

		if (ok)
		{
			exact_match++;
			return;
		}
		mismatch++;
		if (!ref_nan && !cand_nan)
		{
			uint32_t	ulp = ulp_distance(reference, candidate);

			if (ulp > max_ulp)
				max_ulp = ulp;
		}
		mismatch_by_category[category]++;
		if (first_mismatches.size() < 25)
		{
			char	buf[256];

			snprintf(buf, sizeof(buf),
				"amax=0x%08X reference=0x%08X candidate=0x%08X category=%s",
				amax_bits, reference, candidate, category);
			first_mismatches.push_back(buf);
		}
	}

	void	print(void) const
	{
		printf("--- candidate: %s ---\n", name.c_str());
		printf("  total:        %lu\n", (unsigned long)total);
		printf("  exact_match:  %lu (%.6f%%)\n", (unsigned long)exact_match,
			total ? 100.0 * (double)exact_match / (double)total : 0.0);
		printf("  mismatch:     %lu (%.6f%%)\n", (unsigned long)mismatch,
			total ? 100.0 * (double)mismatch / (double)total : 0.0);
		printf("  max_ulp:      %u\n", max_ulp);
		if (!mismatch_by_category.empty())
		{
			printf("  mismatch categories:\n");
			for (auto &kv : mismatch_by_category)
				printf("    %-24s %lu\n", kv.first.c_str(), (unsigned long)kv.second);
			printf("  first %zu mismatch examples:\n", first_mismatches.size());
			for (auto &s : first_mismatches)
				printf("    %s\n", s.c_str());
		}
	}
};

static CandidateStats	g_recip_from_d;
static CandidateStats	g_recip_from_id;
static CandidateStats	g_const_recip_d;

static uint64_t	g_planned = 0;
static uint64_t	g_done = 0;

static void	heartbeat(void)
{
	auto	now = std::chrono::steady_clock::now();
	double	since = std::chrono::duration<double>(now - g_last_heartbeat).count();

	if (since >= 5.0)
	{
		double	elapsed = std::chrono::duration<double>(now - g_start_time).count();
		double	frac = g_planned ? (double)g_done / (double)g_planned : 0.0;
		double	eta = (frac > 0.0) ? (elapsed / frac - elapsed) : 0.0;

		fprintf(stderr,
			"[heartbeat] cases=%lu/%lu elapsed=%.0fs eta=%.0fs cycle=%lu\n",
			(unsigned long)g_done, (unsigned long)g_planned, elapsed, eta,
			(unsigned long)g_cycle);
		g_last_heartbeat = now;
	}
}

static void	check_case(uint32_t amax_bits, const char *category)
{
	uint32_t	current_d = div_once(amax_bits, F32_127);
	uint32_t	current_id;

	bool	amax_is_zero = (amax_bits == 0x00000000u || amax_bits == 0x80000000u);

	// Mirror rtl/q8_scale.sv's own explicit zero-override mux exactly
	// (see baseline-dataflow.md section 5): the divider itself would
	// produce +Inf for 127/0, but the production RTL never lets that
	// reach id_f32_out. The "current_id" reference used for this
	// feasibility study is therefore the PRODUCTION value, not the raw
	// divider output, so candidates are judged against what q8_scale.sv
	// actually emits today.
	if (amax_is_zero)
		current_id = 0x00000000u;
	else
		current_id = div_once(F32_127, amax_bits);

	// Candidates are computed WITHOUT any zero-masking of their own --
	// each is judged on whether it naturally reproduces the production
	// reference (which, for current_id, IS already the masked/production
	// value). A candidate that would need its own explicit zero-check to
	// match is a real, disclosed cost of that candidate, not hidden by
	// silently giving it the same masking the production id gets for free.
	uint32_t	recip_from_d = div_once(F32_ONE, current_d);
	uint32_t	recip_from_id = div_once(F32_ONE, current_id);
	// amax * (1/127) computes amax/127's OWN value, i.e. a candidate for
	// current_d (NOT current_id -- multiplying by the reciprocal of 127
	// is algebraically amax/127, not 127/amax). No zero-masking needed
	// here either: amax=0 times any finite constant is exactly 0,
	// matching current_d's own unmasked zero behavior naturally.
	uint32_t	const_recip_d = mul_once(amax_bits, g_inv127_bits);

	g_recip_from_d.record(amax_bits, current_id, recip_from_d, category);
	g_recip_from_id.record(amax_bits, current_d, recip_from_id, category);
	g_const_recip_d.record(amax_bits, current_d, const_recip_d, category);

	g_done++;
	heartbeat();
}

int	main(int argc, char **argv)
{
	uint64_t	random_count = 20000;
	uint64_t	runtime_dist_count = 2000;

	Verilated::commandArgs(argc, argv);
	if (argc > 1)
		random_count = strtoull(argv[1], nullptr, 10);
	if (argc > 2)
		runtime_dist_count = strtoull(argv[2], nullptr, 10);

	double	inv127 = 1.0 / 127.0;
	float	inv127_f = (float)inv127;

	g_inv127_bits = f32_bits(inv127_f);

	g_recip_from_d.name = "B: reconstructed 1/d (vs. current id)";
	g_recip_from_id.name = "B: reconstructed 1/id (vs. current d)";
	g_const_recip_d.name = "C: constant-reciprocal amax*(1/127) (vs. current d)";

	g_div = new Vmembrane_fp_divider;
	g_mul = new Vmembrane_fp_multiplier;

	g_start_time = std::chrono::steady_clock::now();
	g_last_heartbeat = g_start_time;

	reset_all(5);

	fprintf(stderr, "=== q8_scale divider-pair feasibility differential (EXP-FPGA-DIV-002) ===\n");
	fprintf(stderr, "INV127 constant: 0x%08X (%.9g)\n", g_inv127_bits, (double)inv127_f);

	// -------------------------------------------------------------------
	// Edge cases: zero, +/-0, smallest/largest subnormal, smallest/
	// largest normal, powers of two, mantissa/exponent boundaries,
	// NaN/Inf, values near the amax=127*2^k boundary where d's own
	// rounding could tip an ULP either way.
	// -------------------------------------------------------------------
	std::vector<std::pair<uint32_t, const char *>>	edge_cases = {
		{0x00000000u, "zero_pos"},
		{0x80000000u, "zero_neg"},
		{0x00000001u, "subnormal_smallest"},
		{0x007FFFFFu, "subnormal_largest"},
		{0x00800000u, "normal_smallest"},
		{0x7F7FFFFFu, "normal_largest_finite"},
		{0x7F800000u, "inf_pos"},
		{0xFF800000u, "inf_neg"},
		{0x7FC00000u, "nan_quiet"},
		{0x7FA00000u, "nan_signaling"},
		{0x3F800000u, "pow2_one"},
		{0x40000000u, "pow2_two"},
		{0x3F000000u, "pow2_half"},
		{0x42FE0000u, "amax_eq_127"},		// d rounds to exactly 1.0, id to exactly 1/127... boundary
		{0x437F0000u, "amax_eq_254"},		// 2*127: d rounds to 2.0 exactly (power of two amax/127 case)
		{0x3DCCCCCDu, "amax_eq_one_over_ten"},
	};
	for (uint32_t exp = 0; exp <= 255; exp += 17)		// sparse exponent sweep, both signs, mantissa boundary set
	{
		std::vector<uint32_t>	mants = {0x000000u, 0x000001u, 0x3FFFFFu, 0x400000u, 0x7FFFFEu, 0x7FFFFFu};

		for (uint32_t m : mants)
			for (uint32_t sign : {0u, 1u})
				edge_cases.push_back({(sign << 31) | (exp << 23) | m, "boundary_sweep"});
	}
	for (int shift = 0; shift <= 30; shift++)
		edge_cases.push_back({(uint32_t)shift << 23 | 0x00000000u, "pow2_sweep"});

	g_planned = edge_cases.size() + random_count + runtime_dist_count;

	fprintf(stderr, "[stage] edge cases: %lu\n", (unsigned long)edge_cases.size());
	for (auto &c : edge_cases)
		check_case(c.first, c.second);
	fprintf(stderr, "[stage] edge cases done, total=%lu\n", (unsigned long)g_done);

	fprintf(stderr, "[stage] uniform random amax (finite, non-negative-magnitude bit patterns): %lu cases\n",
		(unsigned long)random_count);
	{
		// amax is always non-negative (q8_maxabs_reduce.sv's own invariant,
		// baseline-dataflow.md section 6) -- sampling the full unsigned
		// magnitude range [0, 0x7FFFFFFF] covers zero, subnormals, normals,
		// inf, and NaN with sign bit always clear, matching the real
		// datapath's domain while still stressing the general-purpose
		// divider/multiplier blocks across their whole input space.
		std::uniform_int_distribution<uint32_t>	dist(0, 0x7FFFFFFFu);

		for (uint64_t i = 0; i < random_count; i++)
			check_case(dist(g_rng), "random_amax_magnitude");
	}
	fprintf(stderr, "[stage] random done, total=%lu\n", (unsigned long)g_done);

	fprintf(stderr, "[stage] real Q8 runtime amax distribution sample: %lu blocks\n",
		(unsigned long)runtime_dist_count);
	{
		// Same block-construction technique as
		// rtl/tb/tb_membrane_fp_divider_radix4.cpp's own "q4 runtime
		// d-distribution sample" stage: 32 random F16 values per block,
		// amax = max(|x|) via F16->F32 widening -- the actual distribution
		// shape this datapath's real Q8_0 encode chain produces amax from
		// (q8_maxabs_reduce.sv's own domain), not an arbitrary bit pattern.
		std::mt19937	block_rng(0xB10CDu);
		std::uniform_int_distribution<uint32_t>	half_dist(0, 0xFFFFu);

		for (uint64_t i = 0; i < runtime_dist_count; i++)
		{
			float	amax = 0.0f;

			for (int j = 0; j < 32; j++)
			{
				uint16_t	h = (uint16_t)half_dist(block_rng);
				float	v = membrane_f16_to_f32(h);

				if (fabsf(v) > amax)
					amax = fabsf(v);
			}
			check_case(f32_bits(amax), "q8_runtime_amax_dist");
		}
	}
	fprintf(stderr, "[stage] runtime distribution done, total=%lu\n", (unsigned long)g_done);

	double	elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - g_start_time).count();

	printf("=== q8_scale divider-pair feasibility differential report ===\n");
	printf("total cases:   %lu\n", (unsigned long)g_done);
	printf("wall_time_s:   %.1f\n", elapsed);
	printf("cycles:        %lu\n", (unsigned long)g_cycle);
	printf("\n");
	g_recip_from_d.print();
	printf("\n");
	g_recip_from_id.print();
	printf("\n");
	g_const_recip_d.print();

	printf("\n=== summary ===\n");
	printf("current d = amax/127 and current id = 127/amax are the PRODUCTION reference (unmodified).\n");
	printf("B (1/d matches id):     %lu/%lu exact (%.6f%%)\n",
		(unsigned long)g_recip_from_d.exact_match, (unsigned long)g_recip_from_d.total,
		g_recip_from_d.total ? 100.0 * (double)g_recip_from_d.exact_match / (double)g_recip_from_d.total : 0.0);
	printf("B (1/id matches d):     %lu/%lu exact (%.6f%%)\n",
		(unsigned long)g_recip_from_id.exact_match, (unsigned long)g_recip_from_id.total,
		g_recip_from_id.total ? 100.0 * (double)g_recip_from_id.exact_match / (double)g_recip_from_id.total : 0.0);
	printf("C (amax*(1/127) matches d):  %lu/%lu exact (%.6f%%)\n",
		(unsigned long)g_const_recip_d.exact_match, (unsigned long)g_const_recip_d.total,
		g_const_recip_d.total ? 100.0 * (double)g_const_recip_d.exact_match / (double)g_const_recip_d.total : 0.0);

	// This tool always exits 0 when it completes its planned case count --
	// a mismatch is an EXPECTED, MEASURED outcome for at least some
	// candidates here (that is the entire point of a feasibility study),
	// not a test failure. The caller (scripts/run-exp-q8-divider-002.sh)
	// checks g_done == g_planned to confirm the run actually completed,
	// not that mismatches == 0.
	bool	completed = (g_done == g_planned);

	printf("\n%s: q8_scale feasibility differential, %lu/%lu planned cases completed\n",
		completed ? "DONE" : "INCOMPLETE", (unsigned long)g_done, (unsigned long)g_planned);

	delete g_div;
	delete g_mul;
	return completed ? 0 : 1;
}
