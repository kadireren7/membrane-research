// EXP-FPGA-DIV-001 Phase B1 variant of rtl/q4_scale.sv.
//
// Identical to rtl/q4_scale.sv in every respect EXCEPT one: `u_div_d`
// (which computes `d = mx / -8.0f`, a division by a compile-time
// constant that is an exact power of two) is replaced by
// fp32_scale_neg_pow2.sv (SHIFT=3) instead of the general-purpose
// membrane_fp_divider.sv. Differential-tested bit-exact against the
// real membrane_fp_divider RTL for this exact operation -- see
// rtl/tb/tb_fp32_scale_neg_pow2.cpp and
// experiments/EXP-FPGA-DIV-001/phase-b1.md.
//
// `u_div_id` (`id = d!=0 ? 1/d : 0`, a VARIABLE divisor) is left
// completely unchanged, still the general membrane_fp_divider -- this
// experiment only targets the one constant-divisor call site, per
// EXP-FPGA-DIV-001 Phase B1's scope. The other three
// membrane_fp_divider instances in this datapath (this module's own
// `u_div_id`, and both of q8_scale.sv's instances) are untouched.
//
// This file exists so rtl/q4_scale.sv's own production behavior is
// never silently changed -- swapping between baseline and this variant
// is a matter of which top-level module a build instantiates (see
// membrane_quant_stream_top_b1.sv), not an edit to any file already in
// production use.
module q4_scale_b1 #(
	parameter int DIV_DELAY = 1
) (
	input	logic			clk,
	input	logic			rst_n,
	input	logic			valid_in,
	input	logic	[31:0]	mx_f32,
	output	logic			valid_out,
	output	logic	[15:0]	d_f16_out,
	output	logic	[31:0]	id_f32_out
);

	logic	[31:0]	d_f32_raw;
	logic	[31:0]	id_f32_raw;
	logic			d_valid, id_valid;
	logic	[0:0]	zero_pipe	[0:DIV_DELAY - 1];
	logic	[31:0]	d_f32_pipe	[0:DIV_DELAY - 1];
	logic			d_is_zero;

	// B1: fp32_scale_neg_pow2 (SHIFT=3) replaces membrane_fp_divider
	// for this one constant-divisor operation. No `b_in` port -- the
	// divisor -8.0 is baked in by SHIFT, not supplied at runtime (see
	// that module's own header comment).
	fp32_scale_neg_pow2 #(.SHIFT(3), .DELAY(DIV_DELAY)) u_div_d (
		.clk(clk), .rst_n(rst_n), .valid_in(valid_in), .a_in(mx_f32),
		.valid_out(d_valid), .result_out(d_f32_raw));

	assign d_is_zero = (d_f32_raw == 32'h0);

	// Unchanged: variable-divisor `id = 1/d` still uses the general
	// divider. Phase B1 does not touch this instance.
	membrane_fp_divider #(.DELAY(DIV_DELAY)) u_div_id (
		.clk(clk), .rst_n(rst_n), .valid_in(d_valid), .a_in(32'h3F800000),
		.b_in(d_f32_raw), .valid_out(id_valid), .result_out(id_f32_raw));

	// Same delay-matching requirement as rtl/q4_scale.sv -- see that
	// file's header comment for why both the zero-gating flag and
	// d_f32_raw itself must be delay-matched through BOTH dividers'
	// combined latency.
	always_ff @(posedge clk) begin
		zero_pipe[0] <= d_is_zero;
		d_f32_pipe[0] <= d_f32_raw;
		for (int k = DIV_DELAY - 1; k > 0; k--) begin
			zero_pipe[k] <= zero_pipe[k - 1];
			d_f32_pipe[k] <= d_f32_pipe[k - 1];
		end
	end

	assign valid_out = id_valid;
	assign d_f16_out = membrane_fp_pkg::f32_to_f16_bits(d_f32_pipe[DIV_DELAY - 1]);
	assign id_f32_out = zero_pipe[DIV_DELAY - 1] ? 32'h0 : id_f32_raw;
endmodule
