#ifndef MEMBRANE_CXL_SIM_WORKLOAD_H
#define MEMBRANE_CXL_SIM_WORKLOAD_H

#include <cstdint>
#include <string>
#include <vector>

namespace sim
{

/*
 * One sequence's decode-phase KV access trace: prompt_len tokens
 * already resident before step 0, then step_bytes.size() decode steps,
 * each growing the sequence's KV footprint by step_bytes[i] real (or
 * realistically-extrapolated) bytes. compute_ns_per_step is the real-
 * measured, model-derived compute-bound per-token decode time (see
 * sim_config.h's SMOLLM2_*_TOK_PER_SEC) -- the time this step would
 * take if the KV memory subsystem were never the bottleneck.
 */
struct sequence_trace_t
{
	uint32_t				prompt_len;
	std::vector<uint32_t>	step_bytes;
	double					compute_ns_per_step;
	std::string				source_model;
	bool						is_real_capture;
};

/* Loads a real .kvtrace file (membrane/kvtrace.h format, written by
 * membrane-kv-trace-capture). compute_ns_per_step must be supplied by
 * the caller (the trace file itself doesn't carry timing, only byte
 * growth) -- pass one of the SMOLLM2_*_TOK_PER_SEC-derived constants. */
bool	load_real_trace(const std::string &path, double compute_ns_per_step,
			sequence_trace_t *out);

/*
 * Builds a synthetic trace reaching `target_context_len` tokens total
 * (prompt_len + step_count == target_context_len), by extrapolating
 * `base`'s own real-measured per-step byte rate (mean of base's
 * step_bytes) rather than inventing a new one -- small deterministic
 * jitter (+/-3%, seeded) is added per step so concurrent sequences in
 * a sweep are not bit-identical clones of each other, which would
 * hide queueing effects a real multi-tenant workload would show.
 * `seed` must differ per sequence in a concurrency sweep for that
 * jitter to actually vary sequence-to-sequence.
 */
sequence_trace_t	make_synthetic_trace(const sequence_trace_t &base,
						uint32_t target_context_len, uint32_t seed);

/* Generates `concurrency` independent synthetic sequences at
 * `context_len`, all derived from `base`'s real-measured rate, each
 * with a distinct seed (seed_base + index) so their jitter differs. */
std::vector<sequence_trace_t>	generate_workload(const sequence_trace_t &base,
									uint32_t concurrency, uint32_t context_len,
									uint32_t seed_base);

}	/* namespace sim */

#endif
