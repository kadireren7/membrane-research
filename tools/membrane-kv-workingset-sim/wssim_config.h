#ifndef MEMBRANE_WSSIM_CONFIG_H
#define MEMBRANE_WSSIM_CONFIG_H

#include <string>

#include "sim_config.h"

/*
 * Phase 6.2 calibration constants, layered on top of Phase 6.1's
 * sim_config.h (reused unmodified: CXL link latency/bandwidth, the
 * near-memory quant/dequant pipeline rate, Q8/Q4 compression ratios).
 * Everything new here is for the working-set/hot-cache/prefetch
 * machinery this phase adds -- labeled ASSUMED like Phase 6.1's own
 * device-local numbers, since no real CXL/near-memory hardware exists
 * to measure block-metadata-SRAM or decompressed-hot-cache access
 * time on.
 */
namespace wssim
{

/* ---- ASSUMED: on-device block metadata SRAM lookup (component 3 of
 * the near-memory pipeline, docs/phase6-cxl-near-memory.md section
 * 10) -- small associative lookup, SRAM-class access time. ---- */
constexpr double METADATA_LOOKUP_NS_PER_BLOCK = 5.0;

/* ---- ASSUMED: host/GPU-resident decompressed hot-cache lookup
 * (component 6) -- on-chip cache/SRAM-class access, faster than the
 * device-local metadata SRAM above only because it never crosses the
 * CXL link at all. ---- */
constexpr double HOTCACHE_LOOKUP_NS_PER_BLOCK = 2.0;

/* REAL, reused from sim_config.h's Phase 6.1 pipeline-count
 * sensitivity study default (docs/phase6-cxl-near-memory.md section
 * 7's "8 (default)" row). */
constexpr int	DEFAULT_QUANT_PIPELINES = 8;

/*
 * Phase 6.4 item 9: the hardware assumptions that previously lived as
 * compile-time constants (sim_config.h's CXL_LINK_LATENCY_NS/
 * CXL_LINK_BANDWIDTH_GBPS/NEARMEM_PIPELINE_BYTES_PER_NS, this file's
 * DEFAULT_QUANT_PIPELINES) are now also available as a runtime value
 * so the hardware-sensitivity matrix can sweep them without
 * recompiling. `default_hardware_profile()` reproduces EXACTLY the
 * old hardcoded values, so every pre-Phase-6.4 call site that doesn't
 * pass a profile explicitly keeps behaving identically -- this is
 * additive, not a silent behavior change.
 */
struct hardware_profile_t
{
	std::string	label = "default";
	double		cxl_link_latency_ns = sim::CXL_LINK_LATENCY_NS;
	double		cxl_link_bandwidth_gbps = sim::CXL_LINK_BANDWIDTH_GBPS;
	double		nearmem_pipeline_bytes_per_ns = sim::NEARMEM_PIPELINE_BYTES_PER_NS;
	int			quant_pipelines = DEFAULT_QUANT_PIPELINES;
};

inline hardware_profile_t	default_hardware_profile()
{
	return (hardware_profile_t{});
}

}	/* namespace wssim */

#endif
