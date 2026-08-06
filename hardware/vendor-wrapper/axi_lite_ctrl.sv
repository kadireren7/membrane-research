// hardware/vendor-wrapper/axi_lite_ctrl.sv
//
// Interface-only AXI4-Lite control/status register skeleton. Exposes a
// minimal register set a host driver needs to bring the core up and
// observe its health -- NOT a full register map for a production
// kernel, and NOT synthesized or simulated against a real toolchain
// (see the header note in axi_stream_adapter.sv -- same caveat
// applies here).
//
// Register map (word-addressed, 32-bit registers, byte offsets shown):
//   0x00  CTRL     [0]=soft_reset (write 1, self-clears), RO otherwise
//   0x04  STATUS   [0]=core_ready, [1]=in_fifo_almost_full,
//                  [2]=out_fifo_almost_empty (read-only)
//   0x08  ERR_CNT  cumulative count of out_error beats observed since
//                  last CTRL soft_reset (read-only, saturating)
//   0x0C  TXN_CNT  cumulative count of retired transactions since last
//                  soft_reset (read-only, saturating)
//
// This is deliberately small: it is the minimum needed for
// hardware/experiment-protocol.md's environment-capture and
// loopback-DMA steps (confirm the core is out of reset and observe
// error/transaction counts), not a claim about what a final production
// register map should contain.

module membrane_axi_lite_ctrl (
	input	logic			aclk,
	input	logic			aresetn,

	// ---- AXI4-Lite write address/data/response ----
	input	logic	[3:0]	s_axi_awaddr,
	input	logic			s_axi_awvalid,
	output	logic			s_axi_awready,
	input	logic	[31:0]	s_axi_wdata,
	input	logic			s_axi_wvalid,
	output	logic			s_axi_wready,
	output	logic	[1:0]	s_axi_bresp,
	output	logic			s_axi_bvalid,
	input	logic			s_axi_bready,

	// ---- AXI4-Lite read address/data ----
	input	logic	[3:0]	s_axi_araddr,
	input	logic			s_axi_arvalid,
	output	logic			s_axi_arready,
	output	logic	[31:0]	s_axi_rdata,
	output	logic	[1:0]	s_axi_rresp,
	output	logic			s_axi_rvalid,
	input	logic			s_axi_rready,

	// ---- Core status inputs (wired from the core/adapter) ----
	input	logic			core_ready,
	input	logic			in_fifo_almost_full,
	input	logic			out_fifo_almost_empty,
	input	logic			out_error_pulse,	// one-cycle pulse per out_error beat
	input	logic			txn_retire_pulse,	// one-cycle pulse per retired transaction

	// ---- Control output ----
	output	logic			soft_reset	// pulses high for one cycle on CTRL[0] write
);

	// TODO (integration, not yet done): this is a minimal placeholder
	// FSM sketch, not a verified AXI4-Lite responder. A real
	// integration should either replace this with a vendor-generated
	// AXI-Lite slave (e.g. Vivado's AXI Lite IP template) wired to the
	// same register semantics above, or fully verify this hand-written
	// version against an AXI4-Lite protocol checker before trusting it
	// on real hardware.

	logic	[31:0]	err_cnt, txn_cnt;
	logic			ctrl_write_pending;

	assign s_axi_awready = 1'b1;
	assign s_axi_wready = 1'b1;
	assign s_axi_bresp = 2'b00;
	assign s_axi_arready = 1'b1;
	assign s_axi_rresp = 2'b00;

	always_ff @(posedge aclk or negedge aresetn) begin
		if (!aresetn) begin
			err_cnt <= '0;
			txn_cnt <= '0;
			soft_reset <= 1'b0;
			s_axi_bvalid <= 1'b0;
			s_axi_rvalid <= 1'b0;
			s_axi_rdata <= '0;
		end else begin
			soft_reset <= 1'b0;

			if (out_error_pulse && !(&err_cnt))
				err_cnt <= err_cnt + 32'd1;
			if (txn_retire_pulse && !(&txn_cnt))
				txn_cnt <= txn_cnt + 32'd1;

			if (s_axi_awvalid && s_axi_wvalid && !s_axi_bvalid) begin
				if (s_axi_awaddr == 4'h0 && s_axi_wdata[0])
					soft_reset <= 1'b1;
				if (s_axi_awaddr == 4'h0 && s_axi_wdata[0]) begin
					err_cnt <= '0;
					txn_cnt <= '0;
				end
				s_axi_bvalid <= 1'b1;
			end else if (s_axi_bvalid && s_axi_bready) begin
				s_axi_bvalid <= 1'b0;
			end

			if (s_axi_arvalid && !s_axi_rvalid) begin
				unique case (s_axi_araddr)
				4'h0:    s_axi_rdata <= 32'b0;
				4'h4:    s_axi_rdata <= {29'b0, out_fifo_almost_empty, in_fifo_almost_full, core_ready};
				4'h8:    s_axi_rdata <= err_cnt;
				4'hC:    s_axi_rdata <= txn_cnt;
				default: s_axi_rdata <= 32'hDEAD_0000;
				endcase
				s_axi_rvalid <= 1'b1;
			end else if (s_axi_rvalid && s_axi_rready) begin
				s_axi_rvalid <= 1'b0;
			end
		end
	end

endmodule
