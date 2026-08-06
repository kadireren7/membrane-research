// EXP-FPGA-DIV-001 Phase B1: a specialized, division-operator-free
// replacement for ONE specific call site -- rtl/q4_scale.sv's
// `u_div_d` instance, which computes `mx_f32 / -8.0f` through the
// general-purpose rtl/membrane_fp_divider.sv.
//
// This module is NOT a general FP32 divider and does not claim IEEE-754
// correctness for its own sake. Its only goal is bit-for-bit parity
// with what rtl/membrane_fp_divider.sv actually produces when its
// `b_in` port is held at the exact constant -(2^SHIFT) (F32
// 0xC1000000 for SHIFT=3, i.e. -8.0) -- including that divider's own
// disclosed non-IEEE simplifications (flush-to-zero underflow instead
// of gradual/subnormal results, and a "hidden bit assumed 1" treatment
// of subnormal operands). Differential-tested against the real
// membrane_fp_divider RTL, not re-derived from an idealized IEEE spec
// -- see rtl/tb/tb_fp32_scale_neg_pow2.cpp and
// experiments/EXP-FPGA-DIV-001/phase-b1.md for the full derivation and
// the exact edge cases (NaN sign handling, Inf/NaN exponent-field
// interception, and the underflow-flush threshold) that make a bare
// exponent subtraction alone insufficient.
//
// Why the output is provably exact for this one operation (re-derived
// here, then differential-tested, not assumed):
//
// membrane_fp_divider.sv computes the significand quotient as
// `quot64 = ((1<<25) * {1,mant_a}) / {1,mant_b}`. For b = -(2^SHIFT)
// exactly, mant_b = 0, so the significand divisor is exactly 2^23, and
// the significand dividend is exactly {1,mant_a} << 25 -- an exact
// division with zero remainder every time (no rounding path is ever
// actually taken: sticky is always 0, the two guard/round bits ending
// up in the discarded low bits are always 0, so `round_up` is always
// false and `mant_overflow` is always false). The surviving mantissa
// bits are therefore always exactly `mant_a`, unchanged. Only the
// exponent moves, by exactly SHIFT (dividing by 2^SHIFT), and the sign
// flips (dividing by a negative number) -- EXCEPT for the special
// cases below, which membrane_fp_divider.sv's own mux structure
// resolves before ever reaching the general arithmetic path, so this
// module resolves them the same way, in the same order:
//
//  1. a is NaN: membrane_fp_divider.sv's `a_is_nan` branch returns
//     `a_in | 32'h00400000` directly -- i.e. the SIGN IS NOT FLIPPED
//     for a NaN operand (unlike every other case below), only the
//     quiet bit is forced. This is the first edge case a plain
//     "subtract SHIFT from the exponent, flip the sign" rule would get
//     wrong.
//  2. a is +-Inf: exponent field 8'hFF would, under the plain linear
//     exponent formula, become `8'hFF - SHIFT` -- a bogus FINITE
//     result. membrane_fp_divider.sv instead special-cases this to a
//     signed infinity (sign flipped). This module intercepts it the
//     same way, before the linear formula ever runs.
//  3. a is exactly zero: signed zero out, sign flipped -- this is
//     already the same output the general formula would give (exponent
//     field 0 is `<= SHIFT` for any `SHIFT >= 0`, landing in the
//     underflow-flush case below), kept as its own explicit branch only
//     to mirror membrane_fp_divider.sv's own structure, not because the
//     result would differ if it were folded into the general case.
//  4. Underflow flush-to-zero: for any finite, non-inf, non-NaN `a`
//     whose raw exponent field is `<= SHIFT` (this covers every
//     genuinely subnormal `a`, since a subnormal's exponent field is
//     always 0, as well as normal `a` values whose exponent is too
//     small to survive an SHIFT-sized right-shift), the general formula
//     computed `exp_result = exp_a - SHIFT` is `<= 0`. Every FP32
//     result in this design uses IEEE754 biased-exponent 0 to mean
//     "zero" (there is no gradual-underflow/subnormal *output* path
//     anywhere in membrane_fp_divider.sv), so this module flushes to a
//     signed zero here too, exactly matching the reference, rather than
//     attempting to construct a subnormal result.
//  5. Overflow (`exp_result >= 255`) is unreachable for this exact
//     operation for any `SHIFT >= 0`: the largest possible finite `a`
//     has exponent field 254, so `exp_result <= 254 - SHIFT <= 254`,
//     always well under 255. Not implemented as a branch (there is
//     nothing for it to do), disclosed here rather than silently
//     omitted.
//
// Synthesizable: no `/`/`%` operator, no `real`/`shortreal`/DPI
// anywhere in this file. The only arithmetic is a fixed-width integer
// subtract for the exponent and a set of muxes -- no iterative
// structure, no wide combinational divide, unlike
// rtl/membrane_fp_divider.sv's `num64/den64`.
module fp32_scale_neg_pow2 #(
	// Divides by exactly -(2^SHIFT). SHIFT=3 reproduces `mx / -8.0f`
	// (the Q4_0 block-scale operation this module targets). Any
	// SHIFT >= 0 is structurally valid (see item 5 above for why
	// overflow never needs handling), but this module has only been
	// differential-tested against the real divider at SHIFT=3.
	parameter int SHIFT = 3,
	// Same DELAY convention as rtl/membrane_fp_divider.sv: purely
	// combinational compute, held for DELAY output-register stages.
	parameter int DELAY = 1
) (
	input	logic			clk,
	input	logic			rst_n,
	input	logic			valid_in,
	input	logic	[31:0]	a_in,
	// No `b_in` port: unlike membrane_fp_divider.sv, the divisor here
	// is not a runtime value, it is the fixed constant -(2^SHIFT)
	// baked into the hardware by the SHIFT parameter -- unlike a
	// general divider, there is no second operand to receive. This is
	// the one deliberate interface difference from
	// membrane_fp_divider.sv; every other port (clk/rst_n/valid_in/
	// valid_out/result_out) and the DELAY/pipeline timing convention
	// match exactly, so this module drops into the same clocked
	// valid-handshake slot rtl/q4_scale.sv's `u_div_d` instance
	// already uses.
	output	logic			valid_out,
	output	logic	[31:0]	result_out
);

	logic			sign_a;
	logic	[7:0]	exp_a;
	logic	[22:0]	mant_a;
	logic			a_is_nan, a_is_inf, a_is_zero;
	logic			result_sign;
	int				exp_a_s, exp_result;
	logic	[31:0]	result_comb;

	assign sign_a = a_in[31];
	assign exp_a  = a_in[30:23];
	assign mant_a = a_in[22:0];

	assign a_is_nan  = (exp_a == 8'hFF) && (mant_a != 23'h0);
	assign a_is_inf  = (exp_a == 8'hFF) && (mant_a == 23'h0);
	assign a_is_zero = (exp_a == 8'h00) && (mant_a == 23'h0);

	// The divisor -(2^SHIFT) is always negative, so the quotient's
	// sign is always the logical NOT of the dividend's sign -- except
	// for the NaN passthrough case below, which does not use this
	// signal at all (matching membrane_fp_divider.sv's own
	// `a_is_nan` branch, which returns `a_in` untouched aside from
	// the quiet bit).
	assign result_sign = ~sign_a;

	always_comb begin
		exp_a_s = exp_a;
		exp_result = exp_a_s - SHIFT;
	end

	always_comb begin
		if (a_is_nan)
			result_comb = a_in | 32'h00400000;
		else if (a_is_inf)
			result_comb = {result_sign, 8'hFF, 23'h0};
		else if (a_is_zero)
			result_comb = {result_sign, 31'h0};
		else if (exp_result <= 0)
			// Covers genuine subnormal `a` (exp_a==0, mant_a!=0) and
			// normal `a` too small to survive the shift -- see item 4
			// in this file's header comment.
			result_comb = {result_sign, 31'h0};
		else
			// Exact: mant_a passed through unchanged (see this file's
			// header comment for why no rounding path is ever taken
			// for this exact divisor), only the exponent moves.
			result_comb = {result_sign, 8'(exp_result), mant_a};
	end

	logic	[31:0]	result_pipe	[0:DELAY - 1];

	always_ff @(posedge clk) begin
		result_pipe[0] <= result_comb;
		for (int k = DELAY - 1; k > 0; k--)
			result_pipe[k] <= result_pipe[k - 1];
	end

	valid_delay_line #(.DEPTH(DELAY)) u_valid (
		.clk(clk), .rst_n(rst_n), .valid_in(valid_in),
		.valid_out(valid_out));

	assign result_out = result_pipe[DELAY - 1];
endmodule
