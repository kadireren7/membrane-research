// EXP-FPGA-DIV-002 Phase B1: experimental variant of rtl/q8_scale.sv that
// replaces its two parallel `membrane_fp_divider` instances (each a
// single-cycle wide combinational divider, DIV_DELAY=1) with two parallel
// `membrane_fp_divider_radix4` instances (the same exact, synthesizable,
// multi-cycle iterative divider already promoted to production for Q4_0's
// `id = 1/d` path, see rtl/q4_scale.sv). Not a fork of that module -- it
// is instantiated here exactly as written in rtl/membrane_fp_divider_radix4.sv,
// per Phase A's own decision (baseline.md section 7, candidate E) that no
// new bit-exactness risk is introduced by this substitution.
//
// ---- item 1: is membrane_fp_divider_radix4 safe to reuse unmodified for
// q8_scale's actual operand shapes? ----
// Yes, verified by inspection AND by evidence already on record in this
// repository, not re-derived from scratch:
//   - The module's own ports (`numerator`, `denominator`, both full 32-bit
//     FP32 operands) carry no hardcoded assumption that either operand is
//     the constant 1.0 -- unlike this module's ONE existing production
//     call site (q4_scale.sv's `u_div_id`, numerator tied to
//     32'h3F800000), which is a caller-side choice, not something baked
//     into membrane_fp_divider_radix4.sv itself (read directly: its own
//     special-case decode treats `numerator`/`denominator` completely
//     symmetrically to `membrane_fp_divider.sv`'s `a_in`/`b_in`).
//   - This was already differentially exercised with BOTH operands
//     independently random ("random_general" category,
//     rtl/tb/tb_membrane_fp_divider_radix4.cpp, EXP-FPGA-DIV-001 Phase
//     B4, part of the 4,456,685-case, 0-mismatch run), not just with
//     numerator pinned to 1.0 -- so this module's general-purpose
//     numerator/denominator behavior is not new, unverified territory.
//   - Zero/Inf/NaN/subnormal decode is copied VERBATIM from
//     `membrane_fp_divider.sv` (same `a_is_nan`/`b_is_nan`/`a_is_inf`/
//     `b_is_inf`/`a_is_zero`/`b_is_zero` tests, same priority chain, same
//     CANON_NAN, same sign rule) -- by construction, not by re-derivation,
//     it decodes q8_scale's own operand pairs (amax vs. 127.0, and 127.0
//     vs. amax) exactly the same way membrane_fp_divider.sv already does,
//     which is what makes this experiment's differential test (item 4)
//     meaningful rather than circular.
// Conclusion: reused directly, unmodified, no fork, per the task's own
// "if valid, don't copy" instruction.
//
// ---- item 2/3: architecture ----
// Two independent membrane_fp_divider_radix4 instances, `u_div_d`
// (numerator=amax, denominator=127.0 -> d) and `u_div_id`
// (numerator=127.0, denominator=amax -> id), driven by the SAME
// `amax_f32` value on the SAME accepted transaction. Real
// in_valid/in_ready and out_valid/out_ready handshakes (unlike baseline
// q8_scale.sv, which had none -- membrane_fp_divider is fixed-latency and
// needed none): this module's own `in_ready` is the AND of both
// dividers' `in_ready` (both single-in-flight, both IDLE), and `in_valid`
// is only ever presented to EITHER divider gated by that same AND, so the
// two instances are accepted ATOMICALLY on the identical cycle -- there
// is no way for one to start a transaction the other doesn't. Rendezvous
// and per-instance result holding come for free from each divider's own
// FSM: `membrane_fp_divider_radix4`'s `S_DONE` state (its own `out_valid`)
// persists, with `quotient` held stable, until ITS OWN `out_ready` pulses
// (see that module's own header) -- so this wrapper drives BOTH
// dividers' `out_ready` with the SAME `consume` signal
// (`both_done && out_ready`), meaning whichever divider finishes first
// simply sits in `S_DONE` (its own `busy` still high, since `busy` covers
// `S_DONE` too) until the other catches up, with no extra holding
// register needed anywhere in this wrapper. `busy` is the OR of both
// instances' own `busy` (only actually differs from AND during any
// transient where the two finish on different cycles -- see the
// structural note below on when that can/can't happen).
//
// ---- a provable structural property, not just an empirical one ----
// For q8_scale's specific operand pairing, `d`'s pair (amax, 127.0) and
// `id`'s pair (127.0, amax) are ALWAYS special (NaN/Inf/zero) on exactly
// the same transactions: 127.0 (32'h42FE0000) is itself never NaN/Inf/
// zero, so `is_special` for `u_div_d` reduces to amax's own NaN/Inf/zero
// status as the NUMERATOR, and for `u_div_id` reduces to the SAME amax
// value's NaN/Inf/zero status as the DENOMINATOR -- the exact same
// exponent/mantissa tests, on the exact same bits, just applied to
// different operand POSITIONS. Since `membrane_fp_divider_radix4`'s cycle
// count from accept to `S_DONE` depends ONLY on this specialness flag
// (2 cycles if special via `EARLY_OUT_SPECIAL`, else a FIXED 13+2-cycle
// general path -- never on the operands' actual VALUES beyond that one
// bit), `u_div_d` and `u_div_id` are guaranteed to reach `S_DONE` on the
// identical cycle for every possible `amax`, given they are always
// started together (see above). The "result holding for an early
// completer" machinery this wrapper implements is therefore real,
// general, defensively-correct infrastructure -- but for q8_scale's own
// operand shape specifically, it is never observed to actually have to
// wait (confirmed, not just argued, by rtl/tb/tb_q8_scale_dual_radix4.cpp's
// own latency measurement: see experiments/EXP-FPGA-DIV-002/phase-b1.md).
// The `` `ifndef SYNTHESIS `` assertions below check this claim directly.
//
// ---- item 5: all-zero / negative-zero behavior, preserved bit-exactly,
// including an existing baseline quirk ----
// `rtl/q8_scale.sv`'s own zero-mask (`assign amax_is_zero = (amax_f32 ==
// 32'h0);`) is a BIT-EXACT comparison against positive zero only -- it
// does NOT catch negative zero (32'hFFFF8000 -> f32 32'h80000000), which
// q8_maxabs_reduce.sv's own invariant (amax is always the max of
// absolute values, never negative) never actually produces at the real
// Q8 encode call site, but which IS a reachable input to this experimental
// module in isolation (and IS one of this phase's own required test
// cases -- item 5's explicit "amax = -0"). For amax=-0.0, baseline
// q8_scale.sv's `id` is therefore UNMASKED and equals whatever
// membrane_fp_divider(127.0, -0.0) computes: `b_is_zero` fires
// (exponent/mantissa test, sign-independent) with `result_sign = sign_a
// (0) ^ sign_b (1) = 1`, i.e. NEGATIVE Infinity -- a real, disclosed,
// pre-existing quirk of the CURRENT production RTL, not something this
// experiment introduces or "fixes". This module reproduces it bit-exactly
// by using the IDENTICAL `amax_f32 == 32'h0` mask condition (not a
// broader magnitude-based zero test) combined with
// `membrane_fp_divider_radix4`'s own verbatim-matching special-case
// decode -- no extra logic was needed to reproduce this quirk, it falls
// out of reusing the same bit-exact check and the same decode table.
// `d` needs no external masking in either baseline or this variant: for
// amax=+0.0 or amax=-0.0, `a_is_zero` fires on the NUMERATOR side of both
// dividers identically, producing `d = {sign_a^sign_b, 0}` = +0.0/-0.0
// respectively in both baseline and this variant, matching by
// construction (verified in rtl/tb/tb_q8_scale_dual_radix4.cpp).
//
// ---- what this module deliberately does NOT do ----
// No `in_ready`/`out_ready` gating is exposed for anything beyond a
// single in-flight transaction (same "single in-flight" discipline as
// every other variable-latency module in this project, e.g. q4_scale.sv's
// own u_div_id) -- a caller must not assert `in_valid` again before the
// previous transaction's `out_valid` has been observed AND consumed (or
// must wait for `in_ready`, which will correctly stay low until then).
// No `real`/`shortreal`/DPI anywhere in this file.
module q8_scale_dual_radix4 (
	input	logic			clk,
	input	logic			rst_n,

	input	logic			in_valid,
	output	logic			in_ready,
	input	logic	[15:0]	amax_f16_in,

	output	logic			out_valid,
	input	logic			out_ready,
	output	logic	[15:0]	d_f16_out,
	output	logic	[31:0]	id_f32_out,
	output	logic			busy
);

	logic	[31:0]	amax_f32;

	assign amax_f32 = membrane_fp_pkg::f16_to_f32_bits(amax_f16_in);

	logic			d_in_ready, id_in_ready;
	logic			d_out_valid, id_out_valid;
	logic			d_busy, id_busy;
	logic	[31:0]	d_quotient, id_quotient;

	// ---- common input acceptance: both dividers see in_valid asserted
	// ONLY on the cycle both are simultaneously ready, so acceptance is
	// atomic by construction (no possibility of one instance starting a
	// transaction the other does not see). ----
	logic	div_in_valid;

	assign in_ready = d_in_ready && id_in_ready;
	assign div_in_valid = in_valid && in_ready;

	// ---- result rendezvous + holding: consume (and thus advance either
	// divider out of its own S_DONE) only once BOTH are done AND the
	// external consumer is ready -- each divider's own S_DONE state IS
	// the "hold this result stable" behavior, so no separate holding
	// register is needed in this wrapper. ----
	logic	both_done, consume;

	assign both_done = d_out_valid && id_out_valid;
	assign consume = both_done && out_ready;

	membrane_fp_divider_radix4 u_div_d (
		.clk(clk), .rst_n(rst_n),
		.in_valid(div_in_valid), .in_ready(d_in_ready),
		.numerator(amax_f32), .denominator(32'h42FE0000),
		.out_valid(d_out_valid), .out_ready(consume),
		.quotient(d_quotient), .busy(d_busy));

	membrane_fp_divider_radix4 u_div_id (
		.clk(clk), .rst_n(rst_n),
		.in_valid(div_in_valid), .in_ready(id_in_ready),
		.numerator(32'h42FE0000), .denominator(amax_f32),
		.out_valid(id_out_valid), .out_ready(consume),
		.quotient(id_quotient), .busy(id_busy));

	assign out_valid = both_done;
	assign busy = d_busy || id_busy;
	assign d_f16_out = membrane_fp_pkg::f32_to_f16_bits(d_quotient);

	// ---- explicit zero-mask, bit-exact with rtl/q8_scale.sv's own
	// `amax_f32 == 32'h0` check (see this file's header for why this is
	// intentionally NOT a broader magnitude-based zero test) -- captured
	// at acceptance, held until this transaction's own rendezvous
	// completes. Safe as a plain hold register (no reset needed) because
	// only one transaction is ever in flight (same technique as
	// q4_scale.sv's own d_f32_hold/zero_hold). ----
	logic	zero_hold;

	always_ff @(posedge clk)
		if (div_in_valid)
			zero_hold <= (amax_f32 == 32'h0);

	assign id_f32_out = zero_hold ? 32'h0 : id_quotient;

`ifndef SYNTHESIS
	// The structural claim from this file's header: for q8_scale's own
	// operand shape, u_div_d and u_div_id always complete on the same
	// cycle -- checked directly, not just argued. A violation here would
	// still be handled correctly by the rendezvous/holding logic above
	// (it is written to be correct in the general case), but it would
	// mean the "never observed to actually hold" claim in phase-b1.md is
	// wrong and needs correcting.
	always_ff @(posedge clk)
		if (rst_n)
			assert (d_out_valid == id_out_valid)
				else $error("q8_scale_dual_radix4: u_div_d/u_div_id completed on different cycles -- structural specialness-symmetry claim violated");
	always_ff @(posedge clk)
		if (rst_n)
			assert (d_in_ready == id_in_ready)
				else $error("q8_scale_dual_radix4: u_div_d/u_div_id readiness diverged -- atomic acceptance invariant violated");
`endif
endmodule
