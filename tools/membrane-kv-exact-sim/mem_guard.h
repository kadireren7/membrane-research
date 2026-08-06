#ifndef MEMBRANE_EXACTSIM_MEM_GUARD_H
#define MEMBRANE_EXACTSIM_MEM_GUARD_H

#include <atomic>
#include <cstdint>
#include <memory>

#include "attn_trace_reader.h"

namespace exactsim
{

/*
 * Phase 6.5 items 9/10: real (not simulated) OS memory pressure
 * monitoring, driving real backpressure -- reads /proc/self/status
 * (VmRSS), /proc/meminfo (MemAvailable, SwapTotal/SwapFree), and
 * /proc/self/stat (majflt) directly; no estimation. Linux-only (the
 * whole point is real numbers from the real kernel this sweep runs
 * under, not a portable abstraction over hypothetical other OSes this
 * project has never run on).
 */
struct mem_sample_t
{
	uint64_t	rss_kb = 0;
	uint64_t	mem_available_kb = 0;
	uint64_t	mem_total_kb = 0;
	uint64_t	swap_total_kb = 0;
	uint64_t	swap_free_kb = 0;
	uint64_t	majflt = 0;
	bool		ok = false;
};

mem_sample_t	sample_process_memory();

enum class mem_action_t
{
	NONE,			/* comfortably under budget */
	SHRINK_CACHE,	/* approaching budget -- shrink the shared chunk
					 * cache's target size */
	REDUCE_WORKERS,	/* over budget -- lower the active worker slot
					 * limit */
	CHECKPOINT_AND_EXIT,	/* critical -- stop claiming new scenarios,
					 * force a checkpoint flush, and exit cleanly
					 * BEFORE the kernel OOM-killer would, so resume
					 * picks up from a clean, real checkpoint instead
					 * of an arbitrary kill point */
};

/*
 * Exit code used when mem_guard_t decides to exit deliberately under
 * memory pressure -- distinct from a normal 0 (all scenarios done)
 * or a crash, so an external supervisor (or a human reading the log)
 * can tell "ran out of budget, safe to just re-invoke and resume"
 * apart from a real failure.
 */
constexpr int	kMemGuardExitCode = 3;

class mem_guard_t
{
public:
	mem_guard_t(uint64_t budget_mib, uint32_t initial_workers,
		uint32_t min_workers);

	/* Call periodically (main.cpp calls this on every scenario
	 * completion, cheap -- two /proc reads take microseconds) with
	 * the shared chunk cache(s) currently in play (may be null before
	 * a model's trace has been opened) so SHRINK_CACHE can act
	 * immediately. Returns the action decided this tick (NONE most of
	 * the time) and updates internal state (active_worker_limit(),
	 * should_exit()) accordingly -- callers don't need to re-derive
	 * anything from the returned action besides logging it. */
	mem_action_t	tick(
		const std::shared_ptr<wssim::attn_trace_chunk_cache_t> &cache);

	uint32_t	active_worker_limit() const
	{
		return (m_active_worker_limit.load());
	}

	bool	should_exit() const { return (m_should_exit.load()); }

	mem_sample_t	last_sample() const { return (m_last); }

private:
	uint64_t				m_budget_kib;
	uint32_t				m_min_workers;
	std::atomic<uint32_t>	m_active_worker_limit;
	std::atomic<bool>		m_should_exit{false};
	mem_sample_t			m_last;
};

}	/* namespace exactsim */

#endif
