// EXP-FPGA-DIV-002 Phase B1 variant of rtl/membrane_quant_stream_top.sv.
//
// The ONE architectural change: `q8_scale` (fixed 1-cycle latency, two
// parallel `membrane_fp_divider` instances) is replaced by
// `q8_scale_dual_radix4` (variable latency, two parallel
// `membrane_fp_divider_radix4` instances, see that file's own header for
// the full derivation of why this substitution carries no new
// bit-exactness risk). Everything else -- data format, mode encoding, the
// Q8_0 decode chain, the Q4_0 encode chain (unchanged radix-4 `q4_scale`,
// already production), the Q4_0 decode chain, the input/output FIFOs, the
// backpressure/no-loss guarantee, the error-flag semantics -- is
// unchanged from `membrane_quant_stream_top.sv`; see that file's own
// header for the full original description, not repeated here except
// where it differs.
//
// ---- item 7: scheduling impact of a now-variable-latency Q8_0 encode ----
// `q8_scale`'s old fixed 1-cycle latency is what let Q8_0 encode ride the
// shared `tag_pipe`/`L_MAX`-padding mechanism alongside Q8_0 decode and
// Q4_0 decode. `q8_scale_dual_radix4` is variable-latency and
// single-in-flight (same structural class as `q4_scale`'s own
// `u_div_id`), so Q8_0 encode can no longer share that fixed-depth pipe.
// **The simplest correct scheduling** (this phase's own explicit
// instruction, "ilk uygulamada en basit doğru scheduling'i kullan") is
// the SAME mechanism this codebase already uses, and has already
// measured, for Q4_0 encode: full serialization. A Q8_0 encode
// transaction is only issued when `in_flight==0` (tag_pipe fully
// drained) AND no OTHER single-in-flight-class transaction
// (`q4enc_inflight`/`q8enc_inflight`) is outstanding; once issued, EVERY
// mode's issuance (including another Q8_0 encode, and Q4_0 encode) is
// blocked until this transaction's own `q8_quantize_pack` finally
// retires it directly (`q8enc_direct_retire`, bypassing `tag_pipe`
// entirely, exactly like `q4enc_direct_retire`). This means, precisely:
// **global issuance blocks entirely** while a Q8_0 encode transaction is
// in flight -- not just Q8_0 encode, and not a lesser restriction -- the
// same real, disclosed collateral-slowdown shape Q4_0 encode's own
// integration already carries (see the production
// `membrane_quant_stream_top.sv`'s own header and
// `experiments/EXP-FPGA-DIV-001/promotion-comparison.md`), now applied
// symmetrically to Q8_0 encode. `q4enc_inflight` and `q8enc_inflight` are
// mutually exclusive by construction (each requires the other be clear,
// AND `in_flight==0`, to be entered), so at most one single-in-flight
// class is ever active, and `tag_pipe` is provably empty (no real,
// non-bubble entry) whenever either is active -- `retire_fire`,
// `q4enc_direct_retire`, and `q8enc_direct_retire` can therefore never
// collide, checked directly by this file's own assertions below, not
// just argued. This phase's own full-datapath comparison
// (`experiments/EXP-FPGA-DIV-002/results/b1-full-datapath.json`) measures
// the real, resulting collateral slowdown on Q8_0 decode / Q4_0
// encode+decode while a Q8_0 encode transaction is outstanding -- not
// estimated here. A smarter scheduler (e.g. letting Q8_0 decode/Q4_0
// paths continue issuing around an in-flight Q8_0 encode, since they
// share no functional unit with it) is architecturally possible and
// explicitly OUT OF SCOPE for this first implementation, per this
// phase's own instruction not to add a reorder buffer yet -- a natural
// CONTINUE-class follow-up if the measured collateral slowdown proves
// too large relative to the area win (see phase-b1.md's own decision).
//
// ---- Q8_0 encode chain shape, mechanically ----
// `q8_maxabs_reduce` (fixed 5 cycles) -> `q8_scale_dual_radix4`
// (VARIABLE) -> `q8_quantize_pack` (fixed 1 cycle, `MUL_DELAY=1`). `x_in`
// is shifted through a 5-deep pipe to align with `q8_maxabs_reduce`'s own
// fixed latency (same technique the unchanged chain uses), then captured
// into a hold register the instant `q8_maxabs_valid` pulses and held
// there (unread, since only one Q8_0 encode is ever in flight) until
// `q8_scale_dual_radix4`'s own `valid_out` finally fires, variable cycles
// later -- the same "capture at the fixed-latency stage's own valid pulse,
// hold across the variable-latency stage, direct-retire at the end"
// pattern `membrane_quant_stream_top.sv`'s own Q4_0 encode chain already
// established, applied one stage later here (Q4_0 encode's variable stage
// begins immediately at issue; Q8_0 encode's begins after the fixed
// 5-cycle maxabs reduction).
module membrane_quant_stream_top_q8_dual_radix4 #(
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

	// ---- Q4_0 encode AND Q8_0 encode in-flight tracking -- see this
	// file's header for why both are now full-serialization,
	// single-in-flight, mutually-exclusive classes. ----
	logic	q4enc_inflight;
	logic	q8enc_inflight;
	logic	enc_mode_pop;

	assign enc_mode_pop = (mode_pop == MODE_Q4_ENC) || (mode_pop == MODE_Q8_ENC);

	// ---- issue gating: reserve an output-FIFO slot before issuing, AND
	// serialize BOTH single-in-flight-class modes against everything
	// else (and against each other). ----
	logic	issue_fire;
	logic	tagpipe_issue_fire;
	int		in_flight;
	int		occ_i;
	int		flight_i;
	logic	slot_ok;

	always_comb begin
		occ_i = out_fifo_occ;
		flight_i = in_flight;
		// Reserve a slot for a Q4_0 OR Q8_0 encode transaction too (both
		// write the output FIFO via their own direct paths, see
		// q4enc_direct_retire/q8enc_direct_retire below).
		slot_ok = (occ_i + flight_i + (q4enc_inflight ? 1 : 0)
			+ (q8enc_inflight ? 1 : 0)) < OUT_FIFO_DEPTH;
		if (enc_mode_pop)
			issue_fire = in_fifo_out_valid && slot_ok && !q4enc_inflight
				&& !q8enc_inflight && (flight_i == 0);
		else
			issue_fire = in_fifo_out_valid && slot_ok && !q4enc_inflight
				&& !q8enc_inflight;
	end

	assign tagpipe_issue_fire = issue_fire && !enc_mode_pop;
	assign in_fifo_out_ready = issue_fire;

	// ---- shared id/mode/valid tag delay pipe (depth L_MAX), used ONLY
	// by the two remaining fixed-latency modes (Q8 decode, Q4 decode) --
	// unlike the production top, Q8_0 encode never enters this pipe
	// either now. ----
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

	// ---- Q4_0 encode in-flight flag: set on issue, cleared when its own
	// datapath (q4_pack) finally retires it. Unchanged from production. ----
	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			q4enc_inflight <= 1'b0;
		else if (issue_fire && mode_pop == MODE_Q4_ENC)
			q4enc_inflight <= 1'b1;
		else if (q4_pack_valid)
			q4enc_inflight <= 1'b0;
	end

	// ---- Q8_0 encode in-flight flag: set on issue, cleared when its own
	// datapath (q8_quantize_pack, fed off q8_scale_dual_radix4) finally
	// retires it. New in this variant. ----
	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n)
			q8enc_inflight <= 1'b0;
		else if (issue_fire && mode_pop == MODE_Q8_ENC)
			q8enc_inflight <= 1'b1;
		else if (q8_qp_valid)
			q8enc_inflight <= 1'b0;
	end

	assign out_fifo_in_valid = retire_fire || q4enc_direct_retire || q8enc_direct_retire;

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
	// (VARIABLE) -> quantize_pack(1, fixed). Retires DIRECTLY
	// (q8enc_direct_retire), not through tag_pipe/L_MAX padding.
	// =====================================================================
	logic	q8enc_valid_in;
	assign q8enc_valid_in = issue_fire && (mode_pop == MODE_Q8_ENC);

	logic			q8_maxabs_valid;
	logic	[15:0]	q8_amax_f16;

	q8_maxabs_reduce u_q8_maxabs (
		.clk(clk), .rst_n(rst_n), .valid_in(q8enc_valid_in), .x_in_flat(data_pop),
		.valid_out(q8_maxabs_valid), .amax_f16_out(q8_amax_f16));

	// x_in delayed 5 cycles to align with q8_maxabs_reduce's own fixed
	// latency -- matches the PRODUCTION top's own q8_x_d[0:4] alignment
	// exactly (that file's q8_x_d[0:5] carries one MORE stage, to align
	// with q8_scale's own extra fixed cycle -- not needed here, since
	// this variant captures x into a hold register right at
	// q8_maxabs_valid instead of riding a fixed pipe any further).
	logic	[15:0]	q8_x_d	[0:4][0:31];

	always_ff @(posedge clk) begin
		for (int j = 0; j < 32; j++)
			q8_x_d[0][j] <= x_in_issue[j];
		for (int k = 4; k > 0; k--)
			for (int j = 0; j < 32; j++)
				q8_x_d[k][j] <= q8_x_d[k - 1][j];
	end

	// Captured the instant q8_maxabs_valid pulses, held stable (unread
	// again) until q8_scale_dual_radix4's own valid_out finally fires --
	// safe because Q8_0 encode is single-in-flight (this file's own
	// q8enc_inflight serialization guarantees it), same technique as the
	// production top's own q4enc_x_hold/q4enc_id_hold.
	logic	[15:0]	q8enc_x_hold	[0:31];
	logic	[ID_WIDTH-1:0]	q8enc_id_hold;

	always_ff @(posedge clk) begin
		if (q8enc_valid_in)
			q8enc_id_hold <= id_pop;
		if (q8_maxabs_valid)
			for (int j = 0; j < 32; j++)
				q8enc_x_hold[j] <= q8_x_d[4][j];
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

	// Error flag: d NaN/Inf, captured when q8_scale_valid pulses, delayed
	// 1 more cycle (matching quantize_pack's own MUL_DELAY) so it lands
	// on the same cycle as q8_packed/q8_qp_valid -- same technique as the
	// production top, just triggered off this variant's own (variable-
	// timed) q8_scale_valid instead of a fixed-delay one.
	logic	q8_err_raw, q8_err_final;

	assign q8_err_raw = f16_is_special(q8_d_f16);

	always_ff @(posedge clk)
		q8_err_final <= q8_err_raw;

	logic	q8enc_direct_retire;
	assign q8enc_direct_retire = q8_qp_valid;

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
	// Q4_0 encode chain: UNCHANGED from production -- scan(0, comb) ->
	// q4_scale (VARIABLE, membrane_fp_divider_radix4 inside) -> pack(2).
	// Direct retire (q4enc_direct_retire). q8_scale.sv is not used
	// anywhere in this file; only its dual-radix4 replacement is.
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

	logic	q4_err_raw;
	logic	[1:0]	q4_err_d2	[0:1];

	assign q4_err_raw = f16_is_special(q4_d_f16);

	always_ff @(posedge clk) begin
		q4_err_d2[0] <= {1'b0, q4_err_raw};
		q4_err_d2[1] <= q4_err_d2[0];
	end

	logic	q4enc_final_err;
	assign q4enc_final_err = q4_err_d2[1][0];

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

	// ---- output mux: mode_sel (tag_pipe-driven, never Q4_0/Q8_0 encode)
	// picks which of the two remaining fixed-latency chains retires;
	// Q4_0/Q8_0 encode retire via their own direct paths, arbitrated
	// below. ----
	logic	[511:0]	result_data;
	logic			result_error;

	always_comb begin
		if (mode_sel == MODE_Q8_DEC) begin
			result_data = q8dec_final_data;
			result_error = q8dec_final_err;
		end else begin // MODE_Q4_DEC (MODE_Q4_ENC/MODE_Q8_ENC never appear in mode_sel, see header)
			result_data = q4dec_final_data;
			result_error = q4dec_final_err;
		end
	end

	// 3-way retire mux: q4enc_direct_retire and q8enc_direct_retire are
	// mutually exclusive with EACH OTHER and with retire_fire (see this
	// file's header + the assertions below), so this is a plain priority
	// mux over three conditions that never actually overlap, not real
	// arbitration.
	always_comb begin
		if (q4enc_direct_retire)
			out_fifo_in_word = {MODE_Q4_ENC, q4enc_id_hold, {368'h0, q4_packed}, q4enc_final_err};
		else if (q8enc_direct_retire)
			out_fifo_in_word = {MODE_Q8_ENC, q8enc_id_hold, {240'h0, q8_packed}, q8_err_final};
		else
			out_fifo_in_word = {mode_sel, id_sel, result_data, result_error};
	end

	// ---- correctness assertions (also serves task item 7: property
	// checks -- no accepted-input loss, no stale output, output valid
	// exactly when expected). ----
`ifndef SYNTHESIS
	always_ff @(posedge clk) begin
		if (rst_n && retire_fire) begin
			if (mode_sel == MODE_Q8_DEC)
				assert (q8dec_final_valid)
					else $error("membrane_quant_stream_top_q8_dual_radix4: Q8 decode latency mismatch at retire");
			else
				assert (q4dec_final_valid)
					else $error("membrane_quant_stream_top_q8_dual_radix4: Q4 decode latency mismatch at retire");
		end
		if (rst_n)
			assert (in_flight >= 0 && in_flight <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_q8_dual_radix4: in_flight credit counter out of range");
		// The 3-way retire-source mutual-exclusion claim this file's
		// header makes -- checked directly.
		if (rst_n)
			assert (!(retire_fire && q4enc_direct_retire))
				else $error("membrane_quant_stream_top_q8_dual_radix4: tag_pipe retire and Q4 encode direct retire collided");
		if (rst_n)
			assert (!(retire_fire && q8enc_direct_retire))
				else $error("membrane_quant_stream_top_q8_dual_radix4: tag_pipe retire and Q8 encode direct retire collided");
		if (rst_n)
			assert (!(q4enc_direct_retire && q8enc_direct_retire))
				else $error("membrane_quant_stream_top_q8_dual_radix4: Q4 and Q8 encode direct retires collided -- mutual exclusion broken");
		// Single-in-flight discipline check for q8_scale_dual_radix4,
		// same convention as q4_scale.sv's own internal assertion.
		if (rst_n && q8_maxabs_valid)
			assert (q8_scale_in_ready)
				else $error("membrane_quant_stream_top_q8_dual_radix4: q8_scale_dual_radix4 not ready when q8_maxabs_valid pulsed -- single in-flight discipline violated");
	end
`endif
endmodule
