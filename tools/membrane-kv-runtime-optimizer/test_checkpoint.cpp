/*
 * Phase 4.2 item 12: fast, model-free tests for the checkpoint/resume
 * format (checkpoint.h) -- extracted specifically so these can run in
 * milliseconds instead of only being reachable through slow end-to-end
 * model runs. Mirrors the C test style in tests/unit/ (TEST_ASSERT,
 * mkstemp'd scratch files) even though this tool is C++.
 */
#define _DEFAULT_SOURCE

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "checkpoint.h"

# define TEST_ASSERT(cond, msg) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			abort(); \
		} \
	} while (0)

static char	g_path[] = "/tmp/membrane-ckpt-test-XXXXXX";

static const char	*COMMIT_A = "c0bc8591e8815c63cb01dd3f051a8b0df02501c9";
static const char	*COMMIT_B = "0000000000000000000000000000000000dead";
static const char	*VERSION_A = "membrane-kv-runtime-optimizer-1.0";
static const char	*VERSION_B = "membrane-kv-runtime-optimizer-9.9";

static void	test_header_round_trip(void)
{
	FILE	*f;

	f = fopen(g_path, "w");
	TEST_ASSERT(f != NULL, "open for header write");
	TEST_ASSERT(write_checkpoint_header(f, "modelA", "hash-abc", COMMIT_A,
			"prompthash-1", VERSION_A), "write header");
	fclose(f);
	checkpoint_state_t	st = load_checkpoint(g_path, "modelA", "conservative",
			"hash-abc", COMMIT_A, "prompthash-1", VERSION_A);
	TEST_ASSERT(st.header_present, "header present after round-trip");
	TEST_ASSERT(st.header_matches, "matching header verifies clean");
	TEST_ASSERT(st.mismatch_reason.empty(), "no mismatch reason on match");
}

static void	test_model_hash_mismatch(void)
{
	FILE	*f;

	f = fopen(g_path, "w");
	TEST_ASSERT(f != NULL, "open");
	write_checkpoint_header(f, "modelA", "hash-abc", COMMIT_A, "prompthash-1",
		VERSION_A);
	fclose(f);
	checkpoint_state_t	st = load_checkpoint(g_path, "modelA", "conservative",
			"hash-DIFFERENT", COMMIT_A, "prompthash-1", VERSION_A);
	TEST_ASSERT(st.header_present, "header present");
	TEST_ASSERT(!st.header_matches, "wrong model hash is a mismatch");
	TEST_ASSERT(st.mismatch_reason.find("model hash") != std::string::npos,
		"mismatch reason names the model hash");
}

static void	test_llama_commit_mismatch(void)
{
	FILE	*f;

	f = fopen(g_path, "w");
	write_checkpoint_header(f, "modelA", "hash-abc", COMMIT_A, "prompthash-1",
		VERSION_A);
	fclose(f);
	checkpoint_state_t	st = load_checkpoint(g_path, "modelA", "conservative",
			"hash-abc", COMMIT_B, "prompthash-1", VERSION_A);
	TEST_ASSERT(st.header_present, "header present");
	TEST_ASSERT(!st.header_matches, "wrong llama.cpp commit is a mismatch");
	TEST_ASSERT(st.mismatch_reason.find("commit") != std::string::npos,
		"mismatch reason names the commit");
}

static void	test_prompt_hash_mismatch(void)
{
	FILE	*f;

	f = fopen(g_path, "w");
	write_checkpoint_header(f, "modelA", "hash-abc", COMMIT_A, "prompthash-1",
		VERSION_A);
	fclose(f);
	checkpoint_state_t	st = load_checkpoint(g_path, "modelA", "conservative",
			"hash-abc", COMMIT_A, "prompthash-DIFFERENT", VERSION_A);
	TEST_ASSERT(st.header_present, "header present");
	TEST_ASSERT(!st.header_matches, "wrong prompt set hash is a mismatch");
	TEST_ASSERT(st.mismatch_reason.find("prompt set hash")
		!= std::string::npos, "mismatch reason names the prompt set hash");
}

static void	test_tool_version_mismatch(void)
{
	FILE	*f;

	f = fopen(g_path, "w");
	write_checkpoint_header(f, "modelA", "hash-abc", COMMIT_A, "prompthash-1",
		VERSION_A);
	fclose(f);
	checkpoint_state_t	st = load_checkpoint(g_path, "modelA", "conservative",
			"hash-abc", COMMIT_A, "prompthash-1", VERSION_B);
	TEST_ASSERT(st.header_present, "header present");
	TEST_ASSERT(!st.header_matches, "wrong tool version is a mismatch");
	TEST_ASSERT(st.mismatch_reason.find("tool version") != std::string::npos,
		"mismatch reason names the tool version");
}

static void	test_different_model_name_is_absent_not_mismatched(void)
{
	FILE	*f;

	f = fopen(g_path, "w");
	write_checkpoint_header(f, "modelA", "hash-abc", COMMIT_A, "prompthash-1",
		VERSION_A);
	fclose(f);
	/* A checkpoint that simply has never seen "modelB" before is a fresh
	 * file for that model, not a stale one -- header_present must stay
	 * false so the caller writes a first header rather than refusing. */
	checkpoint_state_t	st = load_checkpoint(g_path, "modelB", "conservative",
			"anything", COMMIT_A, "anything", VERSION_A);
	TEST_ASSERT(!st.header_present, "unrelated model name has no header");
	TEST_ASSERT(!st.header_matches, "and therefore cannot match");
}

static void	test_candidate_round_trip_accepted(void)
{
	FILE	*f;

	f = fopen(g_path, "w");
	TEST_ASSERT(write_checkpoint_candidate(f, "modelA", "balanced", 12, true,
			true, 0.998765, 99.5, 100.0, 0.0021, 7, 0.999123, 99.9,
			"accepted"), "write accepted candidate");
	fclose(f);
	checkpoint_state_t	st = load_checkpoint(g_path, "modelA", "balanced",
			"hash-abc", COMMIT_A, "prompthash-1", VERSION_A);
	TEST_ASSERT(st.decisions.size() == 1, "one decision loaded");
	const resume_decision_t	&d = st.decisions[0];

	TEST_ASSERT(d.layer == 12, "layer round-trips");
	TEST_ASSERT(d.is_k == true, "K/V flag round-trips");
	TEST_ASSERT(d.accepted == true, "accepted flag round-trips");
	TEST_ASSERT(fabs(d.cosine - 0.998765) < 1e-6, "cosine round-trips");
	TEST_ASSERT(fabs(d.top1 - 99.5) < 1e-4, "top1 round-trips");
	TEST_ASSERT(fabs(d.top5 - 100.0) < 1e-4, "top5 round-trips");
	TEST_ASSERT(fabs(d.kl - 0.0021) < 1e-6, "kl round-trips");
	TEST_ASSERT(d.first_divergence == 7, "first_divergence round-trips");
	TEST_ASSERT(fabs(d.offline_cosine - 0.999123) < 1e-6,
		"offline_cosine round-trips");
	TEST_ASSERT(d.reason == "accepted", "reason round-trips");
}

static void	test_candidate_round_trip_rejected_with_special_chars(void)
{
	FILE		*f;
	std::string	reason = "prompt 'recall.txt' (recall-critical): "
		"cosine 0.997 < 0.9985 \"quoted\"\nnewline";

	f = fopen(g_path, "w");
	TEST_ASSERT(write_checkpoint_candidate(f, "modelA", "balanced", 5, false,
			false, 0.997, 98.0, 99.0, 0.05, 3, 0.9995, 99.8, reason),
		"write rejected candidate with quotes/newline in reason");
	fclose(f);
	checkpoint_state_t	st = load_checkpoint(g_path, "modelA", "balanced",
			"hash-abc", COMMIT_A, "prompthash-1", VERSION_A);
	TEST_ASSERT(st.decisions.size() == 1, "one decision loaded");
	TEST_ASSERT(st.decisions[0].accepted == false, "rejected flag round-trips");
	/* the ESCAPED reason should decode back to something containing the
	 * key substrings -- exact byte-for-byte round-trip of embedded
	 * quotes/newlines through a hand-rolled JSON reader is not promised
	 * (json_escape converts them to \" / \n, which this reader does not
	 * un-escape), but the reason must not be silently dropped or corrupt
	 * the record boundaries of surrounding lines. */
	TEST_ASSERT(st.decisions[0].reason.find("cosine 0.997") != std::string::npos,
		"reason substring survives escaping");
}

static void	test_tier_isolation(void)
{
	FILE	*f;

	f = fopen(g_path, "w");
	write_checkpoint_candidate(f, "modelA", "conservative", 1, false, true,
		0.999, 99.0, 99.5, 0.001, 10, 0.9995, 99.9, "accepted");
	write_checkpoint_candidate(f, "modelA", "aggressive", 2, false, true,
		0.998, 98.5, 99.0, 0.002, 8, 0.9990, 99.5, "accepted");
	fclose(f);
	checkpoint_state_t	cons = load_checkpoint(g_path, "modelA",
			"conservative", "h", COMMIT_A, "p", VERSION_A);
	checkpoint_state_t	aggr = load_checkpoint(g_path, "modelA", "aggressive",
			"h", COMMIT_A, "p", VERSION_A);
	TEST_ASSERT(cons.decisions.size() == 1 && cons.decisions[0].layer == 1,
		"conservative tier only sees its own decision");
	TEST_ASSERT(aggr.decisions.size() == 1 && aggr.decisions[0].layer == 2,
		"aggressive tier only sees its own decision");
}

static void	test_completion_marker(void)
{
	FILE	*f;

	f = fopen(g_path, "w");
	write_checkpoint_candidate(f, "modelA", "balanced", 1, false, true, 0.999,
		99.0, 99.5, 0.001, 10, 0.9995, 99.9, "accepted");
	fclose(f);
	checkpoint_state_t	before = load_checkpoint(g_path, "modelA", "balanced",
			"h", COMMIT_A, "p", VERSION_A);
	TEST_ASSERT(!before.tier_complete,
		"a tier with decisions but no completion marker is NOT complete "
		"-- item 8: a half-finished search must never be treated as a "
		"final policy");
	f = fopen(g_path, "a");
	TEST_ASSERT(write_checkpoint_complete(f, "modelA", "balanced", 1, 12.5),
		"write completion marker");
	fclose(f);
	checkpoint_state_t	after = load_checkpoint(g_path, "modelA", "balanced",
			"h", COMMIT_A, "p", VERSION_A);
	TEST_ASSERT(after.tier_complete, "completion marker is observed");
	checkpoint_state_t	other_tier = load_checkpoint(g_path, "modelA",
			"aggressive", "h", COMMIT_A, "p", VERSION_A);
	TEST_ASSERT(!other_tier.tier_complete,
		"completion marker does not leak to a different tier");
}

/* item 12: "interrupted candidate evaluation" -- a file that stops right
 * after some candidates, with no completion marker, must still load
 * every candidate that WAS fully written, and must report tier_complete
 * = false so the caller knows to keep searching (not treat this as a
 * finished policy). This is exactly what a process kill mid-search
 * leaves behind (each write is flushed immediately, per-record, so a
 * kill can only ever leave a file that ends cleanly after some whole
 * number of complete lines, or possibly one line short of a fprintf
 * buffer nothing was still in flight for). */
static void	test_interrupted_search_state(void)
{
	FILE	*f;

	f = fopen(g_path, "w");
	write_checkpoint_header(f, "modelA", "hash-abc", COMMIT_A, "prompthash-1",
		VERSION_A);
	write_checkpoint_candidate(f, "modelA", "balanced", 0, false, true, 0.999,
		99.0, 99.5, 0.001, 32, 0.9995, 99.9, "accepted");
	write_checkpoint_candidate(f, "modelA", "balanced", 1, false, false,
		0.990, 95.0, 97.0, 0.05, 2, 0.9995, 99.9, "cosine too low");
	fclose(f);
	checkpoint_state_t	st = load_checkpoint(g_path, "modelA", "balanced",
			"hash-abc", COMMIT_A, "prompthash-1", VERSION_A);
	TEST_ASSERT(st.header_matches, "header still matches after interruption");
	TEST_ASSERT(st.decisions.size() == 2, "both pre-interruption decisions "
		"are recovered");
	TEST_ASSERT(!st.tier_complete, "interrupted tier is not complete -- "
		"item 8: a resumed run must keep searching, not export this as "
		"final");
}

static void	test_determinism_two_independent_files(void)
{
	char	path_a[] = "/tmp/membrane-ckpt-test-a-XXXXXX";
	char	path_b[] = "/tmp/membrane-ckpt-test-b-XXXXXX";
	int		fda;
	int		fdb;
	FILE	*fa;
	FILE	*fb;

	fda = mkstemp(path_a);
	fdb = mkstemp(path_b);
	TEST_ASSERT(fda >= 0 && fdb >= 0, "temp files created");
	close(fda);
	close(fdb);
	fa = fopen(path_a, "w");
	fb = fopen(path_b, "w");
	for (int i = 0; i < 5; i++)
	{
		write_checkpoint_candidate(fa, "modelA", "balanced", i, i % 2 == 0,
			i != 3, 0.999 - i * 0.0001, 99.0, 99.5, 0.001, 32 - i, 0.9995,
			99.9, i == 3 ? "rejected" : "accepted");
		write_checkpoint_candidate(fb, "modelA", "balanced", i, i % 2 == 0,
			i != 3, 0.999 - i * 0.0001, 99.0, 99.5, 0.001, 32 - i, 0.9995,
			99.9, i == 3 ? "rejected" : "accepted");
	}
	fclose(fa);
	fclose(fb);
	checkpoint_state_t	sa = load_checkpoint(path_a, "modelA", "balanced",
			"h", COMMIT_A, "p", VERSION_A);
	checkpoint_state_t	sb = load_checkpoint(path_b, "modelA", "balanced",
			"h", COMMIT_A, "p", VERSION_A);
	TEST_ASSERT(sa.decisions.size() == sb.decisions.size(),
		"two independent writes of the same sequence load the same count");
	for (size_t i = 0; i < sa.decisions.size(); i++)
	{
		TEST_ASSERT(sa.decisions[i].layer == sb.decisions[i].layer,
			"deterministic layer ordering");
		TEST_ASSERT(sa.decisions[i].accepted == sb.decisions[i].accepted,
			"deterministic accept/reject");
		TEST_ASSERT(sa.decisions[i].cosine == sb.decisions[i].cosine,
			"deterministic cosine (exact, not just close -- same bytes "
			"written, same bytes must read back)");
	}
	unlink(path_a);
	unlink(path_b);
}

/* item 12: "offline sonuc tek basina acceptance yapamiyor" -- the
 * checkpoint FORMAT itself only ever records "LIVE_RUNTIME" as the
 * backend for a decision (write_checkpoint_candidate hardcodes this),
 * so no accepted/rejected decision can ever have been produced by
 * OFFLINE_BLOB alone, by construction of the format, not just by
 * caller discipline. */
static void	test_only_live_runtime_backend_in_format(void)
{
	FILE		*f;
	std::string	line;
	char		buf[4096];

	f = fopen(g_path, "w");
	write_checkpoint_candidate(f, "modelA", "balanced", 0, false, true, 0.999,
		99.0, 99.5, 0.001, 32, 0.9995, 99.9, "accepted");
	fclose(f);
	f = fopen(g_path, "r");
	TEST_ASSERT(f != NULL, "reopen to inspect raw bytes");
	TEST_ASSERT(fgets(buf, sizeof(buf), f) != NULL, "read the line");
	line = buf;
	fclose(f);
	TEST_ASSERT(line.find("\"backend\":\"LIVE_RUNTIME\"") != std::string::npos,
		"every candidate record is tagged LIVE_RUNTIME");
	TEST_ASSERT(line.find("OFFLINE_BLOB") == std::string::npos,
		"OFFLINE_BLOB never appears as a decision backend in the format");
}

static void	test_prompt_set_hash_changes_with_content(void)
{
	std::vector<std::string>	paths = {"a.txt", "b.txt"};
	std::string	h1 = compute_prompt_set_hash_from_contents(paths,
			{"hello", "world"});
	std::string	h2 = compute_prompt_set_hash_from_contents(paths,
			{"hello", "WORLD"});
	std::string	h3 = compute_prompt_set_hash_from_contents(paths,
			{"hello", "world"});

	TEST_ASSERT(h1 != h2, "changing one prompt's content changes the hash");
	TEST_ASSERT(h1 == h3, "identical inputs hash identically (deterministic)");
}

int	main(void)
{
	int	fd;

	fd = mkstemp(g_path);
	TEST_ASSERT(fd >= 0, "temp file created");
	close(fd);
	test_header_round_trip();
	test_model_hash_mismatch();
	test_llama_commit_mismatch();
	test_prompt_hash_mismatch();
	test_tool_version_mismatch();
	test_different_model_name_is_absent_not_mismatched();
	test_candidate_round_trip_accepted();
	test_candidate_round_trip_rejected_with_special_chars();
	test_tier_isolation();
	test_completion_marker();
	test_interrupted_search_state();
	test_determinism_two_independent_files();
	test_only_live_runtime_backend_in_format();
	test_prompt_set_hash_changes_with_content();
	unlink(g_path);
	printf("all checkpoint tests passed\n");
	return (0);
}
