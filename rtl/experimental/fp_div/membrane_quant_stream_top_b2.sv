// EXP-FPGA-DIV-001 Phase B2 variant of rtl/membrane_quant_stream_top.sv.
//
// membrane_quant_stream_top.sv's entire retirement scheme rests on one
// invariant, stated explicitly in its own header comment: "EVERY mode
// takes the exact same fixed number of cycles, L_MAX, from issue to
// result" -- which is what lets a single shared tag_pipe shift register
// guarantee in-order retirement with no reorder buffer. Phase B2's
// iterative Q4_0 divider (rtl/experimental/fp_div/fp32_div_iterative_exact.sv)
// breaks that invariant on purpose: Q4_0 encode's latency through
// q4_scale_b2.sv is no longer fixed (measured 3-29+ cycles depending on
// operand, see experiments/EXP-FPGA-DIV-001/results/b2-differential.json),
// and is now larger than L_MAX=7 in the common case. This file honestly
// re-architects the retirement path around that fact instead of
// pretending it doesn't matter -- see
// experiments/EXP-FPGA-DIV-001/phase-b2.md section 6 for the full
// reasoning and the throughput cost this creates.
//
// ---- what changed vs. membrane_quant_stream_top.sv / _b1.sv ----
// Q8_0 encode, Q8_0 decode, and Q4_0 decode are BYTE-IDENTICAL to the
// production file -- same fixed L_MAX=7 tag_pipe retirement, completely
// untouched, still true-by-construction in-order (this is what makes
// "Q8 is unaffected" a structural fact checkable by diff, not just an
// empirical claim -- see phase-b2.md).
//
// Q4_0 encode is handled OUTSIDE that shared mechanism entirely:
//   1. A Q4_0 encode transaction is only ISSUED when in_flight==0 (no
//      fixed-latency-mode transaction currently outstanding in the
//      shared tag_pipe) and no other Q4_0 encode transaction is already
//      in flight (`q4enc_inflight`).
//   2. Once issued, `q4enc_inflight` goes high and issuance of ANY
//      transaction (any mode) is blocked until this Q4_0 encode
//      transaction fully retires.
//   3. Its result retires the instant q4_pack_valid pulses (a direct
//      connection, not routed through tag_pipe), competing for the
//      shared output FIFO write port with tag_pipe's own retire_fire --
//      but by construction (2) guarantees these two retire sources can
//      never actually collide on the same cycle: tag_pipe cannot be
//      draining a REAL (non-bubble) entry while q4enc_inflight is high,
//      because nothing was allowed to issue into it during that window.
// This fully serializes Q4_0 encode against every other transaction
// (including other Q4_0 encodes) -- the simplest correct way to keep
// global in-order retirement without building a real reorder buffer,
// at a real, measured, disclosed throughput cost (see phase-b2.md).
// Q4_0 encode's own x_in lanes are held in a dedicated register
// (`q4enc_x_hold`), captured once at issue, rather than shifted through
// a fixed-depth delay pipe -- q4_scale_b2.sv's own header explains why a
// fixed-depth pipe cannot correctly delay-match a variable-latency
// divider.
//
// ---- everything else below (data format, mode encoding, Q8 chains,
// Q4 decode chain, backpressure/no-loss guarantee for the fixed-latency
// modes, error flag semantics) is unchanged from
// rtl/membrane_quant_stream_top.sv -- see that file's own header for
// the full original description, not repeated here except where it
// differs. ----
module membrane_quant_stream_top_b2 #(
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

	// ---- Q4_0 encode in-flight tracking (Phase B2's core change) ----
	logic	q4enc_inflight;

	// ---- issue gating: reserve an output-FIFO slot before issuing, AND
	// (Phase B2) serialize Q4_0 encode against everything else -- see
	// this file's header for why. ----
	logic	issue_fire;
	logic	tagpipe_issue_fire;
	int		in_flight;
	int		occ_i;
	int		flight_i;
	logic	slot_ok;

	always_comb begin
		occ_i = out_fifo_occ;
		flight_i = in_flight;
		// Reserve a slot for a Q4_0 encode transaction too (it writes
		// the output FIFO via its own direct path, see q4enc_direct_retire
		// below) -- otherwise the output FIFO could overflow exactly the
		// way the original module's own comment warns against.
		slot_ok = (occ_i + flight_i + (q4enc_inflight ? 1 : 0)) < OUT_FIFO_DEPTH;
		if (mode_pop == MODE_Q4_ENC)
			issue_fire = in_fifo_out_valid && slot_ok && !q4enc_inflight && (flight_i == 0);
		else
			issue_fire = in_fifo_out_valid && slot_ok && !q4enc_inflight;
	end

	assign tagpipe_issue_fire = issue_fire && (mode_pop != MODE_Q4_ENC);
	assign in_fifo_out_ready = issue_fire;

	// ---- shared id/mode/valid tag delay pipe (depth L_MAX), now used
	// ONLY by the three fixed-latency modes (Q8 encode, Q8 decode, Q4
	// decode) -- Q4_0 encode never enters this pipe. ----
	localparam int TAG_W = 1 + 2 + ID_WIDTH;
	logic	[TAG_W-1:0]	tag_pipe	[0:L_MAX-1];

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			for (int k = 0; k < L_MAX; k++)
				tag_pipe[k] <= '0;
		end else begin
			tag_pipe[0] <= {tagpipe_issue_fire, mode_pop, id_pop};
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
		else if (tagpipe_issue_fire && !retire_fire)
			in_flight <= in_flight + 1;
		else if (!tagpipe_issue_fire && retire_fire)
			in_flight <= in_flight - 1;
	end

	// ---- Q4_0 encode in-flight flag: set on issue, cleared when its
	// own datapath (q4_pack, see below) finally retires it. ----
	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			q4enc_inflight <= 1'b0;
		else if (issue_fire && mode_pop == MODE_Q4_ENC)
			q4enc_inflight <= 1'b1;
		else if (q4_pack_valid)
			q4enc_inflight <= 1'b0;
	end

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
	// Q8_0 encode chain: UNCHANGED from membrane_quant_stream_top.sv --
	// maxabs(5) -> scale(1) -> quantize_pack(1) = 7 = L_MAX, still
	// retiring through the shared tag_pipe.
	// =====================================================================
	logic	q8enc_valid_in;
	assign q8enc_valid_in = issue_fire && (mode_pop == MODE_Q8_ENC);

	logic			q8_maxabs_valid;
	logic	[15:0]	q8_amax_f16;

	q8_maxabs_reduce u_q8_maxabs (
		.clk(clk), .rst_n(rst_n), .valid_in(q8enc_valid_in), .x_in_flat(data_pop),
		.valid_out(q8_maxabs_valid), .amax_f16_out(q8_amax_f16));

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

	logic	q8_err_raw, q8_err_final;

	assign q8_err_raw = f16_is_special(q8_d_f16);

	always_ff @(posedge clk)
		q8_err_final <= q8_err_raw;

	// =====================================================================
	// Q8_0 decode chain: UNCHANGED -- dequantize(1), padded by 6 to reach
	// L_MAX=7, still retiring through the shared tag_pipe.
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
	// Q4_0 encode chain (Phase B2): scan(0, combinational) -> q4_scale_b2
	// (VARIABLE latency, iterative exact divider) -> pack(2). Retires
	// DIRECTLY (q4enc_direct_retire), not through tag_pipe/L_MAX padding
	// -- see this file's header.
	// =====================================================================
	logic	q4enc_valid_in;
	assign q4enc_valid_in = issue_fire && (mode_pop == MODE_Q4_ENC);

	logic	[31:0]	q4_mx_f32;

	q4_scan u_q4_scan (.x_in_flat(data_pop), .mx_f32_out(q4_mx_f32));

	logic			q4_scale_valid;
	logic	[15:0]	q4_d_f16;
	logic	[31:0]	q4_id_f32;
	logic			q4_scale_busy;

	q4_scale_b2 #(.DIV_DELAY(1)) u_q4_scale (
		.clk(clk), .rst_n(rst_n), .valid_in(q4enc_valid_in),
		.mx_f32(q4_mx_f32), .valid_out(q4_scale_valid),
		.d_f16_out(q4_d_f16), .id_f32_out(q4_id_f32), .busy(q4_scale_busy));

	// x_in held stable from issue until q4_scale_valid finally fires
	// (variable latency) -- replaces the original fixed-depth 2-stage
	// q4_x_d pipe, which assumed a constant 2-cycle q4_scale latency
	// that no longer holds. Captured only at issue (q4enc_valid_in),
	// safe because only one Q4_0 encode transaction is ever in flight
	// (this file's own issue-gating guarantee).
	logic	[15:0]	q4enc_x_hold	[0:31];
	logic	[ID_WIDTH-1:0]	q4enc_id_hold;

	always_ff @(posedge clk) begin
		if (q4enc_valid_in) begin
			for (int j = 0; j < 32; j++)
				q4enc_x_hold[j] <= x_in_issue[j];
			q4enc_id_hold <= id_pop;
		end
	end

	logic	[511:0]	q4_x_final_flat;

	always_comb
		for (int j = 0; j < 32; j++)
			q4_x_final_flat[j * 16 +: 16] = q4enc_x_hold[j];

	logic			q4_pack_valid;
	logic	[143:0]	q4_packed;

	q4_pack #(.MUL_DELAY(1)) u_q4_pack (
		.clk(clk), .rst_n(rst_n), .valid_in(q4_scale_valid),
		.x_in_flat(q4_x_final_flat), .d_f16(q4_d_f16), .id_f32(q4_id_f32),
		.valid_out(q4_pack_valid), .packed_out(q4_packed));

	// Error flag: d NaN/Inf, captured when q4_scale_valid pulses, delayed
	// 2 cycles to match q4_pack's own FIXED latency (its multiplier+
	// adder chain, unaffected by Phase B2 -- only q4_scale's own latency
	// changed), landing on the same cycle as q4_pack_valid/q4_packed,
	// exactly as in the production file.
	logic	q4_err_raw;
	logic	[1:0]	q4_err_d2	[0:1];

	assign q4_err_raw = f16_is_special(q4_d_f16);

	always_ff @(posedge clk) begin
		q4_err_d2[0] <= {1'b0, q4_err_raw};
		q4_err_d2[1] <= q4_err_d2[0];
	end

	logic	q4enc_final_err;
	assign q4enc_final_err = q4_err_d2[1][0];

	// Direct retirement: q4_pack_valid IS this transaction's retire
	// signal (no padding, no tag_pipe). See this file's header for why
	// this can never collide with tag_pipe's own retire_fire on the
	// same cycle.
	logic	q4enc_direct_retire;
	assign q4enc_direct_retire = q4_pack_valid;

	// =====================================================================
	// Q4_0 decode chain: UNCHANGED -- unpack(1), padded by 6 to reach
	// L_MAX=7, still retiring through the shared tag_pipe.
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

	// ---- output mux: mode_sel (tag_pipe-driven, never Q4_0 encode)
	// picks which of the three fixed-latency chains retires; Q4_0
	// encode retires via its own direct path, arbitrated below. ----
	logic	[511:0]	result_data;
	logic			result_error;

	always_comb begin
		if (mode_sel == MODE_Q8_ENC) begin
			result_data = {240'h0, q8_packed};
			result_error = q8_err_final;
		end else if (mode_sel == MODE_Q8_DEC) begin
			result_data = q8dec_final_data;
			result_error = q8dec_final_err;
		end else begin // MODE_Q4_DEC (MODE_Q4_ENC never appears in mode_sel, see header)
			result_data = q4dec_final_data;
			result_error = q4dec_final_err;
		end
	end

	// q4enc_direct_retire takes priority in this mux only in the sense
	// that it is a SEPARATE, always-safe-to-check condition -- per this
	// file's header, retire_fire (tag_pipe) is guaranteed 0 whenever
	// q4enc_direct_retire is 1, so there is no real arbitration conflict
	// to resolve, just a 2-way OR/mux.
	assign out_fifo_in_valid = retire_fire || q4enc_direct_retire;
	assign out_fifo_in_word = q4enc_direct_retire
		? {MODE_Q4_ENC, q4enc_id_hold, {368'h0, q4_packed}, q4enc_final_err}
		: {mode_sel, id_sel, result_data, result_error};

`ifndef SYNTHESIS
	always_ff @(posedge clk) begin
		if (rst_n && retire_fire) begin
			if (mode_sel == MODE_Q8_ENC)
				assert (q8_qp_valid)
					else $error("membrane_quant_stream_top_b2: Q8 encode latency mismatch at retire");
			else if (mode_sel == MODE_Q8_DEC)
				assert (q8dec_final_valid)
					else $error("membrane_quant_stream_top_b2: Q8 decode latency mismatch at retire");
			else
				assert (q4dec_final_valid)
					else $error("membrane_quant_stream_top_b2: Q4 decode latency mismatch at retire");
		end
		if (rst_n)
			assert (in_flight >= 0 && in_flight <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_b2: in_flight credit counter out of range");
		// retire_fire and q4enc_direct_retire must never collide -- this
		// is the structural claim this file's header makes; assert it
		// directly rather than only arguing it in prose.
		if (rst_n)
			assert (!(retire_fire && q4enc_direct_retire))
				else $error("membrane_quant_stream_top_b2: tag_pipe retire and Q4 encode direct retire collided -- serialization invariant broken");
	end
`endif
endmodule
