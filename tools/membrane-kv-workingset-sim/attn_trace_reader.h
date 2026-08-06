#ifndef MEMBRANE_WSSIM_ATTN_TRACE_READER_H
#define MEMBRANE_WSSIM_ATTN_TRACE_READER_H

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "membrane/attntrace3.h"

namespace wssim
{

/*
 * Phase 6.5: out-of-core streaming access to a .attntrace3 file.
 *
 * The one consumer that matters (engine.cpp's run_scenario_calibration,
 * see the calibrate-once loop) walks steps strictly sequentially,
 * 0..step_count-1, needing every layer/head/top_k entry of the CURRENT
 * step only -- never revisits an earlier step. Every backend here is
 * built around that access pattern: at most a small, bounded number of
 * decoded chunks are ever resident at once, never the whole trace.
 */
struct trace_metadata_t
{
	std::string	model;
	bool		is_real_capture;
	uint32_t	n_layer;
	uint32_t	n_head;
	uint32_t	block_size_tokens;
	uint32_t	prompt_len;
	uint32_t	step_count;
	uint32_t	top_k;
};

/*
 * Bounded (by byte budget) LRU cache of DECODED chunks (each a flat
 * membrane_attntrace_entry_t vector), shared across every worker
 * thread reading the same model's trace this process -- sizing it
 * per-thread instead would multiply the memory budget by worker
 * count, defeating the whole point (see docs/phase6-out-of-core-
 * simulator.md section on the chunk cache). Refcounted pin/unpin
 * (`acquire`/`release`) keeps an in-use chunk from being evicted out
 * from under a caller; a "loading" placeholder plus a condition
 * variable makes two threads racing to decode the SAME missing chunk
 * collapse into one decode (no duplicate work, no duplicate memory).
 */
class attn_trace_chunk_cache_t
{
public:
	struct stats_t
	{
		uint64_t	hits = 0;
		uint64_t	misses = 0;
		uint64_t	evictions = 0;
		uint64_t	duplicate_loads_avoided = 0;
		size_t		bytes_resident = 0;
		size_t		budget_bytes = 0;
	};

	explicit attn_trace_chunk_cache_t(size_t budget_bytes);

	/*
	 * Returns a pointer to chunk_id's decoded entries, pinning it
	 * (refcount++) so it cannot be evicted until a matching release().
	 * On a cache miss, `loader` is called (WITHOUT the cache lock
	 * held, so other chunks stay usable while this one decodes) to
	 * produce the entries. Safe to call concurrently from multiple
	 * threads, including for the same chunk_id.
	 */
	const std::vector<membrane_attntrace_entry_t>	*acquire(
		uint32_t chunk_id,
		const std::function<std::vector<membrane_attntrace_entry_t>()>
			&loader);

	/* Unpins one reference previously taken by acquire(). */
	void	release(uint32_t chunk_id);

	/* Shrinks the target budget (used by mem_guard under memory
	 * pressure) and immediately evicts unpinned entries to fit, best
	 * effort -- pinned entries are never evicted regardless of
	 * budget, so shrinking below the currently-pinned working set
	 * only takes effect as those pins are released. */
	void	set_budget_bytes(size_t n);

	stats_t	stats() const;

private:
	struct slot_t
	{
		std::vector<membrane_attntrace_entry_t>	data;
		int											refcount = 0;
		bool										loading = false;
		std::list<uint32_t>::iterator				lru_it;
		bool										in_lru = false;
	};

	mutable std::mutex					m_mutex;
	std::condition_variable			m_cv;
	std::unordered_map<uint32_t, slot_t>	m_slots;
	std::list<uint32_t>					m_lru;	/* front = most recently used */
	size_t								m_budget_bytes;
	size_t								m_resident_bytes = 0;
	stats_t								m_stats;

	void	touch_lru_locked(uint32_t chunk_id, slot_t &slot);
	void	evict_to_budget_locked();
};

/*
 * open/get_metadata/at/prefetch_chunk/release_chunk/close -- the
 * interface every backend (in-memory, mmap, buffered-streaming)
 * implements identically, so the same calibration code (see engine.h/
 * engine.cpp's templated run_scenario_calibration_impl) runs against
 * any of them.
 *
 * `at()` returns a pointer to `top_k` entries for (step, layer, head),
 * valid until the NEXT call to at() for a different chunk (backends
 * auto-manage a single "current sequential chunk" pin internally) --
 * exactly the lifetime the strictly-sequential calibration loop needs
 * and nothing more, so no backend has to keep more than one chunk's
 * worth of decoded entries resident for that path alone.
 * `prefetch_chunk`/`release_chunk` are a SEPARATE, caller-owned pin
 * (for --prefetch-depth lookahead) independent of that auto-managed
 * pin -- a chunk can be held by both at once, refcounted correctly
 * either way.
 */
class attn_trace_reader_t
{
public:
	virtual	~attn_trace_reader_t() = default;

	virtual bool	open(const std::string &path) = 0;
	virtual const trace_metadata_t	&get_metadata() const = 0;
	virtual const membrane_attntrace_entry_t	*at(uint32_t step,
						uint32_t layer, uint32_t head) = 0;

	virtual void	prefetch_chunk(uint32_t step) { (void)step; }
	virtual void	release_chunk(uint32_t step) { (void)step; }

	/* --prefetch-depth (item 3): number of chunks beyond the current
	 * one to keep warm in the shared cache ahead of need. 0 (default)
	 * is a no-op on every backend -- IN_MEMORY has no chunks to warm,
	 * and the chunked backends only read ahead when explicitly asked. */
	virtual void	set_prefetch_depth(uint32_t depth) { (void)depth; }

	/* [step_lo, step_hi) -- returns a pointer to the first requested
	 * step's entries; the full range is guaranteed contiguous within
	 * one chunk (callers must keep ranges inside chunk_steps, which
	 * every real caller here does -- the calibration loop only ever
	 * asks for a single step at a time). */
	virtual const membrane_attntrace_entry_t	*read_step_range(
						uint32_t step_lo, uint32_t step_hi)
	{
		if (step_hi <= step_lo)
			return (nullptr);
		return (at(step_lo, 0, 0));
	}

	/* Slice of an already-resident step's entries for
	 * [layer_lo, layer_hi) -- pure pointer arithmetic over what at()
	 * already decoded, no extra I/O. */
	virtual const membrane_attntrace_entry_t	*read_layer_range(
						uint32_t step, uint32_t layer_lo, uint32_t layer_hi)
	{
		if (layer_hi <= layer_lo)
			return (nullptr);
		return (at(step, layer_lo, 0));
	}

	virtual void	close() = 0;

	attn_trace_chunk_cache_t::stats_t	cache_stats() const
	{
		return (m_cache != nullptr ? m_cache->stats()
			: attn_trace_chunk_cache_t::stats_t{});
	}

protected:
	std::shared_ptr<attn_trace_chunk_cache_t>	m_cache;
};

enum class trace_backend_t { IN_MEMORY, MMAP, STREAMING };

/*
 * `cache`, if non-null, is SHARED across every reader instance passed
 * it (see the class comment on attn_trace_chunk_cache_t) -- pass the
 * same cache to one reader per worker thread for a given model's
 * trace. If null, IN_MEMORY backend needs none (it wraps an already-
 * fully-resident attn_trace_t); MMAP/STREAMING create a private
 * per-instance cache sized `private_cache_budget_bytes` if the caller
 * doesn't want sharing (mainly for tests).
 */
std::unique_ptr<attn_trace_reader_t>	make_attn_trace_reader(
	trace_backend_t backend,
	std::shared_ptr<attn_trace_chunk_cache_t> cache
		= nullptr,
	size_t private_cache_budget_bytes = 64u << 20);

/* IN_MEMORY backend doesn't read a path at all -- it adapts an
 * already-loaded attn_trace_t (see attn_workload.h), for small/medium
 * traces and as the parity baseline. Exposed directly (not just via
 * the factory + a path-based open()) since there's no file to open. */
struct attn_trace_t;
std::unique_ptr<attn_trace_reader_t>	make_in_memory_reader(
	const attn_trace_t &trace);

}	/* namespace wssim */

#endif
