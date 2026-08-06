#include <cstring>

#include "Vmembrane_dma_bridge.h"
#include "verilated.h"

#include "fpga_emu_device.h"

FpgaEmuDevice::FpgaEmuDevice()
{
	m_dut = new Vmembrane_dma_bridge;
	m_cycle_count = 0;
	m_cycles_per_beat = 1;
	m_dut->rst_n = 0;
	m_dut->clk = 0;
	m_dut->reg_write = 0;
	m_dut->reg_read = 0;
	m_dut->cmd_push_valid = 0;
	m_dut->payload_in_valid = 0;
	m_dut->payload_out_ready = 0;
	m_dut->completion_ready = 0;
}

FpgaEmuDevice::~FpgaEmuDevice()
{
	delete m_dut;
}

void	FpgaEmuDevice::settle()
{
	m_dut->eval();
}

// Same clocking discipline as Phase 5.3's tb_top_verilator.cpp: settle
// combinational logic with clk low (this is the value the DUT's own
// synchronous logic will actually use for THIS edge's handshake
// decisions), THEN raise the clock. Callers that need to read
// valid/ready must do so between settle() and the rising edge -- see
// cmd_push/payload_push/etc below, which all follow this pattern.
void	FpgaEmuDevice::tick()
{
	m_dut->clk = 0;
	m_dut->eval();
	m_dut->clk = 1;
	m_dut->eval();
	m_cycle_count++;
}

void	FpgaEmuDevice::reset(int cycles)
{
	int	i;

	m_dut->rst_n = 0;
	m_dut->reg_write = 0;
	m_dut->reg_read = 0;
	m_dut->cmd_push_valid = 0;
	m_dut->payload_in_valid = 0;
	m_dut->payload_out_ready = 0;
	m_dut->completion_ready = 0;
	i = 0;
	while (i < cycles)
	{
		tick();
		i++;
	}
	m_dut->rst_n = 1;
	tick();
	m_cycle_count = 0;
}

void	FpgaEmuDevice::mmio_write(uint8_t addr, uint32_t data)
{
	m_dut->reg_write = 1;
	m_dut->reg_read = 0;
	m_dut->reg_addr = addr;
	m_dut->reg_wdata = data;
	settle();
	tick();
	m_dut->reg_write = 0;
	settle();
}

uint32_t	FpgaEmuDevice::mmio_read(uint8_t addr)
{
	uint32_t	v;

	m_dut->reg_write = 0;
	m_dut->reg_read = 1;
	m_dut->reg_addr = addr;
	settle();
	v = m_dut->reg_rdata;
	tick();
	m_dut->reg_read = 0;
	settle();
	return (v);
}

bool	FpgaEmuDevice::cmd_push(const uint8_t header[64], uint64_t max_cycles)
{
	uint64_t	budget;
	bool		accepted;
	int			w;

	for (w = 0; w < 16; w++)
	{
		uint32_t	word;

		word = (uint32_t)header[w * 4]
			| ((uint32_t)header[w * 4 + 1] << 8)
			| ((uint32_t)header[w * 4 + 2] << 16)
			| ((uint32_t)header[w * 4 + 3] << 24);
		m_dut->cmd_push_header[w] = word;
	}
	m_dut->cmd_push_valid = 1;
	budget = 0;
	accepted = false;
	while (budget < max_cycles)
	{
		settle();
		if (m_dut->cmd_push_ready)
			accepted = true;
		tick();
		budget++;
		if (accepted)
			break;
	}
	m_dut->cmd_push_valid = 0;
	settle();
	return (accepted);
}

uint32_t	FpgaEmuDevice::payload_push(const uint8_t *data, uint32_t len,
			uint64_t max_cycles)
{
	uint32_t	sent;
	uint64_t	budget;
	uint32_t	beat_pace;

	sent = 0;
	budget = 0;
	beat_pace = 0;
	while (sent < len && budget < max_cycles)
	{
		uint32_t	word;
		uint32_t	b;

		word = 0;
		b = 0;
		while (b < 4 && sent + b < len)
		{
			word |= (uint32_t)data[sent + b] << (b * 8);
			b++;
		}
		m_dut->payload_in_data = word;
		m_dut->payload_in_valid = (beat_pace == 0) ? 1 : 0;
		settle();
		if (m_dut->payload_in_valid && m_dut->payload_in_ready)
		{
			sent += 4;
			beat_pace = (m_cycles_per_beat > 1) ? (m_cycles_per_beat - 1) : 0;
		}
		else if (beat_pace > 0)
			beat_pace--;
		tick();
		budget++;
	}
	m_dut->payload_in_valid = 0;
	settle();
	return (sent > len ? len : sent);
}

uint32_t	FpgaEmuDevice::payload_pull(uint8_t *data, uint32_t max_len,
			uint64_t max_cycles)
{
	uint32_t	got;
	uint64_t	budget;

	got = 0;
	budget = 0;
	m_dut->payload_out_ready = 1;
	while (got < max_len && budget < max_cycles)
	{
		settle();
		if (m_dut->payload_out_valid && m_dut->payload_out_ready)
		{
			uint32_t	word;
			uint32_t	b;

			word = m_dut->payload_out_data;
			b = 0;
			while (b < 4 && got + b < max_len)
			{
				data[got + b] = (uint8_t)((word >> (b * 8)) & 0xFFu);
				b++;
			}
			got += 4;
		}
		tick();
		budget++;
	}
	m_dut->payload_out_ready = 0;
	settle();
	return (got > max_len ? max_len : got);
}

void	FpgaEmuDevice::transfer(const uint8_t *in_data, uint32_t in_len,
			uint8_t *out_data, uint32_t out_max_len,
			uint32_t *in_sent, uint32_t *out_got, uint64_t max_cycles)
{
	uint32_t	sent;
	uint32_t	got;
	uint64_t	budget;
	uint32_t	in_beat_pace;

	sent = 0;
	got = 0;
	budget = 0;
	in_beat_pace = 0;
	m_dut->payload_out_ready = 1;
	// Also watch (peek, without consuming) completion_valid as an early-
	// exit signal: an error-path completion (malformed header, bad
	// output_capacity, etc.) can arrive having produced FEWER output
	// bytes than out_max_len was sized for (the caller sized it for the
	// success case, since the actual outcome isn't knowable in
	// advance) -- without this, a caller requesting more output bytes
	// than an error path will ever produce would otherwise spin for the
	// entire max_cycles budget waiting for bytes that will never come,
	// even though the transaction is already, genuinely done.
	while ((sent < in_len || got < out_max_len) && budget < max_cycles
			&& !m_dut->completion_valid)
	{
		if (sent < in_len)
		{
			uint32_t	word;
			uint32_t	b;

			word = 0;
			b = 0;
			while (b < 4 && sent + b < in_len)
			{
				word |= (uint32_t)in_data[sent + b] << (b * 8);
				b++;
			}
			m_dut->payload_in_data = word;
			m_dut->payload_in_valid = (in_beat_pace == 0) ? 1 : 0;
		}
		else
			m_dut->payload_in_valid = 0;

		settle();

		if (sent < in_len && m_dut->payload_in_valid && m_dut->payload_in_ready)
		{
			sent += 4;
			in_beat_pace = (m_cycles_per_beat > 1) ? (m_cycles_per_beat - 1) : 0;
		}
		else if (in_beat_pace > 0)
			in_beat_pace--;
		if (got < out_max_len && m_dut->payload_out_valid && m_dut->payload_out_ready)
		{
			uint32_t	word;
			uint32_t	b;

			word = m_dut->payload_out_data;
			b = 0;
			while (b < 4 && got + b < out_max_len)
			{
				out_data[got + b] = (uint8_t)((word >> (b * 8)) & 0xFFu);
				b++;
			}
			got += 4;
		}
		tick();
		budget++;
	}
	m_dut->payload_in_valid = 0;
	m_dut->payload_out_ready = 0;
	settle();
	*in_sent = (sent > in_len ? in_len : sent);
	*out_got = (got > out_max_len ? out_max_len : got);
}

static void	pack_completion(Vmembrane_dma_bridge *dut, uint8_t record[16])
{
	int	w;

	w = 0;
	while (w < 4)
	{
		uint32_t	word;

		word = dut->completion_record[w];
		record[w * 4] = (uint8_t)(word & 0xFFu);
		record[w * 4 + 1] = (uint8_t)((word >> 8) & 0xFFu);
		record[w * 4 + 2] = (uint8_t)((word >> 16) & 0xFFu);
		record[w * 4 + 3] = (uint8_t)((word >> 24) & 0xFFu);
		w++;
	}
}

bool	FpgaEmuDevice::completion_poll(uint8_t record[16])
{
	m_dut->completion_ready = 1;
	settle();
	if (!m_dut->completion_valid)
	{
		m_dut->completion_ready = 0;
		tick();
		settle();
		return (false);
	}
	pack_completion(m_dut, record);
	tick();
	m_dut->completion_ready = 0;
	settle();
	return (true);
}

bool	FpgaEmuDevice::completion_wait(uint8_t record[16], uint64_t max_cycles)
{
	uint64_t	budget;

	budget = 0;
	while (budget < max_cycles)
	{
		if (completion_poll(record))
			return (true);
		budget++;
	}
	return (false);
}

void	FpgaEmuDevice::set_cycles_per_beat(uint32_t cycles)
{
	m_cycles_per_beat = (cycles < 1) ? 1 : cycles;
}

uint32_t	FpgaEmuDevice::processed_blocks()
{
	return (mmio_read(0x24));
}

uint32_t	FpgaEmuDevice::stall_cycles()
{
	return (mmio_read(0x28));
}

uint64_t	FpgaEmuDevice::input_bytes()
{
	uint64_t	lo;
	uint64_t	hi;

	lo = mmio_read(0x2C);
	hi = mmio_read(0x30);
	return (lo | (hi << 32));
}

uint64_t	FpgaEmuDevice::output_bytes()
{
	uint64_t	lo;
	uint64_t	hi;

	lo = mmio_read(0x34);
	hi = mmio_read(0x38);
	return (lo | (hi << 32));
}

uint32_t	FpgaEmuDevice::error_flags()
{
	return (mmio_read(0x20));
}

void	FpgaEmuDevice::clear_error_flags(uint32_t mask)
{
	mmio_write(0x20, mask);
}

bool	FpgaEmuDevice::completion_pending() const
{
	return (m_dut->completion_valid != 0);
}
