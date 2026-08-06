// EXP-FPGA-DIV-002 Phase B1: full-datapath parity test, equivalent in
// scope to rtl/tb/tb_top_verilator.cpp (that file is NOT modified by this
// experiment), parametrized at COMPILE TIME by `MEMBRANE_Q8DUAL_VARIANT`
// to build against either the production `membrane_quant_stream_top` or
// the experimental `membrane_quant_stream_top_q8_dual_radix4` (see
// rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4.sv).
// Modeled directly on EXP-FPGA-DIV-001's own
// `rtl/experimental/fp_div/tb_top_verilator_variant.cpp` (same compile-
// time-DUT-selection technique, same generic issue/retire tracking that
// makes no fixed-per-mode-latency assumption, same per-mode latency
// instrumentation, same adversarial-pattern/deadlock-watchdog structure)
// -- trimmed to the 2-way baseline/Q8DUAL form (no B1/B2/B3/B4 branches,
// not applicable here) and with the adversarial patterns re-targeted at
// Q8_0 encode (THIS experiment's own variable-latency path) instead of
// Q4_0 encode.
//
// Runs >=250,000 transactions per mode (Q8 encode, Q8 decode, Q4 encode,
// Q4 decode; task item 6's own per-mode minimum) plus a mixed-mode
// interleave and three dense adversarial patterns (Q8_0 encode burst,
// alternating Q8_0 encode/Q4_0 encode, dense random-mode mixed), under
// randomized valid/ready backpressure, with explicit reset-mid-stream
// and reset-while-Q8-divider-busy tests, checking id/mode/data on every
// retirement and the DUT's own internal credit-accounting/mutual-
// exclusion assertions (see membrane_quant_stream_top_q8_dual_radix4.sv's
// own `` `ifndef SYNTHESIS `` block -- only actually EXERCISED when this
// file is compiled with Verilator's `--assert` flag, see
// scripts/run-exp-q8-divider-002.sh).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <random>
#include <vector>
#include <string>
#include <algorithm>
#if defined(MEMBRANE_Q8DUAL_VARIANT)
#include "Vmembrane_quant_stream_top_q8_dual_radix4.h"
typedef Vmembrane_quant_stream_top_q8_dual_radix4	DutType;
static const char	*VARIANT_NAME = "membrane_quant_stream_top_q8_dual_radix4 (Phase B1: dual exact radix-4 Q8 dividers)";
#else
#include "Vmembrane_quant_stream_top.h"
typedef Vmembrane_quant_stream_top	DutType;
static const char	*VARIANT_NAME = "membrane_quant_stream_top (baseline)";
#endif
#include "verilated.h"

static int	N_PER_MODE = 250000;
static int	N_MIX = 100000;
static int	N_ADV = 70000;	// per adversarial-pattern stage, argv[3]
static const int ID_WIDTH = 16;

enum { MODE_Q8_ENC = 0, MODE_Q8_DEC = 1, MODE_Q4_ENC = 2, MODE_Q4_DEC = 3 };

static std::vector<uint16_t>	g_x;		// N*32
static std::vector<uint8_t>	g_q8pack;	// N*34
static std::vector<uint16_t>	g_q8dequant;	// N*32
static std::vector<uint8_t>	g_q4pack;	// N*18
static std::vector<uint16_t>	g_q4unpack;	// N*32

static std::mt19937	g_rng(0xC0FFEEu);

static void	load_hex16(const char *path, std::vector<uint16_t> &out, long n)
{
	FILE	*f = fopen(path, "r");
	unsigned int	v;
	long	i;

	if (!f)
	{
		fprintf(stderr, "cannot open %s\n", path);
		exit(1);
	}
	out.resize(n);
	i = 0;
	while (i < n && fscanf(f, "%x", &v) == 1)
	{
		out[i] = (uint16_t)v;
		i++;
	}
	if (i != n)
	{
		fprintf(stderr, "%s: expected %ld entries, got %ld\n", path, n, i);
		exit(1);
	}
	fclose(f);
}

static void	load_hex8(const char *path, std::vector<uint8_t> &out, long n)
{
	FILE	*f = fopen(path, "r");
	unsigned int	v;
	long	i;

	if (!f)
	{
		fprintf(stderr, "cannot open %s\n", path);
		exit(1);
	}
	out.resize(n);
	i = 0;
	while (i < n && fscanf(f, "%x", &v) == 1)
	{
		out[i] = (uint8_t)v;
		i++;
	}
	if (i != n)
	{
		fprintf(stderr, "%s: expected %ld entries, got %ld\n", path, n, i);
		exit(1);
	}
	fclose(f);
}

static void	build_in_data(int mode, int blk, uint32_t words[16])
{
	int	j;

	memset(words, 0, 16 * sizeof(uint32_t));
	if (mode == MODE_Q8_ENC || mode == MODE_Q4_ENC)
	{
		for (j = 0; j < 32; j++)
		{
			uint16_t	v = g_x[(size_t)blk * 32 + j];
			int		word = j / 2;
			int		half = j % 2;

			words[word] |= ((uint32_t)v) << (half * 16);
		}
	}
	else if (mode == MODE_Q8_DEC)
	{
		for (j = 0; j < 34; j++)
		{
			uint8_t	b = g_q8pack[(size_t)blk * 34 + j];
			int	word = j / 4;
			int	byte_in_word = j % 4;

			words[word] |= ((uint32_t)b) << (byte_in_word * 8);
		}
	}
	else // MODE_Q4_DEC
	{
		for (j = 0; j < 18; j++)
		{
			uint8_t	b = g_q4pack[(size_t)blk * 18 + j];
			int	word = j / 4;
			int	byte_in_word = j % 4;

			words[word] |= ((uint32_t)b) << (byte_in_word * 8);
		}
	}
}

static bool	check_out_data(int mode, int blk, const uint32_t got[16],
		uint64_t txn_id, uint64_t cycle)
{
	int	j;
	uint8_t	got_bytes[64];

	memcpy(got_bytes, got, 64);
	if (mode == MODE_Q8_ENC)
	{
		for (j = 0; j < 34; j++)
		{
			if (got_bytes[j] != g_q8pack[(size_t)blk * 34 + j])
			{
				fprintf(stderr,
					"MISMATCH Q8_ENC txn=%lu cycle=%lu blk=%d first_diff_byte=%d expect=%02x got=%02x\n",
					(unsigned long)txn_id, (unsigned long)cycle, blk, j,
					g_q8pack[(size_t)blk * 34 + j], got_bytes[j]);
				return (false);
			}
		}
	}
	else if (mode == MODE_Q8_DEC)
	{
		for (j = 0; j < 32; j++)
		{
			uint16_t	gv = got_bytes[j * 2] | (got_bytes[j * 2 + 1] << 8);

			if (gv != g_q8dequant[(size_t)blk * 32 + j])
			{
				fprintf(stderr,
					"MISMATCH Q8_DEC txn=%lu cycle=%lu blk=%d first_diff_lane=%d expect=%04x got=%04x\n",
					(unsigned long)txn_id, (unsigned long)cycle, blk, j,
					g_q8dequant[(size_t)blk * 32 + j], gv);
				return (false);
			}
		}
	}
	else if (mode == MODE_Q4_ENC)
	{
		for (j = 0; j < 18; j++)
		{
			if (got_bytes[j] != g_q4pack[(size_t)blk * 18 + j])
			{
				fprintf(stderr,
					"MISMATCH Q4_ENC txn=%lu cycle=%lu blk=%d first_diff_byte=%d expect=%02x got=%02x\n",
					(unsigned long)txn_id, (unsigned long)cycle, blk, j,
					g_q4pack[(size_t)blk * 18 + j], got_bytes[j]);
				return (false);
			}
		}
	}
	else // MODE_Q4_DEC
	{
		for (j = 0; j < 32; j++)
		{
			uint16_t	gv = got_bytes[j * 2] | (got_bytes[j * 2 + 1] << 8);

			if (gv != g_q4unpack[(size_t)blk * 32 + j])
			{
				fprintf(stderr,
					"MISMATCH Q4_DEC txn=%lu cycle=%lu blk=%d first_diff_lane=%d expect=%04x got=%04x\n",
					(unsigned long)txn_id, (unsigned long)cycle, blk, j,
					g_q4unpack[(size_t)blk * 32 + j], gv);
				return (false);
			}
		}
	}
	return (true);
}

struct InFlightTxn
{
	int		mode;
	int		blk;
	uint16_t	id;
	uint64_t	issue_cycle;
};

static DutType	*g_dut;
static uint64_t	g_cycle;
static uint64_t	g_fails;
static uint64_t	g_checked;
static std::chrono::steady_clock::time_point	g_start_time;
static std::chrono::steady_clock::time_point	g_last_heartbeat;
static uint64_t	g_total_planned;
static std::string	g_stage;

struct ModeLatencyStats
{
	uint64_t	count = 0;
	uint64_t	sum = 0;
	uint64_t	min_c = UINT64_MAX;
	uint64_t	max_c = 0;
};
static ModeLatencyStats	g_mode_latency[4];

static void	record_mode_latency(int mode, uint64_t lat)
{
	ModeLatencyStats	&s = g_mode_latency[mode];

	s.count++;
	s.sum += lat;
	s.min_c = std::min(s.min_c, lat);
	s.max_c = std::max(s.max_c, lat);
}

// Deadlock/timeout watchdog (task item 6). 200,000 cycles is far beyond
// any legitimate wait -- worst measured single-transaction latency for
// Q8_0 encode in this variant is under 30 cycles (see
// experiments/EXP-FPGA-DIV-002/results/b1-differential.json).
static const uint64_t	DEADLOCK_CYCLE_BOUND = 200000;

static void	heartbeat(void)
{
	auto	now = std::chrono::steady_clock::now();
	double	since_hb = std::chrono::duration<double>(now - g_last_heartbeat).count();

	if (since_hb >= 8.0)
	{
		double	elapsed = std::chrono::duration<double>(now - g_start_time).count();
		double	rate = g_checked > 0 ? (double)g_checked / elapsed : 0.0;
		double	eta = rate > 0 ? (double)(g_total_planned - g_checked) / rate : -1.0;

		fprintf(stderr,
			"[heartbeat] variant=%s stage=%s completed=%lu/%lu elapsed=%.1fs eta=%.1fs cycle=%lu fails=%lu\n",
			VARIANT_NAME, g_stage.c_str(), (unsigned long)g_checked, (unsigned long)g_total_planned,
			elapsed, eta, (unsigned long)g_cycle, (unsigned long)g_fails);
		g_last_heartbeat = now;
	}
}

// See rtl/tb/tb_top_verilator.cpp's own header comment on step_cycle for
// why handshake signals are sampled pre-edge -- identical reasoning and
// identical bug class applies here, unchanged.
static void	step_cycle(bool &accepted_out, bool &retired_out)
{
	g_dut->clk = 0;
	g_dut->eval();

	accepted_out = g_dut->in_valid && g_dut->in_ready;
	retired_out = g_dut->out_valid && g_dut->out_ready;

	g_dut->clk = 1;
	g_dut->eval();

	g_cycle++;
	heartbeat();
}

// dense_issue: true = issue every cycle input is available (adversarial
// burst/alternating stages), false = original ~33%-probability random
// gaps.
static void	run_mode_ex(int fixed_mode, int count, bool dense_issue,
		const int *forced_mode_seq, int forced_mode_seq_len)
{
	std::vector<InFlightTxn>	inflight;
	int	issued = 0;
	int	checked_local = 0;
	uint64_t	next_id = 0;
	std::uniform_int_distribution<int>	stall_dist(0, 2);
	std::uniform_int_distribution<int>	mode_dist(0, 3);
	size_t	head = 0;
	uint64_t	last_progress_cycle = g_cycle;

	while (checked_local < count)
	{
		bool	want_issue = (issued < count) &&
			(dense_issue || stall_dist(g_rng) == 0);
		bool	accepted, retired;
		uint32_t	got[16];
		uint16_t	got_id = 0;
		uint8_t		got_mode = 0;

		if (want_issue)
		{
			int	mode;

			if (forced_mode_seq != nullptr)
				mode = forced_mode_seq[issued % forced_mode_seq_len];
			else
				mode = fixed_mode >= 0 ? fixed_mode : mode_dist(g_rng);

			int	blk = issued % N_PER_MODE;
			uint32_t	words[16];

			build_in_data(mode, blk, words);
			g_dut->in_valid = 1;
			g_dut->in_mode = mode;
			g_dut->in_id = (uint16_t)(next_id & 0xFFFFu);
			for (int w = 0; w < 16; w++)
				g_dut->in_data[w] = words[w];
		}
		else
		{
			g_dut->in_valid = 0;
		}
		g_dut->out_ready = (g_rng() & 1) ? 1 : 0;

		g_dut->eval();
		for (int w = 0; w < 16; w++)
			got[w] = g_dut->out_data[w];
		got_id = (uint16_t)g_dut->out_id;
		got_mode = (uint8_t)g_dut->out_mode;

		step_cycle(accepted, retired);

		if (want_issue && accepted)
		{
			int	mode = g_dut->in_mode;
			InFlightTxn	t;

			t.mode = mode;
			t.blk = issued % N_PER_MODE;
			t.id = (uint16_t)(next_id & 0xFFFFu);
			t.issue_cycle = g_cycle;
			inflight.push_back(t);
			issued++;
			next_id++;
		}
		if (retired)
		{
			if (head >= inflight.size())
			{
				fprintf(stderr, "PROTOCOL ERROR: retire with no in-flight transaction, cycle=%lu\n",
					(unsigned long)g_cycle);
				g_fails++;
			}
			else
			{
				InFlightTxn	&t = inflight[head];

				if (got_id != t.id)
				{
					fprintf(stderr, "ID MISMATCH at retire: expect=%u got=%u cycle=%lu\n",
						t.id, (unsigned)got_id, (unsigned long)g_cycle);
					g_fails++;
				}
				if (!check_out_data(t.mode, t.blk, got, t.id, g_cycle))
					g_fails++;
				if (got_mode != (uint8_t)t.mode)
				{
					fprintf(stderr, "MODE MISMATCH at retire: expect=%d got=%d cycle=%lu\n",
						t.mode, got_mode, (unsigned long)g_cycle);
					g_fails++;
				}
				record_mode_latency(t.mode, g_cycle - t.issue_cycle);
				head++;
				checked_local++;
				g_checked++;
				last_progress_cycle = g_cycle;
			}
		}
		if (head < inflight.size() && (g_cycle - last_progress_cycle) > DEADLOCK_CYCLE_BOUND)
		{
			fprintf(stderr,
				"DEADLOCK/TIMEOUT: no retirement for %lu cycles with %zu transaction(s) outstanding, cycle=%lu\n",
				(unsigned long)DEADLOCK_CYCLE_BOUND, inflight.size() - head, (unsigned long)g_cycle);
			g_fails++;
			break;
		}
	}
	g_dut->in_valid = 0;
	g_dut->out_ready = 0;
	g_dut->eval();
}

static void	run_mode(int fixed_mode, int count)
{
	run_mode_ex(fixed_mode, count, false, nullptr, 0);
}

// Adversarial patterns (task item 6, re-targeted at THIS variant's own
// variable-latency path, Q8_0 encode, instead of EXP-FPGA-DIV-001's
// Q4_0 encode): a long dense Q8_0 encode burst (stresses the
// q8enc_inflight full-serialization gate under sustained pressure), and
// strict Q8_0-encode/Q4_0-encode alternation (stresses the mutual-
// exclusion path between the two single-in-flight classes -- the
// scenario most likely to expose a q4enc_inflight/q8enc_inflight
// interaction bug, if one existed).
static void	run_burst_q8enc(int count)
{
	run_mode_ex(MODE_Q8_ENC, count, true, nullptr, 0);
}

static void	run_alternating_q8enc_q4enc(int count)
{
	static const int	pattern[2] = { MODE_Q8_ENC, MODE_Q4_ENC };

	run_mode_ex(-1, count, true, pattern, 2);
}

static void	run_dense_mixed(int count)
{
	run_mode_ex(-1, count, true, nullptr, 0);
}

static void	do_reset(int cycles)
{
	bool	a, r;

	g_dut->rst_n = 0;
	g_dut->in_valid = 0;
	g_dut->out_ready = 0;
	for (int i = 0; i < cycles; i++)
		step_cycle(a, r);
	g_dut->rst_n = 1;
	step_cycle(a, r);
}

int	main(int argc, char **argv)
{
	Verilated::commandArgs(argc, argv);
	if (argc > 1)
		N_PER_MODE = atoi(argv[1]);
	if (argc > 2)
		N_MIX = atoi(argv[2]);
	if (argc > 3)
		N_ADV = atoi(argv[3]);
	g_dut = new DutType;

	fprintf(stderr, "variant: %s\n", VARIANT_NAME);
	fprintf(stderr, "scope: N_PER_MODE=%d N_MIX=%d N_ADV=%d (total planned=%d)\n",
		N_PER_MODE, N_MIX, N_ADV, N_PER_MODE * 4 + N_MIX + N_ADV * 3);
	fprintf(stderr, "Loading golden vectors (%d blocks per format/direction)...\n", N_PER_MODE);
	load_hex16("/tmp/top_x_120k.txt", g_x, (long)N_PER_MODE * 32);
	load_hex8("/tmp/top_q8pack_120k.txt", g_q8pack, (long)N_PER_MODE * 34);
	load_hex16("/tmp/top_q8dequant_120k.txt", g_q8dequant, (long)N_PER_MODE * 32);
	load_hex8("/tmp/top_q4pack_120k.txt", g_q4pack, (long)N_PER_MODE * 18);
	load_hex16("/tmp/top_q4unpack_120k.txt", g_q4unpack, (long)N_PER_MODE * 32);
	fprintf(stderr, "Vectors loaded.\n");

	g_start_time = std::chrono::steady_clock::now();
	g_last_heartbeat = g_start_time;
	g_total_planned = (uint64_t)N_PER_MODE * 4 + N_MIX + (uint64_t)N_ADV * 3;

	do_reset(5);

	// Pipeline-flush / async-reset-mid-stream test -- identical to
	// rtl/tb/tb_top_verilator.cpp's own.
	{
		bool	a, r;
		uint32_t	words[16];

		fprintf(stderr, "[stage] reset-mid-stream flush test\n");
		build_in_data(MODE_Q8_ENC, 0, words);
		g_dut->in_valid = 1;
		g_dut->in_mode = MODE_Q8_ENC;
		g_dut->in_id = 0xABCD & 0xFFFF;
		for (int w = 0; w < 16; w++)
			g_dut->in_data[w] = words[w];
		g_dut->out_ready = 1;
		for (int i = 0; i < 3; i++)
			step_cycle(a, r);
		g_dut->rst_n = 0;
		g_dut->in_valid = 0;
		for (int i = 0; i < 5; i++)
		{
			g_dut->eval();
			if (g_dut->out_valid)
			{
				fprintf(stderr, "FAIL: out_valid asserted during/after reset with no new input (stale output)\n");
				g_fails++;
			}
			step_cycle(a, r);
		}
		g_dut->rst_n = 1;
		step_cycle(a, r);
		fprintf(stderr, "[stage] reset-mid-stream flush test done (fails so far: %lu)\n",
			(unsigned long)g_fails);
	}

	// Reset while a Q8_0 encode transaction is mid-flight, inside
	// q8_scale_dual_radix4's own iteration window (task item 6: "reset
	// while one/both Q8 dividers busy") -- THIS variant's own new
	// variable-latency path. For the baseline build (no
	// MEMBRANE_Q8DUAL_VARIANT), Q8_0 encode's fixed ~7-cycle latency
	// means this transaction has already retired by cycle 12, so this
	// stage degenerates harmlessly into another ordinary reset-mid-
	// stream check for baseline; for the dual-radix4 variant it lands
	// solidly inside the general-path iteration (block 0's amax is
	// essentially never exactly a NaN/Inf/zero early-out case).
	{
		bool	a, r;
		uint32_t	words[16];

		fprintf(stderr, "[stage] reset while Q8_0 encode divider(s) busy\n");
		build_in_data(MODE_Q8_ENC, 0, words);
		g_dut->in_valid = 1;
		g_dut->in_mode = MODE_Q8_ENC;
		g_dut->in_id = 0xBEEF & 0xFFFF;
		for (int w = 0; w < 16; w++)
			g_dut->in_data[w] = words[w];
		g_dut->out_ready = 1;
		step_cycle(a, r);	// issue cycle
		g_dut->in_valid = 0;
		for (int i = 0; i < 12; i++)
			step_cycle(a, r);	// well inside maxabs(5) + radix4 iteration for the dual-radix4 variant
		g_dut->rst_n = 0;
		for (int i = 0; i < 5; i++)
		{
			g_dut->eval();
			if (g_dut->out_valid)
			{
				fprintf(stderr, "FAIL: out_valid asserted during reset while Q8_0 encode divider(s) were busy\n");
				g_fails++;
			}
			step_cycle(a, r);
		}
		g_dut->rst_n = 1;
		step_cycle(a, r);
		g_dut->eval();
		if (!g_dut->in_ready)
		{
			fprintf(stderr, "FAIL: in_ready not asserted one cycle after reset deassertion (post Q8_0-encode-busy reset)\n");
			g_fails++;
		}
		fprintf(stderr, "[stage] reset while Q8_0 encode divider(s) busy done (fails so far: %lu)\n",
			(unsigned long)g_fails);
	}

	g_stage = "Q8_ENC";
	fprintf(stderr, "[stage] Q8 encode: %d transactions\n", N_PER_MODE);
	run_mode(MODE_Q8_ENC, N_PER_MODE);
	fprintf(stderr, "[stage] Q8 encode done, checked=%lu fails=%lu\n",
		(unsigned long)g_checked, (unsigned long)g_fails);

	g_stage = "Q8_DEC";
	fprintf(stderr, "[stage] Q8 decode: %d transactions\n", N_PER_MODE);
	run_mode(MODE_Q8_DEC, N_PER_MODE);
	fprintf(stderr, "[stage] Q8 decode done, checked=%lu fails=%lu\n",
		(unsigned long)g_checked, (unsigned long)g_fails);

	g_stage = "Q4_ENC";
	fprintf(stderr, "[stage] Q4 encode: %d transactions\n", N_PER_MODE);
	run_mode(MODE_Q4_ENC, N_PER_MODE);
	fprintf(stderr, "[stage] Q4 encode done, checked=%lu fails=%lu\n",
		(unsigned long)g_checked, (unsigned long)g_fails);

	g_stage = "Q4_DEC";
	fprintf(stderr, "[stage] Q4 decode: %d transactions\n", N_PER_MODE);
	run_mode(MODE_Q4_DEC, N_PER_MODE);
	fprintf(stderr, "[stage] Q4 decode done, checked=%lu fails=%lu\n",
		(unsigned long)g_checked, (unsigned long)g_fails);

	g_stage = "MIXED";
	fprintf(stderr, "[stage] mixed-mode interleave: %d transactions\n", N_MIX);
	run_mode(-1, N_MIX);
	fprintf(stderr, "[stage] mixed-mode interleave done, checked=%lu fails=%lu\n",
		(unsigned long)g_checked, (unsigned long)g_fails);

	g_stage = "Q8ENC_BURST";
	fprintf(stderr, "[stage] long Q8_0 encode burst (dense issue): %d transactions\n", N_ADV);
	run_burst_q8enc(N_ADV);
	fprintf(stderr, "[stage] Q8_0 encode burst done, checked=%lu fails=%lu\n",
		(unsigned long)g_checked, (unsigned long)g_fails);

	g_stage = "ALT_Q8ENC_Q4ENC";
	fprintf(stderr, "[stage] alternating Q8_0 encode / Q4_0 encode (dense issue): %d transactions\n", N_ADV);
	run_alternating_q8enc_q4enc(N_ADV);
	fprintf(stderr, "[stage] alternating pattern done, checked=%lu fails=%lu\n",
		(unsigned long)g_checked, (unsigned long)g_fails);

	g_stage = "DENSE_MIXED";
	fprintf(stderr, "[stage] dense random-mode mixed (no issue gaps): %d transactions\n", N_ADV);
	run_dense_mixed(N_ADV);
	fprintf(stderr, "[stage] dense mixed done, checked=%lu fails=%lu\n",
		(unsigned long)g_checked, (unsigned long)g_fails);

	double	elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - g_start_time).count();

	static const char	*mode_names[4] = { "Q8_ENC", "Q8_DEC", "Q4_ENC", "Q4_DEC" };

	printf("=== per-mode retire latency (issue cycle -> retire cycle) ===\n");
	for (int m = 0; m < 4; m++)
	{
		ModeLatencyStats	&s = g_mode_latency[m];
		double	mean = s.count ? (double)s.sum / (double)s.count : 0.0;

		printf("  %-8s count=%-8lu min=%-6lu mean=%-10.3f max=%-6lu cycles/txn=%.3f\n",
			mode_names[m], (unsigned long)s.count,
			(unsigned long)(s.count ? s.min_c : 0), mean,
			(unsigned long)s.max_c, mean);
	}
	printf("total cycles this run: %lu, total transactions checked: %lu, overall cycles/transaction: %.3f\n",
		(unsigned long)g_cycle, (unsigned long)g_checked,
		g_checked ? (double)g_cycle / (double)g_checked : 0.0);

	if (g_fails == 0)
		printf("PASS: %s Verilator cosim, %lu transactions, 0 fails, %.1fs\n",
			VARIANT_NAME, (unsigned long)g_checked, elapsed);
	else
		printf("FAIL: %s Verilator cosim, %lu / %lu fails, %.1fs\n",
			VARIANT_NAME, (unsigned long)g_fails, (unsigned long)g_checked, elapsed);

	delete g_dut;
	return (g_fails == 0 ? 0 : 1);
}
