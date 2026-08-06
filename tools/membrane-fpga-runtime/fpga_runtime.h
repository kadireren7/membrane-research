#ifndef MEMBRANE_FPGA_RUNTIME_H
#define MEMBRANE_FPGA_RUNTIME_H

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "fpga_emu_device.h"
#include "membrane/fpga_dma.h"

// Phase 5.4 host runtime API: device_open/close/submit/poll/wait/
// cancel/get_stats over the FpgaEmuDevice backend (fpga_emu_device.h).
//
// Async model, disclosed: this is a single-threaded, cooperative-
// scheduling design -- submit() only enqueues a request locally; actual
// progress (pushing queued commands into the device, draining
// completions) happens inside pump(), which poll()/wait() call
// internally. A REAL PCIe driver would use a kernel interrupt or
// eventfd to learn about completions asynchronously without polling;
// this emulation has no real hardware interrupt to wait on, so
// poll()/wait() driving the simulated clock themselves is the honest
// stand-in, not a design this phase claims scales to a real multi-
// threaded production driver without further work (see
// docs/phase5-pcie-hardware-loop.md section 3).
typedef struct s_membrane_fpga_stats
{
	uint64_t	submitted;
	uint64_t	completed;
	uint64_t	failed;
	uint64_t	timed_out;
	uint64_t	retried;
	uint64_t	cancelled;
	uint64_t	bytes_in;
	uint64_t	bytes_out;
	uint64_t	device_cycles;
}	membrane_fpga_stats_t;

class FpgaRuntime
{
public:
	explicit FpgaRuntime(uint32_t queue_depth = 16, uint32_t max_retries = 2);
	~FpgaRuntime();

	void	device_open();
	void	device_close();

	// Hard reset mid-flight (task 112's device-reset stress case):
	// resets the underlying RTL (rst_n pulse) AND drops this runtime's
	// own queue/in-flight bookkeeping, matching what a real driver
	// would have to do after a device reset -- any handle that was
	// pending or in flight at the time is simply gone (poll/wait on it
	// afterward correctly report "unknown handle", never a stale/wrong
	// completion).
	void	reset_mid_flight();

	// Constructs a correct, checksummed packet (the normal path) and
	// enqueues it. Returns an opaque handle (nonzero) for
	// poll/wait/cancel, or 0 if the local pending-queue is already at
	// queue_depth (backpressure -- caller should wait/poll before
	// retrying, mirroring a real submission-queue-full condition).
	uint64_t	submit(membrane_fpga_op_t op, const uint8_t *payload,
				uint32_t payload_len, uint32_t element_count,
				uint32_t output_capacity, uint16_t policy_layer_id = 0,
				uint16_t policy_flags = 0, uint32_t flags = 0);

	// Raw path: caller supplies the full 64-byte header verbatim (no
	// checksum/field computation performed here) -- used by the DMA
	// stress tests (task 112) to construct deliberately malformed
	// headers, bad checksums, short output buffers, etc. Still goes
	// through the same queue/retry/timeout machinery as submit().
	uint64_t	raw_submit(const uint8_t header[MEMBRANE_FPGA_DMA_HEADER_BYTES],
				const uint8_t *payload, uint32_t payload_len,
				uint32_t output_capacity_hint);

	// Non-blocking. Returns true iff `handle`'s result is ready (drains
	// available device completions first via pump()).
	bool	poll(uint64_t handle, uint8_t *out_data, uint32_t max_len,
				uint32_t *out_len, uint32_t *status);

	// Blocking, bounded by timeout_cycles of DEVICE clock ticks (not
	// wall-clock time, since this is a cycle-driven emulation).
	bool	wait(uint64_t handle, uint8_t *out_data, uint32_t max_len,
				uint32_t *out_len, uint32_t *status,
				uint64_t timeout_cycles = 20000000);

	// Best-effort: true if `handle` was still queued (not yet issued to
	// the device) and got dropped without ever touching the RTL. Once a
	// request has actually been issued into the bridge, this simplified
	// single-command-in-flight design (rtl/membrane_dma_bridge.sv's own
	// disclosed scope) cannot un-issue it -- cancel() on an in-flight
	// handle returns false and the request still completes normally.
	bool	cancel(uint64_t handle);

	// Drives the device clock, pushes one pending submission into the
	// bridge if the device is idle and something is queued, and drains
	// any available completions into m_completed. Called internally by
	// poll/wait; exposed for tests that need fine-grained control over
	// how much simulated time elapses between actions (task 112's
	// backpressure/timeout/reset-mid-flight scenarios).
	void	pump(uint64_t max_cycles = 1);

	membrane_fpga_stats_t	get_stats() const { return (m_stats); }

	FpgaEmuDevice	&device() { return (m_device); }

private:
	struct Pending
	{
		uint8_t				header[MEMBRANE_FPGA_DMA_HEADER_BYTES];
		std::vector<uint8_t>	payload;
		uint32_t			output_capacity;
		uint32_t			retries_left;
	};

	struct Completed
	{
		std::vector<uint8_t>	data;
		uint32_t			status;
	};

	FpgaEmuDevice				m_device;
	uint32_t					m_queue_depth;
	uint32_t					m_max_retries;
	// Budget issue_next_if_idle() applies to its own synchronous
	// cmd_push/transfer/completion_wait calls -- WITHOUT this, a
	// caller's wait(..., timeout_cycles) only bounds the OUTER retry
	// loop, not the inner blocking device operation it's waiting on,
	// so a too-small timeout_cycles would silently have no effect
	// (the operation just runs to completion using the device layer's
	// own generous multi-million-cycle defaults instead). wait() sets
	// this from its own timeout_cycles right before pumping so a
	// caller's timeout genuinely bounds total device time, which is
	// what makes "did the FPGA path time out" a meaningful, testable
	// condition for fallback logic (task 117) rather than a parameter
	// that only sometimes matters.
	uint64_t					m_op_cycle_budget;
	uint64_t					m_next_handle;
	bool						m_command_in_flight;
	uint64_t					m_in_flight_handle;
	uint32_t					m_in_flight_output_cap;
	std::deque<uint64_t>		m_pending_order;
	std::map<uint64_t, Pending>	m_pending;
	std::map<uint64_t, Completed>	m_completed;
	membrane_fpga_stats_t		m_stats;

	uint64_t	enqueue(const uint8_t header[MEMBRANE_FPGA_DMA_HEADER_BYTES],
				const uint8_t *payload, uint32_t payload_len,
				uint32_t output_capacity);
	void	issue_next_if_idle();
	void	drain_completions();
};

#endif
