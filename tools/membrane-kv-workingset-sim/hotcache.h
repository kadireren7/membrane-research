#ifndef MEMBRANE_WSSIM_HOTCACHE_H
#define MEMBRANE_WSSIM_HOTCACHE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>

namespace wssim
{

enum class eviction_policy_t
{
	LRU = 0,
	LFU,
	ATTENTION_SCORE_AWARE,
	SEGMENTED_LRU,
	COUNT
};

const char	*eviction_policy_name(eviction_policy_t p);

struct cache_key_t
{
	uint32_t	layer;
	uint32_t	kv_group;
	uint32_t	block_id;

	bool	operator==(const cache_key_t &o) const
	{
		return (layer == o.layer && kv_group == o.kv_group
			&& block_id == o.block_id);
	}
};

struct cache_key_hash_t
{
	size_t	operator()(const cache_key_t &k) const
	{
		size_t	h = (size_t)k.layer;
		h = h * 1000003u ^ (size_t)k.kv_group;
		h = h * 1000003u ^ (size_t)k.block_id;
		return (h);
	}
};

/*
 * Host/GPU-resident decompressed hot cache -- component 6 of the
 * near-memory pipeline (docs/phase6-cxl-near-memory.md section 10).
 * Byte-budgeted (`capacity_bytes`), one of four eviction policies.
 *
 * O(log n) hit/insert/evict: an ordered index (`m_order`, a
 * std::multimap keyed by each policy's own eviction-priority metric,
 * ascending = evicted first) is maintained INCREMENTALLY -- each
 * entry stores the iterator into m_order that currently represents
 * it, so a touch/insert erases and reinserts that one iterator
 * (O(log n)) instead of rebuilding the whole ordering from scratch.
 * This replaced an earlier full-rebuild-per-eviction-call design
 * (O(n log n) per call): correct at Phase 6.2's small real-trace
 * scale (a few hundred cache entries at most, capacity pressure
 * rarely even triggered eviction), but a real, measured tractability
 * blocker at Phase 6.3's larger contexts (128K tokens can genuinely
 * fill a 256MiB-class cache, unlike Phase 6.2's ~640-token real
 * traces) -- caught by direct timing measurement during this phase's
 * own development, not by inspection.
 */
class hot_cache_t
{
public:
	hot_cache_t(uint64_t capacity_bytes, eviction_policy_t policy);

	bool	contains(const cache_key_t &k) const;

	/* Marks a hit: updates recency/frequency/score bookkeeping for
	 * `k` (already resident). `score` is that block's real attention
	 * mass this step (used only by ATTENTION_SCORE_AWARE). */
	void	touch_hit(const cache_key_t &k, double score);

	/* Inserts a new (not-yet-resident) block, evicting victims by
	 * the configured policy until there is room. Returns the number
	 * of bytes evicted to make room (0 if it fit without eviction). */
	uint64_t	insert(const cache_key_t &k, uint64_t bytes, double score);

	uint64_t	bytes_used() const { return (m_bytes_used); }
	size_t		entry_count() const { return (m_entries.size()); }
	uint64_t	capacity_bytes() const { return (m_capacity); }

private:
	struct entry_t
	{
		uint64_t	bytes;
		uint32_t	freq;
		double		score;
		bool		protected_segment;	/* SEGMENTED_LRU only */
		std::multimap<double, cache_key_t>::iterator	order_it;
	};

	uint64_t				m_capacity;
	eviction_policy_t		m_policy;
	uint64_t				m_bytes_used;
	uint64_t				m_seq;
	uint64_t				m_protected_bytes_used;

	std::unordered_map<cache_key_t, entry_t, cache_key_hash_t>	m_entries;
	std::multimap<double, cache_key_t>								m_order;

	double	metric_for(const entry_t &e) const;
	/* Removes `k`'s current position from m_order (if present) and
	 * reinserts at its up-to-date metric, updating entry_t::order_it. */
	void	reindex(const cache_key_t &k, entry_t &e);
	void	evict_until_fits(uint64_t incoming_bytes);
};

}	/* namespace wssim */

#endif
