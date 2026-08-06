// EXP-FPGA-DIV-001 Phase B1: this file is rtl/membrane_quant_stream_top.sv
// verbatim, renamed to membrane_quant_stream_top_b1, with exactly one
// instantiation changed (u_q4_scale: q4_scale -> q4_scale_b1, see that
// line's own comment below). Every other line, including the rest of
// this header, is unmodified from the production file. This exists so
// the same class of testbench (see rtl/tb/tb_top_verilator.cpp) can
// build and drive EITHER the production top-level or this Phase B1
// variant, and so rtl/membrane_quant_stream_top.sv itself is never
// touched by this experiment -- see
// experiments/EXP-FPGA-DIV-001/phase-b1.md.
//
// ---- original file header follows, unmodified ----
//
// Phase 5.3: membrane_quant_stream_top -- the single top-level streaming
// pipeline for MEMBRANE's FPGA quantization datapath. One valid/ready
// input stream carrying {mode, transaction id, 512-bit data} feeds four
// mode-selected sub-pipelines (Q8 encode, Q8 decode, Q4 encode, Q4
// decode), each built entirely from this phase's synthesizable, bit-
// exact building blocks (membrane_fp_divider/multiplier/adder,
// membrane_fp_pkg's F16<->F32 and int<->F32 functions, and the six
// rewired q8_*/q4_* modules) -- no `real`/`shortreal`/DPI anywhere in
// this file or anything it instantiates.
//
// ---- data format (one shared 512-bit bus both directions) ----
// in_data/out_data are always 512 bits regardless of mode:
//   Q8/Q4 encode INPUT , Q8/Q4 decode OUTPUT: 32 x F16 lanes (bits
//     [16*j +: 16] = lane j), the full 512 bits are meaningful.
//   Q8 encode OUTPUT: bits [271:0] = the 34-byte Q8_0 packed block
//     (2-byte F16 scale + 32 signed int8 lanes), bits [511:272] = 0.
//   Q4 encode OUTPUT: bits [143:0] = the 18-byte Q4_0 packed block
//     (2-byte F16 scale + 16 packed-nibble bytes), bits [511:144] = 0.
//   Q8/Q4 decode INPUT: same packed-block layout, read from the low
//     272/144 bits of in_data; the unused upper bits are ignored (not
//     required to be zero).
// A single fixed-width bus (rather than per-mode port widths) was
// chosen so this module's port list, and any DMA engine feeding it,
// stays mode-independent -- see docs/phase5-synthesizable-fpga.md's
// CPU/FPGA partition section for the rationale.
//
// ---- mode encoding ----
//   2'b00 = Q8_0 encode      2'b01 = Q8_0 decode
//   2'b10 = Q4_0 encode      2'b11 = Q4_0 decode
//
// ---- ordering guarantee ----
// Exactly one transaction is issued (popped from the input FIFO into
// the compute datapath) per cycle. Each of the four sub-pipelines is
// individually latency-padded (see L_MAX / *_pad arrays below) so that
// EVERY mode takes the exact same fixed number of cycles, L_MAX, from
// issue to result. Because issue order is preserved by the input FIFO
// and every transaction takes the identical fixed latency regardless
// of mode, results necessarily retire in the same order they were
// issued -- output ordering is preserved by construction, not by an
// explicit reorder buffer. (DIV_DELAY/MUL_DELAY are hardcoded to 1
// throughout this module, matching every DELAY value this phase's
// vector tests were actually run against; the L_MAX=7 and per-chain
// padding constants below are only correct for that specific
// configuration -- disclosed, not made a top-level parameter, to keep
// the padding arithmetic trivially checkable by inspection rather than
// a general but untested formula.)
//
// ---- backpressure / no-loss guarantee ----
// A transaction is only issued once the output FIFO has a RESERVED
// slot for it (in_flight + out_fifo_occupancy < OUT_FIFO_DEPTH is
// checked before every issue, see `issue_fire`), so the output FIFO
// can never overflow and an accepted (in_ready-acknowledged) input can
// never be silently dropped -- it is always parked in the input FIFO
// until it is safe to issue. Both FIFOs are the same stream_fifo.sv
// building block used throughout Phase 5.2/5.3.
//
// ---- error flag ----
// out_error is a single, honestly-scoped status bit, not a general
// error/retry protocol (retry semantics belong at the CPU/driver level,
// see docs/phase5-synthesizable-fpga.md): for decode modes it is the OR
// of "this F16 output lane is NaN or Infinity" across all 32 lanes; for
// encode modes it is "the computed block scale (d) is NaN or Infinity"
// (the only way a Q8_0/Q4_0 encode's OWN output can carry a NaN/Inf
// forward, since amax feeds directly into d).
module membrane_quant_stream_top_b1 #(
	parameter int ID_WIDTH = 16,
	parameter int IN_FIFO_DEPTH = 16,
	parameter int OUT_FIFO_DEPTH = 32
) (
	input	logic				clk,
	input	logic				rst_n,

	input	logic				in_valid,
	output	logic				in_ready,
	input	logic	[1:0]		in_mode,
	input	logic	[ID_WIDTH-1:0]	in_id,
	input	logic	[511:0]		in_data,

	output	logic				out_valid,
	input	logic				out_ready,
	output	logic	[1:0]		out_mode,
	output	logic	[ID_WIDTH-1:0]	out_id,
	output	logic	[511:0]		out_data,
	output	logic				out_error
);

	localparam int L_MAX = 7;
	localparam logic [1:0] MODE_Q8_ENC = 2'b00;
	localparam logic [1:0] MODE_Q8_DEC = 2'b01;
	localparam logic [1:0] MODE_Q4_ENC = 2'b10;
	localparam logic [1:0] MODE_Q4_DEC = 2'b11;

	// Assigns the function's own name instead of using `return` -- yosys
	// 0.33 rejects `return` inside module-scoped functions, see
	// membrane_fp_adder.sv's lzc_9 header comment for the full finding.
	function automatic logic f16_is_special(input logic [15:0] w);
		f16_is_special = (w[14:10] == 5'h1F);
	endfunction

	// ---- input FIFO ----
	localparam int IN_WORD_WIDTH = 2 + ID_WIDTH + 512;

	logic				in_fifo_out_valid, in_fifo_out_ready;
	logic	[IN_WORD_WIDTH-1:0]	in_fifo_in_word, in_fifo_out_word;
	logic	[$clog2(IN_FIFO_DEPTH):0]	in_fifo_occ;

	assign in_fifo_in_word = {in_mode, in_id, in_data};

	stream_fifo #(.WIDTH(IN_WORD_WIDTH), .DEPTH(IN_FIFO_DEPTH)) u_in_fifo (
		.clk(clk), .rst_n(rst_n),
		.in_valid(in_valid), .in_ready(in_ready), .in_data(in_fifo_in_word),
		.out_valid(in_fifo_out_valid), .out_ready(in_fifo_out_ready),
		.out_data(in_fifo_out_word), .occupancy(in_fifo_occ));

	logic	[1:0]			mode_pop;
	logic	[ID_WIDTH-1:0]	id_pop;
	logic	[511:0]			data_pop;

	assign {mode_pop, id_pop, data_pop} = in_fifo_out_word;

	// ---- output FIFO ----
	localparam int OUT_WORD_WIDTH = 2 + ID_WIDTH + 512 + 1;

	logic				out_fifo_in_valid, out_fifo_in_ready;
	logic	[OUT_WORD_WIDTH-1:0]	out_fifo_in_word, out_fifo_out_word;
	logic	[$clog2(OUT_FIFO_DEPTH):0]	out_fifo_occ;

	stream_fifo #(.WIDTH(OUT_WORD_WIDTH), .DEPTH(OUT_FIFO_DEPTH)) u_out_fifo (
		.clk(clk), .rst_n(rst_n),
		.in_valid(out_fifo_in_valid), .in_ready(out_fifo_in_ready),
		.in_data(out_fifo_in_word),
		.out_valid(out_valid), .out_ready(out_ready),
		.out_data(out_fifo_out_word), .occupancy(out_fifo_occ));

	assign {out_mode, out_id, out_data, out_error} = out_fifo_out_word;

	// ---- issue gating: reserve an output-FIFO slot before issuing ----
	logic	issue_fire;
	int		in_flight;
	int		occ_i;
	int		flight_i;

	always_comb begin
		occ_i = out_fifo_occ;
		flight_i = in_flight;
		issue_fire = in_fifo_out_valid && ((occ_i + flight_i) < OUT_FIFO_DEPTH);
	end

	assign in_fifo_out_ready = issue_fire;

	// ---- shared id/mode/valid tag delay pipe (depth L_MAX) ----
	localparam int TAG_W = 1 + 2 + ID_WIDTH;
	logic	[TAG_W-1:0]	tag_pipe	[0:L_MAX-1];

	// Must reset to a clean, non-X valid bit: without this, retire_fire
	// (tag_pipe's top bit) stays X for the first L_MAX cycles after
	// power-on, which corrupts the in_flight credit counter below --
	// `issue_fire && !retire_fire` evaluates to X (neither true nor
	// false) whenever retire_fire is X, so NEITHER branch of the
	// increment/decrement `if/else if` fires and an issued transaction's
	// credit is silently never reserved. Caught by this module's own
	// in_flight-range assertion during bring-up, not by a data-mismatch
	// (the very first transaction still produced the right answer; only
	// the credit accounting was wrong).
	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			for (int k = 0; k < L_MAX; k++)
				tag_pipe[k] <= '0;
		end else begin
			tag_pipe[0] <= {issue_fire, mode_pop, id_pop};
			for (int k = L_MAX - 1; k > 0; k--)
				tag_pipe[k] <= tag_pipe[k - 1];
		end
	end

	logic				retire_fire;
	logic	[1:0]		mode_sel;
	logic	[ID_WIDTH-1:0]	id_sel;

	assign {retire_fire, mode_sel, id_sel} = tag_pipe[L_MAX - 1];

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			in_flight <= 0;
		else if (issue_fire && !retire_fire)
			in_flight <= in_flight + 1;
		else if (!issue_fire && retire_fire)
			in_flight <= in_flight - 1;
	end

	assign out_fifo_in_valid = retire_fire;

	// ---- shared 32-lane F16 unpack of the issued transaction ----
	logic	[15:0]	x_in_issue	[0:31];

	assign x_in_issue[0] = data_pop[15:0];
	assign x_in_issue[1] = data_pop[31:16];
	assign x_in_issue[2] = data_pop[47:32];
	assign x_in_issue[3] = data_pop[63:48];
	assign x_in_issue[4] = data_pop[79:64];
	assign x_in_issue[5] = data_pop[95:80];
	assign x_in_issue[6] = data_pop[111:96];
	assign x_in_issue[7] = data_pop[127:112];
	assign x_in_issue[8] = data_pop[143:128];
	assign x_in_issue[9] = data_pop[159:144];
	assign x_in_issue[10] = data_pop[175:160];
	assign x_in_issue[11] = data_pop[191:176];
	assign x_in_issue[12] = data_pop[207:192];
	assign x_in_issue[13] = data_pop[223:208];
	assign x_in_issue[14] = data_pop[239:224];
	assign x_in_issue[15] = data_pop[255:240];
	assign x_in_issue[16] = data_pop[271:256];
	assign x_in_issue[17] = data_pop[287:272];
	assign x_in_issue[18] = data_pop[303:288];
	assign x_in_issue[19] = data_pop[319:304];
	assign x_in_issue[20] = data_pop[335:320];
	assign x_in_issue[21] = data_pop[351:336];
	assign x_in_issue[22] = data_pop[367:352];
	assign x_in_issue[23] = data_pop[383:368];
	assign x_in_issue[24] = data_pop[399:384];
	assign x_in_issue[25] = data_pop[415:400];
	assign x_in_issue[26] = data_pop[431:416];
	assign x_in_issue[27] = data_pop[447:432];
	assign x_in_issue[28] = data_pop[463:448];
	assign x_in_issue[29] = data_pop[479:464];
	assign x_in_issue[30] = data_pop[495:480];
	assign x_in_issue[31] = data_pop[511:496];

	// =====================================================================
	// Q8_0 encode chain: maxabs(5) -> scale(1) -> quantize_pack(1) = 7 = L_MAX
	// =====================================================================
	logic	q8enc_valid_in;
	assign q8enc_valid_in = issue_fire && (mode_pop == MODE_Q8_ENC);

	logic			q8_maxabs_valid;
	logic	[15:0]	q8_amax_f16;

	q8_maxabs_reduce u_q8_maxabs (
		.clk(clk), .rst_n(rst_n), .valid_in(q8enc_valid_in), .x_in_flat(data_pop),
		.valid_out(q8_maxabs_valid), .amax_f16_out(q8_amax_f16));

	// x_in delayed to align with amax (5 cycles), then one more (6 total)
	// to align with d_f16/id_f32 (q8_scale adds 1 more cycle).
	logic	[15:0]	q8_x_d	[0:5][0:31];

	always_ff @(posedge clk) begin
		for (int j = 0; j < 32; j++)
			q8_x_d[0][j] <= x_in_issue[j];
		for (int k = 5; k > 0; k--)
			for (int j = 0; j < 32; j++)
				q8_x_d[k][j] <= q8_x_d[k - 1][j];
	end

	logic			q8_scale_valid;
	logic	[15:0]	q8_d_f16;
	logic	[31:0]	q8_id_f32;

	q8_scale #(.DIV_DELAY(1)) u_q8_scale (
		.clk(clk), .rst_n(rst_n), .valid_in(q8_maxabs_valid),
		.amax_f16_in(q8_amax_f16), .valid_out(q8_scale_valid),
		.d_f16_out(q8_d_f16), .id_f32_out(q8_id_f32));

	// Icarus Verilog does not support connecting a slice of a 2D unpacked
	// array (q8_x_d[5]) directly to a port expecting a 1D unpacked array
	// ("Array slices are not yet supported for continuous assignment"),
	// and q8_quantize_pack.x_in is now a flat 512-bit bus (yosys 0.33
	// doesn't support unpacked-array ports at all, see q8_maxabs_reduce.sv's
	// x_in_flat header comment) -- so this packs q8_x_d[5][*] into a flat
	// bus in one step.
	logic	[511:0]	q8_x_final_flat;

	always_comb
		for (int j = 0; j < 32; j++)
			q8_x_final_flat[j * 16 +: 16] = q8_x_d[5][j];

	logic			q8_qp_valid;
	logic	[271:0]	q8_packed;

	q8_quantize_pack #(.MUL_DELAY(1)) u_q8_qp (
		.clk(clk), .rst_n(rst_n), .valid_in(q8_scale_valid),
		.x_in_flat(q8_x_final_flat), .d_f16_in(q8_d_f16), .id_f32_in(q8_id_f32),
		.valid_out(q8_qp_valid), .packed_out(q8_packed));

	// Error flag: d NaN/Inf, captured when q8_scale_valid pulses, then
	// delayed 1 more cycle (matching quantize_pack's own MUL_DELAY) so it
	// lands on the same cycle as q8_packed/q8_qp_valid.
	logic	q8_err_raw, q8_err_final;

	assign q8_err_raw = f16_is_special(q8_d_f16);

	always_ff @(posedge clk)
		q8_err_final <= q8_err_raw;

	// =====================================================================
	// Q8_0 decode chain: dequantize(1), padded by 6 to reach L_MAX=7
	// =====================================================================
	logic	q8dec_valid_in;
	assign q8dec_valid_in = issue_fire && (mode_pop == MODE_Q8_DEC);

	logic			q8_dq_valid;
	logic	[511:0]	q8_dq_out;

	q8_dequantize #(.MUL_DELAY(1)) u_q8_dq (
		.clk(clk), .rst_n(rst_n), .valid_in(q8dec_valid_in),
		.packed_in(data_pop[271:0]), .valid_out(q8_dq_valid),
		.x_out(q8_dq_out));

	logic	q8_dq_err;

	always_comb begin
		q8_dq_err = 1'b0;
		for (int j = 0; j < 32; j++)
			q8_dq_err = q8_dq_err || f16_is_special(q8_dq_out[j * 16 +: 16]);
	end

	// Bundle {valid, error, data} through the padding pipe so they stay
	// perfectly aligned by construction.
	localparam int Q8DEC_PAD = L_MAX - 1;
	logic	[513:0]	q8dec_pad	[0:Q8DEC_PAD - 1];

	always_ff @(posedge clk) begin
		q8dec_pad[0] <= {q8_dq_valid, q8_dq_err, q8_dq_out};
		for (int k = Q8DEC_PAD - 1; k > 0; k--)
			q8dec_pad[k] <= q8dec_pad[k - 1];
	end

	logic			q8dec_final_valid;
	logic			q8dec_final_err;
	logic	[511:0]	q8dec_final_data;

	assign {q8dec_final_valid, q8dec_final_err, q8dec_final_data} =
		q8dec_pad[Q8DEC_PAD - 1];

	// =====================================================================
	// Q4_0 encode chain: scan(0) -> scale(2) -> pack(2) = 4, padded by 3
	// =====================================================================
	logic	q4enc_valid_in;
	assign q4enc_valid_in = issue_fire && (mode_pop == MODE_Q4_ENC);

	logic	[31:0]	q4_mx_f32;

	q4_scan u_q4_scan (.x_in_flat(data_pop), .mx_f32_out(q4_mx_f32));

	logic			q4_scale_valid;
	logic	[15:0]	q4_d_f16;
	logic	[31:0]	q4_id_f32;

	// EXP-FPGA-DIV-001 Phase B1: the ONLY difference from
	// rtl/membrane_quant_stream_top.sv in this entire file is this one
	// instantiation (q4_scale_b1 instead of q4_scale) -- see
	// rtl/experimental/fp_div/q4_scale_b1.sv's own header for what that
	// changes (only u_div_d, not u_div_id).
	q4_scale_b1 #(.DIV_DELAY(1)) u_q4_scale (
		.clk(clk), .rst_n(rst_n), .valid_in(q4enc_valid_in),
		.mx_f32(q4_mx_f32), .valid_out(q4_scale_valid),
		.d_f16_out(q4_d_f16), .id_f32_out(q4_id_f32));

	// x_in delayed 2 cycles to match q4_scale's own 2-cycle latency
	// (two chained membrane_fp_divider instances, DIV_DELAY=1 each).
	logic	[15:0]	q4_x_d	[0:1][0:31];

	always_ff @(posedge clk) begin
		for (int j = 0; j < 32; j++)
			q4_x_d[0][j] <= x_in_issue[j];
		for (int j = 0; j < 32; j++)
			q4_x_d[1][j] <= q4_x_d[0][j];
	end

	logic	[511:0]	q4_x_final_flat;

	always_comb
		for (int j = 0; j < 32; j++)
			q4_x_final_flat[j * 16 +: 16] = q4_x_d[1][j];

	logic			q4_pack_valid;
	logic	[143:0]	q4_packed;

	q4_pack #(.MUL_DELAY(1)) u_q4_pack (
		.clk(clk), .rst_n(rst_n), .valid_in(q4_scale_valid),
		.x_in_flat(q4_x_final_flat), .d_f16(q4_d_f16), .id_f32(q4_id_f32),
		.valid_out(q4_pack_valid), .packed_out(q4_packed));

	// Error flag: d NaN/Inf, captured when q4_scale_valid pulses, delayed
	// 2 cycles to match q4_pack's own latency (its multiplier+adder
	// chain), landing on the same cycle as q4_pack_valid/q4_packed.
	logic	q4_err_raw;
	logic	[1:0]	q4_err_d2	[0:1];

	assign q4_err_raw = f16_is_special(q4_d_f16);

	always_ff @(posedge clk) begin
		q4_err_d2[0] <= {1'b0, q4_err_raw};
		q4_err_d2[1] <= q4_err_d2[0];
	end

	localparam int Q4ENC_PAD = L_MAX - 4;
	logic	[144:0]	q4enc_pad	[0:Q4ENC_PAD - 1];

	always_ff @(posedge clk) begin
		q4enc_pad[0] <= {q4_err_d2[1][0], q4_packed};
		for (int k = Q4ENC_PAD - 1; k > 0; k--)
			q4enc_pad[k] <= q4enc_pad[k - 1];
	end

	logic			q4enc_final_valid;
	logic	[143:0]	q4enc_final_data;
	logic			q4enc_final_err;

	// q4_pack_valid is padded alongside the data/err bundle above (same
	// depth), read out through its own tiny shift register so the three
	// stay aligned without widening the 145-bit array further.
	logic	[Q4ENC_PAD - 1:0]	q4enc_valid_pad;

	always_ff @(posedge clk) begin
		q4enc_valid_pad[0] <= q4_pack_valid;
		for (int k = Q4ENC_PAD - 1; k > 0; k--)
			q4enc_valid_pad[k] <= q4enc_valid_pad[k - 1];
	end

	assign q4enc_final_valid = q4enc_valid_pad[Q4ENC_PAD - 1];
	assign {q4enc_final_err, q4enc_final_data} = q4enc_pad[Q4ENC_PAD - 1];

	// =====================================================================
	// Q4_0 decode chain: unpack(1), padded by 6 to reach L_MAX=7
	// =====================================================================
	logic	q4dec_valid_in;
	assign q4dec_valid_in = issue_fire && (mode_pop == MODE_Q4_DEC);

	logic			q4_uq_valid;
	logic	[511:0]	q4_uq_out;

	q4_unpack #(.MUL_DELAY(1)) u_q4_uq (
		.clk(clk), .rst_n(rst_n), .valid_in(q4dec_valid_in),
		.packed_in(data_pop[143:0]), .valid_out(q4_uq_valid),
		.x_out(q4_uq_out));

	logic	q4_uq_err;

	always_comb begin
		q4_uq_err = 1'b0;
		for (int j = 0; j < 32; j++)
			q4_uq_err = q4_uq_err || f16_is_special(q4_uq_out[j * 16 +: 16]);
	end

	localparam int Q4DEC_PAD = L_MAX - 1;
	logic	[513:0]	q4dec_pad	[0:Q4DEC_PAD - 1];

	always_ff @(posedge clk) begin
		q4dec_pad[0] <= {q4_uq_valid, q4_uq_err, q4_uq_out};
		for (int k = Q4DEC_PAD - 1; k > 0; k--)
			q4dec_pad[k] <= q4dec_pad[k - 1];
	end

	logic			q4dec_final_valid;
	logic			q4dec_final_err;
	logic	[511:0]	q4dec_final_data;

	assign {q4dec_final_valid, q4dec_final_err, q4dec_final_data} =
		q4dec_pad[Q4DEC_PAD - 1];

	// ---- output mux: mode_sel picks which padded chain result retires ----
	logic	[511:0]	result_data;
	logic			result_error;

	always_comb begin
		case (mode_sel)
			MODE_Q8_ENC: begin
				result_data = {240'h0, q8_packed};
				result_error = q8_err_final;
			end
			MODE_Q8_DEC: begin
				result_data = q8dec_final_data;
				result_error = q8dec_final_err;
			end
			MODE_Q4_ENC: begin
				result_data = {368'h0, q4enc_final_data};
				result_error = q4enc_final_err;
			end
			default: begin // MODE_Q4_DEC
				result_data = q4dec_final_data;
				result_error = q4dec_final_err;
			end
		endcase
	end

	assign out_fifo_in_word = {mode_sel, id_sel, result_data, result_error};

	// ---- correctness assertions (also serves task item 7: property
	// checks -- no accepted-input loss, no stale output, output valid
	// exactly when expected) ----
	// Guarded by `SYNTHESIS (defined via `-D SYNTHESIS` when this file is
	// fed to yosys, see docs/phase5-synthesizable-fpga.md's synthesis
	// section) rather than a `// synthesis translate_off` comment pragma
	// -- that comment convention is not guaranteed to be honored by
	// yosys's read_verilog frontend, whereas a preprocessor `` `ifndef ``
	// is unambiguous with any tool.
`ifndef SYNTHESIS
	always_ff @(posedge clk) begin
		if (rst_n && retire_fire) begin
			case (mode_sel)
				MODE_Q8_ENC: assert (q8_qp_valid)
					else $error("membrane_quant_stream_top: Q8 encode latency mismatch at retire");
				MODE_Q8_DEC: assert (q8dec_final_valid)
					else $error("membrane_quant_stream_top: Q8 decode latency mismatch at retire");
				MODE_Q4_ENC: assert (q4enc_final_valid)
					else $error("membrane_quant_stream_top: Q4 encode latency mismatch at retire");
				MODE_Q4_DEC: assert (q4dec_final_valid)
					else $error("membrane_quant_stream_top: Q4 decode latency mismatch at retire");
				default: ;
			endcase
		end
		if (rst_n)
			assert (in_flight >= 0 && in_flight <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top: in_flight credit counter out of range");
	end
`endif
endmodule
