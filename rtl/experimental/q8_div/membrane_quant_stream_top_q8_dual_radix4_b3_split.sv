// EXP-FPGA-DIV-002 Phase B3, candidate D: mode-split ingress queues.
// Builds directly on Phase B2's own scheduler
// (rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4_b2.sv,
// NOT modified/overwritten by this file) -- every retirement mechanism
// (global sequence tags, per-engine hold registers, the shared
// shadow_hold queue, tag_pipe's own never-frozen shift discipline) is
// reused UNCHANGED. The ONE thing this file changes is REPLACING the
// single 16-deep `in_fifo` with TWO smaller, physically separate ingress
// queues: `enc_fifo` (8 deep, carries Q8_0 AND Q4_0 encode transactions)
// and `dec_fifo` (8 deep, carries Q8_0 AND Q4_0 decode transactions) --
// task item 2 candidate D's own "one small queue for encode-class, one
// small queue for decode-class," total capacity 16, exactly comparable
// to (not exceeding) the original single `IN_FIFO_DEPTH`. A single
// external `in_valid`/`in_id`/`in_mode`/`in_data` port still exists (the
// public interface is unchanged); each accepted transaction is assigned
// its global sequence tag at the MOMENT of external acceptance (still
// strict arrival order, a single shared `issue_seq_ctr`), then routed by
// mode into whichever queue matches.
//
// ---- why this can issue TWO transactions per cycle (unlike the
// lookahead candidates, capped at one) ----
// `enc_fifo`'s own head and `dec_fifo`'s own head are checked and issued
// completely independently every cycle -- they use disjoint hardware
// (`q8_scale_dual_radix4`/`q4_scale` vs. `q8_dequantize`/`q4_unpack`/
// `tag_pipe`), so there is no reason to serialize their ACCEPTANCE
// decisions the way the lookahead candidates' own single-window-slot-
// per-cycle discipline does. This is the one place genuine multi-issue-
// per-cycle concurrency is explored in this phase (task item 2's own
// framing: split queues are architecturally distinct from bounded
// lookahead, not just a smaller/cheaper version of it).
//
// ---- a real, disclosed residual head-of-line cost THIS candidate does
// NOT solve (see phase-b3.md for the measured comparison) ----
// `enc_fifo` itself still has only ONE head -- if that head is, say, a
// Q8_0 encode transaction whose engine is busy, a resource-independent
// Q4_0 encode transaction sitting right behind it in the SAME `enc_fifo`
// is still head-of-line blocked (this candidate adds no lookahead within
// a queue, exactly as task item 2 candidate D describes -- physical
// separation between encode-class and decode-class, nothing more). This
// is a real, smaller-scope version of the original problem, not a full
// solution to it -- disclosed, not hidden, and directly measured against
// the lookahead candidates in results/b3-performance.csv.
//
// ---- ordering (identical guarantee to every other Phase B2/B3
// candidate) ----
// Global sequence tags are assigned in strict EXTERNAL ARRIVAL order
// (the single `in_valid`/`in_ready` port accepts exactly one transaction
// per cycle, same as ever), regardless of which queue a transaction is
// routed into or which queue's head issues first -- retirement (a
// transaction may write to the output FIFO only when its own `seq`
// equals `next_retire_seq`) is exactly as strict as every other
// candidate's own.
module membrane_quant_stream_top_q8_dual_radix4_b3_split #(
	parameter int ID_WIDTH = 16,
	parameter int ENC_FIFO_DEPTH = 8,
	parameter int DEC_FIFO_DEPTH = 8,
	parameter int OUT_FIFO_DEPTH = 32,
	parameter int SHADOW_DEPTH = 4
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
	localparam int SEQ_WIDTH = 8;
	localparam logic [1:0] MODE_Q8_ENC = 2'b00;
	localparam logic [1:0] MODE_Q8_DEC = 2'b01;
	localparam logic [1:0] MODE_Q4_ENC = 2'b10;
	localparam logic [1:0] MODE_Q4_DEC = 2'b11;

	function automatic logic f16_is_special(input logic [15:0] w);
		f16_is_special = (w[14:10] == 5'h1F);
	endfunction

	// ---- global sequence-number assignment at EXTERNAL acceptance time
	// (task item 2 candidate D's own "global sequence tag preserves
	// retirement order"). ----
	logic	[SEQ_WIDTH-1:0]	issue_seq_ctr;
	logic	[SEQ_WIDTH-1:0]	next_retire_seq;
	logic	retire_any_fire;

	logic	q8enc_busy, q4enc_busy;
	logic	q8enc_hold_valid, q4enc_hold_valid;
	logic	q8enc_pending, q4enc_pending;
	logic	primary_pending;

	assign q8enc_pending = q8enc_busy || q8enc_hold_valid;
	assign q4enc_pending = q4enc_busy || q4enc_hold_valid;
	assign primary_pending = q8enc_pending || q4enc_pending;

	localparam int SHADOW_CNT_W = $clog2(SHADOW_DEPTH + 1);
	logic	[SHADOW_CNT_W-1:0]	shadow_reserved_count;
	logic	shadow_hold_valid	[0:SHADOW_DEPTH-1];
	logic	tagpipe_can_issue;

	// blocking_condition predicts whether a dec_fifo entry issuing THIS
	// cycle might not be next_retire_seq's turn by the time it reaches the
	// tag_pipe tail. primary_pending alone is insufficient here (unlike
	// the single-ingress B1/B2/lookahead designs): with two independent
	// ingress queues, an older encode-class transaction can be sitting
	// unissued in enc_fifo (primary_pending==0, engine idle, nothing held)
	// while a younger dec_fifo entry issues -- that younger entry can then
	// reach the tail before the older encode transaction retires. Any
	// already-backlogged decode entry (shadow_reserved_count>0) carries
	// the same risk transitively until the backlog fully drains.
	logic	blocking_condition;
	assign blocking_condition = primary_pending || (enc_fifo_occ > 0) || (shadow_reserved_count > 0);

	assign tagpipe_can_issue = !blocking_condition || (shadow_reserved_count < SHADOW_DEPTH);

	logic	[SHADOW_CNT_W-1:0]	shadow_hold_occ;

	always_comb begin
		shadow_hold_occ = '0;
		for (int k = 0; k < SHADOW_DEPTH; k++)
			if (shadow_hold_valid[k])
				shadow_hold_occ = shadow_hold_occ + 1;
	end

	// ---- two small mode-split ingress queues (task item 2 candidate D). ----
	localparam int Q_WORD_WIDTH = 2 + ID_WIDTH + 512 + SEQ_WIDTH;

	logic	enc_dest;
	assign enc_dest = (in_mode == MODE_Q8_ENC) || (in_mode == MODE_Q4_ENC);

	logic	enc_fifo_in_valid, enc_fifo_in_ready, enc_fifo_out_valid, enc_fifo_out_ready;
	logic	[Q_WORD_WIDTH-1:0]	enc_fifo_in_word, enc_fifo_out_word;
	logic	[$clog2(ENC_FIFO_DEPTH):0]	enc_fifo_occ;

	logic	dec_fifo_in_valid, dec_fifo_in_ready, dec_fifo_out_valid, dec_fifo_out_ready;
	logic	[Q_WORD_WIDTH-1:0]	dec_fifo_in_word, dec_fifo_out_word;
	logic	[$clog2(DEC_FIFO_DEPTH):0]	dec_fifo_occ;

	logic	slot_ok;
	int		enc_fifo_occ_i, dec_fifo_occ_i;
	int		occ_i;
	int		flight_i;

	// Reservation accounting generalized from Phase B2's own: out_fifo
	// occupancy + tag_pipe in-flight credit (in_flight, tracked below,
	// same mechanism as every other Phase B2/B3 candidate) + encode-
	// engine pending + shadow_hold occupancy + BOTH ingress queues' own
	// occupancy (task item 2 candidate D's own two small queues, both
	// counted since either can hold a transaction already committed to
	// eventually retiring).
	always_comb begin
		occ_i = out_fifo_occ;
		flight_i = in_flight;
		enc_fifo_occ_i = enc_fifo_occ;
		dec_fifo_occ_i = dec_fifo_occ;
		slot_ok = (occ_i + flight_i + (q8enc_pending ? 1 : 0)
			+ (q4enc_pending ? 1 : 0) + shadow_hold_occ + enc_fifo_occ_i + dec_fifo_occ_i) < OUT_FIFO_DEPTH;
	end

	assign enc_fifo_in_valid = in_valid && enc_dest && slot_ok;
	assign dec_fifo_in_valid = in_valid && !enc_dest && slot_ok;
	assign in_ready = slot_ok && (enc_dest ? enc_fifo_in_ready : dec_fifo_in_ready);
	assign enc_fifo_in_word = {in_mode, in_id, in_data, issue_seq_ctr};
	assign dec_fifo_in_word = {in_mode, in_id, in_data, issue_seq_ctr};

	stream_fifo #(.WIDTH(Q_WORD_WIDTH), .DEPTH(ENC_FIFO_DEPTH)) u_enc_fifo (
		.clk(clk), .rst_n(rst_n),
		.in_valid(enc_fifo_in_valid), .in_ready(enc_fifo_in_ready), .in_data(enc_fifo_in_word),
		.out_valid(enc_fifo_out_valid), .out_ready(enc_fifo_out_ready),
		.out_data(enc_fifo_out_word), .occupancy(enc_fifo_occ));

	stream_fifo #(.WIDTH(Q_WORD_WIDTH), .DEPTH(DEC_FIFO_DEPTH)) u_dec_fifo (
		.clk(clk), .rst_n(rst_n),
		.in_valid(dec_fifo_in_valid), .in_ready(dec_fifo_in_ready), .in_data(dec_fifo_in_word),
		.out_valid(dec_fifo_out_valid), .out_ready(dec_fifo_out_ready),
		.out_data(dec_fifo_out_word), .occupancy(dec_fifo_occ));

	logic	[1:0]			enc_mode_pop;
	logic	[ID_WIDTH-1:0]	enc_id_pop;
	logic	[511:0]			enc_data_pop;
	logic	[SEQ_WIDTH-1:0]	enc_seq_pop;

	assign {enc_mode_pop, enc_id_pop, enc_data_pop, enc_seq_pop} = enc_fifo_out_word;

	logic	[1:0]			dec_mode_pop;
	logic	[ID_WIDTH-1:0]	dec_id_pop;
	logic	[511:0]			dec_data_pop;
	logic	[SEQ_WIDTH-1:0]	dec_seq_pop;

	assign {dec_mode_pop, dec_id_pop, dec_data_pop, dec_seq_pop} = dec_fifo_out_word;

	// ---- output FIFO (unchanged). ----
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

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			issue_seq_ctr <= '0;
		else if (in_valid && in_ready)
			issue_seq_ctr <= issue_seq_ctr + 1'b1;
	end

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			next_retire_seq <= '0;
		else if (retire_any_fire)
			next_retire_seq <= next_retire_seq + 1'b1;
	end

	// ---- enc_fifo head issuance: Q8_0 encode may issue only when its own
	// engine is idle; Q4_0 encode likewise, independently. ----
	logic	enc_issue_fire;
	logic	q8enc_valid_in, q4enc_valid_in;

	always_comb begin
		if (enc_mode_pop == MODE_Q8_ENC)
			enc_issue_fire = enc_fifo_out_valid && !q8enc_pending;
		else // MODE_Q4_ENC (enc_fifo never carries a decode-class mode)
			enc_issue_fire = enc_fifo_out_valid && !q4enc_pending;
	end
	assign enc_fifo_out_ready = enc_issue_fire;
	assign q8enc_valid_in = enc_issue_fire && (enc_mode_pop == MODE_Q8_ENC);
	assign q4enc_valid_in = enc_issue_fire && (enc_mode_pop == MODE_Q4_ENC);

	// ---- dec_fifo head issuance: same shadow-gated admission as every
	// other Phase B2/B3 candidate. ----
	logic	tagpipe_issue_fire;
	assign tagpipe_issue_fire = dec_fifo_out_valid && tagpipe_can_issue;
	assign dec_fifo_out_ready = tagpipe_issue_fire;

	// ---- shared id/mode/valid/seq/is_extra tag delay pipe (unchanged
	// mechanism from Phase B2 -- fed from dec_fifo's own head). ----
	localparam int TAG_W = 1 + 2 + ID_WIDTH + SEQ_WIDTH + 1;
	logic	[TAG_W-1:0]	tag_pipe	[0:L_MAX-1];

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			for (int k = 0; k < L_MAX; k++)
				tag_pipe[k] <= '0;
		end else begin
			tag_pipe[0] <= {tagpipe_issue_fire, dec_mode_pop, dec_id_pop, dec_seq_pop, blocking_condition};
			for (int k = L_MAX - 1; k > 0; k--)
				tag_pipe[k] <= tag_pipe[k - 1];
		end
	end

	logic				retire_fire;
	logic	[1:0]		mode_sel;
	logic	[ID_WIDTH-1:0]	id_sel;
	logic	[SEQ_WIDTH-1:0]	seq_sel;
	logic				is_extra_sel;

	assign {retire_fire, mode_sel, id_sel, seq_sel, is_extra_sel} = tag_pipe[L_MAX - 1];

	logic	tagpipe_direct_can_retire;
	logic	tagpipe_needs_shadow_capture;

	assign tagpipe_direct_can_retire = retire_fire && (seq_sel == next_retire_seq);
	assign tagpipe_needs_shadow_capture = retire_fire && is_extra_sel && (seq_sel != next_retire_seq);

	int		in_flight;

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			in_flight <= 0;
		else if (tagpipe_issue_fire && !retire_fire)
			in_flight <= in_flight + 1;
		else if (!tagpipe_issue_fire && retire_fire)
			in_flight <= in_flight - 1;
	end

	logic	shadow_clear_direct;
	logic	shadow_any_hold_retire;

	assign shadow_clear_direct = tagpipe_direct_can_retire && is_extra_sel;

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			shadow_reserved_count <= '0;
		else if ((tagpipe_issue_fire && blocking_condition)
				&& !(shadow_clear_direct || shadow_any_hold_retire))
			shadow_reserved_count <= shadow_reserved_count + 1;
		else if (!(tagpipe_issue_fire && blocking_condition)
				&& (shadow_clear_direct || shadow_any_hold_retire))
			shadow_reserved_count <= shadow_reserved_count - 1;
	end

	// ---- shared 32-lane F16 unpack of the enc_fifo-selected transaction. ----
	logic	[15:0]	x_in_issue	[0:31];

	assign x_in_issue[0] = enc_data_pop[15:0];
	assign x_in_issue[1] = enc_data_pop[31:16];
	assign x_in_issue[2] = enc_data_pop[47:32];
	assign x_in_issue[3] = enc_data_pop[63:48];
	assign x_in_issue[4] = enc_data_pop[79:64];
	assign x_in_issue[5] = enc_data_pop[95:80];
	assign x_in_issue[6] = enc_data_pop[111:96];
	assign x_in_issue[7] = enc_data_pop[127:112];
	assign x_in_issue[8] = enc_data_pop[143:128];
	assign x_in_issue[9] = enc_data_pop[159:144];
	assign x_in_issue[10] = enc_data_pop[175:160];
	assign x_in_issue[11] = enc_data_pop[191:176];
	assign x_in_issue[12] = enc_data_pop[207:192];
	assign x_in_issue[13] = enc_data_pop[223:208];
	assign x_in_issue[14] = enc_data_pop[239:224];
	assign x_in_issue[15] = enc_data_pop[255:240];
	assign x_in_issue[16] = enc_data_pop[271:256];
	assign x_in_issue[17] = enc_data_pop[287:272];
	assign x_in_issue[18] = enc_data_pop[303:288];
	assign x_in_issue[19] = enc_data_pop[319:304];
	assign x_in_issue[20] = enc_data_pop[335:320];
	assign x_in_issue[21] = enc_data_pop[351:336];
	assign x_in_issue[22] = enc_data_pop[367:352];
	assign x_in_issue[23] = enc_data_pop[383:368];
	assign x_in_issue[24] = enc_data_pop[399:384];
	assign x_in_issue[25] = enc_data_pop[415:400];
	assign x_in_issue[26] = enc_data_pop[431:416];
	assign x_in_issue[27] = enc_data_pop[447:432];
	assign x_in_issue[28] = enc_data_pop[463:448];
	assign x_in_issue[29] = enc_data_pop[479:464];
	assign x_in_issue[30] = enc_data_pop[495:480];
	assign x_in_issue[31] = enc_data_pop[511:496];

	// =====================================================================
	// Q8_0 encode chain: UNCHANGED from Phase B2, fed from enc_fifo's head.
	// =====================================================================
	logic			q8_maxabs_valid;
	logic	[15:0]	q8_amax_f16;

	q8_maxabs_reduce u_q8_maxabs (
		.clk(clk), .rst_n(rst_n), .valid_in(q8enc_valid_in), .x_in_flat(enc_data_pop),
		.valid_out(q8_maxabs_valid), .amax_f16_out(q8_amax_f16));

	logic	[15:0]	q8_x_d	[0:4][0:31];

	always_ff @(posedge clk) begin
		for (int j = 0; j < 32; j++)
			q8_x_d[0][j] <= x_in_issue[j];
		for (int k = 4; k > 0; k--)
			for (int j = 0; j < 32; j++)
				q8_x_d[k][j] <= q8_x_d[k - 1][j];
	end

	logic	[15:0]	q8enc_x_hold	[0:31];
	logic	[ID_WIDTH-1:0]	q8enc_id_hold;
	logic	[SEQ_WIDTH-1:0]	q8enc_issue_seq;

	always_ff @(posedge clk) begin
		if (q8enc_valid_in) begin
			q8enc_id_hold <= enc_id_pop;
			q8enc_issue_seq <= enc_seq_pop;
		end
		if (q8_maxabs_valid)
			for (int j = 0; j < 32; j++)
				q8enc_x_hold[j] <= q8_x_d[4][j];
	end

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			q8enc_busy <= 1'b0;
		else if (q8enc_valid_in)
			q8enc_busy <= 1'b1;
		else if (q8_qp_valid)
			q8enc_busy <= 1'b0;
	end

	logic			q8_scale_valid;
	logic			q8_scale_in_ready;
	logic			q8_scale_busy;
	logic	[15:0]	q8_d_f16;
	logic	[31:0]	q8_id_f32;

	q8_scale_dual_radix4 u_q8_scale (
		.clk(clk), .rst_n(rst_n),
		.in_valid(q8_maxabs_valid), .in_ready(q8_scale_in_ready),
		.amax_f16_in(q8_amax_f16),
		.out_valid(q8_scale_valid), .out_ready(1'b1),
		.d_f16_out(q8_d_f16), .id_f32_out(q8_id_f32), .busy(q8_scale_busy));

	logic	[511:0]	q8_x_final_flat;

	always_comb
		for (int j = 0; j < 32; j++)
			q8_x_final_flat[j * 16 +: 16] = q8enc_x_hold[j];

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

	logic	[ID_WIDTH-1:0]	q8enc_hold_id;
	logic	[271:0]	q8enc_hold_data;
	logic			q8enc_hold_err;
	logic	[SEQ_WIDTH-1:0]	q8enc_hold_seq;
	logic			q8enc_can_retire;

	assign q8enc_can_retire = q8enc_hold_valid && (q8enc_hold_seq == next_retire_seq);

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			q8enc_hold_valid <= 1'b0;
		end else if (q8enc_can_retire) begin
			q8enc_hold_valid <= 1'b0;
		end else if (q8_qp_valid) begin
			q8enc_hold_valid <= 1'b1;
			q8enc_hold_id <= q8enc_id_hold;
			q8enc_hold_data <= q8_packed;
			q8enc_hold_err <= q8_err_final;
			q8enc_hold_seq <= q8enc_issue_seq;
		end
	end

	// =====================================================================
	// Q8_0 decode chain: UNCHANGED from Phase B2, fed from dec_fifo's head.
	// =====================================================================
	logic	q8dec_valid_in;
	assign q8dec_valid_in = tagpipe_issue_fire && (dec_mode_pop == MODE_Q8_DEC);

	logic			q8_dq_valid;
	logic	[511:0]	q8_dq_out;

	q8_dequantize #(.MUL_DELAY(1)) u_q8_dq (
		.clk(clk), .rst_n(rst_n), .valid_in(q8dec_valid_in),
		.packed_in(dec_data_pop[271:0]), .valid_out(q8_dq_valid),
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
	// Q4_0 encode chain: UNCHANGED from Phase B2, fed from enc_fifo's head.
	// =====================================================================
	logic	[31:0]	q4_mx_f32;

	q4_scan u_q4_scan (.x_in_flat(enc_data_pop), .mx_f32_out(q4_mx_f32));

	logic			q4_scale_valid;
	logic	[15:0]	q4_d_f16;
	logic	[31:0]	q4_id_f32;
	logic			q4_scale_busy;

	q4_scale #(.DIV_DELAY(1)) u_q4_scale (
		.clk(clk), .rst_n(rst_n), .valid_in(q4enc_valid_in),
		.mx_f32(q4_mx_f32), .valid_out(q4_scale_valid),
		.d_f16_out(q4_d_f16), .id_f32_out(q4_id_f32), .busy(q4_scale_busy));

	logic	[15:0]	q4enc_x_hold	[0:31];
	logic	[ID_WIDTH-1:0]	q4enc_id_hold;
	logic	[SEQ_WIDTH-1:0]	q4enc_issue_seq;

	always_ff @(posedge clk) begin
		if (q4enc_valid_in) begin
			for (int j = 0; j < 32; j++)
				q4enc_x_hold[j] <= x_in_issue[j];
			q4enc_id_hold <= enc_id_pop;
			q4enc_issue_seq <= enc_seq_pop;
		end
	end

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			q4enc_busy <= 1'b0;
		else if (q4enc_valid_in)
			q4enc_busy <= 1'b1;
		else if (q4_pack_valid)
			q4enc_busy <= 1'b0;
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

	logic	q4_err_raw;
	logic	[1:0]	q4_err_d2	[0:1];

	assign q4_err_raw = f16_is_special(q4_d_f16);

	always_ff @(posedge clk) begin
		q4_err_d2[0] <= {1'b0, q4_err_raw};
		q4_err_d2[1] <= q4_err_d2[0];
	end

	logic	q4enc_final_err;
	assign q4enc_final_err = q4_err_d2[1][0];

	logic	[ID_WIDTH-1:0]	q4enc_hold_id;
	logic	[143:0]	q4enc_hold_data;
	logic			q4enc_hold_err;
	logic	[SEQ_WIDTH-1:0]	q4enc_hold_seq;
	logic			q4enc_can_retire;

	assign q4enc_can_retire = q4enc_hold_valid && (q4enc_hold_seq == next_retire_seq);

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			q4enc_hold_valid <= 1'b0;
		end else if (q4enc_can_retire) begin
			q4enc_hold_valid <= 1'b0;
		end else if (q4_pack_valid) begin
			q4enc_hold_valid <= 1'b1;
			q4enc_hold_id <= q4enc_id_hold;
			q4enc_hold_data <= q4_packed;
			q4enc_hold_err <= q4enc_final_err;
			q4enc_hold_seq <= q4enc_issue_seq;
		end
	end

	// =====================================================================
	// Q4_0 decode chain: UNCHANGED from Phase B2, fed from dec_fifo's head.
	// =====================================================================
	logic	q4dec_valid_in;
	assign q4dec_valid_in = tagpipe_issue_fire && (dec_mode_pop == MODE_Q4_DEC);

	logic			q4_uq_valid;
	logic	[511:0]	q4_uq_out;

	q4_unpack #(.MUL_DELAY(1)) u_q4_uq (
		.clk(clk), .rst_n(rst_n), .valid_in(q4dec_valid_in),
		.packed_in(dec_data_pop[143:0]), .valid_out(q4_uq_valid),
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

	// ---- tag_pipe result mux (unchanged from Phase B2). ----
	logic	[511:0]	result_data;
	logic			result_error;

	always_comb begin
		if (mode_sel == MODE_Q8_DEC) begin
			result_data = q8dec_final_data;
			result_error = q8dec_final_err;
		end else begin // MODE_Q4_DEC
			result_data = q4dec_final_data;
			result_error = q4dec_final_err;
		end
	end

	// ---- shadow_hold (unchanged mechanism from Phase B2). ----
	logic	[1:0]		shadow_hold_mode	[0:SHADOW_DEPTH-1];
	logic	[ID_WIDTH-1:0]	shadow_hold_id		[0:SHADOW_DEPTH-1];
	logic	[511:0]	shadow_hold_data	[0:SHADOW_DEPTH-1];
	logic			shadow_hold_err		[0:SHADOW_DEPTH-1];
	logic	[SEQ_WIDTH-1:0]	shadow_hold_seq		[0:SHADOW_DEPTH-1];
	logic			shadow_can_retire	[0:SHADOW_DEPTH-1];
	int		shadow_retire_match_count;
	int		shadow_capture_slot;
	logic	shadow_capture_slot_valid;

	always_comb begin
		shadow_capture_slot = 0;
		shadow_capture_slot_valid = 1'b0;
		for (int k = 0; k < SHADOW_DEPTH; k++)
			if (!shadow_hold_valid[k] && !shadow_capture_slot_valid) begin
				shadow_capture_slot = k;
				shadow_capture_slot_valid = 1'b1;
			end
	end

	always_comb begin
		shadow_any_hold_retire = 1'b0;
		shadow_retire_match_count = 0;
		for (int k = 0; k < SHADOW_DEPTH; k++) begin
			shadow_can_retire[k] = shadow_hold_valid[k] && (shadow_hold_seq[k] == next_retire_seq);
			if (shadow_can_retire[k]) begin
				shadow_any_hold_retire = 1'b1;
				shadow_retire_match_count = shadow_retire_match_count + 1;
			end
		end
	end

	logic	[1:0]		shadow_retire_mode;
	logic	[ID_WIDTH-1:0]	shadow_retire_id;
	logic	[511:0]	shadow_retire_data;
	logic			shadow_retire_err;

	always_comb begin
		shadow_retire_mode = MODE_Q8_DEC;
		shadow_retire_id = '0;
		shadow_retire_data = '0;
		shadow_retire_err = 1'b0;
		for (int k = 0; k < SHADOW_DEPTH; k++)
			if (shadow_can_retire[k]) begin
				shadow_retire_mode = shadow_hold_mode[k];
				shadow_retire_id = shadow_hold_id[k];
				shadow_retire_data = shadow_hold_data[k];
				shadow_retire_err = shadow_hold_err[k];
			end
	end

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			for (int k = 0; k < SHADOW_DEPTH; k++)
				shadow_hold_valid[k] <= 1'b0;
		end else begin
			for (int k = 0; k < SHADOW_DEPTH; k++) begin
				if (shadow_can_retire[k]) begin
					shadow_hold_valid[k] <= 1'b0;
				end else if (tagpipe_needs_shadow_capture && shadow_capture_slot_valid
						&& (shadow_capture_slot == k)) begin
					shadow_hold_valid[k] <= 1'b1;
					shadow_hold_mode[k] <= mode_sel;
					shadow_hold_id[k] <= id_sel;
					shadow_hold_data[k] <= result_data;
					shadow_hold_err[k] <= result_error;
					shadow_hold_seq[k] <= seq_sel;
				end
			end
		end
	end

	assign retire_any_fire = q8enc_can_retire || q4enc_can_retire
		|| shadow_any_hold_retire || tagpipe_direct_can_retire;
	assign out_fifo_in_valid = retire_any_fire;

	always_comb begin
		if (q8enc_can_retire)
			out_fifo_in_word = {MODE_Q8_ENC, q8enc_hold_id, {240'h0, q8enc_hold_data}, q8enc_hold_err};
		else if (q4enc_can_retire)
			out_fifo_in_word = {MODE_Q4_ENC, q4enc_hold_id, {368'h0, q4enc_hold_data}, q4enc_hold_err};
		else if (shadow_any_hold_retire)
			out_fifo_in_word = {shadow_retire_mode, shadow_retire_id, shadow_retire_data, shadow_retire_err};
		else
			out_fifo_in_word = {mode_sel, id_sel, result_data, result_error};
	end

	// =====================================================================
	// Formal/state invariants (task item 6). Simulation-only, guarded by
	// `ifndef SYNTHESIS exactly like every other phase in this project.
	// =====================================================================
`ifndef SYNTHESIS
	logic	[31:0]	accepted_count;
	logic	[31:0]	completed_count;
	logic	[SEQ_WIDTH-1:0]	live_seq_count;
	assign live_seq_count = issue_seq_ctr - next_retire_seq;

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			accepted_count <= '0;
			completed_count <= '0;
		end else begin
			if (in_valid && in_ready)
				accepted_count <= accepted_count + 1'b1;
			if (retire_any_fire)
				completed_count <= completed_count + 1'b1;
		end
	end

	always_ff @(posedge clk) begin
		if (rst_n) begin
			assert (accepted_count >= completed_count)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: completed_count exceeded accepted_count");
			assert ((accepted_count - completed_count) <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: more transactions in flight than reserved output capacity");
			assert (live_seq_count <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: live transaction count approaching seq tag wraparound capacity");
			if (q8enc_can_retire)
				assert (q8enc_hold_seq == next_retire_seq)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: q8enc retired out of order");
			if (q4enc_can_retire)
				assert (q4enc_hold_seq == next_retire_seq)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: q4enc retired out of order");
			for (int k = 0; k < SHADOW_DEPTH; k++)
				if (shadow_can_retire[k])
					assert (shadow_hold_seq[k] == next_retire_seq)
						else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: shadow_hold retired out of order");
			if (tagpipe_direct_can_retire)
				assert (seq_sel == next_retire_seq)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: tag_pipe direct retire out of order");
			assert ($countones({q8enc_can_retire, q4enc_can_retire,
				shadow_any_hold_retire, tagpipe_direct_can_retire}) <= 1)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: multiple retire sources matched next_retire_seq simultaneously");
			assert (shadow_retire_match_count <= 1)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: multiple shadow_hold slots matched next_retire_seq simultaneously");
			for (int k = 0; k < SHADOW_DEPTH; k++)
				if (shadow_hold_valid[k])
					assert (shadow_hold_mode[k] == MODE_Q8_DEC || shadow_hold_mode[k] == MODE_Q4_DEC)
						else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: shadow_hold captured a non-tag_pipe mode");
			assert (in_flight >= 0 && in_flight <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: in_flight credit counter out of range");
			if (q8_maxabs_valid)
				assert (q8_scale_in_ready)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: q8_scale_dual_radix4 not ready when q8_maxabs_valid pulsed");
			// enc_fifo carries only encode-class modes, dec_fifo only
			// decode-class modes -- routing correctness (task item 6:
			// "no decode result lost while an encode is pending" implies
			// the two classes' storage must never cross-contaminate).
			if (enc_fifo_out_valid)
				assert (enc_mode_pop == MODE_Q8_ENC || enc_mode_pop == MODE_Q4_ENC)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: enc_fifo carried a non-encode mode");
			if (dec_fifo_out_valid)
				assert (dec_mode_pop == MODE_Q8_DEC || dec_mode_pop == MODE_Q4_DEC)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: dec_fifo carried a non-decode mode");
		end
	end

	logic	rst_n_d1;
	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			rst_n_d1 <= 1'b0;
		else
			rst_n_d1 <= 1'b1;
	end

	always_ff @(posedge clk) begin
		if (rst_n && !rst_n_d1) begin
			assert (issue_seq_ctr == '0 && next_retire_seq == '0
				&& !q8enc_busy && !q8enc_hold_valid
				&& !q4enc_busy && !q4enc_hold_valid
				&& shadow_hold_occ == 0 && shadow_reserved_count == '0
				&& in_flight == 0 && enc_fifo_occ == 0 && dec_fifo_occ == 0)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_split: reset did not clear all live scheduler state");
		end
	end
`endif
endmodule
