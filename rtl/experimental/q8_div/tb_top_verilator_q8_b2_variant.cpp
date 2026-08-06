// EXP-FPGA-DIV-002 Phase B2: full-datapath correctness + performance-matrix
// tool, one C++ source compiled three times (baseline / Phase B1 / Phase B2)
// via -DMEMBRANE_B1_VARIANT / -DMEMBRANE_B2_VARIANT, modeled directly on
// rtl/experimental/q8_div/tb_top_verilator_q8_variant.cpp's own
// compile-time-DUT-selection technique (that file is NOT modified by this
// phase). Each build independently checks its own DUT against the SAME
// golden vectors with the SAME strict FIFO-order id/mode/data checks -- if
// all three builds report 0 mismatches against that one shared reference,
// they agree with each other by transitivity, the same reasoning Phase B1's
// own full-datapath test already relied on for baseline-vs-B1.
//
// This tool does two things per invocation, both against the SAME running
// DUT instance:
//   1. Correctness stages (task item 6): balanced per-mode traffic, mixed-
//      mode interleave, long Q8_0 encode bursts, Q8_0 encode immediately
//      followed by a long Q4_0 decode burst, alternating Q8_0 encode/Q8_0
//      decode, alternating Q8_0 encode/Q4_0 decode, alternating Q8_0
//      encode/Q4_0 encode, dense random-mode mixed traffic, random input
//      gaps, heavy random out_ready backpressure, a queue-full boundary
//      stage (dense issue against out_ready=0 until FIFOs saturate, then
//      release), and reset stages targeting every reachable scheduler
//      state this testbench can external observe/control (idle, Q8_0
//      encode busy, Q4_0 encode busy, and immediately after a tag_pipe
//      transaction has been admitted while Q8_0 encode is busy -- the
//      scenario Phase B2's own shadow-slot mechanism is built around).
//      Every one of these stages checks payload/id/mode against the golden
//      vectors AND strict issue-order retirement, exactly like every prior
//      phase's own full-datapath tests.
//   2. A 10-profile performance matrix (task item 7): uniform random modes,
//      100% each single mode, two alternating patterns, 10%/25%-Q8_0-encode
//      weighted mixes, and a SIMULATED/reconstructed (not a captured real
//      trace -- disclosed, same convention as this project's own "Q8
//      runtime amax distribution" precedent) decode-heavy mix, each
//      reporting per-mode min/mean/p50/p95/p99/max latency, per-mode
//      throughput, and overall cycles/transaction.
//
// This tool does NOT attempt to classify individual cycles into the stall
// taxonomy from results/b2-stall-root-cause.md by instrumenting internal
// DUT signals (that would require adding debug-only ports to the
// experimental top-levels, which this phase deliberately avoids to keep
// the RTL's own external contract minimal) -- the stall breakdown reported
// in results/b2-stall-breakdown.csv is derived ANALYTICALLY from this
// tool's own real, measured aggregate per-mode latency numbers (baseline
// vs. B1 vs. B2), the same technique results/b2-stall-root-cause.md already
// used for the Phase-A-to-B1 comparison -- disclosed as SIMULATED
// (derived from real measurements), not MEASURED_BY_TOOL at the
// individual-cycle-category level.
//
// Given SEQ_WIDTH=8 in the Phase B2 DUT (rtl/experimental/q8_div/
// membrane_quant_stream_top_q8_dual_radix4_b2.sv), this tool's own
// >=4,000,000-transaction correctness scope wraps that 256-entry sequence
// tag many thousands of times over -- real wraparound coverage, not a
// special-cased scenario.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <random>
#include <vector>
#include <string>
#include <algorithm>
#include <map>
#if defined(MEMBRANE_B2_VARIANT)
#include "Vmembrane_quant_stream_top_q8_dual_radix4_b2.h"
typedef Vmembrane_quant_stream_top_q8_dual_radix4_b2	DutType;
static const char	*VARIANT_NAME = "membrane_quant_stream_top_q8_dual_radix4_b2 (Phase B2: scheduler-improved)";
#elif defined(MEMBRANE_B1_VARIANT)
#include "Vmembrane_quant_stream_top_q8_dual_radix4.h"
typedef Vmembrane_quant_stream_top_q8_dual_radix4	DutType;
static const char	*VARIANT_NAME = "membrane_quant_stream_top_q8_dual_radix4 (Phase B1: full serialization)";
#else
#include "Vmembrane_quant_stream_top.h"
typedef Vmembrane_quant_stream_top	DutType;
static const char	*VARIANT_NAME = "membrane_quant_stream_top (baseline)";
#endif
#include "verilated.h"

static int	N_PER_MODE = 300000;	// golden-vector pool size per format/direction
static int	N_MIX = 150000;
static int	N_ADV = 100000;		// per adversarial-pattern stage
static int	N_PROFILE = 200000;	// per performance-matrix profile

enum { MODE_Q8_ENC = 0, MODE_Q8_DEC = 1, MODE_Q4_ENC = 2, MODE_Q4_DEC = 3 };
static const char	*MODE_NAMES[4] = { "Q8_ENC", "Q8_DEC", "Q4_ENC", "Q4_DEC" };
static const int	ID_WIDTH = 16;

static std::vector<uint16_t>	g_x;
static std::vector<uint8_t>	g_q8pack;
static std::vector<uint16_t>	g_q8dequant;
static std::vector<uint8_t>	g_q4pack;
static std::vector<uint16_t>	g_q4unpack;

static std::mt19937	g_rng(0xC0FFEEu);
// Separate RNG stream, advanced exactly once PER ISSUED TRANSACTION (never
// per cycle), used only to pick each transaction's mode. This is what
// makes "identical seeds and traffic" (task item 7) real rather than
// accidental: g_rng (above) is drawn from once or more per CYCLE (want_issue
// gating, out_ready backpressure), and different DUTs take different
// numbers of cycles to retire the same number of transactions (that is
// the whole point of this comparison) -- if mode selection shared that
// same per-cycle stream, the actual SEQUENCE of transaction modes issued
// would silently diverge between baseline/B1/B2 runs partway through any
// stage that mixes modes, undermining the cross-variant comparison this
// tool exists to produce. Keying mode selection to "the Nth transaction
// issued" instead of "the Nth cycle" guarantees the same seed reproduces
// the exact same mode sequence regardless of how fast any given DUT
// retires it.
static std::mt19937	g_rng_mode(0xC0FFEEu);

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
static uint64_t	g_total_planned;
static std::chrono::steady_clock::time_point	g_start_time;
static std::chrono::steady_clock::time_point	g_last_heartbeat;
static std::string	g_stage;

static void	heartbeat(void)
{
	auto	now = std::chrono::steady_clock::now();
	double	since_hb = std::chrono::duration<double>(now - g_last_heartbeat).count();

	if (since_hb >= 5.0)
	{
		double	elapsed = std::chrono::duration<double>(now - g_start_time).count();
		double	rate = elapsed > 0 ? (double)g_checked / elapsed : 0.0;

		fprintf(stderr,
			"[heartbeat] variant=%s stage=%s checked=%lu elapsed=%.1fs rate=%.0f/s cycle=%lu fails=%lu\n",
			VARIANT_NAME, g_stage.c_str(), (unsigned long)g_checked, elapsed, rate,
			(unsigned long)g_cycle, (unsigned long)g_fails);
		g_last_heartbeat = now;
	}
}

static const uint64_t	DEADLOCK_CYCLE_BOUND = 200000;

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

struct ModeLatencyStats
{
	std::vector<uint32_t>	samples;
	uint64_t	count = 0;
	uint64_t	sum = 0;
	uint32_t	min_c = UINT32_MAX;
	uint32_t	max_c = 0;

	void	record(uint32_t lat)
	{
		samples.push_back(lat);
		count++;
		sum += lat;
		if (lat < min_c)
			min_c = lat;
		if (lat > max_c)
			max_c = lat;
	}
	double	mean(void) const { return count ? (double)sum / (double)count : 0.0; }
	uint32_t	percentile(double p)
	{
		if (samples.empty())
			return (0);
		std::vector<uint32_t>	s = samples;
		std::sort(s.begin(), s.end());
		size_t	idx = (size_t)(p * (double)(s.size() - 1));
		return (s[idx]);
	}
};

// Generic traffic driver: covers both the correctness stages (task item 6)
// and the performance-matrix profiles (task item 7) with the same engine --
// checks payload/id/mode against golden vectors and strict FIFO-order
// retirement (fatal on any violation, matching every prior phase's own
// convention), and records per-mode latency samples.
//
// mode_weights: if non-null, a length-4 array of relative weights used to
// pick each issued transaction's mode (uniform random overall if all equal,
// or e.g. {10,30,30,30} for "10% Q8_0 encode, 90% other"). forced_mode_seq
// takes priority if non-null (alternating patterns). fixed_mode (>=0) takes
// priority over both (single-mode streams).
static void	run_stage(const char *stage_name, int fixed_mode, bool dense_issue,
		double backpressure_prob, const int *forced_mode_seq, int forced_mode_seq_len,
		const int *mode_weights, int count, ModeLatencyStats stats_out[4],
		uint64_t &out_total_cycles)
{
	std::vector<InFlightTxn>	inflight;
	int	issued = 0;
	int	checked_local = 0;
	uint64_t	next_id = 0;
	uint64_t	cyc0 = g_cycle;
	std::uniform_real_distribution<double>	unit_dist(0.0, 1.0);
	std::uniform_int_distribution<int>	mode_dist(0, 3);
	size_t	head = 0;
	uint64_t	last_progress_cycle = g_cycle;

	g_stage = stage_name;
	while (checked_local < count)
	{
		bool	want_issue = (issued < count) &&
			(dense_issue || unit_dist(g_rng) > 0.5);
		bool	accepted, retired;
		uint32_t	got[16];
		uint16_t	got_id = 0;
		uint8_t		got_mode = 0;

		if (want_issue)
		{
			int	mode;

			if (forced_mode_seq != nullptr)
				mode = forced_mode_seq[issued % forced_mode_seq_len];
			else if (fixed_mode >= 0)
				mode = fixed_mode;
			else if (mode_weights != nullptr)
			{
				int	total = mode_weights[0] + mode_weights[1] + mode_weights[2] + mode_weights[3];
				int	r = (int)(g_rng_mode() % (unsigned)total);
				int	acc = 0;

				mode = 3;
				for (int m = 0; m < 4; m++)
				{
					acc += mode_weights[m];
					if (r < acc)
					{
						mode = m;
						break;
					}
				}
			}
			else
				mode = mode_dist(g_rng_mode);

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
		g_dut->out_ready = (unit_dist(g_rng) > backpressure_prob) ? 1 : 0;

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
					fprintf(stderr, "ID MISMATCH at retire: expect=%u got=%u cycle=%lu (stage=%s)\n",
						t.id, (unsigned)got_id, (unsigned long)g_cycle, stage_name);
					g_fails++;
				}
				if (!check_out_data(t.mode, t.blk, got, t.id, g_cycle))
					g_fails++;
				if (got_mode != (uint8_t)t.mode)
				{
					fprintf(stderr, "MODE MISMATCH at retire: expect=%d got=%d cycle=%lu (stage=%s)\n",
						t.mode, got_mode, (unsigned long)g_cycle, stage_name);
					g_fails++;
				}
				if (stats_out != nullptr)
					stats_out[t.mode].record((uint32_t)(g_cycle - t.issue_cycle));
				head++;
				checked_local++;
				g_checked++;
				last_progress_cycle = g_cycle;
			}
		}
		if (head < inflight.size() && (g_cycle - last_progress_cycle) > DEADLOCK_CYCLE_BOUND)
		{
			fprintf(stderr,
				"DEADLOCK/TIMEOUT: no retirement for %lu cycles with %zu transaction(s) outstanding, cycle=%lu (stage=%s)\n",
				(unsigned long)DEADLOCK_CYCLE_BOUND, inflight.size() - head, (unsigned long)g_cycle, stage_name);
			g_fails++;
			break;
		}
	}
	g_dut->in_valid = 0;
	g_dut->out_ready = 0;
	g_dut->eval();
	out_total_cycles = g_cycle - cyc0;
}

static void	reset_mid_state(const char *label, int fixed_mode_before, int settle_cycles)
{
	bool	a, r;
	uint32_t	words[16];

	fprintf(stderr, "[stage] reset while %s\n", label);
	build_in_data(fixed_mode_before, 0, words);
	g_dut->in_valid = 1;
	g_dut->in_mode = fixed_mode_before;
	g_dut->in_id = 0xBEEF & 0xFFFF;
	for (int w = 0; w < 16; w++)
		g_dut->in_data[w] = words[w];
	g_dut->out_ready = 1;
	step_cycle(a, r);
	g_dut->in_valid = 0;
	for (int i = 0; i < settle_cycles; i++)
		step_cycle(a, r);
	g_dut->rst_n = 0;
	for (int i = 0; i < 5; i++)
	{
		g_dut->eval();
		if (g_dut->out_valid)
		{
			fprintf(stderr, "FAIL: out_valid asserted during reset while %s\n", label);
			g_fails++;
		}
		step_cycle(a, r);
	}
	g_dut->rst_n = 1;
	step_cycle(a, r);
	g_dut->eval();
	if (!g_dut->in_ready)
	{
		fprintf(stderr, "FAIL: in_ready not asserted one cycle after reset deassertion (post %s)\n", label);
		g_fails++;
	}
	fprintf(stderr, "[stage] reset while %s done (fails so far: %lu)\n", label, (unsigned long)g_fails);
}

// Reset immediately after admitting a tag_pipe transaction while Q8_0
// encode is busy -- the exact scenario Phase B2's shadow-slot mechanism is
// built around (task item 4's "clean reset during every scheduler state").
// Degenerates harmlessly for baseline/B1 (there is no shadow slot to be
// "in," so this is just another reset-mid-stream variant for them).
static void	reset_mid_shadow_admission(void)
{
	bool	a, r;
	uint32_t	words[16];

	fprintf(stderr, "[stage] reset immediately after admitting a tag_pipe txn while Q8_0 encode busy\n");
	build_in_data(MODE_Q8_ENC, 0, words);
	g_dut->in_valid = 1;
	g_dut->in_mode = MODE_Q8_ENC;
	g_dut->in_id = 0x1111 & 0xFFFF;
	for (int w = 0; w < 16; w++)
		g_dut->in_data[w] = words[w];
	g_dut->out_ready = 1;
	step_cycle(a, r);	// issue Q8_0 encode
	build_in_data(MODE_Q8_DEC, 1, words);
	g_dut->in_valid = 1;
	g_dut->in_mode = MODE_Q8_DEC;
	g_dut->in_id = 0x2222 & 0xFFFF;
	for (int w = 0; w < 16; w++)
		g_dut->in_data[w] = words[w];
	step_cycle(a, r);	// issue Q8_0 decode right behind it (the shadow candidate)
	g_dut->in_valid = 0;
	for (int i = 0; i < 3; i++)
		step_cycle(a, r);
	g_dut->rst_n = 0;
	for (int i = 0; i < 5; i++)
	{
		g_dut->eval();
		if (g_dut->out_valid)
		{
			fprintf(stderr, "FAIL: out_valid asserted during reset mid-shadow-admission\n");
			g_fails++;
		}
		step_cycle(a, r);
	}
	g_dut->rst_n = 1;
	step_cycle(a, r);
	g_dut->eval();
	if (!g_dut->in_ready)
	{
		fprintf(stderr, "FAIL: in_ready not asserted one cycle after reset deassertion (post mid-shadow-admission)\n");
		g_fails++;
	}
	fprintf(stderr, "[stage] reset mid-shadow-admission done (fails so far: %lu)\n", (unsigned long)g_fails);
}

// Queue-full boundary case (task item 6): dense issue with out_ready held
// low until both FIFOs saturate (in_ready deasserts, confirming real
// backpressure, not just a slow drain), then release and drain -- a single
// self-contained driver with ONE persistent in-flight deque spanning both
// phases (unlike run_stage, whose in-flight tracking is local to a single
// call and would lose track of anything still outstanding when a
// zero-out_ready phase is deliberately never going to retire anything
// within that call).
static void	queue_full_boundary(int count, ModeLatencyStats stats_out[4])
{
	std::vector<InFlightTxn>	inflight;
	uint64_t	next_id = 0;
	size_t		head = 0;
	int		issued = 0;
	uint64_t	last_progress_cycle = g_cycle;
	std::uniform_real_distribution<double>	unit_dist(0.0, 1.0);

	g_stage = "QUEUE_FULL_SATURATE";
	fprintf(stderr, "[stage] queue-full boundary: dense issue, out_ready=0 until saturation, then drain (%d transactions)\n", count);

	// Phase 1: saturate. Issue as fast as in_ready allows, out_ready held
	// at 0, until in_ready itself deasserts (proof the input FIFO, and
	// therefore the whole reservation chain behind it, is genuinely full)
	// or a generous cycle bound is hit.
	bool	saw_backpressure = false;

	for (int i = 0; i < 5000 && !saw_backpressure; i++)
	{
		bool	accepted, retired;
		uint32_t	words[16];
		int	mode = (int)(g_rng_mode() % 4u);
		int	blk = issued % N_PER_MODE;

		build_in_data(mode, blk, words);
		g_dut->in_valid = 1;
		g_dut->in_mode = mode;
		g_dut->in_id = (uint16_t)(next_id & 0xFFFFu);
		for (int w = 0; w < 16; w++)
			g_dut->in_data[w] = words[w];
		g_dut->out_ready = 0;
		g_dut->eval();
		if (!g_dut->in_ready)
			saw_backpressure = true;
		step_cycle(accepted, retired);
		if (accepted)
		{
			InFlightTxn	t;

			t.mode = mode;
			t.blk = blk;
			t.id = (uint16_t)(next_id & 0xFFFFu);
			t.issue_cycle = g_cycle;
			inflight.push_back(t);
			issued++;
			next_id++;
		}
		if (retired)
		{
			fprintf(stderr, "FAIL: retirement observed while out_ready held at 0 during queue-full saturate\n");
			g_fails++;
		}
	}
	g_dut->in_valid = 0;
	g_dut->eval();
	fprintf(stderr, "[stage] queue-full saturate: issued=%d in_ready_deasserted=%s\n",
		issued, saw_backpressure ? "yes (real backpressure observed)" : "no (never saturated within bound)");

	// Phase 2: drain. Release out_ready, issue the remaining `count`
	// transactions with normal random gaps, checking every retirement
	// (including the ones queued up during phase 1) against the SAME
	// persistent inflight deque.
	g_stage = "QUEUE_FULL_DRAIN";
	while ((int)(head) < count)
	{
		bool	want_issue = (issued < count) && (unit_dist(g_rng) > 0.3);
		bool	accepted, retired;
		uint32_t	got[16];
		uint16_t	got_id = 0;
		uint8_t		got_mode = 0;

		if (want_issue)
		{
			int	mode = (int)(g_rng_mode() % 4u);
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
		g_dut->out_ready = 1;
		g_dut->eval();
		for (int w = 0; w < 16; w++)
			got[w] = g_dut->out_data[w];
		got_id = (uint16_t)g_dut->out_id;
		got_mode = (uint8_t)g_dut->out_mode;

		step_cycle(accepted, retired);

		if (want_issue && accepted)
		{
			InFlightTxn	t;

			t.mode = g_dut->in_mode;
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
				fprintf(stderr, "PROTOCOL ERROR: retire with no in-flight transaction, cycle=%lu (stage=QUEUE_FULL_DRAIN)\n",
					(unsigned long)g_cycle);
				g_fails++;
			}
			else
			{
				InFlightTxn	&t = inflight[head];

				if (got_id != t.id)
				{
					fprintf(stderr, "ID MISMATCH at retire: expect=%u got=%u cycle=%lu (stage=QUEUE_FULL_DRAIN)\n",
						t.id, (unsigned)got_id, (unsigned long)g_cycle);
					g_fails++;
				}
				if (!check_out_data(t.mode, t.blk, got, t.id, g_cycle))
					g_fails++;
				if (got_mode != (uint8_t)t.mode)
				{
					fprintf(stderr, "MODE MISMATCH at retire: expect=%d got=%d cycle=%lu (stage=QUEUE_FULL_DRAIN)\n",
						t.mode, got_mode, (unsigned long)g_cycle);
					g_fails++;
				}
				if (stats_out != nullptr)
					stats_out[t.mode].record((uint32_t)(g_cycle - t.issue_cycle));
				head++;
				g_checked++;
				last_progress_cycle = g_cycle;
			}
		}
		if (head < inflight.size() && (g_cycle - last_progress_cycle) > DEADLOCK_CYCLE_BOUND)
		{
			fprintf(stderr,
				"DEADLOCK/TIMEOUT: no retirement for %lu cycles with %zu transaction(s) outstanding, cycle=%lu (stage=QUEUE_FULL_DRAIN)\n",
				(unsigned long)DEADLOCK_CYCLE_BOUND, inflight.size() - head, (unsigned long)g_cycle);
			g_fails++;
			break;
		}
	}
	g_dut->in_valid = 0;
	g_dut->out_ready = 0;
	g_dut->eval();
	fprintf(stderr, "[stage] queue-full boundary done (fails so far: %lu)\n", (unsigned long)g_fails);
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
	if (argc > 4)
		N_PROFILE = atoi(argv[4]);
	g_dut = new DutType;

	fprintf(stderr, "variant: %s\n", VARIANT_NAME);
	fprintf(stderr, "scope: N_PER_MODE=%d N_MIX=%d N_ADV=%d N_PROFILE=%d\n",
		N_PER_MODE, N_MIX, N_ADV, N_PROFILE);
	fprintf(stderr, "Loading golden vectors (%d blocks per format/direction)...\n", N_PER_MODE);
	load_hex16("/tmp/top_x_120k.txt", g_x, (long)N_PER_MODE * 32);
	load_hex8("/tmp/top_q8pack_120k.txt", g_q8pack, (long)N_PER_MODE * 34);
	load_hex16("/tmp/top_q8dequant_120k.txt", g_q8dequant, (long)N_PER_MODE * 32);
	load_hex8("/tmp/top_q4pack_120k.txt", g_q4pack, (long)N_PER_MODE * 18);
	load_hex16("/tmp/top_q4unpack_120k.txt", g_q4unpack, (long)N_PER_MODE * 32);
	fprintf(stderr, "Vectors loaded.\n");

	g_start_time = std::chrono::steady_clock::now();
	g_last_heartbeat = g_start_time;

	do_reset(5);

	uint64_t	dummy_cycles;
	ModeLatencyStats	correctness_stats[4];

	// ---- reset-in-every-observable-scheduler-state (task item 4/6) ----
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
		fprintf(stderr, "[stage] reset-mid-stream flush test done (fails so far: %lu)\n", (unsigned long)g_fails);
	}
	reset_mid_state("Q8_0 encode divider(s) busy", MODE_Q8_ENC, 12);
	reset_mid_state("Q4_0 encode divider busy", MODE_Q4_ENC, 12);
	reset_mid_shadow_admission();

	uint64_t	correctness_total_planned = (uint64_t)N_PER_MODE * 4 + N_MIX
		+ (uint64_t)N_ADV * 6 + 128;
	fprintf(stderr, "=== correctness scope: %lu transactions planned (task item 6 minimum: 4,000,000) ===\n",
		(unsigned long)correctness_total_planned);

	run_stage("Q8_ENC_balanced", MODE_Q8_ENC, false, 0.5, nullptr, 0, nullptr, N_PER_MODE, correctness_stats, dummy_cycles);
	run_stage("Q8_DEC_balanced", MODE_Q8_DEC, false, 0.5, nullptr, 0, nullptr, N_PER_MODE, correctness_stats, dummy_cycles);
	run_stage("Q4_ENC_balanced", MODE_Q4_ENC, false, 0.5, nullptr, 0, nullptr, N_PER_MODE, correctness_stats, dummy_cycles);
	run_stage("Q4_DEC_balanced", MODE_Q4_DEC, false, 0.5, nullptr, 0, nullptr, N_PER_MODE, correctness_stats, dummy_cycles);
	fprintf(stderr, "[stage] balanced per-mode done, checked=%lu fails=%lu\n", (unsigned long)g_checked, (unsigned long)g_fails);

	run_stage("MIXED_random_gaps", -1, false, 0.5, nullptr, 0, nullptr, N_MIX, correctness_stats, dummy_cycles);
	fprintf(stderr, "[stage] mixed-mode interleave done, checked=%lu fails=%lu\n", (unsigned long)g_checked, (unsigned long)g_fails);

	run_stage("Q8ENC_BURST", MODE_Q8_ENC, true, 0.0, nullptr, 0, nullptr, N_ADV, correctness_stats, dummy_cycles);
	fprintf(stderr, "[stage] long Q8_0 encode burst done, checked=%lu fails=%lu\n", (unsigned long)g_checked, (unsigned long)g_fails);

	run_stage("Q8ENC_THEN_Q4DEC_BURST_enc", MODE_Q8_ENC, true, 0.0, nullptr, 0, nullptr, N_ADV / 4, correctness_stats, dummy_cycles);
	run_stage("Q8ENC_THEN_Q4DEC_BURST_dec", MODE_Q4_DEC, true, 0.0, nullptr, 0, nullptr, N_ADV, correctness_stats, dummy_cycles);
	fprintf(stderr, "[stage] Q8_0 encode -> long Q4_0 decode burst done, checked=%lu fails=%lu\n", (unsigned long)g_checked, (unsigned long)g_fails);

	{
		static const int	pat_q8q8[2] = { MODE_Q8_ENC, MODE_Q8_DEC };
		run_stage("ALT_Q8ENC_Q8DEC", -1, true, 0.0, pat_q8q8, 2, nullptr, N_ADV, correctness_stats, dummy_cycles);
	}
	fprintf(stderr, "[stage] alternating Q8_0 encode/Q8_0 decode done, checked=%lu fails=%lu\n", (unsigned long)g_checked, (unsigned long)g_fails);

	{
		static const int	pat_q8q4e[2] = { MODE_Q8_ENC, MODE_Q4_ENC };
		run_stage("ALT_Q8ENC_Q4ENC", -1, true, 0.0, pat_q8q4e, 2, nullptr, N_ADV, correctness_stats, dummy_cycles);
	}
	fprintf(stderr, "[stage] alternating Q8_0 encode/Q4_0 encode done, checked=%lu fails=%lu\n", (unsigned long)g_checked, (unsigned long)g_fails);

	{
		static const int	pat_q8q4d[2] = { MODE_Q8_ENC, MODE_Q4_DEC };
		run_stage("ALT_Q8ENC_Q4DEC", -1, true, 0.0, pat_q8q4d, 2, nullptr, N_ADV, correctness_stats, dummy_cycles);
	}
	fprintf(stderr, "[stage] alternating Q8_0 encode/Q4_0 decode done, checked=%lu fails=%lu\n", (unsigned long)g_checked, (unsigned long)g_fails);

	run_stage("DENSE_RANDOM_MODE_HEAVY_BP", -1, true, 0.7, nullptr, 0, nullptr, N_ADV, correctness_stats, dummy_cycles);
	fprintf(stderr, "[stage] dense random-mode, heavy random backpressure done, checked=%lu fails=%lu\n", (unsigned long)g_checked, (unsigned long)g_fails);

	queue_full_boundary(N_ADV, correctness_stats);

	fprintf(stderr, "=== correctness total: checked=%lu fails=%lu ===\n", (unsigned long)g_checked, (unsigned long)g_fails);

	// ---- performance matrix (task item 7): 10 traffic profiles, fresh
	// per-profile stats, reported in an easily-parsed line format. ----
	struct Profile
	{
		const char	*name;
		int		fixed_mode;
		const int	*forced_seq;
		int		forced_seq_len;
		const int	*weights;
	};
	static const int	pat_q8q4d_perf[2] = { MODE_Q8_ENC, MODE_Q4_DEC };
	static const int	pat_q8q8_perf[2] = { MODE_Q8_ENC, MODE_Q8_DEC };
	static const int	w_10pct[4] = { 10, 30, 30, 30 };
	static const int	w_25pct[4] = { 25, 25, 25, 25 };
	// SIMULATED/reconstructed decode-heavy mix (NOT a captured real trace):
	// 20% Q8_0 encode, 40% Q8_0 decode, 10% Q4_0 encode, 30% Q4_0 decode --
	// representative of a decode-dominant inference-serving shape (most
	// traffic reads back already-quantized weights/KV entries, a minority
	// quantizes fresh activations), disclosed as a plausible reconstruction
	// only, same convention as this project's own prior "Q8 runtime amax
	// distribution sample" precedent.
	static const int	w_realistic[4] = { 20, 40, 10, 30 };

	Profile	profiles[10] = {
		{ "uniform_random_modes", -1, nullptr, 0, nullptr },
		{ "100pct_Q8_ENC", MODE_Q8_ENC, nullptr, 0, nullptr },
		{ "100pct_Q8_DEC", MODE_Q8_DEC, nullptr, 0, nullptr },
		{ "100pct_Q4_ENC", MODE_Q4_ENC, nullptr, 0, nullptr },
		{ "100pct_Q4_DEC", MODE_Q4_DEC, nullptr, 0, nullptr },
		{ "alt_Q8ENC_Q4DEC", -1, pat_q8q4d_perf, 2, nullptr },
		{ "alt_Q8ENC_Q8DEC", -1, pat_q8q8_perf, 2, nullptr },
		{ "10pct_Q8ENC_90pct_other", -1, nullptr, 0, w_10pct },
		{ "25pct_Q8ENC_75pct_other", -1, nullptr, 0, w_25pct },
		{ "realistic_reconstructed_mix_SIMULATED", -1, nullptr, 0, w_realistic },
	};

	printf("=== performance matrix (task item 7), variant=%s ===\n", VARIANT_NAME);
	for (int p = 0; p < 10; p++)
	{
		ModeLatencyStats	prof_stats[4];
		uint64_t	prof_cycles = 0;

		do_reset(3);
		run_stage(profiles[p].name, profiles[p].fixed_mode, (profiles[p].forced_seq != nullptr),
			0.5, profiles[p].forced_seq, profiles[p].forced_seq_len, profiles[p].weights,
			N_PROFILE, prof_stats, prof_cycles);

		uint64_t	prof_total = 0;
		for (int m = 0; m < 4; m++)
			prof_total += prof_stats[m].count;

		printf("PROFILE %s total_cycles=%lu total_txn=%lu cycles_per_txn=%.4f accepted_per_cycle=%.6f\n",
			profiles[p].name, (unsigned long)prof_cycles, (unsigned long)prof_total,
			prof_total ? (double)prof_cycles / (double)prof_total : 0.0,
			prof_cycles ? (double)prof_total / (double)prof_cycles : 0.0);
		for (int m = 0; m < 4; m++)
		{
			ModeLatencyStats	&s = prof_stats[m];

			if (s.count == 0)
				continue;
			printf("  MODE %-8s count=%-8lu min=%-6u mean=%-10.3f p50=%-6u p95=%-6u p99=%-6u max=%-6u throughput_txn_per_cycle=%.6f\n",
				MODE_NAMES[m], (unsigned long)s.count, s.min_c, s.mean(),
				s.percentile(0.50), s.percentile(0.95), s.percentile(0.99), s.max_c,
				prof_cycles ? (double)s.count / (double)prof_cycles : 0.0);
		}
	}

	double	elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - g_start_time).count();

	printf("=== correctness summary ===\n");
	printf("total cycles this run: %lu, total transactions checked: %lu, overall cycles/transaction: %.3f\n",
		(unsigned long)g_cycle, (unsigned long)g_checked,
		g_checked ? (double)g_cycle / (double)g_checked : 0.0);
	for (int m = 0; m < 4; m++)
	{
		ModeLatencyStats	&s = correctness_stats[m];

		if (s.count == 0)
			continue;
		printf("  %-8s count=%-8lu min=%-6u mean=%-10.3f p50=%-6u p95=%-6u p99=%-6u max=%-6u\n",
			MODE_NAMES[m], (unsigned long)s.count, s.min_c, s.mean(),
			s.percentile(0.50), s.percentile(0.95), s.percentile(0.99), s.max_c);
	}

	if (g_fails == 0 && g_checked >= 4000000)
		printf("PASS: %s Verilator cosim, %lu transactions, 0 fails, %.1fs\n",
			VARIANT_NAME, (unsigned long)g_checked, elapsed);
	else if (g_fails == 0)
		printf("PASS (below 4,000,000 minimum -- quick mode): %s Verilator cosim, %lu transactions, 0 fails, %.1fs\n",
			VARIANT_NAME, (unsigned long)g_checked, elapsed);
	else
		printf("FAIL: %s Verilator cosim, %lu / %lu fails, %.1fs\n",
			VARIANT_NAME, (unsigned long)g_fails, (unsigned long)g_checked, elapsed);

	delete g_dut;
	return (g_fails == 0 ? 0 : 1);
}
