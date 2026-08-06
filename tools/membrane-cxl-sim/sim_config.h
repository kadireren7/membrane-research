#ifndef MEMBRANE_CXL_SIM_CONFIG_H
#define MEMBRANE_CXL_SIM_CONFIG_H

#include <cstdint>
#include <string>

/*
 * Phase 6.1: calibration constants for membrane-cxl-sim.
 *
 * Every constant below is labeled at its definition as either REAL
 * (taken directly from a measurement made earlier in this project,
 * cited to the phase/section that produced it) or ASSUMED
 * (an explicit, disclosed, industry-typical figure -- no real CXL
 * hardware exists in this environment, so link/device-memory numbers
 * for CXL, NVMe, and real PCIe transport are necessarily assumptions;
 * see docs/phase6-cxl-near-memory.md for the full disclosure and
 * citations). Nothing here is fabricated to make a particular
 * baseline look better or worse.
 */
namespace sim
{

/* REAL: Phase 5.x block granularity -- 32 F16 elements per block, 2
 * bytes each (include/membrane/quant_simd.h's MEMBRANE_QSIMD_BLOCK_ELEMS). */
constexpr double BYTES_PER_BLOCK = 64.0;

/* 1 GB/s == 1 byte/ns exactly (1e9 bytes/s / 1e9 ns/s) -- every
 * bandwidth field below is in this unit so service-time math is a
 * plain division with no unit-conversion factor. */
struct link_params_t
{
	double	base_latency_ns;
	double	bandwidth_gbps;	/* == bytes/ns */
};

struct quant_params_t
{
	double	ns_per_block;	/* real per-32-element-block cost */
	int		parallel_pipelines;
};

enum class baseline_t
{
	HOST_ONLY = 0,
	CPU_RAM_OFFLOAD,
	NVME_OFFLOAD,
	PCIE_FPGA_ROUNDTRIP,
	CXL_NO_PROCESSING,
	MEMBRANE_CXL_NEAR_MEMORY,
	COUNT
};

inline const char	*baseline_name(baseline_t b)
{
	switch (b)
	{
	case baseline_t::HOST_ONLY: return ("gpu-host-only");
	case baseline_t::CPU_RAM_OFFLOAD: return ("cpu-ram-offload");
	case baseline_t::NVME_OFFLOAD: return ("nvme-offload");
	case baseline_t::PCIE_FPGA_ROUNDTRIP: return ("pcie-fpga-roundtrip");
	case baseline_t::CXL_NO_PROCESSING: return ("cxl-no-processing");
	case baseline_t::MEMBRANE_CXL_NEAR_MEMORY: return ("membrane-cxl-near-memory");
	default: return ("unknown");
	}
}

/* ---- REAL: Phase 5.4 membrane-quant-bench, re-measured this session
 * (docs/phase5-pcie-hardware-loop.md section 8), scalar backend,
 * 32-element block, ns/block. Used for any baseline where dequant/
 * quant runs on the HOST CPU (competing with inference compute). ---- */
constexpr double CPU_Q8_QUANTIZE_NS_1T   = 123.00;
constexpr double CPU_Q8_DEQUANTIZE_NS_1T = 179.77;
constexpr double CPU_Q4_QUANTIZE_NS_1T   = 77.55;
constexpr double CPU_Q4_DEQUANTIZE_NS_1T = 146.46;
constexpr double CPU_Q8_QUANTIZE_NS_4T   = 32.54;
constexpr double CPU_Q8_DEQUANTIZE_NS_4T = 47.78;
constexpr double CPU_Q4_QUANTIZE_NS_4T   = 20.92;
constexpr double CPU_Q4_DEQUANTIZE_NS_4T = 40.45;

/* ---- REAL: Phase 5.3 membrane_quant_stream_top, the WIDE 512-bit/
 * 64-byte-per-cycle RTL pipeline (docs/phase5-synthesizable-fpga.md
 * section 10), assumed 200MHz clock (disclosed there as an assumption,
 * not a measured Fmax -- carried forward unchanged here). This is the
 * number the near-memory baseline uses, because near-memory placement
 * means the pipeline sits directly next to device DRAM on a wide local
 * bus, NOT behind Phase 5.4's narrow 32-bit DMA-facing port. ---- */
constexpr double NEARMEM_PIPELINE_BYTES_PER_NS = 64.0 / 5.0; /* 64B/cycle @ 200MHz (5ns/cycle) = 12.8 GB/s */

/* ---- REAL: Phase 5.4 membrane-fpga-runtime perf, re-measured this
 * session (docs/phase5-pcie-hardware-loop.md section 7): the bridge's
 * OWN 32-bit DMA-facing port, ~85.0 ns/block sustained, uniform across
 * all four ops. This is the number the PCIe-FPGA-roundtrip baseline
 * uses -- it is deliberately the NARROW number, because that baseline
 * models a discrete coprocessor reached over an actual DMA-facing
 * link, not a near-memory pipeline. ---- */
constexpr double PCIE_FPGA_NS_PER_BLOCK = 85.03;

/* ---- ASSUMED: real PCIe MMIO-doorbell + DMA + completion-interrupt
 * round trip for a discrete accelerator card. Phase 5.4 section 9
 * disclosed that its own emulation charges ~0ns of this (the ~210ns
 * "round-trip latency" it measured is pure internal FIFO/logic cycles,
 * not real transport) and that a real round trip is "almost certainly
 * microseconds, not the ~210ns this emulation reports." No real card
 * is available in this environment to measure it, so this is an
 * explicit point estimate from published driver-level accelerator
 * benchmarks (typical range ~1-10us for a full doorbell/DMA/IRQ cycle
 * on commodity PCIe accelerators); 3000ns is used as the representative
 * value, with 1000/3000/8000ns swept in the sensitivity section of the
 * report to show the conclusion is not sensitive to the exact pick
 * within this range. ---- */
constexpr double PCIE_ROUNDTRIP_ASSUMED_NS = 3000.0;

/* ---- ASSUMED: CXL 2.0/3.0 CXL.mem published industry figures for
 * added latency over local host DRAM, and link bandwidth for an x8
 * link (PCIe5-class physical layer). No real CXL hardware is available
 * in this environment. Commonly cited ranges: ~100-250ns additional
 * round-trip latency over local DRAM; ~32-64 GB/s per x8 link
 * (matching PCIe5 x8 raw rate class). Point estimates below are the
 * middle of each published range. ---- */
constexpr double CXL_LINK_LATENCY_NS = 170.0;
constexpr double CXL_LINK_BANDWIDTH_GBPS = 48.0;

/* ---- ASSUMED: on-appliance DDR5 device DRAM, multi-channel,
 * industry-typical for a memory-expansion-class device. Latency is
 * DDR5-class random access; bandwidth assumes a modest multi-channel
 * controller (NOT HBM-class, since the phase spec asks for 512GB/1TB/
 * 2TB capacities, which is a DDR5 RDIMM regime, not HBM). ---- */
constexpr double DEVICE_DRAM_LATENCY_NS = 100.0;
constexpr double DEVICE_DRAM_BANDWIDTH_GBPS = 120.0;

/* ---- ASSUMED: host system DRAM (the "CPU RAM offload" baseline's
 * target), industry-typical DDR5 dual/quad-channel server figures. ---- */
constexpr double HOST_RAM_LATENCY_NS = 100.0;
constexpr double HOST_RAM_BANDWIDTH_GBPS = 60.0;

/* ---- ASSUMED: NVMe Gen4 SSD, real-world (driver+filesystem, not raw
 * NAND) figures for random small-block reads/writes, industry-typical
 * for high-end consumer/datacenter Gen4 drives. ---- */
constexpr double NVME_LATENCY_NS = 90000.0;
constexpr double NVME_BANDWIDTH_GBPS = 6.5;

/* ---- REAL: Phase 5.4 section 13, actual measured quality/compression
 * for SmolLM2-135M/360M (docs/phase5-pcie-hardware-loop.md). Reused
 * verbatim as the WARM (Q8) / COLD (Q4) tier compression ratios and
 * quality map -- not re-derived or re-thresholded for this phase, per
 * the spec's explicit instruction not to invent a new quality bar. ---- */
constexpr double Q8_COMPRESSION_RATIO = 1.88; /* KV bytes shrink by this much */
constexpr double Q4_COMPRESSION_RATIO = 3.55;
constexpr double Q8_TOP1_MIN_PCT = 96.88;   /* worst real-measured Q8 top1 */
constexpr double Q4_TOP1_MIN_PCT = 71.88;   /* worst real-measured Q4 top1 */

/* ---- REAL: Phase 6.1 membrane-kv-trace-capture, this session
 * (real llama_state_seq_get_size() deltas, see
 * benchmarks/cxl-sim/traces/ and docs/phase6-cxl-near-memory.md
 * section 3). Used only as documentation-level cross-reference; the
 * simulator itself reads the actual .kvtrace files, not these
 * constants. ---- */
constexpr uint32_t SMOLLM2_135M_BYTES_PER_TOKEN = 23052;
constexpr uint32_t SMOLLM2_360M_BYTES_PER_TOKEN = 40972;

/* ---- REAL: Phase 5.4 section 13 real end-to-end tok/s, used as the
 * compute-bound decode rate floor (see workload.h) -- how fast a
 * SINGLE sequence decodes when the KV memory subsystem is not the
 * bottleneck. This simulator does not model multi-sequence compute
 * batching gains/degradation (disclosed scope limit, section on "what
 * this simulator does not model" in the doc) -- it stays fixed per
 * sequence regardless of concurrency; only shared-resource queueing
 * for the MEMORY subsystem varies with concurrency. ---- */
constexpr double SMOLLM2_135M_TOK_PER_SEC = 63.8;
constexpr double SMOLLM2_360M_TOK_PER_SEC = 24.4;

}	/* namespace sim */

#endif
