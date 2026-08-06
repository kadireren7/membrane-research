#ifndef MEMBRANE_EXACTSIM_CALIBRATE_H
#define MEMBRANE_EXACTSIM_CALIBRATE_H

#include <string>
#include <vector>

#include "engine.h"

namespace exactsim
{

/*
 * Phase 6.3: a real per-step CXL demand profile for one (policy,
 * context, per-sequence hot-cache-budget) combination, produced by
 * actually running Phase 6.2's single-channel-predictor engine
 * (wssim::run_scenario_calibration) once. Concurrent sequences in
 * exact_engine.cpp replay this SAME real profile rather than each
 * independently re-running the full per-channel predictor --
 * predicted working-set size for every non-FULL/NO_PREFETCH policy is
 * bounded (not context-growing, Phase 6.2's own measured finding), so
 * the profile is real and representative of what any statistically
 * similar sequence would see, but re-deriving it separately per
 * concurrent sequence at up to 128K context x 512 concurrency is not
 * tractable this session -- the same calibrate-once-replay-many
 * reduction Phase 6.1 itself used for its own synthetic sweep
 * (docs/phase6-cxl-near-memory.md section 3), applied here one level
 * up the stack. Genuine multi-sequence CXL-link/quant-engine
 * contention is NOT calibrated away -- exact_engine.cpp's discrete-
 * event loop applies it in true global time order, for real.
 */
struct calibrated_profile_t
{
	std::string							policy_name;
	uint32_t							context_tokens;
	std::vector<wssim::per_step_calib_t>	steps;
	double								hit_rate;
	double								precision;
	double								recall;
	double								mean_working_set_blocks;
	wssim::layer_head_stats_t				layer_head;
	wssim::coalescing_stats_t				coalescing;
	bool								source_is_real_capture;
};

calibrated_profile_t	calibrate(const wssim::attn_trace_t &trace,
							const wssim::model_calibration_t &model,
							const wssim::scenario_config_t &cfg,
							bool want_layer_head,
							const wssim::hardware_profile_t *hw = nullptr);

/* Phase 6.5: identical to calibrate() above except it reads the trace
 * through an attn_trace_reader_t (in-memory/mmap/buffered-streaming --
 * see attn_trace_reader.h) instead of requiring the whole synthetic-
 * extended trace resident as one attn_trace_t. Both overloads run the
 * same wssim::run_scenario_calibration_impl body underneath (engine.cpp),
 * so their output differs only by whatever the chosen backend's own
 * disclosed precision is (bit-identical for in-memory/mmap/streaming
 * on an uncompressed or exactly-quantization-aligned trace -- see
 * test_attn_trace_reader.cpp's parity tests). */
calibrated_profile_t	calibrate_streamed(wssim::attn_trace_reader_t &trace,
							const wssim::model_calibration_t &model,
							const wssim::scenario_config_t &cfg,
							bool want_layer_head,
							const wssim::hardware_profile_t *hw = nullptr);

}	/* namespace exactsim */

#endif
