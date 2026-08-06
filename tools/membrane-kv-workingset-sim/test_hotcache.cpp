/*
 * Cache/prefetch correctness tests for hot_cache_t (Phase 6.2 item
 * 15's "cache/prefetch correctness tests"). Plain assertions, no
 * llama.cpp dependency -- built unconditionally like
 * tools/membrane-kv-runtime-optimizer/test_checkpoint.cpp.
 */

#include <cstdio>
#include <cstdlib>

#include "hotcache.h"

#define TEST_ASSERT(cond, msg) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			abort(); \
		} \
	} while (0)


using namespace wssim;

static void	test_lru_evicts_oldest_first(void)
{
	hot_cache_t	c(300, eviction_policy_t::LRU);

	c.insert({0, 0, 1}, 100, 0.0);
	c.insert({0, 0, 2}, 100, 0.0);
	c.insert({0, 0, 3}, 100, 0.0);
	TEST_ASSERT(c.bytes_used() == 300, "c.bytes_used() == 300");
	/* Cache is full (300/300). Inserting a 4th entry must evict the
	 * least-recently-touched one (block 1, never touched since
	 * insert). */
	c.insert({0, 0, 4}, 100, 0.0);
	TEST_ASSERT(!c.contains({0, 0, 1}), "!c.contains({0, 0, 1})");
	TEST_ASSERT(c.contains({0, 0, 2}), "c.contains({0, 0, 2})");
	TEST_ASSERT(c.contains({0, 0, 3}), "c.contains({0, 0, 3})");
	TEST_ASSERT(c.contains({0, 0, 4}), "c.contains({0, 0, 4})");
	printf("PASS test_lru_evicts_oldest_first\n");
}

static void	test_lru_touch_protects_entry(void)
{
	hot_cache_t	c(300, eviction_policy_t::LRU);

	c.insert({0, 0, 1}, 100, 0.0);
	c.insert({0, 0, 2}, 100, 0.0);
	c.insert({0, 0, 3}, 100, 0.0);
	/* Touching block 1 makes it MORE recent than 2 and 3. */
	c.touch_hit({0, 0, 1}, 0.0);
	c.insert({0, 0, 4}, 100, 0.0);
	TEST_ASSERT(c.contains({0, 0, 1}), "c.contains({0, 0, 1})");
	TEST_ASSERT(!c.contains({0, 0, 2}), "!c.contains({0, 0, 2})");
	printf("PASS test_lru_touch_protects_entry\n");
}

static void	test_lfu_evicts_least_frequent(void)
{
	hot_cache_t	c(300, eviction_policy_t::LFU);

	c.insert({0, 0, 1}, 100, 0.0);
	c.insert({0, 0, 2}, 100, 0.0);
	c.insert({0, 0, 3}, 100, 0.0);
	c.touch_hit({0, 0, 1}, 0.0);
	c.touch_hit({0, 0, 1}, 0.0);
	c.touch_hit({0, 0, 3}, 0.0);
	/* freq: 1 -> 3, 2 -> 1, 3 -> 2. Least frequent (2) must go. */
	c.insert({0, 0, 4}, 100, 0.0);
	TEST_ASSERT(c.contains({0, 0, 1}), "c.contains({0, 0, 1})");
	TEST_ASSERT(!c.contains({0, 0, 2}), "!c.contains({0, 0, 2})");
	TEST_ASSERT(c.contains({0, 0, 3}), "c.contains({0, 0, 3})");
	printf("PASS test_lfu_evicts_least_frequent\n");
}

static void	test_attention_score_aware_evicts_lowest_score(void)
{
	hot_cache_t	c(300, eviction_policy_t::ATTENTION_SCORE_AWARE);

	c.insert({0, 0, 1}, 100, 0.9);
	c.insert({0, 0, 2}, 100, 0.1);
	c.insert({0, 0, 3}, 100, 0.5);
	c.insert({0, 0, 4}, 100, 0.9);
	TEST_ASSERT(!c.contains({0, 0, 2}), "!c.contains({0, 0, 2})");
	TEST_ASSERT(c.contains({0, 0, 1}), "c.contains({0, 0, 1})");
	TEST_ASSERT(c.contains({0, 0, 3}), "c.contains({0, 0, 3})");
	TEST_ASSERT(c.contains({0, 0, 4}), "c.contains({0, 0, 4})");
	printf("PASS test_attention_score_aware_evicts_lowest_score\n");
}

static void	test_segmented_lru_protects_promoted_entries(void)
{
	/* Small cache so the 80/20 protected/probationary split is
	 * exercised directly: capacity 500, protected cap = 400. */
	hot_cache_t	c(500, eviction_policy_t::SEGMENTED_LRU);

	c.insert({0, 0, 1}, 100, 0.0);
	c.touch_hit({0, 0, 1}, 0.0);	/* promotes 1 to protected */
	c.insert({0, 0, 2}, 100, 0.0);
	c.insert({0, 0, 3}, 100, 0.0);
	c.insert({0, 0, 4}, 100, 0.0);
	c.insert({0, 0, 5}, 100, 0.0);
	/* Cache full at 500/500. New insert must evict a probationary
	 * entry before ever touching the protected one (block 1). */
	c.insert({0, 0, 6}, 100, 0.0);
	TEST_ASSERT(c.contains({0, 0, 1}), "c.contains({0, 0, 1})");
	printf("PASS test_segmented_lru_protects_promoted_entries\n");
}

static void	test_never_exceeds_capacity(void)
{
	hot_cache_t	c(1000, eviction_policy_t::LRU);

	for (uint32_t i = 0; i < 50; i++)
		c.insert({0, 0, i}, 137, 0.0);
	TEST_ASSERT(c.bytes_used() <= 1000, "c.bytes_used() <= 1000");
	printf("PASS test_never_exceeds_capacity\n");
}

static void	test_oversized_entry_not_inserted(void)
{
	hot_cache_t	c(100, eviction_policy_t::LRU);

	c.insert({0, 0, 1}, 500, 0.0);
	TEST_ASSERT(!c.contains({0, 0, 1}), "!c.contains({0, 0, 1})");
	TEST_ASSERT(c.bytes_used() == 0, "c.bytes_used() == 0");
	printf("PASS test_oversized_entry_not_inserted\n");
}

int	main(void)
{
	test_lru_evicts_oldest_first();
	test_lru_touch_protects_entry();
	test_lfu_evicts_least_frequent();
	test_attention_score_aware_evicts_lowest_score();
	test_segmented_lru_protects_promoted_entries();
	test_never_exceeds_capacity();
	test_oversized_entry_not_inserted();
	return (0);
}
