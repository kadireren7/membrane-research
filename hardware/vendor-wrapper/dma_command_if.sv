// hardware/vendor-wrapper/dma_command_if.sv
//
// Platform-independent DMA command/completion abstraction. This is a
// SystemVerilog `interface` (a signal-grouping construct, not an
// executable model of any real DMA engine) meant to be bound between
// membrane_axi_stream_adapter and whatever real DMA engine a specific
// platform provides (XRT's DMA subsystem for Alveo, OPAE's DMA for
// Intel platforms, or a custom PCIe DMA core). No real DMA engine is
// implemented, vendored, or modeled here -- see hardware/README.md for
// why vendor DMA IP is deliberately kept out of this repository.

interface membrane_dma_cmd_if #(
	parameter int ADDR_WIDTH = 64,
	parameter int LEN_WIDTH = 32,
	parameter int ID_WIDTH = 16
);

	// ---- Command (host -> DMA engine): "move LEN bytes starting at
	// ADDR, tag the completion with ID" ----
	logic				cmd_valid;
	logic				cmd_ready;
	logic [ADDR_WIDTH-1:0]	cmd_addr;
	logic [LEN_WIDTH-1:0]	cmd_len;
	logic [ID_WIDTH-1:0]	cmd_id;
	logic				cmd_is_write;	// 1 = host-to-card, 0 = card-to-host

	// ---- Completion (DMA engine -> host): "ID finished, with status" ----
	logic				cpl_valid;
	logic				cpl_ready;
	logic [ID_WIDTH-1:0]	cpl_id;
	logic				cpl_error;

	modport initiator (
		output cmd_valid, cmd_addr, cmd_len, cmd_id, cmd_is_write,
		input  cmd_ready,
		input  cpl_valid, cpl_id, cpl_error,
		output cpl_ready
	);

	modport engine (
		input  cmd_valid, cmd_addr, cmd_len, cmd_id, cmd_is_write,
		output cmd_ready,
		output cpl_valid, cpl_id, cpl_error,
		input  cpl_ready
	);

	// TODO (integration, not yet done): this interface has no timing
	// or ordering guarantees of its own -- those come entirely from
	// whatever real DMA engine implements the `engine` modport on a
	// specific platform. Before relying on this abstraction for a real
	// bring-up, confirm the target platform's actual DMA engine
	// (a) can be driven by a command/completion handshake shaped like
	// this one (most descriptor-ring-based engines can, via a thin
	// shim), and (b) preserves per-`cmd_id` completion ordering if
	// membrane_axi_stream_adapter's transaction ids are used as-is for
	// DMA descriptor tagging too.

endinterface
