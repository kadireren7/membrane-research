#include <cstdio>

#include "membrane/kvtrace.h"
#include "workload.h"

namespace sim
{

/* Deterministic xorshift, same algorithm as tests/unit/test_helpers.h's
 * test_rand_next -- reproducible across runs given the same seed. */
static uint32_t	xorshift32(uint32_t *state)
{
	uint32_t	x;

	x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return (x);
}

bool	load_real_trace(const std::string &path, double compute_ns_per_step,
			sequence_trace_t *out)
{
	FILE						*f;
	membrane_kvtrace_header_t	h;

	f = fopen(path.c_str(), "rb");
	if (f == NULL)
		return (false);
	if (membrane_kvtrace_read_header(f, &h) != MEMBRANE_OK)
	{
		fclose(f);
		return (false);
	}
	out->step_bytes.resize(h.step_count);
	if (membrane_kvtrace_read_steps(f, &h, out->step_bytes.data())
			!= MEMBRANE_OK)
	{
		fclose(f);
		return (false);
	}
	fclose(f);
	out->prompt_len = h.prompt_len;
	out->compute_ns_per_step = compute_ns_per_step;
	out->source_model = h.model;
	out->is_real_capture = (h.source == MEMBRANE_KVTRACE_SOURCE_REAL_CAPTURE);
	return (true);
}

sequence_trace_t	make_synthetic_trace(const sequence_trace_t &base,
						uint32_t target_context_len, uint32_t seed)
{
	sequence_trace_t	out;
	uint64_t			sum;
	double				mean_bytes;
	uint32_t			steps_needed;
	uint32_t			state;
	uint32_t			i;
	double				jitter;
	double				v;

	sum = 0;
	for (uint32_t b : base.step_bytes)
		sum += b;
	mean_bytes = base.step_bytes.empty() ? 4096.0
		: (double)sum / (double)base.step_bytes.size();
	out.prompt_len = base.prompt_len;
	out.compute_ns_per_step = base.compute_ns_per_step;
	out.source_model = base.source_model;
	out.is_real_capture = false;
	steps_needed = (target_context_len > base.prompt_len)
		? (target_context_len - base.prompt_len) : 1;
	out.step_bytes.resize(steps_needed);
	state = (seed == 0) ? 1 : seed;
	i = 0;
	while (i < steps_needed)
	{
		/* +/-3% deterministic jitter around the real-measured mean rate. */
		jitter = ((double)(xorshift32(&state) % 601) - 300.0) / 10000.0;
		v = mean_bytes * (1.0 + jitter);
		if (v < 1.0)
			v = 1.0;
		out.step_bytes[i] = (uint32_t)v;
		i++;
	}
	return (out);
}

std::vector<sequence_trace_t>	generate_workload(const sequence_trace_t &base,
									uint32_t concurrency, uint32_t context_len,
									uint32_t seed_base)
{
	std::vector<sequence_trace_t>	out;
	uint32_t						i;

	out.reserve(concurrency);
	i = 0;
	while (i < concurrency)
	{
		out.push_back(make_synthetic_trace(base, context_len,
			seed_base + i * 2654435761u));
		i++;
	}
	return (out);
}

}	/* namespace sim */
