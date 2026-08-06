// EXP-FPGA-DIV-002 Phase B4, task item 10: isolated-synthesis stand-in
// for q4_scale (rtl/q4_scale.sv, itself NOT modified/overwritten by this
// file). Same purpose and same disclosure as
// q8_scale_dual_radix4_synth_stub.sv (this directory) -- see that file's
// own header for the full rationale. This stub replaces q4_scale's own
// real membrane_fp_scale_neg_pow2-based reciprocal math with a trivial
// fixed-latency (10-cycle) single-in-flight pulse generator of the SAME
// port shape and payload width, so a candidate's real, unmodified
// top-level scheduler file can be synthesized around it in isolation from
// the FP math this experiment already measures accurately elsewhere.
// d_f16_out/id_f32_out below are NOT real reciprocal math -- disclosed as
// ESTIMATED-class isolation in results/b4-synthesis.csv, never claimed as
// a real q4_scale area or timing measurement.
module q4_scale #(
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

	localparam int STUB_LATENCY = 10;

	logic	[3:0]	cnt;
	logic			running;
	logic	[31:0]	mx_hold;

	assign busy = running;

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			running <= 1'b0;
			cnt <= '0;
			valid_out <= 1'b0;
			mx_hold <= '0;
		end else begin
			valid_out <= 1'b0;
			if (valid_in && !running) begin
				running <= 1'b1;
				cnt <= '0;
				mx_hold <= mx_f32;
			end else if (running) begin
				if (cnt == STUB_LATENCY - 1) begin
					running <= 1'b0;
					valid_out <= 1'b1;
				end else begin
					cnt <= cnt + 1'b1;
				end
			end
		end
	end

	assign d_f16_out = mx_hold[15:0];
	assign id_f32_out = mx_hold;

endmodule
