// hardware/vendor-wrapper/membrane_core_wrapper.sv
//
// Platform-independent top-level wrapper: ties the AXI4-Stream data
// path (membrane_axi_stream_adapter -> membrane_quant_stream_top) and
// the AXI4-Lite control/status path (membrane_axi_lite_ctrl) together
// behind one module boundary that exposes only standard bus
// interfaces -- no vendor-specific ports. A real board integration
// instantiates THIS module inside a vendor shell (an XRT kernel
// wrapper for Alveo, a Platform Designer/Qsys component for Intel,
// etc.), not membrane_quant_stream_top directly, so the vendor-specific
// glue stays entirely outside this repository.
//
// Not synthesized or simulated against a real vendor toolchain -- see
// axi_stream_adapter.sv's header note; the same caveat applies to
// every file in this directory.

module membrane_core_wrapper #(
	parameter int ID_WIDTH = 16,
	parameter int IN_FIFO_DEPTH = 16,
	parameter int OUT_FIFO_DEPTH = 32
) (
	input	logic				aclk,
	input	logic				aresetn,

	// ---- Data path: AXI4-Stream slave (host -> core) ----
	input	logic				s_axis_tvalid,
	output	logic				s_axis_tready,
	input	logic	[511:0]			s_axis_tdata,
	input	logic	[2+ID_WIDTH-1:0]	s_axis_tuser,
	input	logic				s_axis_tlast,

	// ---- Data path: AXI4-Stream master (core -> host) ----
	output	logic				m_axis_tvalid,
	input	logic				m_axis_tready,
	output	logic	[511:0]			m_axis_tdata,
	output	logic	[2+ID_WIDTH-1:0]	m_axis_tuser,
	output	logic				m_axis_tlast,
	output	logic				m_axis_tuser_error,

	// ---- Control path: AXI4-Lite slave ----
	input	logic	[3:0]	s_axi_ctrl_awaddr,
	input	logic			s_axi_ctrl_awvalid,
	output	logic			s_axi_ctrl_awready,
	input	logic	[31:0]	s_axi_ctrl_wdata,
	input	logic			s_axi_ctrl_wvalid,
	output	logic			s_axi_ctrl_wready,
	output	logic	[1:0]	s_axi_ctrl_bresp,
	output	logic			s_axi_ctrl_bvalid,
	input	logic			s_axi_ctrl_bready,
	input	logic	[3:0]	s_axi_ctrl_araddr,
	input	logic			s_axi_ctrl_arvalid,
	output	logic			s_axi_ctrl_arready,
	output	logic	[31:0]	s_axi_ctrl_rdata,
	output	logic	[1:0]	s_axi_ctrl_rresp,
	output	logic			s_axi_ctrl_rvalid,
	input	logic			s_axi_ctrl_rready
);

	logic	core_ready_stub;
	logic	soft_reset;
	logic	adapter_aresetn;

	// TODO (integration, not yet done): "core_ready" is stubbed high
	// here -- membrane_quant_stream_top has no explicit ready/idle
	// status output today (it is always ready to accept work once out
	// of reset, per its own valid/ready handshake), so this stub simply
	// reflects "out of reset". A real integration that wants a richer
	// readiness signal (e.g. "FIFOs drained") would need to add that
	// output to membrane_quant_stream_top itself, not fake it here.
	assign core_ready_stub = aresetn;
	assign adapter_aresetn = aresetn & ~soft_reset;

	membrane_axi_stream_adapter #(
		.ID_WIDTH(ID_WIDTH),
		.IN_FIFO_DEPTH(IN_FIFO_DEPTH),
		.OUT_FIFO_DEPTH(OUT_FIFO_DEPTH)
	) u_adapter (
		.aclk(aclk),
		.aresetn(adapter_aresetn),
		.s_axis_tvalid(s_axis_tvalid), .s_axis_tready(s_axis_tready),
		.s_axis_tdata(s_axis_tdata), .s_axis_tuser(s_axis_tuser), .s_axis_tlast(s_axis_tlast),
		.m_axis_tvalid(m_axis_tvalid), .m_axis_tready(m_axis_tready),
		.m_axis_tdata(m_axis_tdata), .m_axis_tuser(m_axis_tuser), .m_axis_tlast(m_axis_tlast),
		.m_axis_tuser_error(m_axis_tuser_error)
	);

	membrane_axi_lite_ctrl u_ctrl (
		.aclk(aclk), .aresetn(aresetn),
		.s_axi_awaddr(s_axi_ctrl_awaddr), .s_axi_awvalid(s_axi_ctrl_awvalid), .s_axi_awready(s_axi_ctrl_awready),
		.s_axi_wdata(s_axi_ctrl_wdata), .s_axi_wvalid(s_axi_ctrl_wvalid), .s_axi_wready(s_axi_ctrl_wready),
		.s_axi_bresp(s_axi_ctrl_bresp), .s_axi_bvalid(s_axi_ctrl_bvalid), .s_axi_bready(s_axi_ctrl_bready),
		.s_axi_araddr(s_axi_ctrl_araddr), .s_axi_arvalid(s_axi_ctrl_arvalid), .s_axi_arready(s_axi_ctrl_arready),
		.s_axi_rdata(s_axi_ctrl_rdata), .s_axi_rresp(s_axi_ctrl_rresp), .s_axi_rvalid(s_axi_ctrl_rvalid), .s_axi_rready(s_axi_ctrl_rready),
		.core_ready(core_ready_stub),
		.in_fifo_almost_full(1'b0),	// TODO: wire from u_adapter.u_core once exposed
		.out_fifo_almost_empty(1'b0),	// TODO: wire from u_adapter.u_core once exposed
		.out_error_pulse(m_axis_tvalid && m_axis_tready && m_axis_tuser_error),
		.txn_retire_pulse(m_axis_tvalid && m_axis_tready),
		.soft_reset(soft_reset)
	);

endmodule
