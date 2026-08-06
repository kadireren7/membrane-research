#include <cstring>

#include "membrane/block.h"
#include "fpga_runtime.h"

FpgaRuntime::FpgaRuntime(uint32_t queue_depth, uint32_t max_retries)
{
	m_queue_depth = queue_depth;
	m_max_retries = max_retries;
	m_next_handle = 1;
	m_command_in_flight = false;
	m_in_flight_handle = 0;
	m_in_flight_output_cap = 0;
	m_op_cycle_budget = 20000000;
	memset(&m_stats, 0, sizeof(m_stats));
}

FpgaRuntime::~FpgaRuntime()
{
}

void	FpgaRuntime::device_open()
{
	m_device.reset();
}

void	FpgaRuntime::device_close()
{
	// No persistent OS-level resource to release for the emulated
	// backend (no fd, no mmap) -- present for API-surface completeness
	// and so a future real-hardware backend has a real teardown point.
}

void	FpgaRuntime::reset_mid_flight()
{
	m_device.reset();
	m_pending.clear();
	m_pending_order.clear();
	m_completed.clear();
	m_command_in_flight = false;
	m_in_flight_handle = 0;
	m_in_flight_output_cap = 0;
}

uint64_t	FpgaRuntime::enqueue(
			const uint8_t header[MEMBRANE_FPGA_DMA_HEADER_BYTES],
			const uint8_t *payload, uint32_t payload_len,
			uint32_t output_capacity)
{
	membrane_fpga_header_t	hdr;
	Pending					p;
	uint64_t				handle;

	if (m_pending_order.size() >= m_queue_depth)
		return (0);
	membrane_fpga_header_decode(header, &hdr);
	handle = hdr.transaction_id;
	memcpy(p.header, header, MEMBRANE_FPGA_DMA_HEADER_BYTES);
	p.payload.assign(payload, payload + payload_len);
	p.output_capacity = output_capacity;
	p.retries_left = m_max_retries;
	m_pending[handle] = p;
	m_pending_order.push_back(handle);
	m_stats.submitted++;
	return (handle);
}

uint64_t	FpgaRuntime::submit(membrane_fpga_op_t op, const uint8_t *payload,
			uint32_t payload_len, uint32_t element_count,
			uint32_t output_capacity, uint16_t policy_layer_id,
			uint16_t policy_flags, uint32_t flags)
{
	membrane_fpga_header_t	hdr;
	uint8_t					header_bytes[MEMBRANE_FPGA_DMA_HEADER_BYTES];

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = MEMBRANE_FPGA_DMA_MAGIC;
	hdr.version_major = MEMBRANE_FPGA_DMA_VERSION_MAJOR;
	hdr.version_minor = MEMBRANE_FPGA_DMA_VERSION_MINOR;
	hdr.transaction_id = m_next_handle++;
	hdr.operation = (uint8_t)op;
	hdr.element_count = element_count;
	hdr.input_byte_length = payload_len;
	hdr.output_capacity = output_capacity;
	hdr.policy_layer_id = policy_layer_id;
	hdr.policy_flags = policy_flags;
	hdr.flags = flags;
	hdr.payload_checksum = membrane_block_checksum(payload, payload_len);
	membrane_fpga_header_encode(&hdr, header_bytes);
	return (enqueue(header_bytes, payload, payload_len, output_capacity));
}

uint64_t	FpgaRuntime::raw_submit(
			const uint8_t header[MEMBRANE_FPGA_DMA_HEADER_BYTES],
			const uint8_t *payload, uint32_t payload_len,
			uint32_t output_capacity_hint)
{
	return (enqueue(header, payload, payload_len, output_capacity_hint));
}

void	FpgaRuntime::issue_next_if_idle()
{
	uint64_t	handle;
	Pending		p;
	std::vector<uint8_t>	out_buf;
	uint32_t	pulled;
	uint32_t	pushed;
	uint8_t		completion[16];
	bool		got_completion;
	Completed	c;

	if (m_command_in_flight || m_pending_order.empty())
		return;
	handle = m_pending_order.front();
	m_pending_order.pop_front();
	p = m_pending[handle];
	m_pending.erase(handle);
	m_command_in_flight = true;
	m_in_flight_handle = handle;
	m_in_flight_output_cap = p.output_capacity;

	// Host-side (this runtime, standing in for the "root-complex-
	// adjacent" logic membrane_dma_bridge.sv's own header comment
	// describes) checksum validation: the RTL bridge deliberately does
	// NOT re-implement a hardware CRC engine (disclosed scope decision,
	// see that file), so a corrupted header/payload must be caught
	// here, before anything is pushed to the device, or not at all.
	{
		membrane_fpga_header_t	hdr;
		membrane_fpga_validate_result_t	vr;

		membrane_fpga_header_decode(p.header, &hdr);
		vr = membrane_fpga_header_validate(&hdr, p.header);
		if (vr == MEMBRANE_FPGA_VALIDATE_BAD_HEADER_CHECKSUM)
		{
			m_stats.failed++;
			c.status = 0x1u; // MEMBRANE_FPGA_ERR_BAD_HEADER_CHECKSUM
			m_completed[handle] = c;
			m_command_in_flight = false;
			return;
		}
		if (vr == MEMBRANE_FPGA_VALIDATE_OK
				&& !membrane_fpga_payload_checksum_ok(&hdr, p.payload.data(),
					(size_t)p.payload.size()))
		{
			m_stats.failed++;
			c.status = 0x2u; // MEMBRANE_FPGA_ERR_BAD_PAYLOAD_CHECKSUM
			m_completed[handle] = c;
			m_command_in_flight = false;
			return;
		}
		// Any OTHER validation failure (bad magic/version/operation/
		// zero element_count) is intentionally let through to the
		// device -- the bridge's own ST_CHECK state (see
		// membrane_dma_bridge.sv) catches those in hardware
		// (MEMBRANE_FPGA_ERR_MALFORMED_HEADER), which is real RTL
		// behavior this runtime should not shadow/pre-empt.
	}

	if (!m_device.cmd_push(p.header, m_op_cycle_budget))
	{
		m_stats.failed++;
		c.status = 0xFFFFFFFFu; // runtime-side: device unresponsive
		m_completed[handle] = c;
		m_command_in_flight = false;
		return;
	}
	// Push input and pull output CONCURRENTLY (single interleaved tick
	// loop) -- doing these sequentially deadlocks once a batch is
	// larger than the bridge's internal buffering, since nothing drains
	// output while a blocking push() call is still running. See
	// FpgaEmuDevice::transfer's header comment.
	out_buf.resize(p.output_capacity > 0 ? p.output_capacity : 1);
	m_device.transfer(p.payload.data(), (uint32_t)p.payload.size(),
		out_buf.data(), p.output_capacity, &pushed, &pulled, m_op_cycle_budget);
	m_stats.bytes_in += pushed;
	m_stats.bytes_out += pulled;

	// If the transfer stopped early WITHOUT a completion already
	// waiting, it genuinely ran out of budget -- report the timeout
	// immediately rather than spending another full m_op_cycle_budget
	// inside completion_wait finding out the same thing. But an early
	// stop WITH a completion pending is not a timeout at all: an error
	// path (malformed header, short output, etc.) can legitimately
	// finish having produced fewer bytes than output_capacity was
	// sized for (see FpgaEmuDevice::transfer's header comment) -- that
	// case must fall through to read the real completion status below,
	// not be misreported as a runtime-side timeout.
	if ((pushed < (uint32_t)p.payload.size()
			|| pulled < (p.output_capacity > 0 ? p.output_capacity : 0))
			&& !m_device.completion_pending())
	{
		m_stats.timed_out++;
		c.status = 0xFFFFFFFEu;
		m_completed[handle] = c;
		m_command_in_flight = false;
		return;
	}

	got_completion = m_device.completion_wait(completion, m_op_cycle_budget);
	m_command_in_flight = false;
	if (!got_completion)
	{
		m_stats.timed_out++;
		c.status = 0xFFFFFFFEu; // runtime-side: completion timeout
		m_completed[handle] = c;
		return;
	}
	c.data.assign(out_buf.begin(), out_buf.begin() + pulled);
	c.status = (uint32_t)(completion[8] | (completion[9] << 8)
			| (completion[10] << 16) | (completion[11] << 24));
	if (c.status != 0)
		m_stats.failed++;
	else
		m_stats.completed++;
	m_completed[handle] = c;
}

void	FpgaRuntime::drain_completions()
{
	// issue_next_if_idle() already drives the full push/pull/wait
	// sequence for the one in-flight command synchronously (matching
	// rtl/membrane_dma_bridge.sv's own one-command-at-a-time scope, see
	// its header comment) -- nothing additional to drain here beyond
	// what pump() below already triggers.
}

void	FpgaRuntime::pump(uint64_t max_cycles)
{
	uint64_t	i;

	issue_next_if_idle();
	drain_completions();
	i = 0;
	while (i < max_cycles)
	{
		m_device.tick();
		i++;
	}
}

bool	FpgaRuntime::poll(uint64_t handle, uint8_t *out_data, uint32_t max_len,
			uint32_t *out_len, uint32_t *status)
{
	std::map<uint64_t, Completed>::iterator	it;

	pump(0);
	it = m_completed.find(handle);
	if (it == m_completed.end())
		return (false);
	*out_len = (uint32_t)it->second.data.size();
	if (*out_len > max_len)
		*out_len = max_len;
	memcpy(out_data, it->second.data.data(), *out_len);
	*status = it->second.status;
	m_completed.erase(it);
	return (true);
}

bool	FpgaRuntime::wait(uint64_t handle, uint8_t *out_data, uint32_t max_len,
			uint32_t *out_len, uint32_t *status, uint64_t timeout_cycles)
{
	uint64_t	spent;
	uint64_t	saved_budget;

	// See m_op_cycle_budget's header comment: without this, a small
	// timeout_cycles here would only bound the outer retry loop below,
	// not the synchronous device work pump() -> issue_next_if_idle()
	// performs, so it would silently have no effect on whether a slow/
	// hung device is actually detected as a timeout.
	saved_budget = m_op_cycle_budget;
	m_op_cycle_budget = timeout_cycles > 0 ? timeout_cycles : 1;
	spent = 0;
	while (spent < timeout_cycles)
	{
		if (poll(handle, out_data, max_len, out_len, status))
		{
			m_op_cycle_budget = saved_budget;
			return (true);
		}
		if (m_pending.find(handle) == m_pending.end()
				&& handle != m_in_flight_handle
				&& m_completed.find(handle) == m_completed.end())
		{
			m_op_cycle_budget = saved_budget;
			return (false); // unknown handle, will never complete
		}
		pump(1);
		spent++;
	}
	m_op_cycle_budget = saved_budget;
	return (false);
}

bool	FpgaRuntime::cancel(uint64_t handle)
{
	std::map<uint64_t, Pending>::iterator	it;
	std::deque<uint64_t>::iterator			oit;

	it = m_pending.find(handle);
	if (it == m_pending.end())
		return (false);
	m_pending.erase(it);
	oit = m_pending_order.begin();
	while (oit != m_pending_order.end())
	{
		if (*oit == handle)
		{
			m_pending_order.erase(oit);
			break;
		}
		++oit;
	}
	m_stats.cancelled++;
	return (true);
}
