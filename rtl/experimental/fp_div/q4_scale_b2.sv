// EXP-FPGA-DIV-001 Phase B2 variant of rtl/q4_scale.sv: builds on Phase
// B1 (rtl/experimental/fp_div/q4_scale_b1.sv, whose `u_div_d` --
// `d = mx/-8.0f`, a constant power-of-two divisor -- stays exactly the
// same fp32_scale_neg_pow2.sv it already used, unmodified) and replaces
// `u_div_id` (`id = 1/d`, the one remaining VARIABLE-divisor operation)
// with the new multi-cycle, iterative, exact
// rtl/experimental/fp_div/fp32_div_iterative_exact.sv instead of the
// general single-cycle combinational membrane_fp_divider.sv.
//
// This is the one functional difference from q4_scale_b1.sv that
// matters architecturally: `u_div_id`'s latency is no longer a fixed,
// compile-time DIV_DELAY -- it varies (measured: 3-29 cycles, see
// experiments/EXP-FPGA-DIV-001/results/b2-differential.json) depending
// on whether `d` hits the iterative divider's EARLY_OUT_SPECIAL path
// (NaN/Inf/zero) or the full 26-cycle iteration. The fixed-depth
// zero_pipe/d_f32_pipe delay-matching arrays q4_scale.sv/q4_scale_b1.sv
// use (correct only for a compile-time-constant DIV_DELAY) are replaced
// here by a single HOLD register for d_f32_raw/d_is_zero, latched once
// when u_div_d's result arrives and read back only when u_div_id
// finally completes -- correct precisely because only one transaction
// is ever in flight through this module at a time (the same "single
// in-flight" property fp32_div_iterative_exact.sv itself guarantees,
// and which membrane_quant_stream_top_b2.sv's own issue-gating logic
// enforces for the whole Q4_0 encode chain -- see that file's header).
//
// `valid_in` must not be reasserted while `busy` is high -- this module
// has no internal input queue (matching the "single in-flight" area-
// first design point). membrane_quant_stream_top_b2.sv's own issue
// gating (q4enc_inflight) guarantees this by construction; a bare
// standalone instantiation of this module is responsible for the same
// discipline itself.
module q4_scale_b2 #(
	parameter int DIV_DELAY = 1
) (
	input	logic			clk,
	input	logic			rst_n,
	input	logic			valid_in,
	input	logic	[31:0]	mx_f32,
	output	logic			valid_out,
	output	logic	[15:0]	d_f16_out,
	output	logic	[31:0]	id_f32_out,
	output	logic			busy
);

	logic	[31:0]	d_f32_raw;
	logic	[31:0]	id_f32_raw;
	logic			d_valid, id_valid, id_ready;
	logic			d_is_zero;

	// Unchanged from Phase B1: mx/-8.0f is an exact power-of-two
	// division, still handled by the specialized, `/`-operator-free
	// fp32_scale_neg_pow2.sv -- see q4_scale_b1.sv's own header for why
	// this stays a fixed DIV_DELAY=1 pipe (untouched by Phase B2).
	fp32_scale_neg_pow2 #(.SHIFT(3), .DELAY(DIV_DELAY)) u_div_d (
		.clk(clk), .rst_n(rst_n), .valid_in(valid_in), .a_in(mx_f32),
		.valid_out(d_valid), .result_out(d_f32_raw));

	assign d_is_zero = (d_f32_raw == 32'h0);

	// Phase B2: `id = 1/d`, the one remaining variable-divisor
	// operation, now the new multi-cycle iterative exact divider
	// instead of membrane_fp_divider.sv. out_ready is tied high: this
	// module (and everything downstream of it in the Q4_0 encode chain,
	// q4_pack.sv) never applies its own backpressure, so the divider's
	// result is always consumed the instant it is ready.
	fp32_div_iterative_exact u_div_id (
		.clk(clk), .rst_n(rst_n),
		.in_valid(d_valid), .in_ready(id_ready),
		.numerator(32'h3F800000), .denominator(d_f32_raw),
		.out_valid(id_valid), .out_ready(1'b1),
		.quotient(id_f32_raw), .busy(busy));

`ifndef SYNTHESIS
	// d_valid only ever pulses once per Q4_0 encode transaction, and the
	// top-level's issue gating never starts a new one before the
	// previous fully retires -- so u_div_id must always be ready
	// (in_ready) whenever d_valid actually fires. A violation here would
	// mean either that gating discipline broke, or this module was
	// instantiated somewhere without it.
	always_ff @(posedge clk)
		if (rst_n && d_valid)
			assert (id_ready)
				else $error("q4_scale_b2: u_div_id not ready when d_valid pulsed -- single in-flight discipline violated");
`endif

	// Single hold register replaces q4_scale.sv/q4_scale_b1.sv's
	// fixed-depth zero_pipe/d_f32_pipe delay-matching arrays -- see this
	// file's header for why a fixed-depth pipe cannot correctly delay-
	// match a variable-latency divider, and why a hold register is
	// correct given the single-in-flight guarantee.
	logic	[31:0]	d_f32_hold;
	logic			zero_hold;

	always_ff @(posedge clk) begin
		if (d_valid) begin
			d_f32_hold <= d_f32_raw;
			zero_hold <= d_is_zero;
		end
	end

	assign valid_out = id_valid;
	assign d_f16_out = membrane_fp_pkg::f32_to_f16_bits(d_f32_hold);
	assign id_f32_out = zero_hold ? 32'h0 : id_f32_raw;
endmodule
