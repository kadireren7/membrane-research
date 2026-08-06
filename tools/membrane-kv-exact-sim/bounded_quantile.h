#ifndef MEMBRANE_EXACTSIM_BOUNDED_QUANTILE_H
#define MEMBRANE_EXACTSIM_BOUNDED_QUANTILE_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace exactsim
{

/*
 * Phase 6.5 item 5/13: exact_engine.cpp's run_concurrent() used to
 * collect EVERY completed step's latency into seq_latencies (a
 * std::vector<std::vector<double>>, one inner vector per sequence)
 * then flatten into all_latencies just to sort it once for p50/p95/
 * p99 -- at the full unified 128K x 512 scale that is up to ~66.8M
 * doubles (~536 MiB), doubled transiently by the flatten step, with
 * NO relationship to any --memory-budget-mib the rest of the sweep
 * respects.
 *
 * This accumulator keeps the exact same percentile computation (still
 * an exact order-statistic selection, never an approximation) but
 * bounds peak memory: samples are held in RAM up to `in_memory_cap`;
 * beyond that, further samples spill to a temp file and the final
 * percentiles are computed via an external (file-based) multi-target
 * quickselect (median-of-sample pivot, equal-bucket short-circuit --
 * the common "every step pinned to the model's compute floor" shape
 * documented in docs/phase6-unified-stress.md resolves in the very
 * first round via that short-circuit, not by ever materializing the
 * dominant bucket). Below the cap, behavior is byte-for-byte the
 * original in-RAM sort -- see test_bounded_quantile.cpp's parity
 * tests against a plain std::sort reference.
 */
class bounded_quantile_accumulator_t
{
public:
	static constexpr size_t	kDefaultInMemoryCap = 2 * 1000 * 1000;

	explicit bounded_quantile_accumulator_t(
		size_t in_memory_cap = kDefaultInMemoryCap);
	~bounded_quantile_accumulator_t();

	bounded_quantile_accumulator_t(const bounded_quantile_accumulator_t &)
		= delete;
	bounded_quantile_accumulator_t	&operator=(
		const bounded_quantile_accumulator_t &) = delete;

	void	add(double v);

	uint64_t	count() const { return (m_count); }
	bool		spilled() const { return (m_spilling); }

	/* Exact p50/p95/p99 (same "idx = floor(p * (n-1))" convention the
	 * pre-6.5 in-RAM code used, so values match exactly for anything
	 * that stays under the in-memory cap). Consumes accumulated state
	 * (temp files are cleaned up); safe to call at most once -- a
	 * second call is a defensive no-op (returns all zero) rather than
	 * touching the already-closed spill file. Returns all zero if
	 * count() == 0. */
	void	finish(double *p50, double *p95, double *p99);

private:
	size_t				m_in_memory_cap;
	std::vector<double>	m_mem;
	bool				m_spilling = false;
	bool				m_finished = false;
	FILE				*m_spill_file = nullptr;
	std::string			m_spill_path;
	uint64_t			m_count = 0;

	void	begin_spill();
};

}	/* namespace exactsim */

#endif
