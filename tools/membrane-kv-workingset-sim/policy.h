#ifndef MEMBRANE_WSSIM_POLICY_H
#define MEMBRANE_WSSIM_POLICY_H

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wssim
{

/*
 * The 8 working-set policies compared by this phase, all operating on
 * one (layer, kv_head_group) CHANNEL at a time -- KV data is
 * physically stored and fetched per KV-head group (GQA shares K/V
 * across several query heads), so that is the real unit a memory
 * system would cache/prefetch, not a global-across-all-heads union
 * (which would badly overstate true working-set size -- different
 * heads attend to different blocks, a well-documented real effect
 * also visible in this phase's own captured traces).
 *
 * FULL and ORACLE are documented, deliberate exceptions to the
 * "causal, only steps < t" rule every other policy follows:
 * FULL is the no-selection baseline (everything is always resident);
 * ORACLE is fed this step's own real ground truth directly, to
 * establish the achievable upper bound (section 12's oracle bound),
 * not a claim that a real predictor could know the future.
 */
enum class policy_t
{
	FULL = 0,
	NO_PREFETCH,	/* Phase 6.3: never proactively fetch anything --
			 * every ground-truth block not already hot is a
			 * compulsory synchronous miss (the "exact cache, no
			 * prefetch" baseline). */
	SLIDING_WINDOW,
	RECENCY_SINKS,
	TOPK_LAG1,
	HEAVY_HITTER,
	RECENCY_FREQUENCY_HYBRID,
	ORACLE,
	MEMBRANE_PREDICTIVE,
	COUNT
};

const char	*policy_name(policy_t p);

struct policy_params_t
{
	uint32_t	sliding_window_tokens = 512;
	uint32_t	sink_blocks = 1;
	uint32_t	heavy_hitter_top_n = 8;
	uint32_t	hybrid_freq_top_n = 4;
	double		heavy_hitter_decay = 0.98;	/* per decode step */
	double		predictive_budget_multiplier = 1.5;
	/* Phase 6.3: caps m_heavy_score/m_freq_count at this many tracked
	 * distinct blocks (a real, standard bounded heavy-hitter-sketch
	 * technique, e.g. Space-Saving -- not just a performance hack: no
	 * real system keeps unbounded per-block statistics forever
	 * either). Without this, per-channel bookkeeping cost grows with
	 * the number of distinct blocks ever seen (effectively with
	 * context length), which made a genuine O(context) predicted-
	 * working-set-size policy still cost O(context^2) internally --
	 * exactly the kind of blowup that made Phase 6.2's largest
	 * scenario take 9 real minutes, and would have made this phase's
	 * 128K-context scenarios intractable. */
	uint32_t	max_tracked_blocks = 128;
};

/*
 * Causal per-channel predictor state (recency/frequency/heavy-hitter
 * history), updated one step at a time via observe(). predict()
 * returns policy p's working set for the step about to be served,
 * using only state accumulated by prior observe() calls (plus, for
 * ORACLE only, the ground-truth argument the caller passes in for
 * that same step -- see policy_t's doc comment).
 */
class channel_predictor_t
{
public:
	channel_predictor_t(uint32_t block_size_tokens,
		const policy_params_t &params);

	std::vector<uint32_t>	predict(policy_t p, uint32_t step,
						uint32_t current_num_blocks,
						const std::vector<uint32_t> &ground_truth_this_step) const;

	void	observe(uint32_t step,
				const std::vector<uint32_t> &ground_truth_blocks);

private:
	uint32_t									m_block_size;
	policy_params_t								m_params;
	std::vector<uint32_t>						m_last_ground_truth;
	std::unordered_map<uint32_t, uint32_t>		m_freq_count;
	std::unordered_map<uint32_t, double>		m_heavy_score;
	std::unordered_map<uint32_t, uint32_t>		m_heavy_last_touch;
	std::vector<uint32_t>						m_recent_sizes;

	std::vector<uint32_t>	sliding_window(uint32_t current_num_blocks) const;
	std::vector<uint32_t>	top_heavy_hitters(uint32_t step, uint32_t n) const;
	std::vector<uint32_t>	top_frequent(uint32_t n) const;
	uint32_t				predicted_budget() const;
	void					prune_if_over_cap(uint32_t step);
};

}	/* namespace wssim */

#endif
