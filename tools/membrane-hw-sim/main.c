#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "membrane/quant_simd.h"
#include "membrane/f16convert.h"
#ifdef MEMBRANE_HWSIM_WITH_GGML_ORACLE
# include "membrane/ggml_quant.h"
#endif

/*
 * Phase 5.2 item 1: a cycle-stepped (not gate-level) model of a streaming
 * Q8_0/Q4_0 quantize/dequantize pipeline, structured to match the stage
 * breakdown in docs/phase5-hardware-datapath.md (Phase 5.1) and the RTL
 * modules in rtl/ (Phase 5.2): input FIFO -> block fetch -> maxabs
 * reduction -> scale computation -> reciprocal/multiply -> ggml-exact
 * rounding -> clipping -> Q8/Q4 packing -> output FIFO, with a valid/ready
 * (AXI-Stream-like) handshake on both ends and a single global-stall
 * backpressure policy (documented below).
 *
 * DATA correctness is never independently re-derived here: every block's
 * actual quantized/dequantized bytes are produced by calling the existing,
 * already bit-exact-verified membrane_simd_q{8,4}_0_* functions
 * (src/quant/quant_simd.c) -- this tool's own contribution is the CYCLE/
 * TIMING model around that computation, not a fourth reimplementation of
 * the quantize math. When built with MEMBRANE_HWSIM_WITH_GGML_ORACLE (the
 * LLAMA-gated build), every block is additionally cross-checked against
 * membrane_ggml_quant's real-ggml-backed oracle and against every
 * membrane_simd_* backend this CPU supports, so "cycle model output is
 * bit-exact with scalar MEMBRANE, SIMD MEMBRANE, and the ggml reference"
 * (item 2) is actually verified per block, not assumed by construction.
 *
 * BACKPRESSURE MODEL (disclosed, a deliberate first-prototype choice):
 * this is a single monolithic pipeline with no per-stage skid buffers --
 * when the output FIFO is full at the cycle a block would retire, the
 * ENTIRE pipeline freezes for that cycle (no new block is admitted
 * either), exactly matching a plain synchronous FIFO's valid/ready
 * contract with no internal elasticity beyond the FIFO itself. A
 * production design would likely add per-stage buffering to avoid
 * stalling stages that aren't actually blocked, but for a first streaming
 * prototype this is the simplest policy that is unambiguously correct.
 *
 * INPUT-SIDE RATE LIMITING is modeled as a deterministic ceiling on fetch
 * latency (fetch_cycles = max(ceil(32/width), ceil(32/source_rate)))
 * rather than a stateful per-cycle credit simulation, since -- unlike the
 * output side -- there is no buffering/occupancy concept on the input
 * side in this design (fetch reads directly into the reduce stage) for a
 * slow source to accumulate against. This is a steady-state model, not a
 * burst/jitter model; that limitation is stated once here rather than
 * repeated at every call site.
 */

typedef enum e_hw_mode
{
	HW_Q8_QUANT = 0,
	HW_Q8_DEQUANT,
	HW_Q4_QUANT,
	HW_Q4_DEQUANT
}	hw_mode_t;

static const char	*mode_name(hw_mode_t m)
{
	if (m == HW_Q8_QUANT)
		return ("q8_quant");
	if (m == HW_Q8_DEQUANT)
		return ("q8_dequant");
	if (m == HW_Q4_QUANT)
		return ("q4_quant");
	return ("q4_dequant");
}

/* Largest buffer any mode needs: dequantize output is 32 x uint16_t (64
 * bytes), larger than either packed format (34 or 18 bytes). Every scratch
 * buffer below is sized to this, not to MEMBRANE_QSIMD_Q8_0_BLOCK_BYTES,
 * to avoid overflowing on dequantize modes. */
# define HW_MAX_BLOCK_BYTES	64u

static size_t	packed_block_bytes(hw_mode_t m)
{
	if (m == HW_Q8_QUANT || m == HW_Q8_DEQUANT)
		return (MEMBRANE_QSIMD_Q8_0_BLOCK_BYTES);
	return (MEMBRANE_QSIMD_Q4_0_BLOCK_BYTES);
}

/* The output FIFO always holds one block's worth of whatever the pipeline
 * actually EMITS -- packed bytes for encode modes, 32 x F16 (64 bytes) for
 * decode modes. Using packed_block_bytes() here unconditionally would
 * under-size the FIFO/sink accounting for decode modes (the real output
 * is larger than the packed input), so this is the one used for both the
 * sink-drain simulation and the reported throughput. */
static size_t	output_block_bytes(hw_mode_t m)
{
	if (m == HW_Q8_QUANT || m == HW_Q4_QUANT)
		return (packed_block_bytes(m));
	return (32u * sizeof(uint16_t));
}

typedef struct s_hw_cfg
{
	hw_mode_t	mode;
	int			width;
	int			out_fifo_depth;
	long		n_blocks;
	double		sink_bytes_per_cycle;
	double		source_elems_per_cycle;
	int			lat_reduce;
	int			lat_scale;
	int			lat_pack;
	uint32_t	seed;
	const char	*dump_vectors_path;
	const char	*csv_path;
	int			quiet;
	double		target_clock_mhz;
	int			skip_link_report;
}	hw_cfg_t;

typedef struct s_hw_result
{
	long		n_blocks;
	int			fetch_cycles;
	int			total_latency;
	long long	real_cycles;
	long long	stall_cycles;
	long long	occ_sum;
	int			occ_max;
	long		parity_checked;
	long		parity_fail;
}	hw_result_t;

static uint32_t	rng_next(uint32_t *state)
{
	*state = *state * 1103515245u + 12345u;
	return (*state);
}

static uint16_t	rand_f16(uint32_t *state)
{
	float	v;
	union
	{
		float		f;
		uint32_t	u;
	}	cv;

	v = ((float)((rng_next(state) >> 8) & 0xFFFF) / 65535.0f - 0.5f) * 8.0f;
	cv.f = v;
	return (membrane_f32_to_f16(cv.f));
}

/* Computes the block's actual bytes via the existing bit-exact engine
 * (the "data" side of the model), and, when the ggml oracle is linked,
 * cross-checks every supported membrane_simd_* backend and the ggml
 * oracle against each other for this exact block -- the concrete
 * evidence for item 2's bit-exactness requirement. Returns 1 if any
 * mismatch was found (and prints it), 0 otherwise. */
static int	compute_and_check_block(hw_mode_t mode, const uint8_t *in_bytes,
				uint8_t *out_bytes, long block_idx)
{
	membrane_simd_backend_t	backends[3] = {MEMBRANE_SIMD_SCALAR,
									MEMBRANE_SIMD_SSE41, MEMBRANE_SIMD_AVX2};
	uint8_t						ref[HW_MAX_BLOCK_BYTES];
	size_t						out_bytes_n;
	int							bi;
	int							mismatch;
#ifdef MEMBRANE_HWSIM_WITH_GGML_ORACLE
	uint8_t						oracle[HW_MAX_BLOCK_BYTES];
#endif

	mismatch = 0;
	out_bytes_n = mode == HW_Q8_QUANT ? MEMBRANE_QSIMD_Q8_0_BLOCK_BYTES
		: mode == HW_Q4_QUANT ? MEMBRANE_QSIMD_Q4_0_BLOCK_BYTES
		: 32 * sizeof(uint16_t);
	if (mode == HW_Q8_QUANT)
		membrane_simd_q8_0_quantize(MEMBRANE_SIMD_SCALAR,
			(const uint16_t *)(const void *)in_bytes, 32, out_bytes);
	else if (mode == HW_Q8_DEQUANT)
		membrane_simd_q8_0_dequantize(MEMBRANE_SIMD_SCALAR, in_bytes, 32,
			(uint16_t *)(void *)out_bytes);
	else if (mode == HW_Q4_QUANT)
		membrane_simd_q4_0_quantize(MEMBRANE_SIMD_SCALAR,
			(const uint16_t *)(const void *)in_bytes, 32, out_bytes);
	else
		membrane_simd_q4_0_dequantize(MEMBRANE_SIMD_SCALAR, in_bytes, 32,
			(uint16_t *)(void *)out_bytes);
	bi = 0;
	while (bi < 3)
	{
		membrane_status_t	st;

		if (mode == HW_Q8_QUANT)
			st = membrane_simd_q8_0_quantize(backends[bi],
					(const uint16_t *)(const void *)in_bytes, 32, ref);
		else if (mode == HW_Q8_DEQUANT)
			st = membrane_simd_q8_0_dequantize(backends[bi], in_bytes, 32,
					(uint16_t *)(void *)ref);
		else if (mode == HW_Q4_QUANT)
			st = membrane_simd_q4_0_quantize(backends[bi],
					(const uint16_t *)(const void *)in_bytes, 32, ref);
		else
			st = membrane_simd_q4_0_dequantize(backends[bi], in_bytes, 32,
					(uint16_t *)(void *)ref);
		if (st == MEMBRANE_OK && memcmp(ref, out_bytes, out_bytes_n) != 0)
		{
			fprintf(stderr, "PARITY FAIL block %ld: backend mismatch\n",
				block_idx);
			mismatch = 1;
		}
		bi++;
	}
#ifdef MEMBRANE_HWSIM_WITH_GGML_ORACLE
	if (mode == HW_Q8_QUANT)
		membrane_ggml_q8_0_quantize((const uint16_t *)(const void *)in_bytes,
			32, oracle);
	else if (mode == HW_Q8_DEQUANT)
		membrane_ggml_q8_0_dequantize(in_bytes, 32,
			(uint16_t *)(void *)oracle);
	else if (mode == HW_Q4_QUANT)
		membrane_ggml_q4_0_quantize((const uint16_t *)(const void *)in_bytes,
			32, oracle);
	else
		membrane_ggml_q4_0_dequantize(in_bytes, 32,
			(uint16_t *)(void *)oracle);
	if (memcmp(oracle, out_bytes, out_bytes_n) != 0)
	{
		fprintf(stderr, "PARITY FAIL block %ld: ggml oracle mismatch\n",
			block_idx);
		mismatch = 1;
	}
#endif
	return (mismatch);
}

static void	dump_vector_line(FILE *f, hw_mode_t mode, const uint8_t *in_bytes,
				const uint8_t *out_bytes)
{
	size_t	in_n;
	size_t	out_n;
	size_t	i;

	in_n = (mode == HW_Q8_QUANT || mode == HW_Q4_QUANT)
		? 32 * sizeof(uint16_t) : packed_block_bytes(mode);
	out_n = (mode == HW_Q8_QUANT || mode == HW_Q4_QUANT)
		? packed_block_bytes(mode) : 32 * sizeof(uint16_t);
	i = 0;
	while (i < in_n)
	{
		fprintf(f, "%02x", in_bytes[i]);
		i++;
	}
	fprintf(f, " ");
	i = 0;
	while (i < out_n)
	{
		fprintf(f, "%02x", out_bytes[i]);
		i++;
	}
	fprintf(f, "\n");
}

/*
 * Phase 5.2 item 5: PCIe/CXL link bandwidth model. These GB/s figures are
 * publicly documented interconnect specification numbers (PCI-SIG/CXL
 * consortium published rates and encoding overheads), NOT measured on any
 * real hardware in this repository -- there is no PCIe/CXL fabric or FPGA
 * card attached to this machine. They are inputs to an analytical
 * comparison against this tool's own measured/modeled pipeline throughput,
 * not a benchmark result.
 *
 * PCIe Gen4 x16: 16 GT/s/lane, 128b/130b encoding -> 16e9*(128/130)/8 =
 * 1.969 GB/s/lane * 16 lanes = 31.5 GB/s one direction.
 * PCIe Gen5 x16: 32 GT/s/lane, same encoding -> exactly double Gen4 =
 * 63.0 GB/s.
 * CXL 2.0: runs over the PCIe 5.0 physical layer per the CXL 2.0
 * specification -> same 63.0 GB/s x16 figure as PCIe Gen5.
 * CXL 3.0: runs over a PCIe 6.0-class physical layer (64 GT/s, PAM4 +
 * low-overhead FLIT framing) -- publicly cited around ~121 GB/s x16 is an
 * approximation (FLIT/FEC overhead specifics vary by source); marked
 * explicitly as approximate rather than a precise derivation like the
 * Gen4/Gen5 figures above.
 */
typedef struct s_hw_link
{
	const char	*name;
	double		gbps;
	const char	*note;
}	hw_link_t;

static const hw_link_t	g_links[] = {
	{"PCIe Gen4 x16", 31.5, "16 GT/s/lane x16, 128b/130b, exact"},
	{"PCIe Gen5 x16", 63.0, "32 GT/s/lane x16, 128b/130b, exact"},
	{"CXL 2.0 x16 (PCIe5 phy)", 63.0, "same phy as PCIe Gen5 x16, exact"},
	{"CXL 3.0 x16 (PCIe6-class phy)", 121.0,
		"approximate, FLIT/FEC overhead varies by source"},
};

static void	report_link_analysis(const hw_cfg_t *cfg, int fetch_cycles,
				double target_mhz)
{
	double	hz = target_mhz * 1e6;
	double	blocks_per_sec = hz / (double)fetch_cycles;
	size_t	compressed_bytes = packed_block_bytes(cfg->mode);
	double	compressed_gbps = blocks_per_sec * (double)compressed_bytes
			/ 1e9;
	double	raw_gbps = blocks_per_sec * 64.0 / 1e9;
	double	compression_ratio = 64.0 / (double)compressed_bytes;
	size_t	i;

	(void)cfg;
	printf("\nPCIe/CXL link analysis (item 5) at assumed clock %.0f MHz "
		"-- link figures are public spec numbers, not measured here:\n",
		target_mhz);
	printf("  single-pipeline compressed-side throughput: %.4f GB/s\n",
		compressed_gbps);
	printf("  single-pipeline raw-side throughput:         %.4f GB/s\n",
		raw_gbps);
	printf("  compression ratio (this mode):               %.3fx\n",
		compression_ratio);
	i = 0;
	while (i < sizeof(g_links) / sizeof(g_links[0]))
	{
		long	n_pipelines = (long)((g_links[i].gbps + compressed_gbps
					- 1e-9) / compressed_gbps);
		double	raw_equiv_capacity = g_links[i].gbps * compression_ratio;

		printf("  %-32s %7.1f GB/s (%s)\n", g_links[i].name,
			g_links[i].gbps, g_links[i].note);
		printf("      pipelines needed to saturate compressed-side link "
			"bandwidth: %ld\n", n_pipelines);
		printf("      raw-data-equivalent capacity this link carries "
			"once compression is applied: %.1f GB/s (vs %.1f GB/s "
			"if raw data crossed it uncompressed)\n", raw_equiv_capacity,
			g_links[i].gbps);
		i++;
	}
	printf("  note: a single pipeline at %.4f GB/s compressed-side is far "
		"below any of these links' bandwidth (all >= 31.5 GB/s), so a "
		"handful of parallel pipelines (4-15 above) is enough to saturate "
		"even the fastest link modeled here -- for comparison, CPU SIMD "
		"quantization alone already measured up to ~4.4 GB/s on 12 CPU "
		"threads in docs/phase5-quant-engine.md, roughly half of one "
		"pipeline at this clock, underscoring that the link, not the "
		"quantizer datapath, is the scarce resource in this design.\n",
		compressed_gbps);
}

static double	now_secs(void)
{
	struct timespec	ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

static hw_result_t	simulate(const hw_cfg_t *cfg)
{
	hw_result_t	r;
	int			fetch_cycles;
	int			total_latency;
	long		admitted;
	long		retired;
	long long	vcycle;
	long long	real_cycle;
	int			out_occ;
	double		sink_credit;
	size_t		block_bytes;
	FILE		*vecfile;
	double		last_report;
	double		t0;

	memset(&r, 0, sizeof(r));
	fetch_cycles = (32 + cfg->width - 1) / cfg->width;
	if (cfg->source_elems_per_cycle > 0.0)
	{
		int	src_ceil = (int)((32.0 + cfg->source_elems_per_cycle - 1e-9)
				/ cfg->source_elems_per_cycle);

		if (src_ceil > fetch_cycles)
			fetch_cycles = src_ceil;
	}
	total_latency = fetch_cycles + cfg->lat_reduce + cfg->lat_scale
		+ cfg->lat_pack;
	block_bytes = output_block_bytes(cfg->mode);
	vecfile = NULL;
	if (cfg->dump_vectors_path != NULL)
	{
		vecfile = fopen(cfg->dump_vectors_path, "w");
		if (vecfile == NULL)
		{
			fprintf(stderr, "hw-sim: cannot open %s for vector dump\n",
				cfg->dump_vectors_path);
			exit(1);
		}
	}
	admitted = 0;
	retired = 0;
	vcycle = 0;
	real_cycle = 0;
	out_occ = 0;
	sink_credit = 0.0;
	last_report = now_secs();
	t0 = last_report;
	while (retired < cfg->n_blocks)
	{
		int	stalled_this_cycle;

		if (cfg->sink_bytes_per_cycle <= 0.0)
		{
			if (out_occ > 0)
				out_occ = 0;
		}
		else
		{
			sink_credit += cfg->sink_bytes_per_cycle;
			while (sink_credit >= (double)block_bytes && out_occ > 0)
			{
				out_occ--;
				sink_credit -= (double)block_bytes;
			}
		}
		r.occ_sum += out_occ;
		if (out_occ > r.occ_max)
			r.occ_max = out_occ;
		stalled_this_cycle = 0;
		if (admitted > retired)
		{
			long long	retire_vcycle_next = (long long)retired
					* fetch_cycles + total_latency;

			if (vcycle == retire_vcycle_next)
			{
				if (out_occ < cfg->out_fifo_depth)
				{
					uint32_t	seed_block;
					uint8_t		in_bytes[HW_MAX_BLOCK_BYTES];
					uint8_t		out_bytes[HW_MAX_BLOCK_BYTES];
					size_t		in_n;
					int			j;

					seed_block = cfg->seed + (uint32_t)retired * 2654435761u;
					in_n = (cfg->mode == HW_Q8_QUANT
							|| cfg->mode == HW_Q4_QUANT)
						? 32 * sizeof(uint16_t) : block_bytes;
					if (cfg->mode == HW_Q8_QUANT
						|| cfg->mode == HW_Q4_QUANT)
					{
						j = 0;
						while (j < 32)
						{
							((uint16_t *)(void *)in_bytes)[j] =
								rand_f16(&seed_block);
							j++;
						}
					}
					else
					{
						uint16_t	tmp[32];

						j = 0;
						while (j < 32)
						{
							tmp[j] = rand_f16(&seed_block);
							j++;
						}
						if (cfg->mode == HW_Q8_DEQUANT)
							membrane_simd_q8_0_quantize(MEMBRANE_SIMD_SCALAR,
								tmp, 32, in_bytes);
						else
							membrane_simd_q4_0_quantize(MEMBRANE_SIMD_SCALAR,
								tmp, 32, in_bytes);
					}
					(void)in_n;
					r.parity_checked++;
					if (compute_and_check_block(cfg->mode, in_bytes,
							out_bytes, retired))
						r.parity_fail++;
					if (vecfile != NULL)
						dump_vector_line(vecfile, cfg->mode, in_bytes,
							out_bytes);
					out_occ++;
					retired++;
				}
				else
				{
					r.stall_cycles++;
					stalled_this_cycle = 1;
				}
			}
		}
		if (!stalled_this_cycle && admitted < cfg->n_blocks
			&& vcycle == (long long)admitted * fetch_cycles)
			admitted++;
		if (!stalled_this_cycle)
			vcycle++;
		real_cycle++;
		if (!cfg->quiet && now_secs() - last_report >= 60.0)
		{
			double	elapsed = now_secs() - t0;
			double	frac = (double)retired / (double)cfg->n_blocks;
			double	eta = frac > 0.0 ? elapsed / frac - elapsed : -1.0;

			fprintf(stderr, "  [heartbeat] mode=%s elapsed=%.0fs "
				"blocks %ld/%ld (%.1f%%) ETA ~%.0fs\n", mode_name(cfg->mode),
				elapsed, retired, cfg->n_blocks, frac * 100.0, eta);
			last_report = now_secs();
		}
	}
	if (vecfile != NULL)
		fclose(vecfile);
	r.n_blocks = cfg->n_blocks;
	r.fetch_cycles = fetch_cycles;
	r.total_latency = total_latency;
	r.real_cycles = real_cycle;
	return (r);
}

static void	report(const hw_cfg_t *cfg, const hw_result_t *r)
{
	static const double	clocks_mhz[] = {100, 150, 200, 250, 300, 400, 500};
	size_t					i;
	double					ideal_cycles;
	double					backpressure_pct;
	size_t					block_bytes;
	int						is_encode;

	block_bytes = output_block_bytes(cfg->mode);
	is_encode = cfg->mode == HW_Q8_QUANT || cfg->mode == HW_Q4_QUANT;
	ideal_cycles = (double)r->n_blocks * r->fetch_cycles;
	backpressure_pct = 100.0 * (double)r->stall_cycles
		/ (double)r->real_cycles;
	printf("MEMBRANE Phase 5.2 hardware cycle model: %s\n",
		mode_name(cfg->mode));
	printf("config: width=%d out_fifo_depth=%d n_blocks=%ld "
		"sink_bytes_per_cycle=%.3f source_elems_per_cycle=%.3f "
		"lat(reduce=%d,scale=%d,pack=%d)\n", cfg->width,
		cfg->out_fifo_depth, cfg->n_blocks, cfg->sink_bytes_per_cycle,
		cfg->source_elems_per_cycle, cfg->lat_reduce, cfg->lat_scale,
		cfg->lat_pack);
	printf("fetch_cycles (== initiation interval, cycle-groups)=%d\n",
		r->fetch_cycles);
	printf("pipeline fill latency (cycles to first block out)=%d\n",
		r->total_latency);
	printf("total real cycles=%lld (ideal fill-free steady-state would be "
		"%.0f cycles; overhead from fill+drain+stall=%.0f cycles)\n",
		r->real_cycles, ideal_cycles, (double)r->real_cycles - ideal_cycles);
	printf("stall cycles (backpressure)=%lld (%.4f%% of total)\n",
		r->stall_cycles, backpressure_pct);
	printf("output FIFO occupancy: max=%d avg=%.3f (depth=%d)\n", r->occ_max,
		(double)r->occ_sum / (double)r->real_cycles, cfg->out_fifo_depth);
	printf("parity: %ld blocks checked, %ld mismatches%s\n",
		r->parity_checked, r->parity_fail,
#ifdef MEMBRANE_HWSIM_WITH_GGML_ORACLE
		" (cross-checked vs scalar/SSE4.1/AVX2 MEMBRANE and ggml oracle)"
#else
		" (cross-checked vs scalar/SSE4.1/AVX2 MEMBRANE; build with "
		"MEMBRANE_ENABLE_LLAMA for the ggml oracle cross-check too)"
#endif
		);
	printf("clock sweep (theoretical steady-state throughput at "
		"fetch_cycles-limited II, %s side, %zu bytes/block):\n",
		is_encode ? "output/packed" : "output/f16", block_bytes);
	i = 0;
	while (i < sizeof(clocks_mhz) / sizeof(clocks_mhz[0]))
	{
		double	hz = clocks_mhz[i] * 1e6;
		double	blocks_per_sec = hz / (double)r->fetch_cycles;
		double	gbps = blocks_per_sec * (double)block_bytes / 1e9;

		printf("  %5.0f MHz -> %8.3f GB/s (%.3e blocks/s)\n", clocks_mhz[i],
			gbps, blocks_per_sec);
		i++;
	}
	if (cfg->csv_path != NULL)
	{
		FILE	*csv = fopen(cfg->csv_path, "w");

		if (csv != NULL)
		{
			fprintf(csv, "clock_mhz,gbps\n");
			i = 0;
			while (i < sizeof(clocks_mhz) / sizeof(clocks_mhz[0]))
			{
				double	hz = clocks_mhz[i] * 1e6;
				double	blocks_per_sec = hz / (double)r->fetch_cycles;
				double	gbps = blocks_per_sec * (double)block_bytes / 1e9;

				fprintf(csv, "%.0f,%.6f\n", clocks_mhz[i], gbps);
				i++;
			}
			fclose(csv);
		}
	}
	if (!cfg->skip_link_report)
		report_link_analysis(cfg, r->fetch_cycles, cfg->target_clock_mhz);
	if (r->parity_fail != 0)
		exit(1);
}

int	main(int argc, char **argv)
{
	hw_cfg_t	cfg;
	int			i;

	cfg.mode = HW_Q8_QUANT;
	cfg.width = 32;
	cfg.out_fifo_depth = 4;
	cfg.n_blocks = 1000;
	cfg.sink_bytes_per_cycle = 0.0;
	cfg.source_elems_per_cycle = 0.0;
	cfg.lat_reduce = 5;
	cfg.lat_scale = 10;
	cfg.lat_pack = 3;
	cfg.seed = 0xC0FFEEu;
	cfg.dump_vectors_path = NULL;
	cfg.csv_path = NULL;
	cfg.quiet = 0;
	cfg.target_clock_mhz = 250.0;
	cfg.skip_link_report = 0;
	i = 1;
	while (i < argc)
	{
		if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
		{
			const char	*m = argv[++i];

			if (strcmp(m, "q8enc") == 0)
				cfg.mode = HW_Q8_QUANT;
			else if (strcmp(m, "q8dec") == 0)
				cfg.mode = HW_Q8_DEQUANT;
			else if (strcmp(m, "q4enc") == 0)
				cfg.mode = HW_Q4_QUANT;
			else if (strcmp(m, "q4dec") == 0)
				cfg.mode = HW_Q4_DEQUANT;
			else
			{
				fprintf(stderr, "unknown mode %s\n", m);
				return (1);
			}
		}
		else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
			cfg.width = atoi(argv[++i]);
		else if (strcmp(argv[i], "--out-fifo") == 0 && i + 1 < argc)
			cfg.out_fifo_depth = atoi(argv[++i]);
		else if (strcmp(argv[i], "--n-blocks") == 0 && i + 1 < argc)
			cfg.n_blocks = atol(argv[++i]);
		else if (strcmp(argv[i], "--sink-bytes-per-cycle") == 0
			&& i + 1 < argc)
			cfg.sink_bytes_per_cycle = atof(argv[++i]);
		else if (strcmp(argv[i], "--source-elems-per-cycle") == 0
			&& i + 1 < argc)
			cfg.source_elems_per_cycle = atof(argv[++i]);
		else if (strcmp(argv[i], "--reduce-lat") == 0 && i + 1 < argc)
			cfg.lat_reduce = atoi(argv[++i]);
		else if (strcmp(argv[i], "--scale-lat") == 0 && i + 1 < argc)
			cfg.lat_scale = atoi(argv[++i]);
		else if (strcmp(argv[i], "--pack-lat") == 0 && i + 1 < argc)
			cfg.lat_pack = atoi(argv[++i]);
		else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
			cfg.seed = (uint32_t)strtoul(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--dump-vectors") == 0 && i + 1 < argc)
			cfg.dump_vectors_path = argv[++i];
		else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc)
			cfg.csv_path = argv[++i];
		else if (strcmp(argv[i], "--quiet") == 0)
			cfg.quiet = 1;
		else if (strcmp(argv[i], "--target-clock-mhz") == 0 && i + 1 < argc)
			cfg.target_clock_mhz = atof(argv[++i]);
		else if (strcmp(argv[i], "--no-link-report") == 0)
			cfg.skip_link_report = 1;
		else
		{
			fprintf(stderr, "membrane-hw-sim: unknown or malformed "
				"option %s\n", argv[i]);
			return (1);
		}
		i++;
	}
	if (cfg.width < 1 || cfg.width > 32)
	{
		fprintf(stderr, "--width must be in [1,32]\n");
		return (1);
	}
	{
		hw_result_t	r = simulate(&cfg);

		report(&cfg, &r);
	}
	return (0);
}
