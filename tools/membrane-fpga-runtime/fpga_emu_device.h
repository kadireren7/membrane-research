#ifndef MEMBRANE_FPGA_EMU_DEVICE_H
#define MEMBRANE_FPGA_EMU_DEVICE_H

#include <cstdint>
#include <vector>

class Vmembrane_dma_bridge;

// Phase 5.4: cycle-accurate PCIe/DMA host emulation, tier 4 of the
// priority order this phase's spec lays out (real Alveo card > cloud
// FPGA > QEMU/XRT > this) -- none of the first three are available in
// this environment (checked: no xbutil/xbmgmt, no Xilinx PCI device, no
// qemu-system-x86_64, no XRT package), disclosed rather than hidden,
// see docs/phase5-pcie-hardware-loop.md section 1.
//
// This class drives rtl/membrane_dma_bridge.sv (which wraps
// rtl/membrane_quant_stream_top.sv, Phase 5.3's fully synthesizable
// quantization pipeline) through Verilator, cycle by cycle, presenting
// an MMIO register + command-push + payload-stream + completion-pop API
// that mirrors what a real PCIe DMA driver would see from actual
// hardware -- the register map is include/membrane/fpga_regs.h, the
// packet format is include/membrane/fpga_dma.h, both shared with the
// (hypothetical, not-yet-existing) real-hardware backend this class is
// a stand-in for.
//
// "Cycle-accurate": every byte of every transfer actually flows through
// the real RTL, one clock edge at a time (via Verilator eval(), same
// clocking discipline as Phase 5.3's tb_top_verilator.cpp -- sample
// handshake signals with clk low, BEFORE the rising edge, not after;
// see step_cycle's header comment there for why sampling after the
// edge silently desyncs accepted/retired tracking at FIFO-boundary
// transitions). The only thing NOT modeled physically is PCIe
// transport itself: `cycles_per_beat` below stands in for "how many
// device clocks does one 4-byte payload beat cost," which for this
// bridge's own 32-bit-wide payload port is naturally 1 (the port
// literally cannot go faster), and can be set higher to model a
// slower, PCIe-bandwidth-limited link explicitly for specific tests.
class FpgaEmuDevice
{
public:
	FpgaEmuDevice();
	~FpgaEmuDevice();

	// Drives rst_n low for `cycles` cycles, then releases it and ticks
	// once more so registers/FIFOs settle. Also resets all counters
	// this class itself tracks (cycle_count, bytes moved).
	void reset(int cycles = 4);

	// ---- register interface ----
	void mmio_write(uint8_t addr, uint32_t data);
	uint32_t mmio_read(uint8_t addr);

	// ---- command push: exactly MEMBRANE_FPGA_DMA_HEADER_BYTES (64)
	// bytes, little-endian wire format (membrane_fpga_header_encode's
	// output). Blocks (ticking the clock) until the bridge's command
	// FIFO accepts it or `max_cycles` elapses; returns false on timeout. ----
	bool cmd_push(const uint8_t header[64], uint64_t max_cycles = 100000);

	// ---- payload streaming: byte-granular convenience wrappers over
	// the bridge's 32-bit-per-beat ports. Return the number of bytes
	// actually transferred before `max_cycles` elapsed (== len on
	// success). ----
	uint32_t payload_push(const uint8_t *data, uint32_t len,
			uint64_t max_cycles = 1000000);
	uint32_t payload_pull(uint8_t *data, uint32_t max_len,
			uint64_t max_cycles = 1000000);

	// Interleaved push+pull in a single tick loop -- REQUIRED for any
	// transfer bigger than the bridge's internal buffering (its own
	// 8-entry result FIFO plus membrane_quant_stream_top's 32-entry
	// output FIFO, ~40 blocks worth): calling payload_push() to
	// completion BEFORE any payload_pull() deadlocks once those FIFOs
	// fill and backpressure the input side, since nothing is draining
	// output while push() blocks (found via this phase's own stress
	// testing -- a batch of 200 blocks hung where 50 didn't). This is
	// the honest analogue of what a real system needs two independent,
	// concurrently-running DMA engines (one per direction) for; this
	// single-threaded emulation instead interleaves both within one
	// cycle-by-cycle loop.
	void transfer(const uint8_t *in_data, uint32_t in_len,
			uint8_t *out_data, uint32_t out_max_len,
			uint32_t *in_sent, uint32_t *out_got,
			uint64_t max_cycles = 10000000);

	// ---- completion queue ----
	// Non-blocking: returns true and fills record[16] if a completion
	// was available this call (pops it), false otherwise (no tick
	// wasted waiting -- caller decides whether/how long to poll).
	bool completion_poll(uint8_t record[16]);
	// Blocking: ticks until a completion arrives or max_cycles elapses.
	bool completion_wait(uint8_t record[16], uint64_t max_cycles = 1000000);

	// One raw clock edge, with correct pre-edge handshake sampling
	// (see class header comment). Exposed so callers needing custom
	// backpressure/stress patterns (task 112) aren't limited to the
	// blocking helpers above.
	void tick();

	// Cost model: cycles charged per 4-byte payload beat beyond the
	// bridge's own native 1-cycle/beat rate, to model a slower,
	// bandwidth-limited link explicitly. Default 1 (native rate, no
	// artificial slowdown).
	void set_cycles_per_beat(uint32_t cycles);

	uint64_t cycle_count() const { return (m_cycle_count); }

	// Peeks completion_valid WITHOUT consuming it (does not assert
	// completion_ready, does not tick) -- lets a caller distinguish
	// "transfer() stopped early because the device is genuinely done"
	// from "transfer() stopped early because max_cycles ran out."
	bool completion_pending() const;

	// Direct register-level access to counters the bridge itself
	// tracks (PROCESSED_BLOCKS, STALL_CYCLES, INPUT_BYTES,
	// OUTPUT_BYTES, ERROR_FLAGS) -- thin wrappers over mmio_read at the
	// fixed offsets in include/membrane/fpga_regs.h.
	uint32_t processed_blocks();
	uint32_t stall_cycles();
	uint64_t input_bytes();
	uint64_t output_bytes();
	uint32_t error_flags();
	void clear_error_flags(uint32_t mask);

private:
	Vmembrane_dma_bridge	*m_dut;
	uint64_t				m_cycle_count;
	uint32_t				m_cycles_per_beat;

	void	settle();
};

#endif
