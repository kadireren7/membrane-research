// EXP-FPGA-DIV-001 Phase B1/B2: full-datapath parity test, equivalent in
// scope to rtl/tb/tb_top_verilator.cpp (that file is NOT modified by
// this experiment), parametrized at COMPILE TIME by
// `MEMBRANE_B1_VARIANT`/`MEMBRANE_B2_VARIANT` to build against the
// production membrane_quant_stream_top, the Phase B1
// membrane_quant_stream_top_b1, or the Phase B2
// membrane_quant_stream_top_b2 (see
// rtl/experimental/fp_div/membrane_quant_stream_top_b1.sv /
// _b2.sv). This is "the same testbench" in the sense the experiment
// spec asks for: one C++ source, compiled three ways (see
// scripts/run-exp-fp-divider-001.sh), identical checks and randomized-
// backpressure/reset-injection/ordering/credit-accounting logic against
// every variant, so any difference in the runs' results is attributable
// to the RTL change alone, not to a testbench divergence. This file's
// generic issue/retire tracking (see run_mode() below) does not assume
// any fixed per-mode latency, so it needed no logic change at all to
// also validate Phase B2's variable-latency, single-in-flight-per-mode
// Q4_0 encode retirement -- only the per-mode latency instrumentation
// below (added for Phase B2's own reporting needs, harmless for
// baseline/B1 too) and the DUT type selection are new.
//
// Runs the same coverage rtl/tb/tb_top_verilator.cpp does: >=100,000
// transactions per mode (Q8 encode, Q8 decode, Q4 encode, Q4 decode)
// plus a mixed-mode interleave, under randomized valid/ready
// backpressure, with an explicit reset-mid-stream flush test, checking
// id/mode/data on every retirement and the input-FIFO/output-FIFO
// credit-accounting assertions already built into
// membrane_quant_stream_top(_b1/_b2).sv itself. Running the SAME mixed
// set of all 4 modes against the B1/B2 top-levels is what confirms the
// Q8 paths (byte-identical RTL in every top) are unaffected by the
// Q4-only change, not a separate test.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <random>
#include <vector>
#include <string>
#include <algorithm>
#if defined(MEMBRANE_B4_VARIANT)
#include "Vmembrane_quant_stream_top_b4.h"
typedef Vmembrane_quant_stream_top_b4	DutType;
static const char	*VARIANT_NAME = "membrane_quant_stream_top_b4 (Phase B4: radix-4 exact Q4 divider, B2-style scheduling)";
#elif defined(MEMBRANE_B3_VARIANT)
#include "Vmembrane_quant_stream_top_b3.h"
typedef Vmembrane_quant_stream_top_b3	DutType;
#ifndef MEMBRANE_B3_LABEL
#define MEMBRANE_B3_LABEL "B3"
#endif
static const char	*VARIANT_NAME = "membrane_quant_stream_top_b3 (Phase B3: decoupled scheduling, " MEMBRANE_B3_LABEL ")";
#elif defined(MEMBRANE_B2_VARIANT)
#include "Vmembrane_quant_stream_top_b2.h"
typedef Vmembrane_quant_stream_top_b2	DutType;
static const char	*VARIANT_NAME = "membrane_quant_stream_top_b2 (Phase B2: iterative exact Q4 divider)";
#elif defined(MEMBRANE_B1_VARIANT)
#include "Vmembrane_quant_stream_top_b1.h"
typedef Vmembrane_quant_stream_top_b1	DutType;
static const char	*VARIANT_NAME = "membrane_quant_stream_top_b1 (Phase B1: neg-pow2 Q4 scale)";
#else
#include "Vmembrane_quant_stream_top.h"
typedef Vmembrane_quant_stream_top	DutType;
static const char	*VARIANT_NAME = "membrane_quant_stream_top (baseline)";
#endif
#include "verilated.h"

// Defaults reproduce the exact 520,000-transaction full scope (matches
// rtl/tb/tb_top_verilator.cpp's own N_PER_MODE/N_MIX). Overridable via
// argv (see main()) so scripts/run-exp-fp-divider-001.sh's `--quick`
// mode can run a small, fast integration smoke test against the SAME
// deterministic golden-vector files and the SAME checks, not a
// separately-written lighter-weight test -- `--full` uses the
// defaults, reproducing the numbers in this experiment's own docs
// exactly.
static int	N_PER_MODE = 120000;
static int	N_MIX = 40000;
static int	N_ADV = 40000;	// Phase B3 adversarial-pattern stages, argv[3]
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

// Returns true on match; on mismatch, prints a detailed report
// (transaction id, cycle, expected, actual, first differing byte) and
// returns false.
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

// Per-mode retire-latency stats (issue cycle -> retire cycle), added
// for Phase B2's own reporting needs (Q4 encode's latency is no longer
// fixed at L_MAX -- this measures it directly instead of assuming it);
// harmless bookkeeping for baseline/B1 too, where it's expected to come
// out constant (==L_MAX) for every mode.
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

// Phase B3 instrumentation (task item 6/7's "queue high-water mark" /
// "stall breakdown" ask): sampled every cycle via
// membrane_quant_stream_top_b3.sv's `verilator public` debug signals.
// A no-op (all counters stay 0) for baseline/B1/B2, which don't define
// MEMBRANE_B3_VARIANT and so never have this function called at all.
#if defined(MEMBRANE_B3_VARIANT)
struct B3Stats
{
	uint64_t	high_water = 0;
	uint64_t	stall_depth_cycles = 0;
	uint64_t	stall_q4busy_cycles = 0;
	uint64_t	stall_outfifo_cycles = 0;
	uint64_t	q4enc_busy_cycles = 0;
	uint64_t	simultaneous_completion_cycles = 0;
};
static B3Stats	g_b3;

static void	sample_b3_stats(void)
{
	g_b3.high_water = std::max<uint64_t>(g_b3.high_water, (uint64_t)g_dut->dbg_outstanding);
	if (g_dut->dbg_stall_depth_o) g_b3.stall_depth_cycles++;
	if (g_dut->dbg_stall_q4busy_o) g_b3.stall_q4busy_cycles++;
	if (g_dut->dbg_stall_outfifo_o) g_b3.stall_outfifo_cycles++;
	if (g_dut->dbg_q4enc_inflight) g_b3.q4enc_busy_cycles++;
	if (g_dut->dbg_simultaneous_completion_o) g_b3.simultaneous_completion_cycles++;
}
#endif

// Deadlock/timeout watchdog (task item 6): if this many consecutive
// cycles pass with no retirement while at least one transaction is
// outstanding, something is stuck -- report it as a hard failure rather
// than hanging forever. 200,000 cycles is far beyond any legitimate wait
// (worst measured single-transaction latency across baseline/B1/B2/B3 is
// under 500 cycles, and randomized out_ready backpressure/`--quick`
// workloads never withhold readiness for anywhere near this long).
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

// See rtl/tb/tb_top_verilator.cpp's own header comment on step_cycle
// for why handshake signals are sampled pre-edge -- identical
// reasoning and identical bug class applies here, this is the same
// convention, unchanged.
static void	step_cycle(bool &accepted_out, bool &retired_out)
{
	g_dut->clk = 0;
	g_dut->eval();

	accepted_out = g_dut->in_valid && g_dut->in_ready;
	retired_out = g_dut->out_valid && g_dut->out_ready;

	g_dut->clk = 1;
	g_dut->eval();

#if defined(MEMBRANE_B3_VARIANT)
	sample_b3_stats();
#endif

	g_cycle++;
	heartbeat();
}

// issue_bias: 0 = original random-gap behavior (stall_dist(0,2)==0, ~33%
// issue probability). >0 = issue every cycle input is available (used by
// the adversarial burst/alternating-pattern stages below to actually
// pack the pipeline instead of leaving gaps).
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

// Adversarial patterns (task item 6): long Q4_0-encode bursts, and strict
// Q4_0-encode/Q8 alternation, both issued DENSELY (no random gaps) so the
// pipeline is actually kept full -- these are exactly the traffic shapes
// most likely to (a) saturate a shallow B3 reorder buffer (verifying the
// `dbg_stall_depth` backpressure path is itself correct, not just
// present) and (b) exercise `dbg_simultaneous_completion` (a fast tag_pipe
// mode chain completing the same cycle as a long-running Q4_0 encode).
// Harmless, valid regression coverage for baseline/B1/B2 too (they simply
// never hit the B3-only instrumentation).
static void	run_burst_q4enc(int count)
{
	run_mode_ex(MODE_Q4_ENC, count, true, nullptr, 0);
}

static void	run_alternating_q4enc_q8(int count)
{
	static const int	pattern[2] = { MODE_Q4_ENC, MODE_Q8_ENC };

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

	// Reset while a Q4_0 encode transaction is mid-flight -- the one
	// scenario specific to Phase B2's variable-latency iterative
	// divider (task item 6: "reset while divider busy"). For
	// baseline/B1 this Q4_0 encode transaction would already have
	// finished well before cycle 15 (fixed ~12-cycle latency), so this
	// stage degenerates harmlessly into another ordinary reset-mid-
	// stream check for those variants; for B2 it lands solidly inside
	// the iterative divider's 26-cycle iteration (general path, since
	// block 0's mx is essentially never exactly a NaN/Inf/zero
	// early-out case). Checks: no stale out_valid during/after reset,
	// in_ready comes back once reset deasserts, and normal operation
	// (checked by every subsequent run_mode() stage, 0 fails required)
	// resumes cleanly afterward.
	{
		bool	a, r;
		uint32_t	words[16];

		fprintf(stderr, "[stage] reset while Q4_0 encode divider busy\n");
		build_in_data(MODE_Q4_ENC, 0, words);
		g_dut->in_valid = 1;
		g_dut->in_mode = MODE_Q4_ENC;
		g_dut->in_id = 0xBEEF & 0xFFFF;
		for (int w = 0; w < 16; w++)
			g_dut->in_data[w] = words[w];
		g_dut->out_ready = 1;
		step_cycle(a, r);	// issue cycle
		g_dut->in_valid = 0;
		for (int i = 0; i < 15; i++)
			step_cycle(a, r);	// well inside the iterative divider's 26-cycle iteration for B2
		g_dut->rst_n = 0;
		for (int i = 0; i < 5; i++)
		{
			g_dut->eval();
			if (g_dut->out_valid)
			{
				fprintf(stderr, "FAIL: out_valid asserted during reset while Q4_0 encode divider was busy\n");
				g_fails++;
			}
			step_cycle(a, r);
		}
		g_dut->rst_n = 1;
		step_cycle(a, r);
		g_dut->eval();
		if (!g_dut->in_ready)
		{
			fprintf(stderr, "FAIL: in_ready not asserted one cycle after reset deassertion (post Q4_0-encode-busy reset)\n");
			g_fails++;
		}
		fprintf(stderr, "[stage] reset while Q4_0 encode divider busy done (fails so far: %lu)\n",
			(unsigned long)g_fails);
	}

#if defined(MEMBRANE_B3_VARIANT)
	// Reset while multiple transactions across different modes are
	// simultaneously outstanding (task item 6: "reset while queues non-
	// empty") -- only meaningful for B3, since baseline/B1/B2 never have
	// more than one transaction outstanding across modes at a time. Issue
	// a Q4_0 encode (long-running), then immediately several Q8_0
	// encode/Q4_0 decode transactions behind it (which the whole point of
	// B3 is to let proceed concurrently), then reset mid-flight.
	{
		bool	a, r;
		uint32_t	words[16];

		fprintf(stderr, "[stage] reset while queues non-empty (B3-specific)\n");
		build_in_data(MODE_Q4_ENC, 0, words);
		g_dut->in_valid = 1;
		g_dut->in_mode = MODE_Q4_ENC;
		g_dut->in_id = 0x1000;
		for (int w = 0; w < 16; w++)
			g_dut->in_data[w] = words[w];
		g_dut->out_ready = 1;
		step_cycle(a, r);
		for (int k = 0; k < 6; k++)
		{
			build_in_data(MODE_Q8_ENC, k + 1, words);
			g_dut->in_valid = 1;
			g_dut->in_mode = MODE_Q8_ENC;
			g_dut->in_id = (uint16_t)(0x1001 + k);
			for (int w = 0; w < 16; w++)
				g_dut->in_data[w] = words[w];
			step_cycle(a, r);
		}
		g_dut->in_valid = 0;
		for (int i = 0; i < 4; i++)
			step_cycle(a, r);
		g_dut->rst_n = 0;
		for (int i = 0; i < 5; i++)
		{
			g_dut->eval();
			if (g_dut->out_valid)
			{
				fprintf(stderr, "FAIL: out_valid asserted during reset with multiple queues non-empty\n");
				g_fails++;
			}
			step_cycle(a, r);
		}
		g_dut->rst_n = 1;
		step_cycle(a, r);
		g_dut->eval();
		if (!g_dut->in_ready)
		{
			fprintf(stderr, "FAIL: in_ready not asserted one cycle after reset deassertion (post queues-non-empty reset)\n");
			g_fails++;
		}
		if (g_dut->dbg_outstanding != 0)
		{
			fprintf(stderr, "FAIL: reorder buffer outstanding count did not reset to 0 (got %u)\n",
				(unsigned)g_dut->dbg_outstanding);
			g_fails++;
		}
		fprintf(stderr, "[stage] reset while queues non-empty done (fails so far: %lu)\n",
			(unsigned long)g_fails);
	}
#endif

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

	// Adversarial mode patterns (task item 6). Valid, harmless coverage
	// for every variant (baseline/B1/B2 must still pass them with 0
	// fails); the point for B3 specifically is exercising the reorder
	// buffer under sustained pressure rather than the occasional random
	// mode switch MIXED already covers.
	g_stage = "Q4ENC_BURST";
	fprintf(stderr, "[stage] long Q4_0 encode burst (dense issue): %d transactions\n", N_ADV);
	run_burst_q4enc(N_ADV);
	fprintf(stderr, "[stage] Q4_0 encode burst done, checked=%lu fails=%lu\n",
		(unsigned long)g_checked, (unsigned long)g_fails);

	g_stage = "ALT_Q4ENC_Q8";
	fprintf(stderr, "[stage] alternating Q4_0 encode / Q8_0 encode (dense issue): %d transactions\n", N_ADV);
	run_alternating_q4enc_q8(N_ADV);
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

#if defined(MEMBRANE_B3_VARIANT)
	printf("=== B3 scheduler stats (%s) ===\n", MEMBRANE_B3_LABEL);
	printf("  reorder buffer high-water mark (outstanding): %lu\n", (unsigned long)g_b3.high_water);
	printf("  issue-stall cycles, depth-bound (outstanding==REORDER_DEPTH): %lu (%.2f%% of run)\n",
		(unsigned long)g_b3.stall_depth_cycles,
		g_cycle ? 100.0 * (double)g_b3.stall_depth_cycles / (double)g_cycle : 0.0);
	printf("  issue-stall cycles, Q4_0 encode divider busy (single-in-flight): %lu (%.2f%% of run)\n",
		(unsigned long)g_b3.stall_q4busy_cycles,
		g_cycle ? 100.0 * (double)g_b3.stall_q4busy_cycles / (double)g_cycle : 0.0);
	printf("  issue-stall cycles, output-FIFO slot unavailable: %lu (%.2f%% of run)\n",
		(unsigned long)g_b3.stall_outfifo_cycles,
		g_cycle ? 100.0 * (double)g_b3.stall_outfifo_cycles / (double)g_cycle : 0.0);
	printf("  Q4_0 encode divider busy cycles (any reason): %lu (%.2f%% of run)\n",
		(unsigned long)g_b3.q4enc_busy_cycles,
		g_cycle ? 100.0 * (double)g_b3.q4enc_busy_cycles / (double)g_cycle : 0.0);
	printf("  cycles with both completion ports firing simultaneously: %lu\n",
		(unsigned long)g_b3.simultaneous_completion_cycles);
#endif

	if (g_fails == 0)
		printf("PASS: %s Verilator cosim, %lu transactions, 0 fails, %.1fs\n",
			VARIANT_NAME, (unsigned long)g_checked, elapsed);
	else
		printf("FAIL: %s Verilator cosim, %lu / %lu fails, %.1fs\n",
			VARIANT_NAME, (unsigned long)g_fails, (unsigned long)g_checked, elapsed);

	delete g_dut;
	return (g_fails == 0 ? 0 : 1);
}
