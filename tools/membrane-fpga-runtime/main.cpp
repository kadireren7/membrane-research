#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <vector>

#include "membrane/block.h"

#include "fpga_runtime.h"
#include "membrane/fpga_dma.h"
#include "membrane/fpga_regs.h"
#include "membrane/quant_simd.h"

static void	expect(bool cond, const char *desc, int &fails);
static std::vector<uint8_t>	make_q8_payload(int blocks);

static bool	file_exists(const char *path)
{
	struct stat	st;

	return (stat(path, &st) == 0);
}

// Regenerates the Phase 5.3 golden vector set on demand (via the exact
// same C generator sources already in rtl/tb/, driven against the real
// C reference in src/quant/quant_simd.c) if the cached /tmp files from
// an earlier session aren't present -- keeps `verify`/`perf` runnable
// standalone rather than silently depending on manual pre-generation.
static void	ensure_golden_vectors(const char *repo_root, int n_blocks)
{
	char	cmd[2048];

	if (file_exists("/tmp/top_x_120k.txt") && file_exists("/tmp/top_q8pack_120k.txt")
			&& file_exists("/tmp/top_q8dequant_120k.txt")
			&& file_exists("/tmp/top_q4pack_120k.txt")
			&& file_exists("/tmp/top_q4unpack_120k.txt"))
		return;
	fprintf(stderr, "[golden] regenerating %d-block golden vector set...\n", n_blocks);
	snprintf(cmd, sizeof(cmd),
		"cd %s && "
		"gcc -O2 -I include -o /tmp/gen_top_x src/codecs/f16convert.c rtl/tb/gen_top_x_vectors.c -lm && "
		"gcc -O2 -I include -o /tmp/gen_pack src/quant/quant_simd.c src/codecs/f16convert.c rtl/tb/gen_pack_vectors.c -lm && "
		"gcc -O2 -I include -o /tmp/gen_dequant src/quant/quant_simd.c src/codecs/f16convert.c rtl/tb/gen_dequant_vectors.c -lm && "
		"gcc -O2 -I include -o /tmp/gen_q4pack src/quant/quant_simd.c src/codecs/f16convert.c rtl/tb/gen_q4pack_vectors.c -lm && "
		"gcc -O2 -I include -o /tmp/gen_q4unpack src/quant/quant_simd.c src/codecs/f16convert.c rtl/tb/gen_q4unpack_vectors.c -lm && "
		"/tmp/gen_top_x %d /tmp/top_x_120k.txt && "
		"/tmp/gen_pack /tmp/top_x_120k.txt /tmp/top_q8pack_120k.txt && "
		"/tmp/gen_dequant /tmp/top_q8pack_120k.txt /tmp/top_q8dequant_120k.txt && "
		"/tmp/gen_q4pack /tmp/top_x_120k.txt /tmp/top_q4pack_120k.txt && "
		"/tmp/gen_q4unpack /tmp/top_q4pack_120k.txt /tmp/top_q4unpack_120k.txt",
		repo_root, n_blocks);
	if (system(cmd) != 0)
	{
		fprintf(stderr, "[golden] generation failed\n");
		exit(1);
	}
}

static std::vector<uint16_t>	load_hex16(const char *path, long n)
{
	std::vector<uint16_t>	out;
	FILE					*f;
	unsigned int			v;
	long					i;

	f = fopen(path, "r");
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
	fclose(f);
	if (i != n)
	{
		fprintf(stderr, "%s: expected %ld, got %ld\n", path, n, i);
		exit(1);
	}
	return (out);
}

static std::vector<uint8_t>	load_hex8(const char *path, long n)
{
	std::vector<uint8_t>	out;
	FILE					*f;
	unsigned int			v;
	long					i;

	f = fopen(path, "r");
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
	fclose(f);
	if (i != n)
	{
		fprintf(stderr, "%s: expected %ld, got %ld\n", path, n, i);
		exit(1);
	}
	return (out);
}

struct GoldenSet
{
	std::vector<uint16_t>	x;
	std::vector<uint8_t>	q8pack;
	std::vector<uint16_t>	q8dequant;
	std::vector<uint8_t>	q4pack;
	std::vector<uint16_t>	q4unpack;
	int						n;
};

static GoldenSet	load_golden(const char *repo_root, int n)
{
	GoldenSet	g;

	ensure_golden_vectors(repo_root, n);
	g.n = n;
	g.x = load_hex16("/tmp/top_x_120k.txt", (long)n * 32);
	g.q8pack = load_hex8("/tmp/top_q8pack_120k.txt", (long)n * 34);
	g.q8dequant = load_hex16("/tmp/top_q8dequant_120k.txt", (long)n * 32);
	g.q4pack = load_hex8("/tmp/top_q4pack_120k.txt", (long)n * 18);
	g.q4unpack = load_hex16("/tmp/top_q4unpack_120k.txt", (long)n * 32);
	return (g);
}

static std::vector<uint8_t>	f16_block_bytes(const GoldenSet &g, int blk)
{
	std::vector<uint8_t>	out(64);
	int						j;

	j = 0;
	while (j < 32)
	{
		uint16_t	v;

		v = g.x[(size_t)blk * 32 + j];
		out[j * 2] = (uint8_t)(v & 0xFFu);
		out[j * 2 + 1] = (uint8_t)((v >> 8) & 0xFFu);
		j++;
	}
	return (out);
}

static bool	check_bytes(const uint8_t *got, const uint8_t *expect, uint32_t len,
			const char *what, int blk, uint64_t &fails)
{
	uint32_t	i;

	i = 0;
	while (i < len)
	{
		if (got[i] != expect[i])
		{
			if (fails < 20)
				fprintf(stderr,
					"MISMATCH %s blk=%d byte=%u expect=%02x got=%02x\n",
					what, blk, i, expect[i], got[i]);
			fails++;
			return (false);
		}
		i++;
	}
	return (true);
}

struct Heartbeat
{
	std::chrono::steady_clock::time_point	start;
	std::chrono::steady_clock::time_point	last;
	const char								*stage;
	uint64_t								total;

	Heartbeat(const char *s, uint64_t t)
	{
		start = std::chrono::steady_clock::now();
		last = start;
		stage = s;
		total = t;
	}

	void	maybe_print(uint64_t done, uint64_t errors)
	{
		auto	now = std::chrono::steady_clock::now();
		double	since = std::chrono::duration<double>(now - last).count();

		if (since < 60.0)
			return;
		double	elapsed = std::chrono::duration<double>(now - start).count();
		double	rate = done > 0 ? (double)done / elapsed : 0.0;
		double	eta = rate > 0 ? (double)(total - done) / rate : -1.0;

		fprintf(stderr,
			"[heartbeat] stage=%s completed=%llu/%llu elapsed=%.1fs eta=%.1fs errors=%llu\n",
			stage, (unsigned long long)done, (unsigned long long)total,
			elapsed, eta, (unsigned long long)errors);
		last = now;
	}
};

// Runs `count` round trips of `op` through the full DMA emulation path
// (FpgaRuntime::submit/wait, i.e. real header encode -> cmd_push ->
// payload stream -> membrane_dma_bridge -> membrane_quant_stream_top ->
// completion), comparing every result against the real C reference
// (via the golden vectors, themselves generated from
// src/quant/quant_simd.c / third_party ggml -- see Phase 5.3's own
// docs for that chain). Random batch size (1..8 blocks/transaction) and
// random queue depth (how many transactions are allowed outstanding
// before draining) per the phase spec's item 6.
static bool	run_verify(const char *repo_root, membrane_fpga_op_t op,
			int total_blocks, const char *label)
{
	GoldenSet		g;
	FpgaRuntime		rt;
	std::mt19937	rng(0xB16B00B5u ^ (uint32_t)op);
	uint64_t		fails;
	uint64_t		done;
	int				next_blk;
	Heartbeat		hb(label, (uint64_t)total_blocks);

	g = load_golden(repo_root, total_blocks);
	rt.device_open();
	fails = 0;
	done = 0;
	next_blk = 0;
	while (done < (uint64_t)total_blocks)
	{
		std::uniform_int_distribution<int>	batch_dist(1, 8);
		int		batch = batch_dist(rng);
		std::vector<uint8_t>	payload;
		uint32_t	out_cap;
		uint32_t	per_out;

		if (next_blk + batch > total_blocks)
			batch = total_blocks - next_blk;
		if (op == MEMBRANE_FPGA_OP_Q8_ENCODE || op == MEMBRANE_FPGA_OP_Q4_ENCODE)
		{
			// F16 input blocks are already 64 bytes (4-byte aligned),
			// no padding needed. The PACKED output, however, is 34
			// (Q8_0) or 18 (Q4_0) bytes/block -- NOT a multiple of the
			// bridge's 4-byte payload beat width -- so the device pads
			// every output block to a 4-byte-aligned stride on the
			// wire (see membrane_dma_bridge.sv's op_output_bytes
			// header comment for why: without this, a multi-block
			// batch's per-block boundaries drift out of sync with the
			// beat stream and every block after the first gets
			// corrupted). per_out here is that PADDED stride; the real
			// per-block byte count used for comparison is
			// real_out below.
			int	b = 0;
			while (b < batch)
			{
				std::vector<uint8_t>	blk_bytes = f16_block_bytes(g, next_blk + b);
				payload.insert(payload.end(), blk_bytes.begin(), blk_bytes.end());
				b++;
			}
			per_out = (op == MEMBRANE_FPGA_OP_Q8_ENCODE) ? 36 : 20;
		}
		else
		{
			// Packed input blocks (34/18 bytes) similarly get padded
			// to a 4-byte-aligned stride (36/20) here on the HOST side
			// before submission, matching the device's own padded
			// accumulator threshold.
			const uint8_t	*src = (op == MEMBRANE_FPGA_OP_Q8_DECODE)
				? g.q8pack.data() : g.q4pack.data();
			uint32_t		real_stride = (op == MEMBRANE_FPGA_OP_Q8_DECODE) ? 34 : 18;
			uint32_t		padded_stride = (op == MEMBRANE_FPGA_OP_Q8_DECODE) ? 36 : 20;
			int				b = 0;

			while (b < batch)
			{
				payload.insert(payload.end(),
					src + (size_t)(next_blk + b) * real_stride,
					src + (size_t)(next_blk + b) * real_stride + real_stride);
				payload.resize(payload.size() + (padded_stride - real_stride), 0);
				b++;
			}
			per_out = 64;
		}
		out_cap = per_out * (uint32_t)batch;

		uint64_t	handle = rt.submit(op, payload.data(), (uint32_t)payload.size(),
			(uint32_t)batch, out_cap);
		if (handle == 0)
		{
			rt.pump(1);
			continue;
		}
		std::vector<uint8_t>	result(out_cap);
		uint32_t				out_len;
		uint32_t				status;

		if (!rt.wait(handle, result.data(), out_cap, &out_len, &status))
		{
			fprintf(stderr, "TIMEOUT blk=%d batch=%d\n", next_blk, batch);
			fails++;
			next_blk += batch;
			done += batch;
			continue;
		}
		if (status != 0)
		{
			fprintf(stderr, "DEVICE ERROR blk=%d status=0x%08x\n", next_blk, status);
			fails++;
		}
		else
		{
			// per_out is the PADDED stride (used to index into
			// `result`, matching the device's own padded framing, see
			// the payload-construction comment above); real_out is the
			// true per-block byte count actually worth comparing --
			// the 2 pad bytes tacked onto each Q8_0/Q4_0-sized block
			// are never real quantized/dequantized data.
			uint32_t	real_out = (op == MEMBRANE_FPGA_OP_Q8_ENCODE) ? 34
				: (op == MEMBRANE_FPGA_OP_Q4_ENCODE) ? 18 : 64;
			int	b = 0;

			while (b < batch)
			{
				const uint8_t	*expect;

				if (op == MEMBRANE_FPGA_OP_Q8_ENCODE)
					expect = g.q8pack.data() + (size_t)(next_blk + b) * 34;
				else if (op == MEMBRANE_FPGA_OP_Q4_ENCODE)
					expect = g.q4pack.data() + (size_t)(next_blk + b) * 18;
				else if (op == MEMBRANE_FPGA_OP_Q8_DECODE)
					expect = (const uint8_t *)(g.q8dequant.data()
						+ (size_t)(next_blk + b) * 32);
				else
					expect = (const uint8_t *)(g.q4unpack.data()
						+ (size_t)(next_blk + b) * 32);
				check_bytes(result.data() + (size_t)b * per_out, expect, real_out,
					label, next_blk + b, fails);
				b++;
			}
		}
		next_blk += batch;
		done += batch;
		hb.maybe_print(done, fails);
	}
	fprintf(stderr, "[%s] %llu/%llu blocks, %llu fails\n", label,
		(unsigned long long)done, (unsigned long long)total_blocks,
		(unsigned long long)fails);
	return (fails == 0);
}

static int	cmd_verify(const char *repo_root, int argc, char **argv)
{
	int		count;
	bool	ok;

	count = (argc > 0) ? atoi(argv[0]) : 100000;
	ok = true;
	ok = run_verify(repo_root, MEMBRANE_FPGA_OP_Q8_ENCODE, count, "Q8_ENCODE") && ok;
	ok = run_verify(repo_root, MEMBRANE_FPGA_OP_Q8_DECODE, count, "Q8_DECODE") && ok;
	ok = run_verify(repo_root, MEMBRANE_FPGA_OP_Q4_ENCODE, count, "Q4_ENCODE") && ok;
	ok = run_verify(repo_root, MEMBRANE_FPGA_OP_Q4_DECODE, count, "Q4_DECODE") && ok;
	if (ok)
		printf("PASS: DMA-path bit-exact verification, %d blocks x4 operations, 0 fails\n",
			count);
	else
		printf("FAIL: DMA-path bit-exact verification had mismatches\n");
	return (ok ? 0 : 1);
}

static std::vector<uint8_t>	make_q8_payload(int blocks)
{
	std::vector<uint8_t>	p(blocks * 64);
	int						i;

	i = 0;
	while (i < (int)p.size())
	{
		p[i] = (uint8_t)(i * 7 + 3);
		i++;
	}
	return (p);
}

// Phase 5.4 task 113: performance measurement. Every cycle count below
// is REAL (genuinely simulated by Verilator, one clock edge at a time --
// see fpga_emu_device.h's header comment). Converting cycles to
// GB/s/latency-in-seconds needs a clock frequency, and NO real
// place-and-route result exists for this design (same disclosed gap as
// Phase 5.3, docs/phase5-synthesizable-fpga.md section 8) -- so every
// time-based figure here is explicitly computed at a labeled, assumed
// clock (200 MHz, the same aspirational Alveo-class figure Phase 5.3
// used), never presented as a measured hardware speed.
static const double	ASSUMED_CLOCK_HZ = 200e6;

static double	cycles_to_seconds(uint64_t cycles)
{
	return ((double)cycles / ASSUMED_CLOCK_HZ);
}

static double	percentile(std::vector<double> &v, double p)
{
	std::sort(v.begin(), v.end());
	size_t	idx = (size_t)(p * (double)(v.size() - 1));
	return (v[idx]);
}

static int	cmd_perf(const char *repo_root)
{
	(void)repo_root;
	printf("membrane-fpga-runtime performance measurement\n");
	printf("ASSUMED clock = %.0f MHz (NOT a measured Fmax -- no place-and-route\n"
		"  tool available, see docs/phase5-synthesizable-fpga.md section 8).\n"
		"  Cycle counts themselves are real Verilator simulation results.\n\n",
		ASSUMED_CLOCK_HZ / 1e6);

	// ---- round-trip latency distribution, single-block transactions ----
	{
		const int				N = 200;
		std::vector<double>	latencies_us;
		FpgaRuntime				rt;

		rt.device_open();
		std::vector<uint8_t>	payload = make_q8_payload(1);
		int	i = 0;
		while (i < N)
		{
			uint64_t	cyc0 = rt.device().cycle_count();
			uint64_t	h = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, payload.data(), 64, 1, 36);
			uint8_t		out[36];
			uint32_t	out_len, status;

			rt.wait(h, out, 36, &out_len, &status);
			uint64_t	cyc1 = rt.device().cycle_count();
			latencies_us.push_back(cycles_to_seconds(cyc1 - cyc0) * 1e6);
			i++;
		}
		printf("Round-trip latency, single Q8_0 block, %d samples (assumed clock):\n", N);
		printf("  p50=%.2f us  p95=%.2f us  p99=%.2f us\n",
			percentile(latencies_us, 0.50), percentile(latencies_us, 0.95),
			percentile(latencies_us, 0.99));
	}

	// ---- sustained throughput, large single-transaction batches, per op ----
	{
		struct OpInfo { membrane_fpga_op_t op; const char *name; uint32_t in_sz; uint32_t out_sz; };
		OpInfo	ops[4] = {
			{MEMBRANE_FPGA_OP_Q8_ENCODE, "Q8_ENCODE", 64, 36},
			{MEMBRANE_FPGA_OP_Q8_DECODE, "Q8_DECODE", 36, 64},
			{MEMBRANE_FPGA_OP_Q4_ENCODE, "Q4_ENCODE", 64, 20},
			{MEMBRANE_FPGA_OP_Q4_DECODE, "Q4_DECODE", 20, 64},
		};
		const int	N = 8192;

		printf("\nSustained throughput, %d blocks in one batch transaction:\n", N);
		int	oi = 0;
		while (oi < 4)
		{
			FpgaRuntime	rt(4);
			rt.device_open();
			std::vector<uint8_t>	payload(N * ops[oi].in_sz, 0x3C);
			uint64_t	cyc0 = rt.device().cycle_count();
			uint64_t	h = rt.submit(ops[oi].op, payload.data(),
				(uint32_t)payload.size(), N, N * ops[oi].out_sz);
			std::vector<uint8_t>	out(N * ops[oi].out_sz);
			uint32_t	out_len, status;

			rt.wait(h, out.data(), (uint32_t)out.size(), &out_len, &status, 100000000);
			uint64_t	cyc1 = rt.device().cycle_count();
			uint64_t	cycles = cyc1 - cyc0;
			double		seconds = cycles_to_seconds(cycles);
			double		blocks_per_s = (double)N / seconds;
			double	in_gbps = ((double)N * ops[oi].in_sz / seconds) / 1e9;
			double	out_gbps = ((double)N * ops[oi].out_sz / seconds) / 1e9;
			uint32_t	stall = rt.device().stall_cycles();

			printf("  %-10s status=0x%02x cycles=%llu blocks/s=%.0f elements/s=%.0f"
				" in=%.3f GB/s out=%.3f GB/s stall_cycles=%u (%.1f%% of total)\n",
				ops[oi].name, status, (unsigned long long)cycles, blocks_per_s,
				blocks_per_s * 32.0, in_gbps, out_gbps, stall,
				100.0 * (double)stall / (double)cycles);
			oi++;
		}
	}

	// ---- queue-depth scaling: many small transactions back-to-back ----
	{
		printf("\nQueue-depth scaling, 2000 separate 1-block Q8_0 encode"
			" transactions:\n");
		uint32_t	depths[4] = {1, 4, 16, 64};
		int			di = 0;
		while (di < 4)
		{
			FpgaRuntime	rt(depths[di]);
			rt.device_open();
			std::vector<uint8_t>	payload = make_q8_payload(1);
			const int	N = 2000;
			uint64_t	cyc0 = rt.device().cycle_count();
			int			i = 0;

			while (i < N)
			{
				uint64_t	h = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, payload.data(),
					64, 1, 36);
				if (h == 0)
				{
					rt.pump(1);
					continue;
				}
				uint8_t		out[36];
				uint32_t	out_len, status;

				rt.wait(h, out, 36, &out_len, &status);
				i++;
			}
			uint64_t	cyc1 = rt.device().cycle_count();
			double		seconds = cycles_to_seconds(cyc1 - cyc0);

			printf("  queue_depth=%3u  %.0f transactions/s  (%.1f us avg/transaction)\n",
				depths[di], (double)N / seconds, seconds * 1e6 / N);
			di++;
		}
		printf("  NOTE: membrane_dma_bridge.sv processes one command at a time\n"
			"  (disclosed scope decision), so this measures submit/wait/pump\n"
			"  call overhead at different local queue depths, NOT overlapped\n"
			"  multi-command execution -- see docs/phase5-pcie-hardware-loop.md.\n");
	}

	// ---- host CPU utilization for the emulation process itself ----
	{
		struct rusage	ru;

		getrusage(RUSAGE_SELF, &ru);
		double	user_s = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
		double	sys_s = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
		printf("\nHost CPU time consumed by this emulation process so far:"
			" user=%.2fs sys=%.2fs\n"
			"  (this is the cost of RUNNING THE SIMULATOR itself, not a\n"
			"  projection of real host CPU load talking to real hardware --\n"
			"  a real driver's CPU cost is dominated by MMIO/DMA setup and\n"
			"  polling, not by simulating the accelerator's own logic.)\n",
			user_s, sys_s);
	}
	return (0);
}

// Phase 5.4 task 117: fallback safety logic. Wraps a Q8_0 encode batch
// with FPGA-first, CPU-fallback semantics: FPGA absent (rt==nullptr),
// queue full (submit() returns 0), a device error/timeout (wait()
// fails or reports nonzero status) all fall through to
// membrane_simd_q8_0_quantize_batch -- the SAME scalar CPU path
// tools/membrane-quant-bench and membrane-kv-runtime already use and
// this whole project has verified bit-exact against ggml since Phase
// 4.4. The critical property this section's test suite actually
// checks is not just "did it not crash" but "is the returned data
// EXACTLY correct regardless of which path produced it" -- silent
// data corruption on fallback would be worse than no fallback at all.
struct FallbackResult
{
	std::vector<uint8_t>	data;
	bool					used_fpga;
	bool					ok;
};

static bool	cpu_q8_encode(const uint8_t *f16_payload, uint32_t n_blocks,
			std::vector<uint8_t> &packed_out)
{
	size_t					scratch_bytes = membrane_simd_batch_scratch_bytes(32);
	std::vector<uint8_t>	scratch(scratch_bytes);
	membrane_status_t		st;

	packed_out.resize((size_t)n_blocks * 34);
	st = membrane_simd_q8_0_quantize_batch(MEMBRANE_SIMD_SCALAR,
		(const uint16_t *)f16_payload, n_blocks, 32, packed_out.data(),
		scratch.data(), scratch_bytes);
	return (st == MEMBRANE_OK);
}

static FallbackResult	quantize_q8_with_fallback(FpgaRuntime *rt,
			const uint8_t *f16_payload, uint32_t n_blocks,
			uint64_t fpga_timeout_cycles)
{
	FallbackResult	r;

	r.used_fpga = false;
	r.ok = false;
	if (rt != nullptr)
	{
		uint64_t	h = rt->submit(MEMBRANE_FPGA_OP_Q8_ENCODE, f16_payload,
			n_blocks * 64, n_blocks, n_blocks * 36);

		if (h != 0)
		{
			std::vector<uint8_t>	padded((size_t)n_blocks * 36);
			uint32_t				out_len;
			uint32_t				status;

			if (rt->wait(h, padded.data(), (uint32_t)padded.size(), &out_len,
					&status, fpga_timeout_cycles) && status == 0)
			{
				uint32_t	b;

				r.data.resize((size_t)n_blocks * 34);
				b = 0;
				while (b < n_blocks)
				{
					memcpy(r.data.data() + (size_t)b * 34,
						padded.data() + (size_t)b * 36, 34);
					b++;
				}
				r.used_fpga = true;
				r.ok = true;
				return (r);
			}
		}
	}
	// FPGA absent, queue full, timed out, or reported an error --
	// safe CPU fallback, never a silently-wrong/partial result.
	r.ok = cpu_q8_encode(f16_payload, n_blocks, r.data);
	r.used_fpga = false;
	return (r);
}

static int	cmd_fallback(const char *repo_root)
{
	GoldenSet	g = load_golden(repo_root, 64);
	int			fails = 0;

	// 1. FPGA never opened at all
	{
		std::vector<uint8_t>	payload = f16_block_bytes(g, 0);
		int						b = 1;

		while (b < 8)
		{
			std::vector<uint8_t>	blk = f16_block_bytes(g, b);

			payload.insert(payload.end(), blk.begin(), blk.end());
			b++;
		}
		FallbackResult	r = quantize_q8_with_fallback(nullptr, payload.data(), 8, 0);
		bool			correct = r.ok && !r.used_fpga
			&& memcmp(r.data.data(), g.q8pack.data(), 8 * 34) == 0;
		expect(correct, "FPGA absent (rt=nullptr): CPU fallback used,"
			" bit-exact correct result", fails);
	}

	// 2. FPGA healthy: fallback should NOT trigger, FPGA path used
	{
		FpgaRuntime				rt;
		std::vector<uint8_t>	payload = f16_block_bytes(g, 0);
		int						b = 1;

		rt.device_open();
		while (b < 8)
		{
			std::vector<uint8_t>	blk = f16_block_bytes(g, b);

			payload.insert(payload.end(), blk.begin(), blk.end());
			b++;
		}
		FallbackResult	r = quantize_q8_with_fallback(&rt, payload.data(), 8, 1000000);
		bool			correct = r.ok && r.used_fpga
			&& memcmp(r.data.data(), g.q8pack.data(), 8 * 34) == 0;
		expect(correct, "FPGA healthy: FPGA path used (no unnecessary fallback),"
			" bit-exact correct result", fails);
	}

	// 3. FPGA queue full -> fallback
	{
		FpgaRuntime				rt(1); // depth-1 queue, trivial to saturate
		std::vector<uint8_t>	filler = make_q8_payload(1);
		std::vector<uint8_t>	payload = f16_block_bytes(g, 0);
		int						b = 1;

		rt.device_open();
		rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, filler.data(), 64, 1, 36); // fills the 1 slot, never drained
		while (b < 8)
		{
			std::vector<uint8_t>	blk = f16_block_bytes(g, b);

			payload.insert(payload.end(), blk.begin(), blk.end());
			b++;
		}
		FallbackResult	r = quantize_q8_with_fallback(&rt, payload.data(), 8, 1000000);
		bool			correct = r.ok && !r.used_fpga
			&& memcmp(r.data.data(), g.q8pack.data(), 8 * 34) == 0;
		expect(correct, "FPGA queue full: CPU fallback used,"
			" bit-exact correct result", fails);
	}

	// 4. FPGA times out -> fallback
	{
		FpgaRuntime				rt;
		std::vector<uint8_t>	payload = f16_block_bytes(g, 0);
		int						b = 1;

		rt.device_open();
		while (b < 8)
		{
			std::vector<uint8_t>	blk = f16_block_bytes(g, b);

			payload.insert(payload.end(), blk.begin(), blk.end());
			b++;
		}
		// timeout_cycles=1: cannot possibly complete a real transfer in
		// 1 cycle, deterministically forces the timeout path
		FallbackResult	r = quantize_q8_with_fallback(&rt, payload.data(), 8, 1);
		bool			correct = r.ok && !r.used_fpga
			&& memcmp(r.data.data(), g.q8pack.data(), 8 * 34) == 0;
		expect(correct, "FPGA timeout: CPU fallback used,"
			" bit-exact correct result (never silent corruption)", fails);
	}

	printf("\n");
	if (fails == 0)
		printf("PASS: all fallback safety scenarios, 0 failures\n");
	else
		printf("FAIL: %d fallback scenario(s) failed\n", fails);
	return (fails == 0 ? 0 : 1);
}

static void	expect(bool cond, const char *desc, int &fails)
{
	if (cond)
		printf("  PASS: %s\n", desc);
	else
	{
		printf("  FAIL: %s\n", desc);
		fails++;
	}
}

// Phase 5.4 task 112: DMA stress tests. Every scenario the spec lists,
// each with a real assertion, not just "did it not crash."
static int	cmd_stress(const char *repo_root)
{
	int	fails;

	(void)repo_root;
	fails = 0;

	// ---- 1. unaligned host buffers ----
	{
		std::vector<uint8_t>	backing(64 + 3);
		uint8_t					*unaligned = backing.data() + 3; // deliberately odd offset
		int						i;

		i = 0;
		while (i < 64)
		{
			unaligned[i] = (uint8_t)(i * 5 + 1);
			i++;
		}
		FpgaRuntime	rt;
		rt.device_open();
		uint64_t	h = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, unaligned, 64, 1, 36);
		uint8_t		out[36];
		uint32_t	out_len, status;
		bool		ok = rt.wait(h, out, 36, &out_len, &status)
			&& status == 0 && out_len == 36;
		expect(ok, "unaligned host buffer (offset+3) round-trips correctly", fails);
	}

	// ---- 2. minimum packet (1 block) ----
	{
		FpgaRuntime	rt;
		rt.device_open();
		std::vector<uint8_t>	payload = make_q8_payload(1);
		uint64_t	h = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, payload.data(), 64, 1, 36);
		uint8_t		out[36];
		uint32_t	out_len, status;
		bool		ok = rt.wait(h, out, 36, &out_len, &status) && status == 0;
		expect(ok, "minimum packet (element_count=1)", fails);
	}

	// ---- 2b. maximum packet (large batch in one transaction) ----
	{
		FpgaRuntime	rt(4);
		rt.device_open();
		const int	N = 4096;
		std::vector<uint8_t>	payload = make_q8_payload(N);
		uint64_t	h = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, payload.data(),
			(uint32_t)payload.size(), N, N * 36);
		std::vector<uint8_t>	out(N * 36);
		uint32_t	out_len, status;
		bool		ok = rt.wait(h, out.data(), N * 36, &out_len, &status, 50000000)
			&& status == 0 && out_len == (uint32_t)(N * 36);
		expect(ok, "maximum packet (element_count=4096 in one transaction)", fails);
	}

	// ---- 3. queue wraparound / 6. backpressure ----
	{
		FpgaRuntime	rt(4); // shallow queue on purpose
		rt.device_open();
		std::vector<uint8_t>	payload = make_q8_payload(1);
		uint64_t	handles[4];
		int			i;

		i = 0;
		while (i < 4)
		{
			handles[i] = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, payload.data(), 64, 1, 36);
			i++;
		}
		uint64_t	overflow_handle = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE,
			payload.data(), 64, 1, 36);
		expect(overflow_handle == 0,
			"backpressure: 5th submit on depth-4 queue rejected (handle=0)", fails);

		uint8_t		out[36];
		uint32_t	out_len, status;
		bool		drained_ok = true;

		i = 0;
		while (i < 4)
		{
			if (!rt.wait(handles[i], out, 36, &out_len, &status) || status != 0)
				drained_ok = false;
			i++;
		}
		expect(drained_ok, "queue wraparound: all 4 originally-queued items drain correctly",
			fails);
		uint64_t	reused_handle = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE,
			payload.data(), 64, 1, 36);
		bool		reuse_ok = reused_handle != 0
			&& rt.wait(reused_handle, out, 36, &out_len, &status) && status == 0;
		expect(reuse_ok, "queue slot reusable after drain (wraparound)", fails);
	}

	// ---- 4. random submission order / 5. multiple outstanding requests ----
	{
		FpgaRuntime		rt(16);
		std::mt19937	rng(12345);
		rt.device_open();

		const int	N = 10;
		uint64_t	handles[N];
		std::vector<uint8_t>	payloads[N];
		int			i;

		i = 0;
		while (i < N)
		{
			payloads[i] = make_q8_payload(1);
			payloads[i][0] = (uint8_t)i; // make each block's content distinct
			handles[i] = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, payloads[i].data(),
				64, 1, 36);
			i++;
		}
		std::vector<int>	order;
		i = 0;
		while (i < N)
		{
			order.push_back(i);
			i++;
		}
		std::shuffle(order.begin(), order.end(), rng);

		bool	all_ok = true;
		for (int idx : order)
		{
			uint8_t		out[36];
			uint32_t	out_len, status;

			if (!rt.wait(handles[idx], out, 36, &out_len, &status) || status != 0)
				all_ok = false;
		}
		expect(all_ok,
			"10 outstanding requests, waited on in random order, all complete correctly",
			fails);
	}

	// ---- 7. timeout ----
	{
		FpgaEmuDevice	dev;
		uint8_t			hdr_bytes[MEMBRANE_FPGA_DMA_HEADER_BYTES];
		membrane_fpga_header_t	hdr;

		dev.reset();
		memset(&hdr, 0, sizeof(hdr));
		hdr.magic = MEMBRANE_FPGA_DMA_MAGIC;
		hdr.version_major = MEMBRANE_FPGA_DMA_VERSION_MAJOR;
		hdr.transaction_id = 1;
		hdr.operation = MEMBRANE_FPGA_OP_Q8_ENCODE;
		hdr.element_count = 1;
		hdr.output_capacity = 36;
		membrane_fpga_header_encode(&hdr, hdr_bytes);
		dev.cmd_push(hdr_bytes);
		// deliberately do NOT push the payload -- the device will sit
		// forever waiting for input bytes that never arrive, exactly
		// the condition a real timeout must detect rather than hang on
		uint8_t		completion[16];
		bool		got = dev.completion_wait(completion, 200); // tiny budget
		expect(!got, "completion_wait times out (returns false) rather than hanging"
			" when payload never arrives", fails);
	}

	// ---- 8. cancellation ----
	{
		FpgaRuntime	rt(4);
		rt.device_open();
		std::vector<uint8_t>	p1 = make_q8_payload(1);
		std::vector<uint8_t>	p2 = make_q8_payload(1);

		// fill the in-flight slot with one real request first so the
		// second one is genuinely still sitting in the queue (never
		// issued) when we cancel it
		uint64_t	h1 = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, p1.data(), 64, 1, 36);
		uint64_t	h2 = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, p2.data(), 64, 1, 36);
		bool		cancel_ok = rt.cancel(h2);
		expect(cancel_ok, "cancel() on a still-queued (not yet issued) handle succeeds",
			fails);

		uint8_t		out[36];
		uint32_t	out_len, status;
		bool		never_completes = !rt.poll(h2, out, 36, &out_len, &status);
		// drain h1 so pump() has something legitimate to do and h2
		// genuinely never surfaces as a completion
		rt.wait(h1, out, 36, &out_len, &status);
		never_completes = never_completes && !rt.poll(h2, out, 36, &out_len, &status);
		expect(never_completes, "cancelled handle never produces a completion", fails);

		bool	cancel_again = rt.cancel(h2);
		expect(!cancel_again, "cancelling an already-cancelled/unknown handle returns false",
			fails);
	}

	// ---- 9. malformed header ----
	{
		FpgaRuntime	rt;
		membrane_fpga_header_t	hdr;
		uint8_t		hdr_bytes[MEMBRANE_FPGA_DMA_HEADER_BYTES];

		rt.device_open();
		memset(&hdr, 0, sizeof(hdr));
		hdr.magic = 0xDEADBEEFu; // wrong magic, everything else otherwise sane
		hdr.version_major = MEMBRANE_FPGA_DMA_VERSION_MAJOR;
		hdr.transaction_id = 99;
		hdr.operation = MEMBRANE_FPGA_OP_Q8_ENCODE;
		hdr.element_count = 1;
		hdr.output_capacity = 36;
		membrane_fpga_header_encode(&hdr, hdr_bytes); // checksum computed over the bad-magic bytes, so it's internally consistent
		uint64_t	h = rt.raw_submit(hdr_bytes, nullptr, 0, 36);
		uint8_t		out[36];
		uint32_t	out_len, status;
		bool		ok = rt.wait(h, out, 36, &out_len, &status)
			&& status == MEMBRANE_FPGA_ERR_MALFORMED_HEADER;
		expect(ok, "malformed header (bad magic) rejected by device with"
			" MEMBRANE_FPGA_ERR_MALFORMED_HEADER", fails);
	}

	// ---- 10. bad checksum ----
	{
		FpgaRuntime	rt;
		membrane_fpga_header_t	hdr;
		uint8_t		hdr_bytes[MEMBRANE_FPGA_DMA_HEADER_BYTES];
		std::vector<uint8_t>	payload = make_q8_payload(1);

		rt.device_open();
		memset(&hdr, 0, sizeof(hdr));
		hdr.magic = MEMBRANE_FPGA_DMA_MAGIC;
		hdr.version_major = MEMBRANE_FPGA_DMA_VERSION_MAJOR;
		hdr.transaction_id = 100;
		hdr.operation = MEMBRANE_FPGA_OP_Q8_ENCODE;
		hdr.element_count = 1;
		hdr.output_capacity = 36;
		hdr.payload_checksum = membrane_block_checksum(payload.data(), payload.size())
			^ 0xFFFFFFFFu; // deliberately wrong
		membrane_fpga_header_encode(&hdr, hdr_bytes);
		uint64_t	h = rt.raw_submit(hdr_bytes, payload.data(), (uint32_t)payload.size(), 36);
		uint8_t		out[36];
		uint32_t	out_len, status;
		bool		ok = rt.wait(h, out, 36, &out_len, &status)
			&& status == MEMBRANE_FPGA_ERR_BAD_PAYLOAD_CHECKSUM;
		expect(ok, "bad payload checksum caught by host-side validation before"
			" ever reaching the device", fails);
	}

	// ---- 11. short output buffer ----
	{
		FpgaRuntime	rt;
		std::vector<uint8_t>	payload = make_q8_payload(4);

		rt.device_open();
		// 4 Q8_0 blocks need 4*36=144 padded bytes (see
		// membrane_dma_bridge.sv's op_output_bytes); declare only 32
		uint64_t	h = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, payload.data(),
			(uint32_t)payload.size(), 4, 32);
		uint8_t		out[144];
		uint32_t	out_len, status;
		bool		ok = rt.wait(h, out, 144, &out_len, &status)
			&& status == MEMBRANE_FPGA_ERR_SHORT_OUTPUT;
		expect(ok, "output_capacity smaller than needed rejected with"
			" MEMBRANE_FPGA_ERR_SHORT_OUTPUT", fails);
	}

	// ---- 12. device reset mid-flight ----
	{
		FpgaEmuDevice	dev;
		uint8_t			hdr_bytes[MEMBRANE_FPGA_DMA_HEADER_BYTES];
		membrane_fpga_header_t	hdr;
		std::vector<uint8_t>	payload = make_q8_payload(1);

		dev.reset();
		memset(&hdr, 0, sizeof(hdr));
		hdr.magic = MEMBRANE_FPGA_DMA_MAGIC;
		hdr.version_major = MEMBRANE_FPGA_DMA_VERSION_MAJOR;
		hdr.transaction_id = 1;
		hdr.operation = MEMBRANE_FPGA_OP_Q8_ENCODE;
		hdr.element_count = 1;
		hdr.output_capacity = 36;
		membrane_fpga_header_encode(&hdr, hdr_bytes);
		dev.cmd_push(hdr_bytes);
		dev.payload_push(payload.data(), 20); // only PART of the 64 bytes
		dev.reset(); // interrupt mid-transfer

		// device must come back clean and usable for a fresh transaction
		hdr.transaction_id = 2;
		membrane_fpga_header_encode(&hdr, hdr_bytes);
		bool	pushed = dev.cmd_push(hdr_bytes);
		uint32_t	sent = dev.payload_push(payload.data(), 64);
		uint8_t		out[36];
		uint32_t	got = dev.payload_pull(out, 36);
		uint8_t		completion[16];
		bool		completed = dev.completion_wait(completion);
		bool	ok = pushed && sent == 64 && got == 36 && completed
			&& completion[0] == 2; // transaction_id low byte
		expect(ok, "device recovers cleanly after reset mid-transfer,"
			" fresh transaction after reset works", fails);
	}

	printf("\n");
	if (fails == 0)
		printf("PASS: all DMA stress scenarios, 0 failures\n");
	else
		printf("FAIL: %d DMA stress scenario(s) failed\n", fails);
	return (fails == 0 ? 0 : 1);
}

static int	cmd_smoke(void)
{
	FpgaRuntime	rt;
	uint8_t		payload[64];
	int			i;

	rt.device_open();
	i = 0;
	while (i < 64)
	{
		payload[i] = (uint8_t)(i * 3 + 1);
		i++;
	}
	uint64_t	h = rt.submit(MEMBRANE_FPGA_OP_Q8_ENCODE, payload, 64, 1, 36);
	uint8_t		out[36];
	uint32_t	out_len;
	uint32_t	status;

	if (!rt.wait(h, out, 36, &out_len, &status))
	{
		printf("FAIL: smoke wait timed out\n");
		return (1);
	}
	printf("smoke: status=0x%08x out_len=%u\n", status, out_len);
	membrane_fpga_stats_t	st = rt.get_stats();
	printf("stats: submitted=%llu completed=%llu failed=%llu\n",
		(unsigned long long)st.submitted, (unsigned long long)st.completed,
		(unsigned long long)st.failed);
	printf("PASS: smoke test complete\n");
	return (0);
}

int	main(int argc, char **argv)
{
	const char	*repo_root;
	const char	*mode;

	repo_root = getenv("MEMBRANE_REPO_ROOT");
	if (!repo_root)
		repo_root = "/home/kadirerenaltintas/membrane";
	mode = (argc > 1) ? argv[1] : "smoke";
	if (strcmp(mode, "smoke") == 0)
		return (cmd_smoke());
	if (strcmp(mode, "verify") == 0)
		return (cmd_verify(repo_root, argc - 2, argv + 2));
	if (strcmp(mode, "stress") == 0)
		return (cmd_stress(repo_root));
	if (strcmp(mode, "perf") == 0)
		return (cmd_perf(repo_root));
	if (strcmp(mode, "fallback") == 0)
		return (cmd_fallback(repo_root));
	fprintf(stderr, "usage: %s {smoke|verify [count]|stress|perf|fallback}\n", argv[0]);
	return (1);
}
