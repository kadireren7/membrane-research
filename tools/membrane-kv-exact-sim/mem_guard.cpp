#define _DEFAULT_SOURCE

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mem_guard.h"

namespace exactsim
{

namespace
{

bool	read_kb_field(FILE *f, const char *prefix, uint64_t *out)
{
	char	line[256];
	size_t	plen = strlen(prefix);

	rewind(f);
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		if (strncmp(line, prefix, plen) == 0)
		{
			unsigned long long	v = 0;

			if (sscanf(line + plen, " %llu", &v) == 1)
			{
				*out = (uint64_t)v;
				return (true);
			}
			return (false);
		}
	}
	return (false);
}

uint64_t	read_majflt(void)
{
	FILE	*f = fopen("/proc/self/stat", "r");
	char	buf[4096];
	size_t	n;

	if (f == nullptr)
		return (0);
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	if (n == 0)
		return (0);
	buf[n] = '\0';

	char	*paren = strrchr(buf, ')');
	if (paren == nullptr)
		return (0);
	/* Fields after "comm)", 0-indexed: state=0 ppid=1 pgrp=2 session=3
	 * tty_nr=4 tpgid=5 flags=6 minflt=7 cminflt=8 majflt=9. */
	char		*cursor = paren + 1;
	int			field = 0;
	unsigned long long	majflt = 0;
	char		tok[64];
	int			consumed;

	while (field <= 9 && sscanf(cursor, "%63s%n", tok, &consumed) == 1)
	{
		if (field == 9)
		{
			majflt = strtoull(tok, nullptr, 10);
			break ;
		}
		cursor += consumed;
		field++;
	}
	return ((uint64_t)majflt);
}

}	/* anonymous namespace */

mem_sample_t	sample_process_memory(void)
{
	mem_sample_t	s;
	FILE			*status = fopen("/proc/self/status", "r");
	FILE			*meminfo = fopen("/proc/meminfo", "r");

	if (status != nullptr)
	{
		s.ok = read_kb_field(status, "VmRSS:", &s.rss_kb) || s.ok;
		fclose(status);
	}
	if (meminfo != nullptr)
	{
		s.ok = read_kb_field(meminfo, "MemTotal:", &s.mem_total_kb) || s.ok;
		s.ok = read_kb_field(meminfo, "MemAvailable:", &s.mem_available_kb)
			|| s.ok;
		s.ok = read_kb_field(meminfo, "SwapTotal:", &s.swap_total_kb) || s.ok;
		s.ok = read_kb_field(meminfo, "SwapFree:", &s.swap_free_kb) || s.ok;
		fclose(meminfo);
	}
	s.majflt = read_majflt();
	return (s);
}

mem_guard_t::mem_guard_t(uint64_t budget_mib, uint32_t initial_workers,
	uint32_t min_workers)
	: m_budget_kib(budget_mib * 1024u), m_min_workers(min_workers),
	m_active_worker_limit(initial_workers)
{
}

mem_action_t	mem_guard_t::tick(
	const std::shared_ptr<wssim::attn_trace_chunk_cache_t> &cache)
{
	m_last = sample_process_memory();
	if (m_budget_kib == 0 || !m_last.ok)
		return (mem_action_t::NONE);	/* no budget declared, or /proc
						 * unreadable (non-Linux) -- don't
						 * guess, don't act. */

	double	rss_ratio = (double)m_last.rss_kb / (double)m_budget_kib;
	double	swap_ratio = m_last.swap_total_kb > 0
		? (double)(m_last.swap_total_kb - m_last.swap_free_kb)
			/ (double)m_last.swap_total_kb
		: 0.0;
	/* MemAvailable below ~128 MiB system-wide is a real, immediate
	 * kernel-OOM risk regardless of how comfortably this process
	 * looks under ITS OWN budget -- other processes/the page cache
	 * eviction pressure this machine already showed (see
	 * docs/phase6-unified-stress.md section 0) can still trigger a
	 * kill. */
	bool	system_critical = m_last.mem_available_kb > 0
		&& m_last.mem_available_kb < (128u * 1024u);

	mem_action_t	action = mem_action_t::NONE;

	if (rss_ratio > 1.05 || system_critical || swap_ratio > 0.85)
		action = mem_action_t::CHECKPOINT_AND_EXIT;
	else if (rss_ratio > 0.90 || swap_ratio > 0.60)
		action = mem_action_t::REDUCE_WORKERS;
	else if (rss_ratio > 0.75)
		action = mem_action_t::SHRINK_CACHE;

	if (action == mem_action_t::CHECKPOINT_AND_EXIT)
		m_should_exit.store(true);
	else if (action == mem_action_t::REDUCE_WORKERS)
	{
		uint32_t	cur = m_active_worker_limit.load();

		if (cur > m_min_workers)
			m_active_worker_limit.store(cur - 1);
	}
	if ((action == mem_action_t::SHRINK_CACHE
			|| action == mem_action_t::REDUCE_WORKERS
			|| action == mem_action_t::CHECKPOINT_AND_EXIT)
			&& cache != nullptr)
	{
		/* Idempotent target (not a repeated halving) -- reserve
		 * ~30% of the overall budget for the chunk cache, the rest
		 * for per-worker transients (bounded_quantile_accumulator_t's
		 * in-memory cap, per-sequence bookkeeping, etc). Escalating
		 * pressure is handled by reducing workers/exiting, not by
		 * shrinking the cache toward zero. */
		size_t	target = (size_t)(m_budget_kib * 1024ull * 0.30);

		if (target < (16u << 20))
			target = 16u << 20;
		cache->set_budget_bytes(target);
	}
	return (action);
}

}	/* namespace exactsim */
