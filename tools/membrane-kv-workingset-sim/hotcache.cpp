#include "hotcache.h"

namespace wssim
{

const char	*eviction_policy_name(eviction_policy_t p)
{
	switch (p)
	{
	case eviction_policy_t::LRU: return ("lru");
	case eviction_policy_t::LFU: return ("lfu");
	case eviction_policy_t::ATTENTION_SCORE_AWARE: return ("attention-score-aware");
	case eviction_policy_t::SEGMENTED_LRU: return ("segmented-lru");
	default: return ("unknown");
	}
}

hot_cache_t::hot_cache_t(uint64_t capacity_bytes, eviction_policy_t policy)
	: m_capacity(capacity_bytes), m_policy(policy), m_bytes_used(0),
	  m_seq(0), m_protected_bytes_used(0)
{
}

bool	hot_cache_t::contains(const cache_key_t &k) const
{
	return (m_entries.find(k) != m_entries.end());
}

double	hot_cache_t::metric_for(const entry_t &e) const
{
	if (m_policy == eviction_policy_t::LFU)
		return ((double)e.freq);
	if (m_policy == eviction_policy_t::ATTENTION_SCORE_AWARE)
		return (e.score);
	/* LRU and SEGMENTED_LRU both rank by recency (a monotonic
	 * sequence number stashed in `score` by touch_hit/insert below);
	 * segmented additionally always evicts probationary entries
	 * before any protected one (large additive offset). */
	double	recency = e.score;
	if (m_policy == eviction_policy_t::SEGMENTED_LRU && e.protected_segment)
		return (recency + 1e18);
	return (recency);
}

void	hot_cache_t::reindex(const cache_key_t &k, entry_t &e)
{
	m_order.erase(e.order_it);
	e.order_it = m_order.emplace(metric_for(e), k);
}

void	hot_cache_t::evict_until_fits(uint64_t incoming_bytes)
{
	while (m_bytes_used + incoming_bytes > m_capacity && !m_order.empty())
	{
		auto	it = m_order.begin();
		const cache_key_t	victim = it->second;
		auto	eit = m_entries.find(victim);
		if (eit != m_entries.end())
		{
			m_bytes_used -= eit->second.bytes;
			if (eit->second.protected_segment)
				m_protected_bytes_used -= eit->second.bytes;
			m_entries.erase(eit);
		}
		m_order.erase(it);
	}
}

void	hot_cache_t::touch_hit(const cache_key_t &k, double score)
{
	auto	it = m_entries.find(k);
	if (it == m_entries.end())
		return ;
	it->second.freq++;
	m_seq++;
	if (m_policy == eviction_policy_t::ATTENTION_SCORE_AWARE)
		it->second.score = score;
	else
		it->second.score = (double)m_seq;
	if (m_policy == eviction_policy_t::SEGMENTED_LRU
			&& !it->second.protected_segment)
	{
		uint64_t	protected_cap = (m_capacity * 4) / 5;	/* 80/20 split */
		if (m_protected_bytes_used + it->second.bytes <= protected_cap)
		{
			it->second.protected_segment = true;
			m_protected_bytes_used += it->second.bytes;
		}
	}
	reindex(k, it->second);
}

uint64_t	hot_cache_t::insert(const cache_key_t &k, uint64_t bytes,
					double score)
{
	if (m_entries.find(k) != m_entries.end())
	{
		touch_hit(k, score);
		return (0);
	}
	uint64_t	before = m_bytes_used;
	evict_until_fits(bytes);
	uint64_t	evicted = 0;
	if (m_bytes_used < before)
		evicted = before - m_bytes_used;
	m_seq++;
	if (bytes <= m_capacity)
	{
		entry_t	e;
		e.bytes = bytes;
		e.freq = 1;
		e.score = (m_policy == eviction_policy_t::ATTENTION_SCORE_AWARE)
			? score : (double)m_seq;
		e.protected_segment = false;
		e.order_it = m_order.emplace(metric_for(e), k);
		m_entries.emplace(k, e);
		m_bytes_used += bytes;
	}
	return (evicted);
}

}	/* namespace wssim */
