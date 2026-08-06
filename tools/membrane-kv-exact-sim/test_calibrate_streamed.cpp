/*
 * Phase 6.5 item 4: calibrate() (in-memory attn_trace_t) vs.
 * calibrate_streamed() (attn_trace_reader_t -- mmap and buffered-
 * streaming backends) must produce identical calibrated_profile_t
 * output on the same underlying trace content, since both route
 * through the SAME templated run_scenario_calibration_impl body in
 * engine.cpp. Exercises the MEMBRANE_PREDICTIVE policy with layer/head
 * detail and coalescing enabled, since those are the paths that
 * actually walk every entry (not just ground_truth membership).
 */

#define _DEFAULT_SOURCE

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "attn_trace_reader.h"
#include "calibrate.h"

#define TEST_ASSERT(cond, msg) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			abort(); \
		} \
	} while (0)

using namespace exactsim;
using namespace wssim;

static void	fill_trace(attn_trace_t *t, uint32_t step_count,
				uint32_t n_layer, uint32_t n_head, uint32_t top_k)
{
	t->model = "test-model";
	t->is_real_capture = true;
	t->n_layer = n_layer;
	t->n_head = n_head;
	t->block_size_tokens = 32;
	t->prompt_len = 256;
	t->step_count = step_count;
	t->top_k = top_k;
	t->entries.assign((size_t)step_count * n_layer * n_head * top_k,
		membrane_attntrace_entry_t{UINT32_MAX, 0.0f});
	/* A handful of recurring "hot" block ids plus scattered cold ones
	 * -- shaped enough for MEMBRANE_PREDICTIVE's channel_predictor_t
	 * to actually exercise precision/recall/coalescing, not just hit
	 * a trivial all-miss or all-hit path. */
	for (size_t i = 0; i < t->entries.size(); i++)
	{
		if (i % 6 == 0)
			continue ;
		t->entries[i].block_id = (i % 3 == 0) ? (uint32_t)(i % 4)
			: (uint32_t)(200 + i % 50);
		t->entries[i].score = 0.5f;
	}
}

static void	write_v3(const attn_trace_t &t, const char *path,
				uint32_t chunk_steps)
{
	membrane_attntrace3_writer_t	w;
	FILE							*f = fopen(path, "w+b");

	TEST_ASSERT(f != nullptr, "open v3 file for write");
	TEST_ASSERT(membrane_attntrace3_writer_open(f, &w, t.model.c_str(),
		MEMBRANE_ATTNTRACE_SOURCE_REAL_CAPTURE, t.n_layer, t.n_head,
		t.block_size_tokens, t.prompt_len, t.step_count, t.top_k,
		chunk_steps) == MEMBRANE_OK, "writer_open");
	for (uint32_t c = 0; c < w.h.chunk_count; c++)
	{
		uint32_t	clen = membrane_attntrace3_writer_chunk_len(&w, c);
		uint32_t	step_lo = c * chunk_steps;
		std::vector<membrane_attntrace_entry_t>	chunk(
			(size_t)clen * t.n_layer * t.n_head * t.top_k);

		for (uint32_t s = 0; s < clen; s++)
			for (uint32_t l = 0; l < t.n_layer; l++)
				for (uint32_t h = 0; h < t.n_head; h++)
				{
					const membrane_attntrace_entry_t	*src
						= t.at(step_lo + s, l, h);
					size_t	base = (((size_t)s * t.n_layer + l) * t.n_head
							+ h) * t.top_k;
					for (uint32_t k = 0; k < t.top_k; k++)
						chunk[base + k] = src[k];
				}
		TEST_ASSERT(membrane_attntrace3_writer_put_chunk(f, &w, c,
			chunk.data(), 1) == MEMBRANE_OK, "put_chunk");
	}
	TEST_ASSERT(membrane_attntrace3_writer_close(f, &w) == MEMBRANE_OK,
		"writer_close");
	fclose(f);
}

static void	assert_profiles_match(const calibrated_profile_t &a,
				const calibrated_profile_t &b, const char *label)
{
	TEST_ASSERT(a.policy_name == b.policy_name, label);
	TEST_ASSERT(a.context_tokens == b.context_tokens, label);
	TEST_ASSERT(a.hit_rate == b.hit_rate, label);
	TEST_ASSERT(a.precision == b.precision, label);
	TEST_ASSERT(a.recall == b.recall, label);
	TEST_ASSERT(a.mean_working_set_blocks == b.mean_working_set_blocks,
		label);
	TEST_ASSERT(a.source_is_real_capture == b.source_is_real_capture, label);
	TEST_ASSERT(a.steps.size() == b.steps.size(), label);
	for (size_t i = 0; i < a.steps.size(); i++)
	{
		TEST_ASSERT(a.steps[i].prefetch_bytes == b.steps[i].prefetch_bytes,
			label);
		TEST_ASSERT(a.steps[i].compulsory_miss_bytes
			== b.steps[i].compulsory_miss_bytes, label);
	}
	TEST_ASSERT(a.layer_head.per_layer_hit_rate
		== b.layer_head.per_layer_hit_rate, label);
	TEST_ASSERT(a.layer_head.per_head_hit_rate
		== b.layer_head.per_head_hit_rate, label);
	TEST_ASSERT(a.coalescing.naive_request_count
		== b.coalescing.naive_request_count, label);
	TEST_ASSERT(a.coalescing.coalesced_request_count
		== b.coalescing.coalesced_request_count, label);
	TEST_ASSERT(a.coalescing.real_needed_bytes
		== b.coalescing.real_needed_bytes, label);
	TEST_ASSERT(a.coalescing.transferred_bytes_with_padding
		== b.coalescing.transferred_bytes_with_padding, label);
}

static void	test_calibrate_streamed_matches_in_memory(void)
{
	attn_trace_t	trace;
	uint32_t		step_count = 40;
	uint32_t		n_layer = 3;
	uint32_t		n_head = 4;
	uint32_t		top_k = 4;
	uint32_t		chunk_steps = 7;	/* not a divisor of step_count */
	char			path[] = "/tmp/membrane-calibrate-streamed-XXXXXX";
	int				fd = mkstemp(path);

	TEST_ASSERT(fd >= 0, "temp file");
	close(fd);
	fill_trace(&trace, step_count, n_layer, n_head, top_k);
	write_v3(trace, path, chunk_steps);

	model_calibration_t	model{"test-model", n_layer, 2, 4096,
		1.0e9 / 60.0};
	scenario_config_t		cfg{};
	cfg.policy = policy_t::MEMBRANE_PREDICTIVE;
	cfg.eviction = eviction_policy_t::SEGMENTED_LRU;
	cfg.block_size_tokens = 32;
	cfg.hot_cache_bytes = 1u << 20;
	cfg.warm_tier_is_q8 = true;
	cfg.coalescing_window = 4;

	calibrated_profile_t	ref = calibrate(trace, model, cfg, true);

	auto	mmap_reader = make_attn_trace_reader(trace_backend_t::MMAP);
	TEST_ASSERT(mmap_reader->open(path) == true, "mmap reader opens");
	calibrated_profile_t	via_mmap = calibrate_streamed(*mmap_reader, model,
		cfg, true);
	assert_profiles_match(ref, via_mmap,
		"calibrate_streamed (mmap) must match calibrate (in-memory)");
	mmap_reader->close();

	auto	stream_reader = make_attn_trace_reader(trace_backend_t::STREAMING);
	TEST_ASSERT(stream_reader->open(path) == true,
		"streaming reader opens");
	calibrated_profile_t	via_stream = calibrate_streamed(*stream_reader,
		model, cfg, true);
	assert_profiles_match(ref, via_stream,
		"calibrate_streamed (streaming) must match calibrate (in-memory)");
	stream_reader->close();

	unlink(path);
	printf("PASS test_calibrate_streamed_matches_in_memory\n");
}

int	main(void)
{
	test_calibrate_streamed_matches_in_memory();
	return (0);
}
