// EXP-FPGA-DIV-001 Phase B2: synthesizable, multi-cycle, exact iterative
// FP32 divider -- replaces the single-cycle wide combinational `/`
// operator inside membrane_fp_divider.sv (see that file's header) with a
// radix-2 restoring-division iteration over the 24-bit significands,
// producing the exact SAME 26-bit fixed-point quotient
// (quot64 = ((1<<25)*{1,mant_a}) / {1,mant_b}, in membrane_fp_divider.sv's
// own notation) one bit per cycle instead of via Verilog's combinational
// `/`/`%` operators. Restoring division is not an approximation that
// then needs correcting -- it IS integer division, computed a different
// (iterative, one-bit-per-cycle) way; the quotient/remainder it produces
// are mathematically identical to Verilog's `/`/`%` on the same
// operands, for every input, by the definition of floor division (this
// is argued here, and separately checked empirically for 2M+ cases, see
// rtl/tb/tb_fp32_div_iterative_exact.cpp and
// experiments/EXP-FPGA-DIV-001/phase-b2.md). Every special-case
// (NaN/Inf/zero) branch, the rounding tail (guard/round/sticky,
// round-to-nearest-even), and the disclosed non-IEEE simplifications
// (subnormal operands treated as if normal -- hidden bit forced to 1
// regardless of the exponent field, flush-to-zero instead of gradual
// underflow) are copied VERBATIM in structure from
// membrane_fp_divider.sv's own logic -- see that file for the
// derivation, not re-derived here.
//
// This module is general-purpose (numerator/denominator are both
// runtime inputs, same as membrane_fp_divider.sv), even though
// EXP-FPGA-DIV-001 Phase B2 only actually calls it with a constant
// numerator (1.0f, Q4_0's `id = 1/d`) -- kept general per this phase's
// own task spec ("numerator burada 1.0f olabilir fakat modulu mumkunse
// parametric/general tutmak").
//
// ---- architecture: IDLE -> ITER -> ROUND -> [DRAIN] -> DONE -> IDLE ----
//   IDLE:  in_ready=1. On accept (in_valid && in_ready), every operand
//          field and special-case flag is decoded combinationally
//          (cheap: equality/OR/XOR checks on the raw bits, not a divide)
//          and latched. If EARLY_OUT_SPECIAL and the operation is a
//          special case (NaN/Inf/zero, exactly membrane_fp_divider.sv's
//          own is_special conditions), the FSM skips straight to ROUND
//          next cycle -- none of those cases touch the significand
//          divider in membrane_fp_divider.sv either, they're muxed in
//          ahead of it. Otherwise (or if EARLY_OUT_SPECIAL=0) it enters
//          ITER; the iteration hardware itself is always well-defined
//          (the constructed divisor {1,mant_b} is never actually zero,
//          same hidden-bit-forced-1 property membrane_fp_divider.sv's
//          own general path relies on), so EARLY_OUT_SPECIAL=0 is a
//          genuinely safe (if wasteful) configuration, not a hazard.
//   ITER:  MANT_ITER_WIDTH cycles (default/only-verified value 26), one
//          radix-2 restoring-division step per cycle, producing one
//          quotient bit per cycle, MSB first: cycle 0 compares the raw
//          24-bit {1,mant_a} against {1,mant_b} with no shift (this is
//          exactly membrane_fp_divider.sv's num64/den64 relationship at
//          the top 24 significant dividend bits); every subsequent
//          cycle left-shifts the running remainder by one bit (the
//          dividend's lower 25 bits are all zero by construction, so
//          "shifting in the next dividend bit" always shifts in a 0)
//          before comparing/conditionally-subtracting again. After 26
//          such steps, quot_reg holds bit-identical value to
//          membrane_fp_divider.sv's quot64[25:0], and rem_reg holds a
//          value that is nonzero if and only if rem64 is nonzero (used
//          for the sticky bit) -- both by the definition of restoring
//          division, not by approximation.
//   ROUND: one cycle: normalize (the one possible left-shift), round
//          (guard/round/sticky, round-to-nearest-even), handle mantissa-
//          overflow-from-rounding and the flush-to-zero/flush-to-
//          infinity exponent bounds, and mux in the special-case result
//          if this transaction took the early-out path -- this
//          combinational tail is structurally identical to
//          membrane_fp_divider.sv's own, fed by quot_reg/rem_reg instead
//          of a combinational `/`/`%`.
//   DRAIN: OUT_REG_DEPTH extra cycles of pure output-holding delay
//          (0 by default -- see OUT_REG_DEPTH below), implemented as a
//          down-counter rather than a literal shift-register chain
//          since the held value never changes during drain.
//   DONE:  out_valid=1, quotient held stable, waits for out_ready before
//          returning to IDLE.
//
// Only one transaction is ever in flight (in_ready is only ever
// asserted from IDLE) -- this is deliberately the "area-first, single
// in-flight" first design point EXP-FPGA-DIV-001 Phase B2's task spec
// asks for, not a pipelined (II=1) divider; initiation interval equals
// measured latency. See experiments/EXP-FPGA-DIV-001/phase-b2.md for
// the measured throughput cost this creates at the Q4_0 integration
// point.
//
// ---- reset-mid-computation ----
// `rst_n` is asynchronous and unconditional: it forces the FSM back to
// IDLE and drops out_valid/busy regardless of what state a computation
// was in, safely discarding any half-finished iteration -- there is no
// external side effect to discard, the only state is this module's own
// registers.
//
// ---- parameters ----
// MANT_ITER_WIDTH, GUARD_BITS: exposed as parameters per this phase's
// task spec, but only their default values (26, 2) are verified exact
// against membrane_fp_divider.sv -- they are properties of FP32's fixed
// 24-bit significand width and membrane_fp_divider.sv's own specific
// 2-guard/round-bit convention, not free knobs. Changing them would
// require re-deriving both this module's iteration count AND its
// rounding tail, not attempted here -- a simulation-only elaboration
// check rejects any other value (guarded out of synthesis, see below)
// rather than silently building a subtly wrong divider.
// EARLY_OUT_SPECIAL: genuinely optional (0 or 1) -- see IDLE above.
// OUT_REG_DEPTH: genuinely optional extra output-holding delay cycles
// after ROUND (0 default), conceptually the same DELAY convention as
// membrane_fp_divider.sv/valid_delay_line.sv, implemented as a
// down-counter (see DRAIN above).
module fp32_div_iterative_exact #(
	parameter int MANT_ITER_WIDTH = 26,
	parameter int GUARD_BITS = 2,
	parameter bit EARLY_OUT_SPECIAL = 1'b1,
	parameter int OUT_REG_DEPTH = 0
) (
	input	logic			clk,
	input	logic			rst_n,

	input	logic			in_valid,
	output	logic			in_ready,
	input	logic	[31:0]	numerator,
	input	logic	[31:0]	denominator,

	output	logic			out_valid,
	input	logic			out_ready,
	output	logic	[31:0]	quotient,
	output	logic			busy
);

	localparam logic [31:0] CANON_NAN = 32'hFFC00000;

	// FSM state encoding via localparam (not `typedef enum`) -- matches
	// this repository's own established house style for yosys 0.33
	// compatibility (see membrane_quant_stream_top.sv's MODE_* constants
	// and this experiment's own phase-5.3 findings on yosys 0.33
	// frontend fragility, disclosed in
	// docs/phase5-synthesizable-fpga.md).
	localparam logic [2:0] S_IDLE  = 3'd0;
	localparam logic [2:0] S_ITER  = 3'd1;
	localparam logic [2:0] S_ROUND = 3'd2;
	localparam logic [2:0] S_DRAIN = 3'd3;
	localparam logic [2:0] S_DONE  = 3'd4;
	logic	[2:0]	state;

`ifndef SYNTHESIS
	initial begin
		if (MANT_ITER_WIDTH != 26)
			$error("fp32_div_iterative_exact: MANT_ITER_WIDTH must be 26 (the only value derived/verified for FP32's 24-bit significand)");
		if (GUARD_BITS != 2)
			$error("fp32_div_iterative_exact: GUARD_BITS must be 2 (the only value matching membrane_fp_divider.sv's rounding tail)");
	end
`endif

	// ---- combinational decode of the operands being accepted THIS
	// cycle (valid whenever in_valid, latched into the *_lat registers
	// only when in_valid && in_ready) ----
	logic			sign_a_d, sign_b_d, result_sign_d;
	logic	[7:0]	exp_a_d, exp_b_d;
	logic	[22:0]	mant_a_d, mant_b_d;
	logic			a_is_nan_d, b_is_nan_d, a_is_inf_d, b_is_inf_d;
	logic			a_is_zero_d, b_is_zero_d;
	logic			is_special_d;
	logic	[31:0]	special_result_d;

	assign sign_a_d = numerator[31];
	assign exp_a_d = numerator[30:23];
	assign mant_a_d = numerator[22:0];
	assign sign_b_d = denominator[31];
	assign exp_b_d = denominator[30:23];
	assign mant_b_d = denominator[22:0];
	assign result_sign_d = sign_a_d ^ sign_b_d;

	assign a_is_nan_d = (exp_a_d == 8'hFF) && (mant_a_d != 23'h0);
	assign b_is_nan_d = (exp_b_d == 8'hFF) && (mant_b_d != 23'h0);
	assign a_is_inf_d = (exp_a_d == 8'hFF) && (mant_a_d == 23'h0);
	assign b_is_inf_d = (exp_b_d == 8'hFF) && (mant_b_d == 23'h0);
	assign a_is_zero_d = (exp_a_d == 8'h00) && (mant_a_d == 23'h0);
	assign b_is_zero_d = (exp_b_d == 8'h00) && (mant_b_d == 23'h0);

	// Same priority chain, same fixed constants, as
	// membrane_fp_divider.sv's own result_comb mux -- copied verbatim so
	// bit-exactness is a matter of visual inspection, not re-derivation.
	always_comb begin
		if (a_is_nan_d) begin
			is_special_d = 1'b1;
			special_result_d = numerator | 32'h00400000;
		end else if (b_is_nan_d) begin
			is_special_d = 1'b1;
			special_result_d = denominator | 32'h00400000;
		end else if (a_is_inf_d && b_is_inf_d) begin
			is_special_d = 1'b1;
			special_result_d = CANON_NAN;
		end else if (a_is_zero_d && b_is_zero_d) begin
			is_special_d = 1'b1;
			special_result_d = CANON_NAN;
		end else if (a_is_inf_d) begin
			is_special_d = 1'b1;
			special_result_d = {result_sign_d, 8'hFF, 23'h0};
		end else if (b_is_inf_d) begin
			is_special_d = 1'b1;
			special_result_d = {result_sign_d, 31'h0};
		end else if (a_is_zero_d) begin
			is_special_d = 1'b1;
			special_result_d = {result_sign_d, 31'h0};
		end else if (b_is_zero_d) begin
			is_special_d = 1'b1;
			special_result_d = {result_sign_d, 8'hFF, 23'h0};
		end else begin
			is_special_d = 1'b0;
			special_result_d = 32'h0;
		end
	end

	// ---- latched operand fields (captured at accept time, stable for
	// the whole computation since only one transaction is ever in
	// flight) ----
	logic			result_sign_lat;
	logic	[7:0]	exp_a_lat, exp_b_lat;
	logic			is_special_lat;
	logic	[31:0]	special_result_lat;

	// ---- iterative restoring-division datapath ----
	logic	[4:0]	iter_cnt;	// counts down MANT_ITER_WIDTH-1 .. 0
	logic	[23:0]	rem_reg;	// current remainder, always < denom_full, fits in 24 bits between iterations
	logic	[25:0]	quot_reg;	// MANT_ITER_WIDTH accumulated quotient bits, MSB-first
	logic	[23:0]	denom_full;	// {1, mant_b}, latched divisor significand -- never zero

	logic	[24:0]	iter_cmp_val;	// this cycle's 25-bit value to compare against denom_full
	logic			iter_ge;
	logic	[24:0]	iter_rem_next;

	// First iteration (iter_cnt == MANT_ITER_WIDTH-1) compares the raw
	// latched {1,mant_a} (already sitting in rem_reg from the IDLE->ITER
	// transition, see the FSM below) with no shift; every subsequent
	// iteration shifts the previous remainder left by one bit before
	// comparing (the dividend's lower 25 bits, all zero by construction
	// -- see this file's header -- are what's "shifted in").
	always_comb begin
		if (iter_cnt == 5'(MANT_ITER_WIDTH - 1))
			iter_cmp_val = {1'b0, rem_reg};
		else
			iter_cmp_val = {rem_reg, 1'b0};
		iter_ge = iter_cmp_val >= {1'b0, denom_full};
		iter_rem_next = iter_ge ? (iter_cmp_val - {1'b0, denom_full}) : iter_cmp_val;
	end

	// ---- rounding-tail combinational logic (structurally identical to
	// membrane_fp_divider.sv, fed by quot_reg/rem_reg instead of a
	// combinational `/`/`%`) ----
	logic			norm_shift;
	logic	[25:0]	q_norm;
	logic			sticky;
	logic	[22:0]	mant_result_trunc;
	logic			round_up;
	logic	[23:0]	mant_rounded;
	logic			mant_overflow;
	int				exp_a_s, exp_b_s, exp_result;
	logic	[31:0]	general_result;
	logic	[31:0]	final_result_comb;

	assign norm_shift = !quot_reg[25];
	assign q_norm = norm_shift ? {quot_reg[24:0], 1'b0} : quot_reg;
	assign sticky = (rem_reg != 24'h0);
	assign mant_result_trunc = q_norm[24:2];
	assign round_up = q_norm[1] && (q_norm[0] || sticky || mant_result_trunc[0]);
	assign mant_rounded = round_up
		? ({1'b0, mant_result_trunc} + 24'h1) : {1'b0, mant_result_trunc};
	assign mant_overflow = mant_rounded[23];

	always_comb begin
		exp_a_s = exp_a_lat;
		exp_b_s = exp_b_lat;
		exp_result = exp_a_s - exp_b_s + 127 - (norm_shift ? 1 : 0)
			+ (mant_overflow ? 1 : 0);
	end

	always_comb begin
		if (exp_result >= 255)
			general_result = {result_sign_lat, 8'hFF, 23'h0};
		else if (exp_result <= 0)
			general_result = {result_sign_lat, 31'h0};
		else
			general_result = {result_sign_lat, 8'(exp_result),
				mant_overflow ? 23'h0 : mant_rounded[22:0]};
	end

	assign final_result_comb = is_special_lat ? special_result_lat : general_result;

	// ---- output hold register + optional drain delay ----
	logic	[31:0]	result_hold;
	logic	[15:0]	drain_cnt;

	// ---- main FSM ----
	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			state <= S_IDLE;
			iter_cnt <= '0;
			rem_reg <= '0;
			quot_reg <= '0;
			denom_full <= '0;
			result_sign_lat <= 1'b0;
			exp_a_lat <= '0;
			exp_b_lat <= '0;
			is_special_lat <= 1'b0;
			special_result_lat <= '0;
			result_hold <= '0;
			drain_cnt <= '0;
		end else begin
			case (state)
			S_IDLE: begin
				if (in_valid && in_ready) begin
					result_sign_lat <= result_sign_d;
					exp_a_lat <= exp_a_d;
					exp_b_lat <= exp_b_d;
					is_special_lat <= is_special_d;
					special_result_lat <= special_result_d;
					denom_full <= {1'b1, mant_b_d};
					rem_reg <= {1'b1, mant_a_d};
					quot_reg <= '0;
					iter_cnt <= 5'(MANT_ITER_WIDTH - 1);
					if (EARLY_OUT_SPECIAL && is_special_d)
						state <= S_ROUND;
					else
						state <= S_ITER;
				end
			end
			S_ITER: begin
				rem_reg <= iter_rem_next[23:0];
				quot_reg <= {quot_reg[24:0], iter_ge};
				if (iter_cnt == 5'd0)
					state <= S_ROUND;
				else
					iter_cnt <= iter_cnt - 5'd1;
			end
			S_ROUND: begin
				result_hold <= final_result_comb;
				if (OUT_REG_DEPTH > 0) begin
					drain_cnt <= 16'(OUT_REG_DEPTH - 1);
					state <= S_DRAIN;
				end else begin
					state <= S_DONE;
				end
			end
			S_DRAIN: begin
				if (drain_cnt == 16'd0)
					state <= S_DONE;
				else
					drain_cnt <= drain_cnt - 16'd1;
			end
			S_DONE: begin
				if (out_ready)
					state <= S_IDLE;
			end
			default: state <= S_IDLE;
			endcase
		end
	end

	assign in_ready = (state == S_IDLE);
	assign busy = (state != S_IDLE);
	assign out_valid = (state == S_DONE);
	assign quotient = result_hold;
endmodule
