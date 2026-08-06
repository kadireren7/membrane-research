// EXP-FPGA-DIV-001 Phase B4: synthesizable, multi-cycle, exact iterative
// FP32 divider -- same reference behavior as
// rtl/experimental/fp_div/fp32_div_iterative_exact.sv (Phase B2's radix-2
// divider) and, transitively, membrane_fp_divider.sv, but produces TWO
// quotient bits per cycle instead of one, halving the main iteration count
// (13 cycles instead of 26 for the same 26-bit fixed-point quotient).
//
// ---- what "radix-4" means here, precisely (task explicitly asks for
// honest naming if the module isn't really radix-4) ----
// By definition, a radix-4 divider produces a quotient digit from the set
// {0,1,2,3} (2 bits) per iteration. This module DOES that -- every ITER
// cycle advances the quotient by exactly 2 bits. The INTERNAL
// implementation technique, though, is deliberately the simplest one that
// is trivially provable correct by construction: each ITER cycle runs TWO
// of Phase B2's own radix-2 restoring-division steps (shift, compare,
// conditional-subtract) chained COMBINATIONALLY, and registers the result
// only once. This is a standard, textbook way to build a radix-4 restoring
// divider (as opposed to a direct parallel 4-way magnitude comparison
// against {0, D, 2D, 3D}, which is the other standard construction, and
// which typically underlies real SRT radix-4 dividers using a redundant
// digit set and carry-save arithmetic for speed). This module is NOT an
// SRT divider -- no redundant digit set, no carry-save arithmetic -- it is
// a non-redundant, restoring radix-4 divider built from two chained
// radix-2 steps. The real, disclosed trade-off of this choice: the
// combinational path per ITER cycle is roughly TWICE as deep as Phase B2's
// per-cycle path (two chained compare-and-subtract steps instead of one),
// which is exactly the well-known radix-4-vs-radix-2 trade (fewer cycles,
// longer combinational path per cycle) -- no real Fmax/timing-closure
// claim is made anywhere in this file (no vendor place-and-route tool
// exists in this environment, same disclosure as every prior phase).
//
// ---- why this construction is bit-exact by construction, not just by
// empirical testing (the empirical testing, >=2M cases, still happens --
// see rtl/tb/tb_fp32_div_iterative_radix4_exact.cpp -- this is belt AND
// suspenders, not either/or) ----
// Given rem_reg/quot_reg after N radix-2 steps (Phase B2's own algorithm),
// running two MORE radix-2 steps produces EXACTLY the same rem_reg/
// quot_reg as running two separate radix-2 cycles would, because the
// second step's inputs (the shifted rem_reg, the same denom_full) are
// computed purely combinationally from the first step's own output within
// the same cycle -- there is no approximation, reordering, or redundant/
// carry-save digit anywhere in the chain. After 13 such paired cycles
// (26 individual radix-2 steps total), quot_reg/rem_reg are the same
// values Phase B2's own module reaches after its 26 individual cycles,
// which are themselves bit-identical to membrane_fp_divider.sv's
// quot64/rem64 (argued in that module's own header, not re-argued here).
// The FIRST step of the FIRST ITER cycle keeps Phase B2's own special
// "no shift" comparison (it compares the raw latched {1,mant_a} with no
// shift, exactly matching membrane_fp_divider.sv's num64/den64
// relationship at the top 24 bits) -- every other step, in every other
// position, uses the standard "shift the running remainder left by one,
// shift in a 0" pattern (the dividend's lower bits are all zero by
// construction, same reasoning as Phase B2's own header).
//
// ---- everything else (special-case decode, is_special priority chain,
// rounding tail: normalize/guard/round/sticky/round-to-nearest-even,
// flush-to-zero/flush-to-infinity exponent bounds, FSM shape IDLE -> ITER
// -> ROUND -> [DRAIN] -> DONE -> IDLE, single-in-flight discipline,
// asynchronous unconditional reset, EARLY_OUT_SPECIAL/OUT_REG_DEPTH
// parameters) is copied VERBATIM from fp32_div_iterative_exact.sv -- see
// that file's own header for the full derivation, not repeated here
// except where the iteration itself differs. The rounding tail operates
// purely on the FINAL quot_reg/rem_reg values, which are unchanged by
// switching from 26 single steps to 13 paired steps -- so it needs no
// changes at all beyond being copied. ----
module fp32_div_iterative_radix4_exact #(
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
	// Two radix-2 steps per ITER cycle -- MANT_ITER_WIDTH must be even
	// (26 is) for this to divide evenly; enforced below, simulation-only.
	localparam int PAIR_ITER_WIDTH = MANT_ITER_WIDTH / 2;

	localparam logic [2:0] S_IDLE  = 3'd0;
	localparam logic [2:0] S_ITER  = 3'd1;
	localparam logic [2:0] S_ROUND = 3'd2;
	localparam logic [2:0] S_DRAIN = 3'd3;
	localparam logic [2:0] S_DONE  = 3'd4;
	logic	[2:0]	state;

`ifndef SYNTHESIS
	initial begin
		if (MANT_ITER_WIDTH != 26)
			$error("fp32_div_iterative_radix4_exact: MANT_ITER_WIDTH must be 26 (the only value derived/verified for FP32's 24-bit significand)");
		if (GUARD_BITS != 2)
			$error("fp32_div_iterative_radix4_exact: GUARD_BITS must be 2 (the only value matching membrane_fp_divider.sv's rounding tail)");
		if ((MANT_ITER_WIDTH % 2) != 0)
			$error("fp32_div_iterative_radix4_exact: MANT_ITER_WIDTH must be even (two radix-2 steps per ITER cycle)");
	end
`endif

	// ---- combinational decode of the operands being accepted THIS
	// cycle -- byte-for-byte identical to fp32_div_iterative_exact.sv. ----
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

	logic			result_sign_lat;
	logic	[7:0]	exp_a_lat, exp_b_lat;
	logic			is_special_lat;
	logic	[31:0]	special_result_lat;

	// ---- iterative restoring-division datapath: PAIR_ITER_WIDTH cycles,
	// two radix-2 steps chained combinationally per cycle. ----
	logic	[4:0]	iter_pair_cnt;	// counts down PAIR_ITER_WIDTH-1 .. 0
	logic	[23:0]	rem_reg;
	logic	[25:0]	quot_reg;
	logic	[23:0]	denom_full;

	// Step 1: same "no shift on the very first step of the whole
	// computation" special case as fp32_div_iterative_exact.sv -- every
	// other position uses the standard left-shift-in-a-0.
	logic	[24:0]	step1_cmp;
	logic			step1_ge;
	logic	[24:0]	step1_rem;
	// Step 2: always shifted (it is, by construction, never the very
	// first step of the whole computation).
	logic	[24:0]	step2_cmp;
	logic			step2_ge;
	logic	[24:0]	step2_rem;

	always_comb begin
		if (iter_pair_cnt == 5'(PAIR_ITER_WIDTH - 1))
			step1_cmp = {1'b0, rem_reg};
		else
			step1_cmp = {rem_reg, 1'b0};
		step1_ge = step1_cmp >= {1'b0, denom_full};
		step1_rem = step1_ge ? (step1_cmp - {1'b0, denom_full}) : step1_cmp;

		step2_cmp = {step1_rem[23:0], 1'b0};
		step2_ge = step2_cmp >= {1'b0, denom_full};
		step2_rem = step2_ge ? (step2_cmp - {1'b0, denom_full}) : step2_cmp;
	end

	// ---- rounding-tail combinational logic -- byte-for-byte identical to
	// fp32_div_iterative_exact.sv, operates only on the final quot_reg/
	// rem_reg values (unaffected by single-step vs. paired-step
	// iteration). ----
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

	logic	[31:0]	result_hold;
	logic	[15:0]	drain_cnt;

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			state <= S_IDLE;
			iter_pair_cnt <= '0;
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
					iter_pair_cnt <= 5'(PAIR_ITER_WIDTH - 1);
					if (EARLY_OUT_SPECIAL && is_special_d)
						state <= S_ROUND;
					else
						state <= S_ITER;
				end
			end
			S_ITER: begin
				rem_reg <= step2_rem[23:0];
				quot_reg <= {quot_reg[23:0], step1_ge, step2_ge};
				if (iter_pair_cnt == 5'd0)
					state <= S_ROUND;
				else
					iter_pair_cnt <= iter_pair_cnt - 5'd1;
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
