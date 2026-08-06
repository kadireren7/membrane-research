// EXP-FPGA-DIV-002 Phase B2: scheduler-improved variant of Phase B1's
// membrane_quant_stream_top_q8_dual_radix4.sv (rtl/experimental/q8_div/
// membrane_quant_stream_top_q8_dual_radix4.sv, NOT modified/overwritten by
// this file). Same Q8_0 datapath (q8_maxabs_reduce -> q8_scale_dual_radix4
// -> q8_quantize_pack, q8_scale_dual_radix4 itself byte-for-byte reused
// unmodified, same 4,052,224-case Phase B1 differential result still
// applies), Q4_0/Q8_0 decode chains unchanged, Q4_0 encode chain
// unchanged. The ONE thing this file changes is the SCHEDULER: Phase
// B1's blanket "block ALL other issuance while a Q8_0 or Q4_0 encode
// transaction is in flight" is replaced by a resource-aware scheduler
// that lets Q8_0 decode, Q4_0 decode, and the OTHER single-in-flight
// encode class each issue independently, while still preserving strict
// global in-order retirement (see results/b2-stall-root-cause.md section
// 0 for why that ordering contract is confirmed, not assumed, from
// rtl/membrane_quant_stream_top.sv's own header + live assertions).
//
// ---- the mechanism, precisely (see phase-b2.md for the full derivation
// of why this is the smallest correct change, and why several simpler-
// looking alternatives are NOT correct) ----
// Every transaction, of every mode, is assigned a strictly-increasing
// SEQ_WIDTH-bit `seq` tag the cycle it is issued (accepted out of the
// input FIFO into its own engine). A single free-running `next_retire_seq`
// counter names whose turn it currently is to write to the shared output
// FIFO; a transaction may retire ONLY when its own `seq` equals
// `next_retire_seq`, and `next_retire_seq` advances by exactly one on
// every retirement -- this is the ENTIRE in-order guarantee, independent
// of which engine/mode actually produced the result, independent of the
// order engines happen to FINISH in. `q8_scale_dual_radix4` and
// `q4_scale` (each single-in-flight, variable latency) now issue and run
// completely independently of each other and of Q8_0/Q4_0 decode (they
// share zero hardware, so there is no structural reason to serialize
// them) -- each keeps ONE small result-holding register
// (`q8enc_hold_*`/`q4enc_hold_*`) for the case its own result finishes
// before its turn comes; the register drains the instant its turn
// arrives. Q8_0 decode and Q4_0 decode still ride the existing shared,
// fixed-latency `tag_pipe`/`L_MAX` mechanism completely UNCHANGED (same
// depth, same per-mode padding, same unconditional every-cycle shift --
// this file adds a `seq` field to each `tag_pipe` stage but never stalls
// or freezes the shift register itself, which is what avoids a subtle
// data-loss hazard: freezing a plain shift register while an
// unconditional, un-backpressured producer (`q8_dequantize`/`q4_unpack`,
// each a fixed one-cycle pipeline with no ready signal) is about to write
// a fresh result into it would silently lose that result -- see
// phase-b2.md's "why not just stall tag_pipe" subsection for the full
// counter-example). Instead, `tag_pipe` keeps shifting every cycle,
// unconditionally, exactly as Phase B1/production do; the ONLY new
// behavior is at the tail: if the tail holds a real, completed entry
// whose `seq` is NOT yet `next_retire_seq` (this can only happen for the
// SINGLE tag_pipe entry admitted while some other engine was already
// outstanding -- see the `shadow_reserved` gate below, which caps this at
// exactly one at a time), that one entry's payload is captured into a
// SINGLE additional hold register, `shadow_hold_*`, instead of being
// lost, and drains the instant its turn comes. Total added state per this
// mechanism: two per-engine hold registers (Q8_0/Q4_0 encode, one each,
// matching the task's own "one pending Q8_ENC slot"/"small result holding
// register" allowance) plus ONE shared hold register for the tag_pipe
// classes (`shadow_hold`) -- not a general N-deep reorder buffer, and
// specifically NOT the depth-4/8 ROB sweep EXP-FPGA-DIV-001 Phase B3
// already evaluated and rejected for being disproportionately large (see
// that phase's own decision.md) -- this mechanism is smaller than even
// EXP-FPGA-DIV-001's smallest rejected configuration.
//
// ---- why a NEW tag_pipe-class transaction may only be admitted while
// blocking if the ONE shared shadow slot is free (`shadow_reserved`) ----
// If we let tag_pipe accept unlimited new entries while some Q8_0/Q4_0
// encode is outstanding, every one of them would eventually reach the
// tail before that (much longer-running) encode retires, each needing
// its own hold slot -- unbounded growth, exactly what this phase's task
// forbids ("no depth 4/8 ROB sweep," "bounded bookkeeping only").
// `shadow_reserved` is set the cycle a tag_pipe entry is admitted while
// `primary_pending` (some Q8_0/Q4_0 encode outstanding) is true, and
// stays set until THAT SPECIFIC entry fully drains (whether it turns out
// to retire directly at the tail, if the blocking encode happens to
// finish first, or via `shadow_hold`) -- while set, further tag_pipe
// admission is blocked (falls back to Phase-B1-style serialization for
// tag_pipe modes specifically) until it clears. Each admitted tag_pipe
// entry carries its own `is_extra` bit (captured = `primary_pending` at
// its own issue time) precisely so the tail can tell, without any
// separate tracking, whether the entry currently retiring IS the one
// `shadow_reserved` refers to.
//
// ---- Q8_0/Q4_0 encode mutual independence ----
// Unlike Phase B1 (`q8enc_inflight`/`q4enc_inflight` were mutually
// exclusive -- starting either required BOTH to be idle), this file lets
// Q8_0 encode and Q4_0 encode issue completely independently of each
// other (`q8enc_pending`/`q4enc_pending` each gate only their OWN
// single-in-flight discipline) -- they use disjoint hardware
// (`q8_scale_dual_radix4`'s two dividers vs. `q4_scale`'s own divider
// instance), so there is no structural reason to serialize them either.
// `primary_pending = q8enc_pending || q4enc_pending` (used only to gate
// the shared tag_pipe shadow slot) is unaffected by this relaxation.
//
// ---- what is deliberately NOT changed ----
// `q8_scale_dual_radix4`'s own internal FSM/handshake/rendezvous logic
// (byte-for-byte reused, see that file). The external port list, mode
// encoding, and 512-bit data format contract
// (docs/phase5-synthesizable-fpga.md). `L_MAX`/per-chain padding
// constants (unchanged from Phase B1/production -- no "latency-alignment
// bypass" was needed here, since decoupling retirement from tag_pipe's
// own fixed-padding shape via `seq` already solves the ordering problem
// without touching the padding itself).
module membrane_quant_stream_top_q8_dual_radix4_b2 #(
	parameter int ID_WIDTH = 16,
	parameter int IN_FIFO_DEPTH = 16,
	parameter int OUT_FIFO_DEPTH = 32,
	// Task item 10's own depth-1-vs-depth-2 shadow-slot sweep: 1 (default,
	// the primary design point -- "one pending Q8_ENC slot"/"small result
	// holding register" from this file's own header) or 2 ("at most a
	// 2-entry completion queue if strictly necessary"). No other value is
	// evaluated or supported (task item 10 explicitly forbids depth 4/8).
	// Overridden at elaboration time (Verilator `-GSHADOW_DEPTH=2` / Yosys
	// `chparam -set SHADOW_DEPTH 2`), not by editing this file per build --
	// see scripts/run-exp-q8-divider-002.sh's own --phase b2 handling.
	parameter int SHADOW_DEPTH = 1
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

`ifndef SYNTHESIS
	initial begin
		if (SHADOW_DEPTH != 1 && SHADOW_DEPTH != 2)
			$error("membrane_quant_stream_top_q8_dual_radix4_b2: SHADOW_DEPTH must be 1 or 2 (task item 10's own sweep scope -- no depth 4/8 evaluated)");
	end
`endif

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

	// =====================================================================
	// Global sequence-number in-order-retirement bookkeeping (task item 2/
	// item 5's "sequence tag/counter").
	// =====================================================================
	logic	[SEQ_WIDTH-1:0]	issue_seq_ctr;
	logic	[SEQ_WIDTH-1:0]	next_retire_seq;
	logic	retire_any_fire;

	// ---- Q8_0/Q4_0 encode pending tracking (busy = executing, hold_valid
	// = completed but awaiting its retirement turn). Each is single-
	// in-flight against ITSELF only -- independent of the other, per this
	// file's own header. ----
	logic	q8enc_busy, q4enc_busy;
	logic	q8enc_hold_valid, q4enc_hold_valid;
	logic	q8enc_pending, q4enc_pending;
	logic	primary_pending;

	assign q8enc_pending = q8enc_busy || q8enc_hold_valid;
	assign q4enc_pending = q4enc_busy || q4enc_hold_valid;
	assign primary_pending = q8enc_pending || q4enc_pending;

	// ---- shared tag_pipe "shadow" slot(s): at most SHADOW_DEPTH tag_pipe-
	// class (Q8_0/Q4_0 decode) transactions admitted while primary_pending
	// may be outstanding-and-unretired at a time (task item 10's own
	// depth-1/depth-2 sweep). shadow_reserved_count tracks how many such
	// admitted-under-blocking entries currently live ANYWHERE in the
	// system (still mid tag_pipe, at its tail, or already captured into a
	// shadow_hold slot) -- gates admission; shadow_hold_valid[] tracks
	// only the subset that have actually reached the tail and been
	// captured. ----
	localparam int SHADOW_CNT_W = $clog2(SHADOW_DEPTH + 1);
	logic	[SHADOW_CNT_W-1:0]	shadow_reserved_count;
	logic	shadow_hold_valid	[0:SHADOW_DEPTH-1];

	// ---- issue gating (mode-aware, task item 2) ----
	logic	issue_fire;
	logic	tagpipe_issue_fire;
	int		in_flight;
	int		occ_i;
	int		flight_i;
	logic	slot_ok;
	logic	tagpipe_can_issue;

	assign tagpipe_can_issue = !primary_pending || (shadow_reserved_count < SHADOW_DEPTH);

	logic	[SHADOW_CNT_W-1:0]	shadow_hold_occ;

	always_comb begin
		shadow_hold_occ = '0;
		for (int k = 0; k < SHADOW_DEPTH; k++)
			if (shadow_hold_valid[k])
				shadow_hold_occ = shadow_hold_occ + 1;
	end

	always_comb begin
		occ_i = out_fifo_occ;
		flight_i = in_flight;
		slot_ok = (occ_i + flight_i + (q8enc_pending ? 1 : 0)
			+ (q4enc_pending ? 1 : 0) + shadow_hold_occ) < OUT_FIFO_DEPTH;
		if (mode_pop == MODE_Q8_ENC)
			issue_fire = in_fifo_out_valid && slot_ok && !q8enc_pending;
		else if (mode_pop == MODE_Q4_ENC)
			issue_fire = in_fifo_out_valid && slot_ok && !q4enc_pending;
		else // MODE_Q8_DEC, MODE_Q4_DEC
			issue_fire = in_fifo_out_valid && slot_ok && tagpipe_can_issue;
	end

	assign tagpipe_issue_fire = issue_fire
		&& (mode_pop == MODE_Q8_DEC || mode_pop == MODE_Q4_DEC);
	assign in_fifo_out_ready = issue_fire;

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			issue_seq_ctr <= '0;
		else if (issue_fire)
			issue_seq_ctr <= issue_seq_ctr + 1'b1;
	end

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			next_retire_seq <= '0;
		else if (retire_any_fire)
			next_retire_seq <= next_retire_seq + 1'b1;
	end

	// ---- shared id/mode/valid/seq/is_extra tag delay pipe (depth L_MAX),
	// used ONLY by Q8_0 decode / Q4_0 decode. Always shifts unconditionally,
	// every cycle -- see this file's header for why freezing it would be
	// an actual correctness bug, not just a missed optimization. ----
	localparam int TAG_W = 1 + 2 + ID_WIDTH + SEQ_WIDTH + 1;
	logic	[TAG_W-1:0]	tag_pipe	[0:L_MAX-1];

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			for (int k = 0; k < L_MAX; k++)
				tag_pipe[k] <= '0;
		end else begin
			tag_pipe[0] <= {tagpipe_issue_fire, mode_pop, id_pop, issue_seq_ctr, primary_pending};
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

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			in_flight <= 0;
		else if (tagpipe_issue_fire && !retire_fire)
			in_flight <= in_flight + 1;
		else if (!tagpipe_issue_fire && retire_fire)
			in_flight <= in_flight - 1;
	end

	// ---- shadow_reserved_count: incremented on admitting an
	// admitted-under-blocking tag_pipe entry (capped at SHADOW_DEPTH by
	// tagpipe_can_issue above); decremented once any ONE such entry fully
	// drains, whether directly (the blocking primary happened to finish
	// first, so no capture was ever needed) or via a shadow_hold slot.
	// shadow_clear_direct and shadow_any_hold_retire are mutually
	// exclusive per cycle (out_fifo accepts exactly one word/cycle, and
	// the 4-way -- now (2+SHADOW_DEPTH)-way -- retire-source mutual-
	// exclusion assertion below covers this generally), so a plain
	// if/else if net-credit update (same style as in_flight above) is
	// exact, not an approximation. ----
	logic	shadow_clear_direct;
	logic	shadow_any_hold_retire;

	assign shadow_clear_direct = tagpipe_direct_can_retire && is_extra_sel;

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			shadow_reserved_count <= '0;
		else if ((tagpipe_issue_fire && primary_pending)
				&& !(shadow_clear_direct || shadow_any_hold_retire))
			shadow_reserved_count <= shadow_reserved_count + 1;
		else if (!(tagpipe_issue_fire && primary_pending)
				&& (shadow_clear_direct || shadow_any_hold_retire))
			shadow_reserved_count <= shadow_reserved_count - 1;
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
	// Q8_0 encode chain: maxabs(5, fixed) -> q8_scale_dual_radix4
	// (VARIABLE, reused unmodified from Phase B1) -> quantize_pack(1,
	// fixed). Retires via q8enc_hold once its turn comes.
	// =====================================================================
	logic	q8enc_valid_in;
	assign q8enc_valid_in = issue_fire && (mode_pop == MODE_Q8_ENC);

	logic			q8_maxabs_valid;
	logic	[15:0]	q8_amax_f16;

	q8_maxabs_reduce u_q8_maxabs (
		.clk(clk), .rst_n(rst_n), .valid_in(q8enc_valid_in), .x_in_flat(data_pop),
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
			q8enc_id_hold <= id_pop;
			q8enc_issue_seq <= issue_seq_ctr;
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
	// Q8_0 decode chain: UNCHANGED -- dequantize(1), padded by 6 to reach
	// L_MAX=7, still retiring through the shared tag_pipe (now seq-gated).
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
	// Q4_0 encode chain: UNCHANGED datapath (scan(0) -> q4_scale(VARIABLE)
	// -> pack(2)); scheduling relaxed to run independently of Q8_0 encode
	// (this file's header). Retires via q4enc_hold once its turn comes.
	// =====================================================================
	logic	q4enc_valid_in;
	assign q4enc_valid_in = issue_fire && (mode_pop == MODE_Q4_ENC);

	logic	[31:0]	q4_mx_f32;

	q4_scan u_q4_scan (.x_in_flat(data_pop), .mx_f32_out(q4_mx_f32));

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
			q4enc_id_hold <= id_pop;
			q4enc_issue_seq <= issue_seq_ctr;
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
	// Q4_0 decode chain: UNCHANGED -- unpack(1), padded by 6 to reach
	// L_MAX=7, still retiring through the shared tag_pipe (now seq-gated).
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

	// ---- tag_pipe result mux (Q8_0 decode / Q4_0 decode only -- Q8_0/Q4_0
	// encode never appear in mode_sel, same invariant as Phase B1/production). ----
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

	// ---- shadow_hold: catches up to SHADOW_DEPTH tag_pipe-class entries
	// that reach the tail before their turn, one per slot. Captures
	// result_data/result_error (already mode_sel-muxed above) the one
	// cycle it's needed, into whichever slot is currently free (lowest
	// index preferred, deterministic -- tagpipe_can_issue's own gate
	// guarantees a free slot always exists when capture is needed, since
	// admission is capped at SHADOW_DEPTH live admitted-under-blocking
	// entries). ----
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

	// Priority-independent (mutual-exclusion asserted below) mux over
	// whichever slot's shadow_can_retire is set.
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

	// ---- final retire arbitration: mutually exclusive by construction
	// (exactly one seq value is ever `next_retire_seq` at a time, and
	// every live transaction holds a distinct seq -- checked directly by
	// the assertions below, not just argued). ----
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
	// Formal/state invariants (task item 5). Simulation-only counters plus
	// live assertions, guarded by `ifndef SYNTHESIS exactly like every
	// other phase in this project.
	// =====================================================================
`ifndef SYNTHESIS
	logic	[31:0]	accepted_count;
	logic	[31:0]	completed_count;
	// Computed at SEQ_WIDTH precision deliberately: issue_seq_ctr and
	// next_retire_seq both wrap mod 2**SEQ_WIDTH, so their difference must
	// be taken at that SAME width to stay a correct modular "how many
	// live" count -- comparing a width-promoted (32-bit) subtraction
	// against OUT_FIFO_DEPTH would break the moment next_retire_seq wraps
	// past issue_seq_ctr (e.g. issue_seq_ctr=2, next_retire_seq=254: the
	// correct mod-256 answer is 4, but a 32-bit subtraction gives a huge
	// unsigned underflow value) -- exercised for real by this phase's own
	// 4,000,000+-transaction differential run (SEQ_WIDTH=8 wraps every 256
	// transactions).
	logic	[SEQ_WIDTH-1:0]	live_seq_count;
	assign live_seq_count = issue_seq_ctr - next_retire_seq;

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			accepted_count <= '0;
			completed_count <= '0;
		end else begin
			if (issue_fire)
				accepted_count <= accepted_count + 1'b1;
			if (retire_any_fire)
				completed_count <= completed_count + 1'b1;
		end
	end

	always_ff @(posedge clk) begin
		if (rst_n) begin
			// accepted_count >= completed_count (task item 5).
			assert (accepted_count >= completed_count)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b2: completed_count exceeded accepted_count");
			// accepted_count - completed_count <= actual buffer capacity
			// (OUT_FIFO_DEPTH, the real system-wide reservation bound --
			// slot_ok already enforces this structurally; asserted
			// directly here too).
			assert ((accepted_count - completed_count) <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b2: more transactions in flight than reserved output capacity");
			// no tag reuse while live: the number of currently-live seq
			// values (issue_seq_ctr - next_retire_seq, mod 2**SEQ_WIDTH)
			// must never approach 2**SEQ_WIDTH, or two live transactions
			// could alias to the same seq. OUT_FIFO_DEPTH (32) is the real
			// bound on live transactions (same reservation as above);
			// 2**SEQ_WIDTH=256 gives an 8x margin.
			assert (live_seq_count <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b2: live transaction count approaching seq tag wraparound capacity");
			// outputs retire in required order: whichever source retires
			// this cycle must be retiring exactly next_retire_seq's value
			// (tautological given the mux conditions -- asserted directly,
			// belt and suspenders).
			if (q8enc_can_retire)
				assert (q8enc_hold_seq == next_retire_seq)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b2: q8enc retired out of order");
			if (q4enc_can_retire)
				assert (q4enc_hold_seq == next_retire_seq)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b2: q4enc retired out of order");
			for (int k = 0; k < SHADOW_DEPTH; k++)
				if (shadow_can_retire[k])
					assert (shadow_hold_seq[k] == next_retire_seq)
						else $error("membrane_quant_stream_top_q8_dual_radix4_b2: shadow_hold retired out of order");
			if (tagpipe_direct_can_retire)
				assert (seq_sel == next_retire_seq)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b2: tag_pipe direct retire out of order");
			// at most one retire source (q8enc/q4enc/any shadow slot/
			// tag_pipe direct) fires per cycle (mutual exclusion -- every
			// live transaction holds a unique seq, so at most one can
			// equal next_retire_seq at a time).
			assert ($countones({q8enc_can_retire, q4enc_can_retire,
				shadow_any_hold_retire, tagpipe_direct_can_retire}) <= 1)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b2: multiple retire sources matched next_retire_seq simultaneously");
			// ...and within shadow_hold itself, at most one SLOT may match
			// next_retire_seq at a time (same underlying uniqueness
			// argument, checked directly for the multi-slot case too).
			assert (shadow_retire_match_count <= 1)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b2: multiple shadow_hold slots matched next_retire_seq simultaneously");
			// Q4 and Q8 paths never overwrite each other's pending state:
			// shadow_hold (tag_pipe-class only) must never hold a Q8_0/Q4_0
			// encode mode tag, in any slot.
			for (int k = 0; k < SHADOW_DEPTH; k++)
				if (shadow_hold_valid[k])
					assert (shadow_hold_mode[k] == MODE_Q8_DEC || shadow_hold_mode[k] == MODE_Q4_DEC)
						else $error("membrane_quant_stream_top_q8_dual_radix4_b2: shadow_hold captured a non-tag_pipe mode");
			// in_flight credit counter stays in range (same convention as
			// Phase B1/production).
			assert (in_flight >= 0 && in_flight <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b2: in_flight credit counter out of range");
			// Single-in-flight discipline for each encode engine (own
			// class only, now independent of the other -- see this file's
			// header).
			if (q8_maxabs_valid)
				assert (q8_scale_in_ready)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b2: q8_scale_dual_radix4 not ready when q8_maxabs_valid pulsed");
		end
	end

	// reset clears every live transaction: checked one cycle after reset
	// deassertion.
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
				&& in_flight == 0)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b2: reset did not clear all live scheduler state");
		end
	end
`endif
endmodule
