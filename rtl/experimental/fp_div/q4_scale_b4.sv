// EXP-FPGA-DIV-001 Phase B4 variant of rtl/q4_scale.sv: byte-for-byte
// identical to rtl/experimental/fp_div/q4_scale_b2.sv (Phase B2) EXCEPT
// `u_div_id` (`id = 1/d`) is `fp32_div_iterative_radix4_exact` instead of
// `fp32_div_iterative_exact` -- the ONE change this phase makes. `u_div_d`
// (`d = mx/-8.0f`, Phase B1's constant power-of-two shortcut,
// `fp32_scale_neg_pow2`) is unchanged, exactly as B2 left it. This module
// deliberately does NOT use Phase B3's `membrane_completion_reorder` --
// B3 was rejected as an architecture (see decision.md) precisely because
// its area cost was disproportionate to its throughput gain; this phase's
// hypothesis is that speeding up the divider itself, with NO new
// scheduling complexity, is the better lever.
//
// Everything else in this file's own reasoning (single hold register
// replacing the fixed-depth zero_pipe/d_f32_pipe delay-matching arrays,
// correct because only one transaction is ever in flight; the
// `` `ifndef SYNTHESIS `` single-in-flight assertion) is unchanged from
// q4_scale_b2.sv -- see that file's own header for the full reasoning, not
// repeated here.
module q4_scale_b4 #(
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

	fp32_scale_neg_pow2 #(.SHIFT(3), .DELAY(DIV_DELAY)) u_div_d (
		.clk(clk), .rst_n(rst_n), .valid_in(valid_in), .a_in(mx_f32),
		.valid_out(d_valid), .result_out(d_f32_raw));

	assign d_is_zero = (d_f32_raw == 32'h0);

	// Phase B4: `id = 1/d`, now the radix-4 exact iterative divider (two
	// quotient bits/cycle, ~half the cycle count of Phase B2's radix-2
	// divider for the same operation) instead of
	// fp32_div_iterative_exact. out_ready tied high, same reasoning as
	// q4_scale_b2.sv: nothing downstream ever applies backpressure.
	fp32_div_iterative_radix4_exact u_div_id (
		.clk(clk), .rst_n(rst_n),
		.in_valid(d_valid), .in_ready(id_ready),
		.numerator(32'h3F800000), .denominator(d_f32_raw),
		.out_valid(id_valid), .out_ready(1'b1),
		.quotient(id_f32_raw), .busy(busy));

`ifndef SYNTHESIS
	always_ff @(posedge clk)
		if (rst_n && d_valid)
			assert (id_ready)
				else $error("q4_scale_b4: u_div_id not ready when d_valid pulsed -- single in-flight discipline violated");
`endif

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
