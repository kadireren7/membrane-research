// EXP-FPGA-DIV-002 Phase B4, task item 10: isolated-synthesis stand-in
// for q8_scale_dual_radix4 (rtl/experimental/q8_div/q8_scale_dual_radix4.sv,
// itself NOT modified/overwritten by this file). Full-top synth_ecp5 has
// timed out for every scheduler top-level this experiment has ever
// synthesized (Phase A/B1/B2/B3), so this phase measures the scheduler
// logic in isolation instead: this stub REPLACES the real dual-radix4
// divider pair's own math with a trivial fixed-latency (16-cycle,
// matching the real module's own measured II) single-in-flight pulse
// generator of the SAME port shape and payload width, so a candidate's
// real, unmodified top-level scheduler file (membrane_quant_stream_top_
// q8_dual_radix4_b3_split.sv / _b4_r1.sv / _b4_r2.sv / _b4_r3.sv, NONE
// of which are modified by this file) can be synthesized around it
// quickly, in isolation from the large FP divider tree that is already
// measured separately and accurately by this experiment's own
// q8_scale_dual_radix4 reference synthesis (results/b4-synthesis.csv's
// own "ref-q8scale-dual-radix4" row).
//
// NOT real: d_f16_out/id_f32_out below are NOT the real reciprocal/
// quotient math -- they are a pass-through of the input amax value,
// disclosed here and in every "wrap-*" row of results/b4-synthesis.csv
// as ESTIMATED-class isolation, never claimed as a real divider area or
// timing measurement. What IS real: every other module instantiated by
// the candidate top-level (ingress queues, hold registers, shadow_hold/
// dec_hold, tag_pipe, the retirement mux, q4_pack/q4_unpack/
// q8_dequantize/q8_quantize_pack/q8_maxabs_reduce) is the candidate's own
// unmodified, already-tested source.
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

	localparam int STUB_LATENCY = 16;

	logic	[4:0]	cnt;
	logic			running;
	logic	[15:0]	amax_hold;

	assign in_ready = !running;
	assign busy = running;

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			running <= 1'b0;
			cnt <= '0;
			out_valid <= 1'b0;
			amax_hold <= '0;
		end else begin
			if (in_valid && in_ready) begin
				running <= 1'b1;
				cnt <= '0;
				amax_hold <= amax_f16_in;
			end else if (running && !out_valid) begin
				if (cnt == STUB_LATENCY - 1)
					out_valid <= 1'b1;
				else
					cnt <= cnt + 1'b1;
			end else if (out_valid && out_ready) begin
				out_valid <= 1'b0;
				running <= 1'b0;
			end
		end
	end

	assign d_f16_out = amax_hold;
	assign id_f32_out = {16'h0, amax_hold};

endmodule
