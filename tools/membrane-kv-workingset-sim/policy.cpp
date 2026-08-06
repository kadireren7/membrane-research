#include <algorithm>
#include <cmath>
#include <set>

#include "policy.h"

namespace wssim
{

const char	*policy_name(policy_t p)
{
	switch (p)
	{
	case policy_t::FULL: return ("full-attention");
	case policy_t::NO_PREFETCH: return ("no-prefetch");
	case policy_t::SLIDING_WINDOW: return ("sliding-window");
	case policy_t::RECENCY_SINKS: return ("recency+sinks");
	case policy_t::TOPK_LAG1: return ("topk-attention-blocks");
	case policy_t::HEAVY_HITTER: return ("heavy-hitter-blocks");
	case policy_t::RECENCY_FREQUENCY_HYBRID: return ("recency-frequency-hybrid");
	case policy_t::ORACLE: return ("oracle");
	case policy_t::MEMBRANE_PREDICTIVE: return ("membrane-predictive");
	default: return ("unknown");
	}
}

channel_predictor_t::channel_predictor_t(uint32_t block_size_tokens,
		const policy_params_t &params)
	: m_block_size(block_size_tokens), m_params(params)
{
}

std::vector<uint32_t>	channel_predictor_t::sliding_window(
		uint32_t current_num_blocks) const
{
	std::vector<uint32_t>	out;
	uint32_t				window_blocks;
	uint32_t				start;

	window_blocks = (m_params.sliding_window_tokens + m_block_size - 1)
		/ m_block_size;
	if (window_blocks == 0)
		window_blocks = 1;
	start = (current_num_blocks > window_blocks)
		? (current_num_blocks - window_blocks) : 0;
	for (uint32_t b = start; b < current_num_blocks; b++)
		out.push_back(b);
	return (out);
}

std::vector<uint32_t>	channel_predictor_t::top_heavy_hitters(
		uint32_t step, uint32_t n) const
{
	std::vector<std::pair<double, uint32_t>>	scored;

	scored.reserve(m_heavy_score.size());
	for (const auto &kv : m_heavy_score)
	{
		uint32_t	last = m_heavy_last_touch.at(kv.first);
		double		eff = kv.second
			* std::pow(m_params.heavy_hitter_decay, (double)(step - last));
		scored.emplace_back(eff, kv.first);
	}
	/* Only the top `n` (a small constant, 4-8) are ever needed, so a
	 * partial sort -- O(m log n) instead of a full O(m log m) sort --
	 * is enough. This matters here specifically: HEAVY_HITTER and
	 * MEMBRANE_PREDICTIVE call this once per (step, channel), and at
	 * the larger synthetic contexts in sweep B (up to 4096 steps x
	 * 160 channels), m (distinct blocks ever seen by this channel)
	 * grows into the low hundreds -- a real performance fix caught
	 * during this phase's own development (a full sort per call made
	 * the largest sweep-B scenario take several real minutes). */
	uint32_t	take = n < scored.size() ? n : (uint32_t)scored.size();
	std::partial_sort(scored.begin(), scored.begin() + take, scored.end(),
		[](const auto &a, const auto &b) { return (a.first > b.first); });
	std::vector<uint32_t>	out;
	for (uint32_t i = 0; i < take; i++)
		out.push_back(scored[i].second);
	return (out);
}

std::vector<uint32_t>	channel_predictor_t::top_frequent(uint32_t n) const
{
	std::vector<std::pair<uint32_t, uint32_t>>	scored(m_freq_count.begin(),
		m_freq_count.end());

	std::sort(scored.begin(), scored.end(),
		[](const auto &a, const auto &b) { return (a.second > b.second); });
	std::vector<uint32_t>	out;
	for (uint32_t i = 0; i < n && i < scored.size(); i++)
		out.push_back(scored[i].first);
	return (out);
}

uint32_t	channel_predictor_t::predicted_budget() const
{
	if (m_recent_sizes.empty())
		return (0);
	uint64_t	sum = 0;
	for (uint32_t s : m_recent_sizes)
		sum += s;
	double	avg = (double)sum / m_recent_sizes.size();
	uint32_t	budget = (uint32_t)std::lround(
		avg * m_params.predictive_budget_multiplier);
	return (budget < 1 ? 1 : budget);
}

std::vector<uint32_t>	channel_predictor_t::predict(policy_t p,
		uint32_t step, uint32_t current_num_blocks,
		const std::vector<uint32_t> &ground_truth_this_step) const
{
	if (p == policy_t::FULL)
	{
		std::vector<uint32_t>	out;
		for (uint32_t b = 0; b < current_num_blocks; b++)
			out.push_back(b);
		return (out);
	}
	if (p == policy_t::NO_PREFETCH)
		return (std::vector<uint32_t>());
	if (p == policy_t::ORACLE)
		return (ground_truth_this_step);
	if (p == policy_t::SLIDING_WINDOW)
		return (sliding_window(current_num_blocks));
	if (p == policy_t::RECENCY_SINKS)
	{
		/* Deduplicated against the sliding window for the same reason
		 * as RECENCY_FREQUENCY_HYBRID below: at small current_num_blocks
		 * the window can already cover the sink blocks. */
		std::vector<uint32_t>	out = sliding_window(current_num_blocks);
		for (uint32_t s = 0; s < m_params.sink_blocks
				&& s < current_num_blocks; s++)
			if (std::find(out.begin(), out.end(), s) == out.end())
				out.push_back(s);
		return (out);
	}
	if (p == policy_t::TOPK_LAG1)
	{
		if (step == 0 || m_last_ground_truth.empty())
			return (sliding_window(current_num_blocks));
		return (m_last_ground_truth);
	}
	if (p == policy_t::HEAVY_HITTER)
	{
		if (m_heavy_score.empty())
			return (sliding_window(current_num_blocks));
		return (top_heavy_hitters(step, m_params.heavy_hitter_top_n));
	}
	if (p == policy_t::RECENCY_FREQUENCY_HYBRID)
	{
		/* Deduplicated: a block that is both in the recency window
		 * AND among the top frequent blocks must appear only once,
		 * or precision/recall/working-set-size all get inflated by
		 * double-counting (a real bug caught during this phase's own
		 * development -- an earlier version returned a plain
		 * concatenation and recall came out above 1.0). */
		std::set<uint32_t>	out;
		for (uint32_t b : sliding_window(current_num_blocks))
			out.insert(b);
		for (uint32_t b : top_frequent(m_params.hybrid_freq_top_n))
			out.insert(b);
		return (std::vector<uint32_t>(out.begin(), out.end()));
	}
	/* MEMBRANE_PREDICTIVE: recency + persistent heavy-hitters + last
	 * step's real access + a fixed attention-sink block, capped at a
	 * budget derived from recently observed working-set sizes -- the
	 * "basic, explainable" first predictor the spec asks for. */
	std::set<uint32_t>	pool;
	for (uint32_t b : sliding_window(current_num_blocks))
		pool.insert(b);
	for (uint32_t b : top_heavy_hitters(step, m_params.heavy_hitter_top_n))
		pool.insert(b);
	for (uint32_t b : m_last_ground_truth)
		pool.insert(b);
	for (uint32_t s = 0; s < m_params.sink_blocks && s < current_num_blocks;
			s++)
		pool.insert(s);
	uint32_t	budget = predicted_budget();
	if (budget == 0 || budget >= pool.size())
		return (std::vector<uint32_t>(pool.begin(), pool.end()));
	/* Rank the pool: recency (closer to current = higher), heavy
	 * score, and last-step membership all contribute; ties favor
	 * more recent blocks. */
	std::vector<std::pair<double, uint32_t>>	scored;
	for (uint32_t b : pool)
	{
		double	recency_component = (double)b / (current_num_blocks
			? current_num_blocks : 1);
		double	heavy_component = 0.0;
		auto	hit = m_heavy_score.find(b);
		if (hit != m_heavy_score.end())
		{
			uint32_t	last = m_heavy_last_touch.at(b);
			heavy_component = hit->second
				* std::pow(m_params.heavy_hitter_decay,
					(double)(step - last));
		}
		double	lag1_component = 0.0;
		if (std::find(m_last_ground_truth.begin(), m_last_ground_truth.end(),
				b) != m_last_ground_truth.end())
			lag1_component = 1.0;
		double	sink_component = (b < m_params.sink_blocks) ? 1.0 : 0.0;
		scored.emplace_back(0.4 * recency_component + 0.3 * heavy_component
			+ 0.2 * lag1_component + 0.1 * sink_component, b);
	}
	std::sort(scored.begin(), scored.end(),
		[](const auto &a, const auto &b) { return (a.first > b.first); });
	std::vector<uint32_t>	out;
	for (uint32_t i = 0; i < budget && i < scored.size(); i++)
		out.push_back(scored[i].second);
	return (out);
}

/* Evicts the single lowest-effective-score tracked block once the map
 * exceeds max_tracked_blocks -- a real, standard bounded heavy-hitter
 * technique (see policy_params_t::max_tracked_blocks's doc comment),
 * not merely a size cap: a block evicted here is, by construction, one
 * this channel has observed comparatively rarely/long ago, so losing
 * its exact count is a small, disclosed approximation of true
 * frequency for blocks that were never serious contenders anyway. */
void	channel_predictor_t::prune_if_over_cap(uint32_t step)
{
	if (m_heavy_score.size() <= m_params.max_tracked_blocks)
		return ;
	uint32_t	worst_block = 0;
	double		worst_score = 0.0;
	bool		have = false;
	for (const auto &kv : m_heavy_score)
	{
		uint32_t	last = m_heavy_last_touch.at(kv.first);
		double		eff = kv.second
			* std::pow(m_params.heavy_hitter_decay, (double)(step - last));
		if (!have || eff < worst_score)
		{
			worst_score = eff;
			worst_block = kv.first;
			have = true;
		}
	}
	if (have)
	{
		m_heavy_score.erase(worst_block);
		m_heavy_last_touch.erase(worst_block);
		m_freq_count.erase(worst_block);
	}
}

void	channel_predictor_t::observe(uint32_t step,
		const std::vector<uint32_t> &ground_truth_blocks)
{
	for (uint32_t b : ground_truth_blocks)
	{
		m_freq_count[b]++;
		double	prev = 0.0;
		auto	hit = m_heavy_score.find(b);
		if (hit != m_heavy_score.end())
		{
			uint32_t	last = m_heavy_last_touch[b];
			prev = hit->second
				* std::pow(m_params.heavy_hitter_decay,
					(double)(step - last));
		}
		m_heavy_score[b] = prev + 1.0;
		m_heavy_last_touch[b] = step;
		prune_if_over_cap(step);
	}
	m_last_ground_truth = ground_truth_blocks;
	m_recent_sizes.push_back((uint32_t)ground_truth_blocks.size());
	if (m_recent_sizes.size() > 16)
		m_recent_sizes.erase(m_recent_sizes.begin());
}

}	/* namespace wssim */
