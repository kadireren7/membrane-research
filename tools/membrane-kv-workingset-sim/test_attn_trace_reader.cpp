/*
 * Phase 6.5 item 2 tests: the bounded chunk cache (LRU eviction,
 * pin/refcount, duplicate-load avoidance under concurrency) and
 * cross-backend parity (in-memory vs mmap vs buffered-streaming must
 * return identical entries for identical trace content).
 */

#define _DEFAULT_SOURCE

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

#include "attn_trace_reader.h"
#include "attn_workload.h"

#define TEST_ASSERT(cond, msg) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			abort(); \
		} \
	} while (0)

using namespace wssim;

static std::vector<membrane_attntrace_entry_t>	fake_chunk(uint32_t chunk_id,
					size_t n)
{
	std::vector<membrane_attntrace_entry_t>	out(n);

	for (size_t i = 0; i < n; i++)
	{
		out[i].block_id = chunk_id * 1000u + (uint32_t)i;
		out[i].score = 0.5f;
	}
	return (out);
}

static void	test_cache_hit_and_miss_stats(void)
{
	attn_trace_chunk_cache_t	cache(1u << 30);	/* huge budget: no
									 * eviction pressure in this test */
	int							load_count = 0;
	auto	loader = [&]()
	{
		load_count++;
		return (fake_chunk(0, 16));
	};

	const auto	*p1 = cache.acquire(0, loader);
	TEST_ASSERT(p1 != nullptr && p1->size() == 16, "first acquire decodes");
	TEST_ASSERT(load_count == 1, "loader called exactly once on miss");
	cache.release(0);

	const auto	*p2 = cache.acquire(0, loader);
	TEST_ASSERT(p2 == p1, "second acquire returns the SAME cached buffer");
	TEST_ASSERT(load_count == 1, "loader NOT called again on hit");
	cache.release(0);

	attn_trace_chunk_cache_t::stats_t	s = cache.stats();
	TEST_ASSERT(s.hits == 1 && s.misses == 1,
		"stats reflect exactly one hit and one miss");
	printf("PASS test_cache_hit_and_miss_stats\n");
}

static void	test_lru_evicts_unpinned_only(void)
{
	/* Budget for exactly 2 chunks of 16 entries each. */
	size_t						chunk_bytes = 16 * sizeof(membrane_attntrace_entry_t);
	attn_trace_chunk_cache_t	cache(chunk_bytes * 2);
	auto	loader_for = [](uint32_t id) { return [id] { return (fake_chunk(id, 16)); }; };

	cache.acquire(0, loader_for(0));
	cache.release(0);
	cache.acquire(1, loader_for(1));
	cache.release(1);
	/* Both 0 and 1 resident, at budget. Pin 0 so it can't be evicted,
	 * then load a 3rd chunk -- eviction must take chunk 1 (LRU,
	 * unpinned), not chunk 0 (pinned). */
	const auto	*pinned0 = cache.acquire(0, loader_for(0));
	(void)pinned0;
	cache.acquire(2, loader_for(2));
	cache.release(2);

	attn_trace_chunk_cache_t::stats_t	s = cache.stats();
	TEST_ASSERT(s.evictions == 1, "exactly one eviction happened");

	int	reload_count = 0;
	cache.acquire(1, [&] { reload_count++; return (fake_chunk(1, 16)); });
	TEST_ASSERT(reload_count == 1,
		"evicted chunk 1 had to be reloaded from scratch");
	cache.release(1);

	int	pinned_reload_count = 0;
	cache.acquire(0, [&] { pinned_reload_count++; return (fake_chunk(0, 16)); });
	TEST_ASSERT(pinned_reload_count == 0,
		"pinned chunk 0 was never evicted, so no reload was needed");
	cache.release(0);
	cache.release(0);	/* release the pin taken above */
	printf("PASS test_lru_evicts_unpinned_only\n");
}

static void	test_concurrent_duplicate_load_avoided(void)
{
	attn_trace_chunk_cache_t	cache(1u << 30);
	std::atomic<int>			load_count{0};
	std::vector<std::thread>	pool;
	auto	loader = [&]()
	{
		load_count.fetch_add(1);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		return (fake_chunk(7, 64));
	};

	for (int i = 0; i < 8; i++)
		pool.emplace_back([&]
		{
			const auto	*p = cache.acquire(7, loader);
			TEST_ASSERT(p != nullptr && p->size() == 64,
				"every concurrent acquirer sees the decoded chunk");
			cache.release(7);
		});
	for (auto &t : pool)
		t.join();

	TEST_ASSERT(load_count.load() == 1,
		"8 threads racing to load the SAME chunk collapse into 1 decode");
	printf("PASS test_concurrent_duplicate_load_avoided\n");
}

/* A corrupt/truncated chunk's decode() throws (see mmap_reader_t::
 * decode / buffered_streaming_reader_t::decode) -- the cache must not
 * leave the chunk's "loading" placeholder stuck, or every OTHER
 * thread waiting on that same chunk_id would hang in m_cv.wait()
 * forever. This test drives that directly at the cache level, no
 * reader/file involved. */
static void	test_throwing_loader_does_not_leave_slot_stuck(void)
{
	attn_trace_chunk_cache_t	cache(1u << 30);
	bool						threw = false;

	try
	{
		cache.acquire(0, []() -> std::vector<membrane_attntrace_entry_t>
		{
			throw std::runtime_error("simulated corrupt chunk");
		});
	}
	catch (const std::runtime_error &)
	{
		threw = true;
	}
	TEST_ASSERT(threw, "a throwing loader's exception propagates to the "
		"caller, not swallowed");

	/* If the slot were stuck in loading=true, this would hang inside
	 * acquire()'s wait loop instead of calling the loader again. */
	int	load_count = 0;
	const auto	*p = cache.acquire(0, [&]
	{
		load_count++;
		return (fake_chunk(0, 8));
	});
	TEST_ASSERT(p != nullptr && p->size() == 8,
		"the same chunk_id can be loaded successfully after a prior "
		"throwing attempt");
	TEST_ASSERT(load_count == 1, "the retry actually invoked the loader "
		"(slot was not left in some other stuck state)");
	cache.release(0);
	printf("PASS test_throwing_loader_does_not_leave_slot_stuck\n");
}

/* Multiple threads racing to load the SAME chunk, where the winning
 * decode throws -- every waiting thread must wake up and get a
 * chance to retry (one of them succeeding), not hang forever on a
 * placeholder nobody will ever clear. */
static void	test_throwing_loader_unblocks_concurrent_waiters(void)
{
	attn_trace_chunk_cache_t	cache(1u << 30);
	std::atomic<int>			attempt{0};
	std::atomic<int>			successes{0};
	std::atomic<int>			exceptions{0};
	std::vector<std::thread>	pool;

	for (int i = 0; i < 8; i++)
		pool.emplace_back([&]
		{
			try
			{
				const auto	*p = cache.acquire(0, [&]()
					-> std::vector<membrane_attntrace_entry_t>
				{
					/* Only the very first attempt (across all
					 * threads) fails -- simulates one real transient
					 * corruption, not a permanently-broken chunk. */
					if (attempt.fetch_add(1) == 0)
					{
						std::this_thread::sleep_for(
							std::chrono::milliseconds(20));
						throw std::runtime_error("simulated corrupt chunk");
					}
					return (fake_chunk(0, 4));
				});
				if (p != nullptr)
					successes.fetch_add(1);
				cache.release(0);
			}
			catch (const std::runtime_error &)
			{
				exceptions.fetch_add(1);
			}
		});
	for (auto &t : pool)
		t.join();

	/* The key property under test: every thread terminated (the test
	 * itself finishing at all, rather than the join() above hanging,
	 * IS the assertion) -- these counts just confirm the outcomes
	 * make sense (nobody silently vanished). */
	TEST_ASSERT(successes.load() + exceptions.load() == 8,
		"every one of the 8 threads either succeeded or observed the "
		"exception -- none hung or was lost");
	TEST_ASSERT(successes.load() >= 1,
		"at least one thread succeeded after the first, failing "
		"attempt (the cache recovered, not permanently wedged)");
	printf("PASS test_throwing_loader_unblocks_concurrent_waiters "
		"(successes=%d exceptions=%d)\n", successes.load(),
		exceptions.load());
}

static void	fill_test_trace(attn_trace_t *t, uint32_t step_count,
				uint32_t n_layer, uint32_t n_head, uint32_t top_k)
{
	t->model = "test-model";
	t->is_real_capture = true;
	t->n_layer = n_layer;
	t->n_head = n_head;
	t->block_size_tokens = 32;
	t->prompt_len = 100;
	t->step_count = step_count;
	t->top_k = top_k;
	t->entries.assign((size_t)step_count * n_layer * n_head * top_k,
		membrane_attntrace_entry_t{UINT32_MAX, 0.0f});
	for (size_t i = 0; i < t->entries.size(); i++)
	{
		if (i % 5 == 0)
			continue ;	/* leave the UINT32_MAX sentinel in place */
		/* Scores restricted to exact multiples of 1/255 so the
		 * lossless in-memory path and the v3 (8-bit-quantized) path
		 * are expected to be BIT-IDENTICAL, not just "close" -- v3's
		 * quantization error is covered separately by
		 * tests/unit/test_attntrace3.c. */
		uint32_t	q = (uint32_t)(i % 256);

		t->entries[i].block_id = (uint32_t)(i % 3000);
		t->entries[i].score = (float)q / 255.0f;
	}
}

static void	write_v3_from_trace(const attn_trace_t &t, const char *path,
				uint32_t chunk_steps, int compress)
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
			chunk.data(), compress) == MEMBRANE_OK, "put_chunk");
	}
	TEST_ASSERT(membrane_attntrace3_writer_close(f, &w) == MEMBRANE_OK,
		"writer_close");
	fclose(f);
}

static void	assert_readers_identical(attn_trace_reader_t &ref,
				attn_trace_reader_t &other, uint32_t step_count,
				uint32_t n_layer, uint32_t n_head, uint32_t top_k,
				const char *label)
{
	for (uint32_t step = 0; step < step_count; step++)
		for (uint32_t l = 0; l < n_layer; l++)
			for (uint32_t h = 0; h < n_head; h++)
			{
				const membrane_attntrace_entry_t	*a = ref.at(step, l, h);
				const membrane_attntrace_entry_t	*b = other.at(step, l, h);

				for (uint32_t k = 0; k < top_k; k++)
				{
					TEST_ASSERT(a[k].block_id == b[k].block_id,
						label);
					TEST_ASSERT(a[k].score == b[k].score, label);
				}
			}
}

static void	test_backend_parity(void)
{
	attn_trace_t	trace;
	uint32_t		step_count = 37;
	uint32_t		n_layer = 3;
	uint32_t		n_head = 2;
	uint32_t		top_k = 4;
	uint32_t		chunk_steps = 8;	/* deliberately not a divisor of
							 * step_count -- exercises the uneven
							 * last chunk on the read side too. */
	char			path[] = "/tmp/membrane-attn-reader-parity-XXXXXX";
	int				fd = mkstemp(path);

	TEST_ASSERT(fd >= 0, "temp file");
	close(fd);
	fill_test_trace(&trace, step_count, n_layer, n_head, top_k);

	for (int compress = 0; compress <= 1; compress++)
	{
		write_v3_from_trace(trace, path, chunk_steps, compress);

		auto	mem_reader = make_in_memory_reader(trace);
		TEST_ASSERT(mem_reader->open("") == true, "in-memory reader opens");

		auto	mmap_reader = make_attn_trace_reader(trace_backend_t::MMAP);
		TEST_ASSERT(mmap_reader->open(path) == true, "mmap reader opens");

		auto	stream_reader = make_attn_trace_reader(
			trace_backend_t::STREAMING);
		TEST_ASSERT(stream_reader->open(path) == true,
			"streaming reader opens");

		const trace_metadata_t	&md = mmap_reader->get_metadata();
		TEST_ASSERT(md.n_layer == n_layer && md.n_head == n_head
			&& md.top_k == top_k && md.step_count == step_count,
			"mmap reader metadata matches the source trace");

		assert_readers_identical(*mem_reader, *mmap_reader, step_count,
			n_layer, n_head, top_k,
			"in-memory vs mmap backend must be bit-identical");
		assert_readers_identical(*mem_reader, *stream_reader, step_count,
			n_layer, n_head, top_k,
			"in-memory vs streaming backend must be bit-identical");

		mmap_reader->close();
		stream_reader->close();
	}
	unlink(path);
	printf("PASS test_backend_parity\n");
}

/* Re-reads the same v3 file backward-then-forward through a SINGLE
 * mmap reader instance (chunk_steps small enough to force several
 * chunk switches) -- the auto-managed "current chunk" pin must
 * release the old chunk and acquire the new one correctly every time,
 * in either direction, not just in the strictly-forward order the
 * real calibration loop happens to use. */
static void	test_reader_handles_out_of_order_step_access(void)
{
	attn_trace_t	trace;
	uint32_t		step_count = 20;
	uint32_t		n_layer = 2;
	uint32_t		n_head = 2;
	uint32_t		top_k = 2;
	uint32_t		chunk_steps = 4;
	char			path[] = "/tmp/membrane-attn-reader-order-XXXXXX";
	int				fd = mkstemp(path);

	TEST_ASSERT(fd >= 0, "temp file");
	close(fd);
	fill_test_trace(&trace, step_count, n_layer, n_head, top_k);
	write_v3_from_trace(trace, path, chunk_steps, 1);

	auto	mem_reader = make_in_memory_reader(trace);
	mem_reader->open("");
	auto	mmap_reader = make_attn_trace_reader(trace_backend_t::MMAP);
	TEST_ASSERT(mmap_reader->open(path) == true, "mmap reader opens");

	uint32_t	order[] = {19, 0, 5, 4, 3, 19, 10, 0};

	for (uint32_t step : order)
		for (uint32_t l = 0; l < n_layer; l++)
			for (uint32_t h = 0; h < n_head; h++)
			{
				const membrane_attntrace_entry_t	*a = mem_reader->at(step,
					l, h);
				const membrane_attntrace_entry_t	*b = mmap_reader->at(step,
					l, h);

				for (uint32_t k = 0; k < top_k; k++)
				{
					TEST_ASSERT(a[k].block_id == b[k].block_id,
						"out-of-order step access still matches");
					TEST_ASSERT(a[k].score == b[k].score,
						"out-of-order step access still matches");
				}
			}
	mmap_reader->close();
	unlink(path);
	printf("PASS test_reader_handles_out_of_order_step_access\n");
}

/* extend_synthetic() (in-memory, full-float precision) and
 * extend_synthetic_to_file() (out-of-core, one chunk resident at a
 * time) must run the SAME per-step generator over the SAME carried
 * xorshift32 stream (see the comment on generate_synthetic_step in
 * attn_workload.cpp) -- block ids must match exactly; scores only
 * within one v3 8-bit quantization step, since writing through
 * .attntrace3 quantizes them (the same, already-tested, disclosed
 * lossy step test_attntrace3.c covers) -- this test is about the
 * GENERATOR not diverging between the two code paths, not about
 * re-proving quantization error bounds. */
static void	test_extend_synthetic_streaming_matches_in_memory(void)
{
	attn_trace_t	native;
	uint32_t		native_steps = 11;
	uint32_t		n_layer = 3;
	uint32_t		n_head = 2;
	uint32_t		top_k = 4;
	uint32_t		target_steps = 53;	/* several native cycles,
							 * uneven vs chunk_steps too */
	uint32_t		chunk_steps = 8;
	uint32_t		seed = 424242u;
	char			path[] = "/tmp/membrane-extend-synth-XXXXXX";
	int				fd = mkstemp(path);

	TEST_ASSERT(fd >= 0, "temp file");
	close(fd);
	fill_test_trace(&native, native_steps, n_layer, n_head, top_k);
	native.is_real_capture = true;

	attn_trace_t	in_memory = extend_synthetic(native, target_steps, seed);
	TEST_ASSERT(extend_synthetic_to_file(native, target_steps, seed, path,
		chunk_steps, 1) == true, "extend_synthetic_to_file succeeds");

	auto	reader = make_attn_trace_reader(trace_backend_t::MMAP);
	TEST_ASSERT(reader->open(path) == true, "reader opens synthetic v3 file");
	const trace_metadata_t	&md = reader->get_metadata();
	TEST_ASSERT(md.step_count == target_steps
		&& md.n_layer == n_layer && md.n_head == n_head
		&& md.top_k == top_k, "streamed synthetic metadata matches");

	for (uint32_t step = 0; step < target_steps; step++)
		for (uint32_t l = 0; l < n_layer; l++)
			for (uint32_t h = 0; h < n_head; h++)
			{
				const membrane_attntrace_entry_t	*a = in_memory.at(step, l,
					h);
				const membrane_attntrace_entry_t	*b = reader->at(step, l,
					h);

				for (uint32_t k = 0; k < top_k; k++)
				{
					TEST_ASSERT(a[k].block_id == b[k].block_id,
						"streamed synthetic block_id matches in-memory "
						"exactly");
					float	diff = a[k].score - b[k].score;
					if (diff < 0)
						diff = -diff;
					TEST_ASSERT(diff <= (1.0f / 255.0f) + 1e-6f,
						"streamed synthetic score matches in-memory "
						"within one v3 quantization step");
				}
			}
	reader->close();
	unlink(path);
	printf("PASS test_extend_synthetic_streaming_matches_in_memory\n");
}

/* Real, measurable effect of --prefetch-depth: over a walk that
 * touches every chunk exactly once, the TOTAL miss count is the same
 * either way (every chunk is decoded exactly once regardless -- the
 * cache simply prevents a second decode of something already
 * resident). What prefetch actually changes is WHEN each chunk gets
 * decoded: with depth > 0, by the time at() itself crosses into a new
 * chunk, readahead_from() (triggered on the PREVIOUS boundary
 * crossing) has usually already decoded it, so THAT specific acquire
 * call registers as a cache HIT instead of a miss. With depth 0
 * (default), every boundary crossing's own acquire is a fresh miss,
 * so it never contributes to cache.stats().hits at all. */
static void	test_prefetch_depth_turns_boundary_misses_into_hits(void)
{
	attn_trace_t	trace;
	uint32_t		step_count = 64;
	uint32_t		n_layer = 2;
	uint32_t		n_head = 2;
	uint32_t		top_k = 2;
	uint32_t		chunk_steps = 8;	/* 8 chunks total */
	char			path[] = "/tmp/membrane-attn-reader-prefetch-XXXXXX";
	int				fd = mkstemp(path);

	TEST_ASSERT(fd >= 0, "temp file");
	close(fd);
	fill_test_trace(&trace, step_count, n_layer, n_head, top_k);
	write_v3_from_trace(trace, path, chunk_steps, 0);

	auto	cache_no_prefetch = std::make_shared<attn_trace_chunk_cache_t>(
		1u << 30);
	auto	reader_no_prefetch = make_attn_trace_reader(trace_backend_t::MMAP,
		cache_no_prefetch);
	TEST_ASSERT(reader_no_prefetch->open(path) == true, "reader opens");
	for (uint32_t step = 0; step < step_count; step += chunk_steps)
		reader_no_prefetch->at(step, 0, 0);
	auto	stats_no_prefetch = cache_no_prefetch->stats();
	TEST_ASSERT(stats_no_prefetch.misses == 8,
		"with prefetch disabled, every one of the 8 chunk-boundary "
		"steps is a fresh miss");
	TEST_ASSERT(stats_no_prefetch.hits == 0,
		"with prefetch disabled, no boundary crossing ever finds its "
		"chunk already resident");
	reader_no_prefetch->close();

	auto	cache_prefetch = std::make_shared<attn_trace_chunk_cache_t>(
		1u << 30);
	auto	reader_prefetch = make_attn_trace_reader(trace_backend_t::MMAP,
		cache_prefetch);
	TEST_ASSERT(reader_prefetch->open(path) == true, "reader opens");
	reader_prefetch->set_prefetch_depth(2);
	for (uint32_t step = 0; step < step_count; step += chunk_steps)
		reader_prefetch->at(step, 0, 0);
	auto	stats_prefetch = cache_prefetch->stats();
	TEST_ASSERT(stats_prefetch.hits > stats_no_prefetch.hits,
		"prefetch_depth=2 makes most boundary crossings find their "
		"chunk already resident (a hit), unlike the no-prefetch walk "
		"which never does");
	printf("PASS test_prefetch_depth_turns_boundary_misses_into_hits "
		"(hits: no-prefetch=%llu prefetch=%llu)\n",
		(unsigned long long)stats_no_prefetch.hits,
		(unsigned long long)stats_prefetch.hits);
	reader_prefetch->close();
	unlink(path);
}

int	main(void)
{
	test_cache_hit_and_miss_stats();
	test_lru_evicts_unpinned_only();
	test_concurrent_duplicate_load_avoided();
	test_backend_parity();
	test_reader_handles_out_of_order_step_access();
	test_extend_synthetic_streaming_matches_in_memory();
	test_prefetch_depth_turns_boundary_misses_into_hits();
	test_throwing_loader_does_not_leave_slot_stuck();
	test_throwing_loader_unblocks_concurrent_waiters();
	return (0);
}
