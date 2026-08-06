#define _DEFAULT_SOURCE

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "bounded_quantile.h"

namespace exactsim
{

namespace
{

constexpr size_t	kPivotSampleCap = 4096;
constexpr size_t	kIoBufDoubles = 8192;

std::string	make_temp_path()
{
	char	tmpl[] = "/tmp/membrane-quantile-XXXXXX";
	int		fd = mkstemp(tmpl);

	if (fd < 0)
		return (std::string());
	close(fd);
	return (std::string(tmpl));
}

/* Reads an evenly-strided sample of up to kPivotSampleCap values from
 * `path` (which holds `count` raw little-endian-native doubles) and
 * returns their median -- a representative pivot without ever
 * requiring the whole file resident, and without assuming any
 * particular distribution shape (stride sampling, not "first N",
 * avoids pathological locality in how the simulation happened to
 * order events). */
double	pick_pivot(const std::string &path, uint64_t count)
{
	FILE				*f = fopen(path.c_str(), "rb");
	uint64_t			sample_n = std::min<uint64_t>(count, kPivotSampleCap);
	uint64_t			stride = count / sample_n;
	std::vector<double>	sample;

	if (f == nullptr)
		throw std::runtime_error(
			"bounded_quantile: failed to open spill file for pivot sampling: "
			+ path);
	sample.reserve((size_t)sample_n);
	if (stride == 0)
		stride = 1;
	for (uint64_t i = 0; i < sample_n; i++)
	{
		double	v;

		if (fseek(f, (long)(i * stride * sizeof(double)), SEEK_SET) != 0)
			break ;
		if (fread(&v, sizeof(v), 1, f) != 1)
			break ;
		sample.push_back(v);
	}
	fclose(f);
	if (sample.empty())
		throw std::runtime_error(
			"bounded_quantile: could not read any sample for pivot "
			"selection from: " + path);
	std::sort(sample.begin(), sample.end());
	return (sample[sample.size() / 2]);
}

struct rank_query_t
{
	size_t		result_index;
	uint64_t	rank;	/* 0-indexed order statistic within the CURRENT
						 * partition */
};

/*
 * One round of external (file-based) multi-target quickselect: reads
 * `path` (exactly `count` raw doubles) once, partitions into two temp
 * files (< pivot, > pivot) plus a pure count of values == pivot
 * (never materialized -- this is what makes the common "almost every
 * sample is the model's compute floor" shape resolve in one round
 * instead of blowing memory or recursing to a useless depth). Each
 * outstanding query is either resolved (its rank lands in the ==
 * bucket -> answer is `pivot`) or handed to the matching recursive
 * call with its rank shifted to be relative to that sub-partition.
 * `path` is always consumed (deleted) by this call.
 */
void	select_round(const std::string &path, uint64_t count,
			std::vector<rank_query_t> queries, std::vector<double> &results)
{
	if (queries.empty())
	{
		unlink(path.c_str());
		return ;
	}
	if (count <= 1)
	{
		FILE	*f = fopen(path.c_str(), "rb");
		double	v = 0.0;

		if (f != nullptr)
		{
			if (fread(&v, sizeof(v), 1, f) != 1)
				v = 0.0;
			fclose(f);
		}
		for (const rank_query_t &q : queries)
			results[q.result_index] = v;
		unlink(path.c_str());
		return ;
	}

	double		pivot = pick_pivot(path, count);
	std::string	lt_path = make_temp_path();
	std::string	gt_path = make_temp_path();
	FILE		*fin = fopen(path.c_str(), "rb");
	FILE		*flt = fopen(lt_path.c_str(), "wb");
	FILE		*fgt = fopen(gt_path.c_str(), "wb");
	uint64_t	n_lt = 0;
	uint64_t	n_eq = 0;
	uint64_t	n_gt = 0;
	/* Heap, not a stack array: this function recurses (twice per
	 * call), and an 8192-double (64 KiB) stack frame repeated across
	 * enough unbalanced-pivot levels could exhaust a thread's stack --
	 * moving the one large buffer off the stack removes that risk
	 * regardless of how skewed the input distribution is. */
	std::vector<double>	buf(kIoBufDoubles);
	size_t		got;

	if (fin == nullptr || flt == nullptr || fgt == nullptr)
	{
		if (fin != nullptr)
			fclose(fin);
		if (flt != nullptr)
			fclose(flt);
		if (fgt != nullptr)
			fclose(fgt);
		unlink(path.c_str());
		unlink(lt_path.c_str());
		unlink(gt_path.c_str());
		throw std::runtime_error(
			"bounded_quantile: failed to open a temp file during "
			"external quickselect (disk full or fd exhaustion?)");
	}
	while ((got = fread(buf.data(), sizeof(double), kIoBufDoubles, fin)) > 0)
	{
		for (size_t i = 0; i < got; i++)
		{
			if (buf[i] < pivot)
			{
				fwrite(&buf[i], sizeof(double), 1, flt);
				n_lt++;
			}
			else if (buf[i] > pivot)
			{
				fwrite(&buf[i], sizeof(double), 1, fgt);
				n_gt++;
			}
			else
				n_eq++;
		}
	}
	fclose(fin);
	fclose(flt);
	fclose(fgt);
	unlink(path.c_str());

	std::vector<rank_query_t>	lt_queries;
	std::vector<rank_query_t>	gt_queries;

	for (const rank_query_t &q : queries)
	{
		if (q.rank < n_lt)
			lt_queries.push_back(q);
		else if (q.rank < n_lt + n_eq)
			results[q.result_index] = pivot;
		else
			gt_queries.push_back({q.result_index, q.rank - n_lt - n_eq});
	}
	select_round(lt_path, n_lt, std::move(lt_queries), results);
	select_round(gt_path, n_gt, std::move(gt_queries), results);
}

uint64_t	rank_for_percentile(double p, uint64_t n)
{
	return ((uint64_t)(p * (double)(n - 1)));
}

}	/* anonymous namespace */

bounded_quantile_accumulator_t::bounded_quantile_accumulator_t(
	size_t in_memory_cap)
	: m_in_memory_cap(in_memory_cap)
{
}

bounded_quantile_accumulator_t::~bounded_quantile_accumulator_t()
{
	if (m_spill_file != nullptr)
		fclose(m_spill_file);
	if (!m_spill_path.empty())
		unlink(m_spill_path.c_str());
}

void	bounded_quantile_accumulator_t::begin_spill()
{
	m_spill_path = make_temp_path();
	m_spill_file = fopen(m_spill_path.c_str(), "wb");
	if (!m_mem.empty())
	{
		fwrite(m_mem.data(), sizeof(double), m_mem.size(), m_spill_file);
		m_mem.clear();
		m_mem.shrink_to_fit();
	}
	m_spilling = true;
}

void	bounded_quantile_accumulator_t::add(double v)
{
	m_count++;
	if (!m_spilling && m_mem.size() >= m_in_memory_cap)
		begin_spill();
	if (m_spilling)
		fwrite(&v, sizeof(v), 1, m_spill_file);
	else
		m_mem.push_back(v);
}

void	bounded_quantile_accumulator_t::finish(double *p50, double *p95,
				double *p99)
{
	*p50 = 0.0;
	*p95 = 0.0;
	*p99 = 0.0;
	if (m_finished || m_count == 0)
		return ;
	m_finished = true;
	if (!m_spilling)
	{
		std::vector<double>	sorted = m_mem;

		std::sort(sorted.begin(), sorted.end());
		*p50 = sorted[rank_for_percentile(0.50, m_count)];
		*p95 = sorted[rank_for_percentile(0.95, m_count)];
		*p99 = sorted[rank_for_percentile(0.99, m_count)];
		return ;
	}
	fflush(m_spill_file);
	fclose(m_spill_file);
	m_spill_file = nullptr;

	std::vector<double>			results(3, 0.0);
	std::vector<rank_query_t>		queries = {
		{0, rank_for_percentile(0.50, m_count)},
		{1, rank_for_percentile(0.95, m_count)},
		{2, rank_for_percentile(0.99, m_count)},
	};

	select_round(m_spill_path, m_count, std::move(queries), results);
	m_spill_path.clear();	/* select_round already unlinked it */
	*p50 = results[0];
	*p95 = results[1];
	*p99 = results[2];
}

}	/* namespace exactsim */
