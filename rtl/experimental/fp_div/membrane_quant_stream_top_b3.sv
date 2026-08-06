// EXP-FPGA-DIV-001 Phase B3 variant of rtl/membrane_quant_stream_top.sv.
//
// Builds on Phase B2 (rtl/experimental/fp_div/membrane_quant_stream_top_b2.sv):
// same iterative Q4_0 divider (fp32_div_iterative_exact via q4_scale_b2,
// BYTE-IDENTICAL, not modified), same Q8_0 encode/decode and Q4_0 decode
// chains (BYTE-IDENTICAL to production, still retiring through the shared
// fixed-depth tag_pipe). The ONE thing this file changes is the scheduling
// decision documented as the root cause of B2's collateral slowdown in
// experiments/EXP-FPGA-DIV-001/phase-b3-root-cause.md: B2 blocked ALL
// issuance (any mode) while a Q4_0 encode transaction was in flight, purely
// to keep global in-order retirement correct given the single-word-per-
// cycle output port. This file decouples issuance of the three fixed-
// latency modes from Q4_0 encode's presence, using a small, bounded
// membrane_completion_reorder buffer (rtl/experimental/fp_div/
// membrane_completion_reorder.sv) to hold "early" completions until their
// turn instead of preventing them from ever occurring.
//
// ---- what changed vs membrane_quant_stream_top_b2.sv ----
// - The `in_flight` counter and the `!q4enc_inflight` gate on Q8_0
//   encode/decode and Q4_0 decode issuance are GONE. Those three modes now
//   issue into tag_pipe purely based on `issue_allow` (from the reorder
//   buffer: fewer than REORDER_DEPTH transactions currently outstanding,
//   ACROSS ALL MODES) and `slot_ok` (output FIFO has a reserved slot) --
//   exactly like every other mode, no Q4_0-encode-specific case any more.
// - Q4_0 encode issuance keeps exactly one gate beyond `issue_allow`/
//   `slot_ok`: `!q4enc_inflight`, because there is exactly one physical
//   fp32_div_iterative_exact instance and it is itself single-in-flight by
//   its own design (its in_ready is only asserted from IDLE) -- this is a
//   structural resource constraint, not a scheduling choice, and is
//   unrelated to the ordering fix.
// - Every issued transaction (any mode) is tagged with a sequence number
//   from membrane_completion_reorder, carried through tag_pipe alongside
//   {mode, id} for the three fixed-latency chains, or held in
//   `q4enc_seq_hold` alongside `q4enc_id_hold`/`q4enc_x_hold` for Q4_0
//   encode (identical convention B2 already uses for id/x -- see that
//   file's header for why a hold register, not a fixed-depth pipe, is
//   correct here).
// - `tag_pipe`'s own completion (`retire_fire`) and Q4_0 encode's own
//   completion (`q4_pack_valid`) both now feed the reorder buffer's two
//   completion ports DIRECTLY, and are explicitly ALLOWED to be
//   simultaneously true (the reorder buffer has two write ports for
//   exactly this reason) -- this is the opposite of B2's own invariant
//   (which required they never collide). The reorder buffer, not the top
//   level, is what makes this safe: see that module's own header for why
//   a direct-mapped buffer indexed by `seq mod REORDER_DEPTH` cannot alias
//   two live entries, given the `outstanding < REORDER_DEPTH` issue gate.
// - The reorder buffer's own single drain port feeds the output FIFO
//   directly, replacing B2's `retire_fire || q4enc_direct_retire` OR-mux.
//
// ---- everything else (data format, mode encoding, Q8 chains, Q4 decode
// chain, backpressure/no-loss guarantee, error flag semantics, the
// tag_pipe mechanism itself, q4_scale_b2/fp32_div_iterative_exact) is
// unchanged from membrane_quant_stream_top_b2.sv -- see that file's own
// header and membrane_quant_stream_top.sv's header for the full original
// description, not repeated here except where it differs. ----
module membrane_quant_stream_top_b3 #(
	parameter int ID_WIDTH = 16,
	parameter int IN_FIFO_DEPTH = 16,
	parameter int OUT_FIFO_DEPTH = 32,
	parameter int REORDER_DEPTH = 4
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
	output	logic				out_error,

	// ---- debug/instrumentation-only outputs (task item 6's "stall
	// breakdown" / "queue high-water mark" ask), NOT part of the shared
	// in_*/out_* production contract -- carry no behavioral effect on
	// the datapath, exist purely so the Verilator testbench can observe
	// scheduler internals without associative signal probing (which is
	// fragile across Verilator versions; plain output ports are not).
	// A real integration of this experimental variant would simply leave
	// these unconnected.
	output	logic	[$clog2(REORDER_DEPTH+1)-1:0]	dbg_outstanding,
	output	logic				dbg_q4enc_inflight,
	output	logic				dbg_stall_depth_o,
	output	logic				dbg_stall_q4busy_o,
	output	logic				dbg_stall_outfifo_o,
	output	logic				dbg_simultaneous_completion_o
);

	localparam int L_MAX = 7;
	localparam int SEQ_WIDTH = 8;
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

	// ---- Q4_0 encode in-flight tracking: structural (one physical
	// divider instance), NOT the scheduling gate any more -- see header. ----
	logic	q4enc_inflight;

	// ---- completion reorder buffer (Phase B3's core change) ----
	logic	[SEQ_WIDTH-1:0]	issue_seq;
	logic			issue_allow;
	logic	[$clog2(REORDER_DEPTH+1)-1:0]	outstanding;

	logic	reorder_a_valid;
	logic	[SEQ_WIDTH-1:0]	reorder_a_seq;
	logic	[OUT_WORD_WIDTH-1:0]	reorder_a_payload;
	logic	reorder_b_valid;
	logic	[SEQ_WIDTH-1:0]	reorder_b_seq;
	logic	[OUT_WORD_WIDTH-1:0]	reorder_b_payload;

	logic	issue_fire;

	membrane_completion_reorder #(
		.DEPTH(REORDER_DEPTH), .SEQ_WIDTH(SEQ_WIDTH), .PAYLOAD_WIDTH(OUT_WORD_WIDTH)
	) u_reorder (
		.clk(clk), .rst_n(rst_n),
		.a_valid(reorder_a_valid), .a_seq(reorder_a_seq), .a_payload(reorder_a_payload),
		.b_valid(reorder_b_valid), .b_seq(reorder_b_seq), .b_payload(reorder_b_payload),
		.issue_fire(issue_fire), .issue_seq(issue_seq), .issue_allow(issue_allow),
		.out_valid(out_fifo_in_valid), .out_ready(out_fifo_in_ready), .out_payload(out_fifo_in_word),
		.outstanding(outstanding));

	// ---- issue gating: fewer than REORDER_DEPTH transactions outstanding
	// (any mode), a reserved output-FIFO slot, and (Q4_0 encode only) the
	// single physical divider free. No mode but Q4_0 encode has any
	// special case any more -- this is the actual fix, see header. ----
	int		occ_i;
	int		outst_i;
	logic	slot_ok;

	always_comb begin
		occ_i = out_fifo_occ;
		outst_i = outstanding;
		slot_ok = (occ_i + outst_i) < OUT_FIFO_DEPTH;
		if (mode_pop == MODE_Q4_ENC)
			issue_fire = in_fifo_out_valid && issue_allow && slot_ok && !q4enc_inflight;
		else
			issue_fire = in_fifo_out_valid && issue_allow && slot_ok;
	end

	assign in_fifo_out_ready = issue_fire;

	// Debug/instrumentation only (see port declarations above) -- lets
	// the C++ testbench classify WHY a pending transaction failed to
	// issue on a given cycle, per task item 6's "stall breakdown" ask.
	logic	dbg_stall_depth;
	logic	dbg_stall_q4busy;
	logic	dbg_stall_outfifo;

	assign dbg_stall_depth   = in_fifo_out_valid && !issue_allow;
	assign dbg_stall_outfifo = in_fifo_out_valid && issue_allow && !slot_ok;
	assign dbg_stall_q4busy  = in_fifo_out_valid && issue_allow && slot_ok
		&& (mode_pop == MODE_Q4_ENC) && q4enc_inflight;

	// Debug-only: both completion ports fired the same cycle -- the
	// scenario B2's own invariant forbade and B3's reorder buffer exists
	// specifically to make safe (task item 6's "simultaneous completions"
	// coverage point).
	logic	dbg_simultaneous_completion;

	assign dbg_outstanding = outstanding;
	assign dbg_q4enc_inflight = q4enc_inflight;
	assign dbg_stall_depth_o = dbg_stall_depth;
	assign dbg_stall_q4busy_o = dbg_stall_q4busy;
	assign dbg_stall_outfifo_o = dbg_stall_outfifo;
	assign dbg_simultaneous_completion_o = dbg_simultaneous_completion;

	assign reorder_a_seq = tag_pipe_seq_sel;

	logic	tagpipe_issue_fire;
	assign tagpipe_issue_fire = issue_fire && (mode_pop != MODE_Q4_ENC);

	// ---- shared id/mode/seq/valid tag delay pipe (depth L_MAX), used by
	// the three fixed-latency modes exactly as in B2; widened by
	// SEQ_WIDTH to carry this transaction's global sequence number to its
	// completion, for the reorder buffer to place it correctly. ----
	localparam int TAG_W = 1 + 2 + ID_WIDTH + SEQ_WIDTH;
	logic	[TAG_W-1:0]	tag_pipe	[0:L_MAX-1];

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			for (int k = 0; k < L_MAX; k++)
				tag_pipe[k] <= '0;
		end else begin
			tag_pipe[0] <= {tagpipe_issue_fire, mode_pop, id_pop, issue_seq};
			for (int k = L_MAX - 1; k > 0; k--)
				tag_pipe[k] <= tag_pipe[k - 1];
		end
	end

	logic				retire_fire;
	logic	[1:0]		mode_sel;
	logic	[ID_WIDTH-1:0]	id_sel;
	logic	[SEQ_WIDTH-1:0]	tag_pipe_seq_sel;

	assign {retire_fire, mode_sel, id_sel, tag_pipe_seq_sel} = tag_pipe[L_MAX - 1];
	assign reorder_a_valid = retire_fire;
	assign dbg_simultaneous_completion = reorder_a_valid && reorder_b_valid;

	// ---- Q4_0 encode in-flight flag: set on issue, cleared when its own
	// datapath (q4_pack, see below) finally retires it -- unchanged from
	// B2 except this is no longer what gates other modes' issuance. ----
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
	// Q8_0 encode chain: BYTE-IDENTICAL to membrane_quant_stream_top.sv --
	// maxabs(5) -> scale(1) -> quantize_pack(1) = 7 = L_MAX, still
	// retiring through the shared tag_pipe. Can now issue concurrently
	// with an in-flight Q4_0 encode -- that is the whole point of B3.
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
	// Q8_0 decode chain: BYTE-IDENTICAL -- dequantize(1), padded by 6 to
	// reach L_MAX=7, still retiring through the shared tag_pipe.
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
	// Q4_0 encode chain (unchanged from Phase B2): scan(0, combinational)
	// -> q4_scale_b2 (VARIABLE latency, iterative exact divider) ->
	// pack(2). Retires into the reorder buffer's port B, not directly to
	// the output FIFO any more (B2's `q4enc_direct_retire` concept is
	// still real -- it is just port B of a shared arbiter now, not an OR
	// with an invariant that the two sides never collide).
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

	// x_in/id/seq held stable from issue until q4_scale_valid finally
	// fires (variable latency) -- same convention as B2, plus the
	// sequence tag this phase adds. Captured only at issue
	// (q4enc_valid_in), safe because only one Q4_0 encode transaction is
	// ever in flight (q4enc_inflight, structural, section 4 of the
	// root-cause doc).
	logic	[15:0]	q4enc_x_hold	[0:31];
	logic	[ID_WIDTH-1:0]	q4enc_id_hold;
	logic	[SEQ_WIDTH-1:0]	q4enc_seq_hold;

	always_ff @(posedge clk) begin
		if (q4enc_valid_in) begin
			for (int j = 0; j < 32; j++)
				q4enc_x_hold[j] <= x_in_issue[j];
			q4enc_id_hold <= id_pop;
			q4enc_seq_hold <= issue_seq;
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
	// 2 cycles to match q4_pack's own FIXED latency -- unchanged from B2.
	logic	q4_err_raw;
	logic	[1:0]	q4_err_d2	[0:1];

	assign q4_err_raw = f16_is_special(q4_d_f16);

	always_ff @(posedge clk) begin
		q4_err_d2[0] <= {1'b0, q4_err_raw};
		q4_err_d2[1] <= q4_err_d2[0];
	end

	logic	q4enc_final_err;
	assign q4enc_final_err = q4_err_d2[1][0];

	assign reorder_b_valid   = q4_pack_valid;
	assign reorder_b_seq     = q4enc_seq_hold;
	assign reorder_b_payload = {MODE_Q4_ENC, q4enc_id_hold, {368'h0, q4_packed}, q4enc_final_err};

	// =====================================================================
	// Q4_0 decode chain: BYTE-IDENTICAL -- unpack(1), padded by 6 to reach
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

	// ---- tag_pipe completion payload: mode_sel picks which of the three
	// fixed-latency chains' own output register is live this cycle. ----
	logic	[511:0]	result_data;
	logic			result_error;

	always_comb begin
		if (mode_sel == MODE_Q8_ENC) begin
			result_data = {240'h0, q8_packed};
			result_error = q8_err_final;
		end else if (mode_sel == MODE_Q8_DEC) begin
			result_data = q8dec_final_data;
			result_error = q8dec_final_err;
		end else begin // MODE_Q4_DEC (MODE_Q4_ENC never appears in mode_sel)
			result_data = q4dec_final_data;
			result_error = q4dec_final_err;
		end
	end

	assign reorder_a_payload = {mode_sel, id_sel, result_data, result_error};

`ifndef SYNTHESIS
	always_ff @(posedge clk) begin
		if (rst_n && retire_fire) begin
			if (mode_sel == MODE_Q8_ENC)
				assert (q8_qp_valid)
					else $error("membrane_quant_stream_top_b3: Q8 encode latency mismatch at retire");
			else if (mode_sel == MODE_Q8_DEC)
				assert (q8dec_final_valid)
					else $error("membrane_quant_stream_top_b3: Q8 decode latency mismatch at retire");
			else
				assert (q4dec_final_valid)
					else $error("membrane_quant_stream_top_b3: Q4 decode latency mismatch at retire");
		end
		if (rst_n)
			assert (outstanding <= REORDER_DEPTH)
				else $error("membrane_quant_stream_top_b3: reorder buffer outstanding count out of range");
		// q4_scale_b2's own single-in-flight discipline (asserted inside
		// that module too) must still hold: a new Q4_0 encode may not be
		// issued while one is already in flight.
		if (rst_n && q4enc_valid_in)
			assert (!q4enc_inflight)
				else $error("membrane_quant_stream_top_b3: Q4_0 encode issued while another was still in flight -- single-divider invariant broken");
	end
`endif
endmodule
