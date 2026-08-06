// EXP-FPGA-DIV-001 Phase B4 variant of rtl/membrane_quant_stream_top.sv.
//
// Byte-for-byte identical in STRUCTURE to
// rtl/experimental/fp_div/membrane_quant_stream_top_b2.sv (Phase B2) --
// same full-serialization scheduling (`q4enc_inflight` blocks issuance of
// every mode while one Q4_0 encode transaction is in flight, exactly as
// B2 left it), same shared `tag_pipe` for the three fixed-latency chains,
// same direct-retire path for Q4_0 encode. The ONE change: `q4_scale_b2`
// is replaced by `q4_scale_b4` (Phase B4's radix-4 exact iterative
// divider instead of Phase B2's radix-2 one). This file deliberately does
// NOT use Phase B3's `membrane_completion_reorder`/decoupled-issuance
// scheduling -- see decision.md for why B3 was rejected as an
// architecture (real but disproportionate area cost for its throughput
// gain) and `phase-b4.md` for this phase's alternative hypothesis: speed
// up the divider itself, add zero new scheduling complexity, and measure
// whether that alone recovers a meaningful fraction of B3's intended
// throughput win without B3's area cost.
//
// Because Q4_0 encode's own latency is expected to roughly halve (Phase
// B4's radix-4 divider processes 2 quotient bits/cycle instead of B2's 1,
// see fp32_div_iterative_radix4_exact.sv's own header), the collateral
// slowdown this full-serialization scheme imposes on Q8_0 encode/decode
// and Q4_0 decode should shrink roughly proportionally -- that is exactly
// what `phase-b4.md`'s full-datapath comparison measures, not assumed
// here.
//
// ---- everything below (data format, mode encoding, Q8 chains, Q4 decode
// chain, backpressure/no-loss guarantee, error flag semantics, the
// tag_pipe/q4enc_inflight scheduling mechanism itself) is unchanged from
// membrane_quant_stream_top_b2.sv -- see that file's own header and
// membrane_quant_stream_top.sv's header for the full original
// description, not repeated here except where it differs. ----
module membrane_quant_stream_top_b4 #(
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

	// ---- Q4_0 encode in-flight tracking (same full-serialization
	// scheduling as Phase B2 -- see this file's header for why). ----
	logic	q4enc_inflight;

	logic	issue_fire;
	logic	tagpipe_issue_fire;
	int		in_flight;
	int		occ_i;
	int		flight_i;
	logic	slot_ok;

	always_comb begin
		occ_i = out_fifo_occ;
		flight_i = in_flight;
		slot_ok = (occ_i + flight_i + (q4enc_inflight ? 1 : 0)) < OUT_FIFO_DEPTH;
		if (mode_pop == MODE_Q4_ENC)
			issue_fire = in_fifo_out_valid && slot_ok && !q4enc_inflight && (flight_i == 0);
		else
			issue_fire = in_fifo_out_valid && slot_ok && !q4enc_inflight;
	end

	assign tagpipe_issue_fire = issue_fire && (mode_pop != MODE_Q4_ENC);
	assign in_fifo_out_ready = issue_fire;

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
	// Q8_0 encode chain: UNCHANGED -- maxabs(5) -> scale(1) ->
	// quantize_pack(1) = 7 = L_MAX, still retiring through the shared
	// tag_pipe.
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
	// Q4_0 encode chain (Phase B4): scan(0, combinational) -> q4_scale_b4
	// (VARIABLE latency, radix-4 exact iterative divider) -> pack(2).
	// Retires DIRECTLY (q4enc_direct_retire), not through tag_pipe/L_MAX
	// padding -- same mechanism as Phase B2, see this file's header.
	// =====================================================================
	logic	q4enc_valid_in;
	assign q4enc_valid_in = issue_fire && (mode_pop == MODE_Q4_ENC);

	logic	[31:0]	q4_mx_f32;

	q4_scan u_q4_scan (.x_in_flat(data_pop), .mx_f32_out(q4_mx_f32));

	logic			q4_scale_valid;
	logic	[15:0]	q4_d_f16;
	logic	[31:0]	q4_id_f32;
	logic			q4_scale_busy;

	q4_scale_b4 #(.DIV_DELAY(1)) u_q4_scale (
		.clk(clk), .rst_n(rst_n), .valid_in(q4enc_valid_in),
		.mx_f32(q4_mx_f32), .valid_out(q4_scale_valid),
		.d_f16_out(q4_d_f16), .id_f32_out(q4_id_f32), .busy(q4_scale_busy));

	// x_in held stable from issue until q4_scale_valid finally fires
	// (variable latency) -- same convention as Phase B2.
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

	// ---- output mux ----
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

	assign out_fifo_in_valid = retire_fire || q4enc_direct_retire;
	assign out_fifo_in_word = q4enc_direct_retire
		? {MODE_Q4_ENC, q4enc_id_hold, {368'h0, q4_packed}, q4enc_final_err}
		: {mode_sel, id_sel, result_data, result_error};

`ifndef SYNTHESIS
	always_ff @(posedge clk) begin
		if (rst_n && retire_fire) begin
			if (mode_sel == MODE_Q8_ENC)
				assert (q8_qp_valid)
					else $error("membrane_quant_stream_top_b4: Q8 encode latency mismatch at retire");
			else if (mode_sel == MODE_Q8_DEC)
				assert (q8dec_final_valid)
					else $error("membrane_quant_stream_top_b4: Q8 decode latency mismatch at retire");
			else
				assert (q4dec_final_valid)
					else $error("membrane_quant_stream_top_b4: Q4 decode latency mismatch at retire");
		end
		if (rst_n)
			assert (in_flight >= 0 && in_flight <= OUT_FIFO_DEPTH)
				else $error("membrane_quant_stream_top_b4: in_flight credit counter out of range");
		if (rst_n)
			assert (!(retire_fire && q4enc_direct_retire))
				else $error("membrane_quant_stream_top_b4: tag_pipe retire and Q4 encode direct retire collided -- serialization invariant broken");
	end
`endif
endmodule
