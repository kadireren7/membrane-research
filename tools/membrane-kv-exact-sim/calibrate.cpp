#include "calibrate.h"
#include "policy.h"

namespace exactsim
{

calibrated_profile_t	calibrate(const wssim::attn_trace_t &trace,
						const wssim::model_calibration_t &model,
						const wssim::scenario_config_t &cfg,
						bool want_layer_head,
						const wssim::hardware_profile_t *hw)
{
	calibrated_profile_t		out{};
	wssim::layer_head_stats_t	lh;
	wssim::coalescing_stats_t	coal{};

	out.policy_name = wssim::policy_name(cfg.policy);
	out.context_tokens = trace.prompt_len + trace.step_count;
	out.source_is_real_capture = trace.is_real_capture;
	wssim::scenario_result_t	r = wssim::run_scenario_calibration(trace,
		model, cfg, &out.steps, want_layer_head ? &lh : nullptr,
		cfg.coalescing_window > 0 ? &coal : nullptr, hw);
	out.hit_rate = r.hot_cache_hit_rate;
	out.precision = r.precision;
	out.recall = r.recall;
	out.mean_working_set_blocks = r.mean_working_set_blocks;
	if (want_layer_head)
		out.layer_head = lh;
	out.coalescing = coal;
	return (out);
}

calibrated_profile_t	calibrate_streamed(wssim::attn_trace_reader_t &trace,
						const wssim::model_calibration_t &model,
						const wssim::scenario_config_t &cfg,
						bool want_layer_head,
						const wssim::hardware_profile_t *hw)
{
	calibrated_profile_t				out{};
	wssim::layer_head_stats_t			lh;
	wssim::coalescing_stats_t			coal{};
	const wssim::trace_metadata_t		&md = trace.get_metadata();

	out.policy_name = wssim::policy_name(cfg.policy);
	out.context_tokens = md.prompt_len + md.step_count;
	out.source_is_real_capture = md.is_real_capture;
	wssim::scenario_result_t	r = wssim::run_scenario_calibration_streamed(
		trace, model, cfg, &out.steps, want_layer_head ? &lh : nullptr,
		cfg.coalescing_window > 0 ? &coal : nullptr, hw);
	out.hit_rate = r.hot_cache_hit_rate;
	out.precision = r.precision;
	out.recall = r.recall;
	out.mean_working_set_blocks = r.mean_working_set_blocks;
	if (want_layer_head)
		out.layer_head = lh;
	out.coalescing = coal;
	return (out);
}

}	/* namespace exactsim */
