// hardware/vendor-wrapper/axi_stream_adapter.sv
//
// Interface-only AXI4-Stream <-> membrane_quant_stream_top adapter
// skeleton. Wraps the existing, cosimulation-verified native
// valid/ready/mode/id/data ports (rtl/membrane_quant_stream_top.sv,
// unmodified) behind a standard AXI4-Stream slave/master pair so a
// vendor shell (XRT kernel, Platform Designer/Qsys component, etc.)
// can integrate it without touching the verified core.
//
// This file has NOT been synthesized or simulated against a real
// vendor toolchain (none is available in this environment -- see
// docs/phase8-hardware-validation-plan.md Level A). It is a real,
// syntactically-intended SystemVerilog skeleton, not a placebo comment
// block, but it should be re-verified (at minimum: re-cosimulated
// against rtl/tb/tb_top_verilator.cpp's golden vectors with this
// adapter in the loop) before being trusted for a real bring-up.
//
// Sideband packing: mode (2 bits) and transaction id (ID_WIDTH bits)
// travel in TUSER rather than a separate header beat, since
// membrane_quant_stream_top's native interface already carries them
// as parallel signals every cycle -- TUSER is the natural AXI4-Stream
// home for "metadata alongside this beat", not TDATA itself.

module membrane_axi_stream_adapter #(
	parameter int ID_WIDTH = 16,
	parameter int IN_FIFO_DEPTH = 16,
	parameter int OUT_FIFO_DEPTH = 32
) (
	input	logic				aclk,
	input	logic				aresetn,

	// ---- AXI4-Stream slave (host -> core) ----
	input	logic				s_axis_tvalid,
	output	logic				s_axis_tready,
	input	logic	[511:0]			s_axis_tdata,
	input	logic	[2+ID_WIDTH-1:0]	s_axis_tuser,	// {mode, id}
	input	logic				s_axis_tlast,	// expected always 1 (single-beat transactions)

	// ---- AXI4-Stream master (core -> host) ----
	output	logic				m_axis_tvalid,
	input	logic				m_axis_tready,
	output	logic	[511:0]			m_axis_tdata,
	output	logic	[2+ID_WIDTH-1:0]	m_axis_tuser,	// {mode, id}
	output	logic				m_axis_tlast,	// always 1 (single-beat transactions)
	output	logic				m_axis_tuser_error	// out_error, carried alongside, not packed into tuser
);

	logic	[1:0]			in_mode;
	logic	[ID_WIDTH-1:0]	in_id;
	logic	[1:0]			out_mode;
	logic	[ID_WIDTH-1:0]	out_id;

	assign {in_mode, in_id} = s_axis_tuser;
	assign m_axis_tuser = {out_mode, out_id};
	assign m_axis_tlast = 1'b1;

	// TODO (integration, not yet done): assert s_axis_tlast == 1 in
	// simulation -- this adapter assumes single-beat (512-bit)
	// transactions matching membrane_quant_stream_top's native
	// granularity, per docs/phase5-synthesizable-fpga.md section 8's
	// "single AXI4-Stream beat per transaction" design choice. A real
	// integration with a host DMA engine that bursts multiple beats
	// per descriptor must either disable bursting for this kernel or
	// add real multi-beat framing here -- not assumed away silently.

	membrane_quant_stream_top #(
		.ID_WIDTH(ID_WIDTH),
		.IN_FIFO_DEPTH(IN_FIFO_DEPTH),
		.OUT_FIFO_DEPTH(OUT_FIFO_DEPTH)
	) u_core (
		.clk(aclk),
		.rst_n(aresetn),

		.in_valid(s_axis_tvalid),
		.in_ready(s_axis_tready),
		.in_mode(in_mode),
		.in_id(in_id),
		.in_data(s_axis_tdata),

		.out_valid(m_axis_tvalid),
		.out_ready(m_axis_tready),
		.out_mode(out_mode),
		.out_id(out_id),
		.out_data(m_axis_tdata),
		.out_error(m_axis_tuser_error)
	);

endmodule
