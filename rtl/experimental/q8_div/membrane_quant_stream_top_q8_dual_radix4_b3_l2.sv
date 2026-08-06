// EXP-FPGA-DIV-002 Phase B3, candidate B: bounded 2-entry input lookahead.
// Builds directly on Phase B2's own scheduler
// (rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4_b2.sv,
// NOT modified/overwritten by this file) -- every retirement mechanism
// (global sequence tags, per-engine hold registers, the shared
// shadow_hold queue, tag_pipe's own never-frozen shift discipline) is
// reused UNCHANGED. The ONE thing this file changes is WHERE a
// transaction's `seq` tag gets assigned and WHICH transaction issues each
// cycle: instead of only ever inspecting `in_fifo`'s own strict head
// (Phase B2), this file drains up to LOOKAHEAD_DEPTH=2 transactions from
// `in_fifo`'s head into a small, parallel-readable window, assigns each
// its `seq` tag the moment it ENTERS the window (in strict arrival
// order -- `in_fifo` itself guarantees this), and each cycle scans the
// window from oldest (index 0) to youngest, issuing the FIRST entry that
// is resource-issuable -- exactly Phase B2's own per-mode issuability
// rules (`!q8enc_pending`, `!q4enc_pending`, `tagpipe_can_issue`), just
// evaluated per window slot instead of only at the fixed head. See
// results/b3-hol-analysis.md for the measured evidence motivating this
// (58-66% of Phase B2's own blocked-head cycles have their first
// executable younger transaction within a depth-2 lookahead) and
// phase-b3.md for the full architecture derivation.
//
// ---- why this preserves strict global in-order retirement ----
// A transaction's `seq` is assigned once, at the cycle it enters the
// window (`window_fill`), from the SAME single free-running
// `issue_seq_ctr` Phase B2 used -- since `in_fifo` only ever presents its
// head, and the window is filled strictly at that head, `window_fill`
// events happen in the EXACT SAME order transactions arrived in,
// regardless of which window SLOT a transaction is later issued from.
// `seq` therefore always reflects arrival order, never issue order --
// retirement (unchanged from Phase B2: only the source whose own `seq`
// equals `next_retire_seq` may write to the output FIFO) is exactly as
// strict as Phase B2's own, even though ISSUANCE may now happen out of
// window-position order (a younger, resource-independent entry may issue
// before an older, still-blocked one). Issue order, execution completion
// order, and retirement order are three explicitly separate concepts
// here (task item 4) -- only retirement order is externally visible, and
// it is untouched.
//
// ---- window mechanics (left-packed, compacted on removal) ----
// The window holds at most LOOKAHEAD_DEPTH live entries, always
// left-packed (no gaps -- index 0 is always the oldest live entry, index
// `window_occ-1` the youngest). Each cycle: (1) the window is scanned
// oldest-to-youngest for the first resource-issuable slot; if found, that
// slot issues into its own engine (Q8_0/Q4_0 encode, or tag_pipe for
// Q8_0/Q4_0 decode -- identical downstream logic to Phase B2, just fed
// from `sel_mode`/`sel_id`/`sel_data`/`sel_seq` instead of directly from
// `in_fifo`'s own head), then every slot after the issued one shifts down
// by one position (compaction), and (2) if `in_fifo` has data AND the
// window has room AND `slot_ok` (the same output-FIFO reservation check
// Phase B2 already used, generalized to also reserve capacity for
// whatever is currently sitting in the window), a new entry is pulled
// into the first free slot. At most ONE transaction issues per cycle
// (matching Phase B2's own single-issue-per-cycle discipline, the
// smallest correct extension -- task item 2 does not require multi-issue
// and split-queue candidate D, evaluated separately, is where genuine
// multi-issue-per-cycle concurrency is explored instead).
//
// ---- starvation freedom (task item 3), proved by construction ----
// The selection scan always prefers the LOWEST window index among
// currently-issuable slots. Since the window is left-packed in ARRIVAL
// order, index 0 is always the single oldest live entry; the instant its
// own target engine becomes free (or shadow capacity frees up), it is,
// by definition, the lowest-index issuable slot and is selected THAT
// cycle -- no younger entry can ever be chosen ahead of an
// equally-issuable older one. This is checked directly by a live
// assertion below (`window_valid[0] && slot_issuable[0] implies
// window_issue_idx == 0`), not just argued. The window's own bounded
// depth (2) additionally caps how many times ANY single older entry can
// be bypassed before it becomes the sole remaining occupant (at which
// point it is trivially the only selectable entry) -- a structural bound
// on bypass count, not a separate counter.
module membrane_quant_stream_top_q8_dual_radix4_b3_l2 #(
	parameter int ID_WIDTH = 16,
	parameter int IN_FIFO_DEPTH = 16,
	parameter int OUT_FIFO_DEPTH = 32,
	parameter int SHADOW_DEPTH = 2,
	// Task item 2 candidates B/C: 2 or 4. No other value evaluated (task
	// item 2 explicitly forbids lookahead >4).
	parameter int LOOKAHEAD_DEPTH = 2
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
	localparam int WIN_IDX_W = (LOOKAHEAD_DEPTH > 1) ? $clog2(LOOKAHEAD_DEPTH) : 1;

`ifndef SYNTHESIS
	initial begin
		if (LOOKAHEAD_DEPTH != 2 && LOOKAHEAD_DEPTH != 4)
			$error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: LOOKAHEAD_DEPTH must be 2 or 4 (task item 2's own sweep scope)");
		if (SHADOW_DEPTH != LOOKAHEAD_DEPTH)
			$error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: SHADOW_DEPTH should match LOOKAHEAD_DEPTH (task item 6: 'bounded result slots corresponding only to actual ingress capacity')");
	end
`endif

	function automatic logic f16_is_special(input logic [15:0] w);
		f16_is_special = (w[14:10] == 5'h1F);
	endfunction

	// ---- input FIFO (unchanged mechanism from Phase B2 -- its own head
	// feeds the lookahead window below, not the engines directly). ----
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

	// ---- output FIFO (unchanged) ----
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
	// Global sequence-number bookkeeping. issue_seq_ctr now increments at
	// WINDOW-FILL time (arrival-into-window, still strictly arrival
	// order), not at issue time -- see this file's own header for why
	// that is exactly what preserves strict retirement order under
	// out-of-window-position issuance.
	// =====================================================================
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

	assign tagpipe_can_issue = !primary_pending || (shadow_reserved_count < SHADOW_DEPTH);

	logic	[SHADOW_CNT_W-1:0]	shadow_hold_occ;

	always_comb begin
		shadow_hold_occ = '0;
		for (int k = 0; k < SHADOW_DEPTH; k++)
			if (shadow_hold_valid[k])
				shadow_hold_occ = shadow_hold_occ + 1;
	end

	// =====================================================================
	// Bounded input lookahead window (task item 2 candidate B). See this
	// file's own header for the full mechanics.
	// =====================================================================
	logic				window_valid	[0:LOOKAHEAD_DEPTH-1];
	logic	[1:0]		window_mode		[0:LOOKAHEAD_DEPTH-1];
	logic	[ID_WIDTH-1:0]	window_id	[0:LOOKAHEAD_DEPTH-1];
	logic	[511:0]		window_data		[0:LOOKAHEAD_DEPTH-1];
	logic	[SEQ_WIDTH-1:0]	window_seq	[0:LOOKAHEAD_DEPTH-1];

	int		window_occ;

	always_comb begin
		window_occ = LOOKAHEAD_DEPTH;
		for (int k = LOOKAHEAD_DEPTH - 1; k >= 0; k--)
			if (!window_valid[k])
				window_occ = k;
	end

	logic	window_fill;
	logic	slot_ok;
	int		occ_i;
	int		flight_i;
	int		in_flight;

	always_comb begin
		occ_i = out_fifo_occ;
		flight_i = in_flight;
		slot_ok = (occ_i + flight_i + (q8enc_pending ? 1 : 0)
			+ (q4enc_pending ? 1 : 0) + shadow_hold_occ + window_occ) < OUT_FIFO_DEPTH;
		window_fill = in_fifo_out_valid && (window_occ < LOOKAHEAD_DEPTH) && slot_ok;
	end

	assign in_fifo_out_ready = window_fill;

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			issue_seq_ctr <= '0;
		else if (window_fill)
			issue_seq_ctr <= issue_seq_ctr + 1'b1;
	end

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			next_retire_seq <= '0;
		else if (retire_any_fire)
			next_retire_seq <= next_retire_seq + 1'b1;
	end

	// ---- per-slot issuability + oldest-first selection (task item 3:
	// "always choose the oldest issuable transaction"). ----
	logic	[LOOKAHEAD_DEPTH-1:0]	slot_issuable;

	always_comb begin
		for (int k = 0; k < LOOKAHEAD_DEPTH; k++) begin
			if (window_mode[k] == MODE_Q8_ENC)
				slot_issuable[k] = !q8enc_pending;
			else if (window_mode[k] == MODE_Q4_ENC)
				slot_issuable[k] = !q4enc_pending;
			else
				slot_issuable[k] = tagpipe_can_issue;
		end
	end

	logic					window_any_issuable;
	logic	[WIN_IDX_W-1:0]	window_issue_idx;

	always_comb begin
		window_any_issuable = 1'b0;
		window_issue_idx = '0;
		for (int k = 0; k < LOOKAHEAD_DEPTH; k++)
			if (!window_any_issuable && window_valid[k] && slot_issuable[k]) begin
				window_any_issuable = 1'b1;
				window_issue_idx = k;
			end
	end

	logic					issue_fire;
	logic	[1:0]			sel_mode;
	logic	[ID_WIDTH-1:0]	sel_id;
	logic	[511:0]			sel_data;
	logic	[SEQ_WIDTH-1:0]	sel_seq;
	logic					tagpipe_issue_fire;

	assign issue_fire = window_any_issuable;

	always_comb begin
		sel_mode = window_mode[0];
		sel_id = window_id[0];
		sel_data = window_data[0];
		sel_seq = window_seq[0];
		for (int k = 0; k < LOOKAHEAD_DEPTH; k++)
			if (window_any_issuable && (k == window_issue_idx)) begin
				sel_mode = window_mode[k];
				sel_id = window_id[k];
				sel_data = window_data[k];
				sel_seq = window_seq[k];
			end
	end

	assign tagpipe_issue_fire = issue_fire && (sel_mode == MODE_Q8_DEC || sel_mode == MODE_Q4_DEC);

	// ---- window compaction + fill (single clocked update, task item 6:
	// "FIFO/lookahead compaction preserves all entries exactly once"). ----
	logic				next_window_valid	[0:LOOKAHEAD_DEPTH-1];
	logic	[1:0]		next_window_mode	[0:LOOKAHEAD_DEPTH-1];
	logic	[ID_WIDTH-1:0]	next_window_id	[0:LOOKAHEAD_DEPTH-1];
	logic	[511:0]		next_window_data	[0:LOOKAHEAD_DEPTH-1];
	logic	[SEQ_WIDTH-1:0]	next_window_seq	[0:LOOKAHEAD_DEPTH-1];

	always_comb begin
		for (int k = 0; k < LOOKAHEAD_DEPTH; k++) begin
			next_window_valid[k] = window_valid[k];
			next_window_mode[k] = window_mode[k];
			next_window_id[k] = window_id[k];
			next_window_data[k] = window_data[k];
			next_window_seq[k] = window_seq[k];
		end
		if (issue_fire) begin
			for (int k = 0; k < LOOKAHEAD_DEPTH; k++) begin
				if ((k >= window_issue_idx) && (k < LOOKAHEAD_DEPTH - 1)) begin
					next_window_valid[k] = window_valid[k + 1];
					next_window_mode[k] = window_mode[k + 1];
					next_window_id[k] = window_id[k + 1];
					next_window_data[k] = window_data[k + 1];
					next_window_seq[k] = window_seq[k + 1];
				end else if (k >= window_issue_idx) begin
					// k == LOOKAHEAD_DEPTH-1, the one index the shift
					// above never reaches: becomes empty after
					// compaction by default, overridden below only if
					// it is exactly the correct post-compaction fill
					// target.
					next_window_valid[k] = 1'b0;
				end
			end
			// Correct post-compaction fill target is window_occ-1 (the
			// slot immediately after the last remaining valid entry
			// once compaction removes one) -- NOT unconditionally the
			// last window index. An earlier draft of this file placed a
			// same-cycle fill at the fixed last index regardless of
			// pre-issue occupancy, which is only correct when the
			// window was already full (in which case window_fill is
			// separately gated off anyway, see window_fill's own
			// definition) -- for any PARTIAL occupancy, that placed the
			// new entry past a gap instead of immediately after the
			// shifted-down survivors, breaking the window's own
			// left-packing invariant. Caught by this file's own
			// left-packing assertion during Phase B3's correctness
			// smoke run, before any reported number was produced; fixed
			// here.
			if (window_fill) begin
				for (int k = 0; k < LOOKAHEAD_DEPTH; k++) begin
					if (k == window_occ - 1) begin
						next_window_valid[k] = 1'b1;
						next_window_mode[k] = mode_pop;
						next_window_id[k] = id_pop;
						next_window_data[k] = data_pop;
						next_window_seq[k] = issue_seq_ctr;
					end
				end
			end
		end else if (window_fill) begin
			for (int k = 0; k < LOOKAHEAD_DEPTH; k++) begin
				if (k == window_occ) begin
					next_window_valid[k] = 1'b1;
					next_window_mode[k] = mode_pop;
					next_window_id[k] = id_pop;
					next_window_data[k] = data_pop;
					next_window_seq[k] = issue_seq_ctr;
				end
			end
		end
	end

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			for (int k = 0; k < LOOKAHEAD_DEPTH; k++)
				window_valid[k] <= 1'b0;
		end else begin
			for (int k = 0; k < LOOKAHEAD_DEPTH; k++) begin
				window_valid[k] <= next_window_valid[k];
				window_mode[k] <= next_window_mode[k];
				window_id[k] <= next_window_id[k];
				window_data[k] <= next_window_data[k];
				window_seq[k] <= next_window_seq[k];
			end
		end
	end

	// ---- shared id/mode/valid/seq/is_extra tag delay pipe (unchanged
	// mechanism from Phase B2 -- fed from the window-selected slot instead
	// of in_fifo's own head directly). ----
	localparam int TAG_W = 1 + 2 + ID_WIDTH + SEQ_WIDTH + 1;
	logic	[TAG_W-1:0]	tag_pipe	[0:L_MAX-1];

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			for (int k = 0; k < L_MAX; k++)
				tag_pipe[k] <= '0;
		end else begin
			tag_pipe[0] <= {tagpipe_issue_fire, sel_mode, sel_id, sel_seq, primary_pending};
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

	// ---- shared 32-lane F16 unpack of the SELECTED (window-issued)
	// transaction. ----
	logic	[15:0]	x_in_issue	[0:31];

	assign x_in_issue[0] = sel_data[15:0];
	assign x_in_issue[1] = sel_data[31:16];
	assign x_in_issue[2] = sel_data[47:32];
	assign x_in_issue[3] = sel_data[63:48];
	assign x_in_issue[4] = sel_data[79:64];
	assign x_in_issue[5] = sel_data[95:80];
	assign x_in_issue[6] = sel_data[111:96];
	assign x_in_issue[7] = sel_data[127:112];
	assign x_in_issue[8] = sel_data[143:128];
	assign x_in_issue[9] = sel_data[159:144];
	assign x_in_issue[10] = sel_data[175:160];
	assign x_in_issue[11] = sel_data[191:176];
	assign x_in_issue[12] = sel_data[207:192];
	assign x_in_issue[13] = sel_data[223:208];
	assign x_in_issue[14] = sel_data[239:224];
	assign x_in_issue[15] = sel_data[255:240];
	assign x_in_issue[16] = sel_data[271:256];
	assign x_in_issue[17] = sel_data[287:272];
	assign x_in_issue[18] = sel_data[303:288];
	assign x_in_issue[19] = sel_data[319:304];
	assign x_in_issue[20] = sel_data[335:320];
	assign x_in_issue[21] = sel_data[351:336];
	assign x_in_issue[22] = sel_data[367:352];
	assign x_in_issue[23] = sel_data[383:368];
	assign x_in_issue[24] = sel_data[399:384];
	assign x_in_issue[25] = sel_data[415:400];
	assign x_in_issue[26] = sel_data[431:416];
	assign x_in_issue[27] = sel_data[447:432];
	assign x_in_issue[28] = sel_data[463:448];
	assign x_in_issue[29] = sel_data[479:464];
	assign x_in_issue[30] = sel_data[495:480];
	assign x_in_issue[31] = sel_data[511:496];

	// =====================================================================
	// Q8_0 encode chain: UNCHANGED from Phase B2, fed from sel_*.
	// =====================================================================
	logic	q8enc_valid_in;
	assign q8enc_valid_in = issue_fire && (sel_mode == MODE_Q8_ENC);

	logic			q8_maxabs_valid;
	logic	[15:0]	q8_amax_f16;

	q8_maxabs_reduce u_q8_maxabs (
		.clk(clk), .rst_n(rst_n), .valid_in(q8enc_valid_in), .x_in_flat(sel_data),
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
			q8enc_id_hold <= sel_id;
			q8enc_issue_seq <= sel_seq;
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
	// Q8_0 decode chain: UNCHANGED from Phase B2, fed from sel_*.
	// =====================================================================
	logic	q8dec_valid_in;
	assign q8dec_valid_in = issue_fire && (sel_mode == MODE_Q8_DEC);

	logic			q8_dq_valid;
	logic	[511:0]	q8_dq_out;

	q8_dequantize #(.MUL_DELAY(1)) u_q8_dq (
		.clk(clk), .rst_n(rst_n), .valid_in(q8dec_valid_in),
		.packed_in(sel_data[271:0]), .valid_out(q8_dq_valid),
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
	// Q4_0 encode chain: UNCHANGED from Phase B2, fed from sel_*.
	// =====================================================================
	logic	q4enc_valid_in;
	assign q4enc_valid_in = issue_fire && (sel_mode == MODE_Q4_ENC);

	logic	[31:0]	q4_mx_f32;

	q4_scan u_q4_scan (.x_in_flat(sel_data), .mx_f32_out(q4_mx_f32));

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
			q4enc_id_hold <= sel_id;
			q4enc_issue_seq <= sel_seq;
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
	// Q4_0 decode chain: UNCHANGED from Phase B2, fed from sel_*.
	// =====================================================================
	logic	q4dec_valid_in;
	assign q4dec_valid_in = issue_fire && (sel_mode == MODE_Q4_DEC);

	logic			q4_uq_valid;
	logic	[511:0]	q4_uq_out;

	q4_unpack #(.MUL_DELAY(1)) u_q4_uq (
		.clk(clk), .rst_n(rst_n), .valid_in(q4dec_valid_in),
		.packed_in(sel_data[143:0]), .valid_out(q4_uq_valid),
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
			if (window_fill)
				accepted_count <= accepted_count + 1'b1;
			if (retire_any_fire)
				completed_count <= completed_count + 1'b1;
		end
	end

	always_ff @(posedge clk) begin
		if (rst_n) begin
			assert (accepted_count >= completed_count)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: completed_count exceeded accepted_count");
			assert ((accepted_count - completed_count) <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: more transactions in flight than reserved output capacity");
			assert (live_seq_count <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: live transaction count approaching seq tag wraparound capacity");
			if (q8enc_can_retire)
				assert (q8enc_hold_seq == next_retire_seq)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: q8enc retired out of order");
			if (q4enc_can_retire)
				assert (q4enc_hold_seq == next_retire_seq)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: q4enc retired out of order");
			for (int k = 0; k < SHADOW_DEPTH; k++)
				if (shadow_can_retire[k])
					assert (shadow_hold_seq[k] == next_retire_seq)
						else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: shadow_hold retired out of order");
			if (tagpipe_direct_can_retire)
				assert (seq_sel == next_retire_seq)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: tag_pipe direct retire out of order");
			assert ($countones({q8enc_can_retire, q4enc_can_retire,
				shadow_any_hold_retire, tagpipe_direct_can_retire}) <= 1)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: multiple retire sources matched next_retire_seq simultaneously");
			assert (shadow_retire_match_count <= 1)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: multiple shadow_hold slots matched next_retire_seq simultaneously");
			for (int k = 0; k < SHADOW_DEPTH; k++)
				if (shadow_hold_valid[k])
					assert (shadow_hold_mode[k] == MODE_Q8_DEC || shadow_hold_mode[k] == MODE_Q4_DEC)
						else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: shadow_hold captured a non-tag_pipe mode");
			assert (in_flight >= 0 && in_flight <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: in_flight credit counter out of range");
			if (q8_maxabs_valid)
				assert (q8_scale_in_ready)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: q8_scale_dual_radix4 not ready when q8_maxabs_valid pulsed");
			// ---- lookahead-specific invariants (task item 3/6) ----
			// starvation freedom, proved directly: the oldest window entry,
			// if issuable, is ALWAYS the one selected -- no younger entry
			// can ever be chosen ahead of an equally-issuable older one.
			if (window_valid[0] && slot_issuable[0])
				assert (window_issue_idx == 0)
					else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: a younger window entry issued ahead of an equally-issuable older one (starvation risk)");
			// window stays left-packed (no gaps) at all times -- a
			// structural invariant the compaction logic relies on.
			for (int k = 0; k < LOOKAHEAD_DEPTH - 1; k++)
				if (!window_valid[k])
					assert (!window_valid[k + 1])
						else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: window left-packing invariant violated (gap detected)");
			// bounded window occupancy.
			assert (window_occ >= 0 && window_occ <= LOOKAHEAD_DEPTH)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: window occupancy out of range");
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
				&& in_flight == 0 && window_occ == 0)
				else $error("membrane_quant_stream_top_q8_dual_radix4_b3_l2: reset did not clear all live scheduler state");
		end
	end
`endif
endmodule
