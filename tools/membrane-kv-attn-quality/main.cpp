/*
 * membrane-kv-attn-quality: Phase 6.2's real APPROXIMATE-mode quality
 * measurement -- unlike membrane-kv-workingset-sim (which only
 * simulates byte/latency effects of a working set, section 6A / EXACT
 * mode, on top of a llama.cpp KV cache that always stays fully
 * intact), this tool actually removes non-selected KV blocks from a
 * REAL llama.cpp decode via the public llama_memory_seq_rm() API, so
 * attention genuinely cannot see them -- exactly "secilmeyen bloklar
 * attention hesabina katilmaz" (unselected blocks don't participate
 * in the attention computation).
 *
 * Scope, disclosed plainly: llama_memory_seq_rm operates on the
 * shared KV cache position axis, which is common to every layer and
 * head -- there is no public API to evict a block for one (layer,
 * kv_head_group) channel only. So this tool's eviction decision is
 * necessarily GLOBAL (one working set per step, applied uniformly
 * across all layers/heads), coarser than
 * membrane-kv-workingset-sim's per-channel analysis. That makes this
 * a conservative (if anything, pessimistic) real measurement: a true
 * per-channel approximate system has strictly more information and
 * could be less aggressive than a global one, so real quality loss
 * from a genuinely selective system would likely be even smaller than
 * what is measured here.
 *
 * Two policies are evaluated for real (a representative subset of
 * membrane-kv-workingset-sim's 8 -- the simulator covers all 8
 * analytically; this tool validates a subset against actual model
 * output, matching how Phase 6.1 validated only a subset of its
 * simulated scenarios against real captured traces):
 *   - sliding-window+sink: pure position-based, no attention capture
 *     needed (StreamingLLM-style).
 *   - topk-lag1-attention: evicts using the REAL previous step's
 *     attention distribution (same cb_eval "kq_soft_max-<il>"
 *     mechanism as membrane-kv-attn-trace-capture), aggregated across
 *     all layers/heads for this tool's necessarily global decision.
 *
 * Metrics per (model, prompt category, policy), comparing the
 * eviction run against an unmodified baseline run from the identical
 * prompt: top1 match rate, top5 overlap rate, mean logit cosine
 * similarity, mean KL divergence, first-divergence step -- the same
 * vocabulary Phase 4.2's checkpoint.h already established
 * (tools/membrane-kv-runtime-optimizer/checkpoint.h), reused here for
 * consistency rather than inventing new metric names.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "ggml.h"
#include "ggml-backend.h"
#include "llama.h"

static int	die(const char *msg)
{
	fprintf(stderr, "membrane-kv-attn-quality: %s\n", msg);
	return (-1);
}

static std::string	read_file(const char *path)
{
	std::string	s;
	FILE		*f;
	char		buf[4096];
	size_t		n;

	f = fopen(path, "rb");
	if (f == NULL)
		return (s);
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		s.append(buf, n);
	fclose(f);
	return (s);
}

static std::vector<llama_token>	build_prompt_tokens(
		const llama_vocab *vocab, const std::string &text, int target)
{
	std::vector<llama_token>	base;
	std::vector<llama_token>	out;
	int							n;

	base.resize(text.size() + 8);
	n = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(),
			base.data(), (int32_t)base.size(), true, false);
	if (n < 0)
		return (out);
	base.resize(n);
	out = base;
	while ((int)out.size() < target)
		out.insert(out.end(), base.begin() + 1, base.end());
	if ((int)out.size() > target)
		out.resize(target);
	return (out);
}

enum class quality_policy_t
{
	SLIDING_WINDOW_SINK = 0,
	TOPK_LAG1_ATTENTION
};

static const char	*policy_name(quality_policy_t p)
{
	return (p == quality_policy_t::SLIDING_WINDOW_SINK
		? "sliding-window+sink" : "topk-lag1-attention");
}

typedef struct s_cb_state
{
	uint32_t	block_size;
	bool		active;
	/* Global (all layers/heads pooled) attention mass per block for
	 * the step currently being computed -- this tool's necessarily
	 * coarser real-time analogue of attn_workload.h's per-channel
	 * ground truth (see the file header comment for why). */
	std::vector<double>	block_scores;
}	cb_state_t;

static float	tensor_f32_at(const uint8_t *data, const size_t *nb,
					size_t i0, size_t i1, size_t i2, size_t i3)
{
	size_t	off = i3 * nb[3] + i2 * nb[2] + i1 * nb[1] + i0 * nb[0];
	return (*(const float *)&data[off]);
}

static bool	eval_cb(ggml_tensor *t, bool ask, void *user_data)
{
	cb_state_t	*st = (cb_state_t *)user_data;

	if (ask)
		return (true);
	if (!st->active || t->type != GGML_TYPE_F32)
		return (true);
	if (strncmp(t->name, "kq_soft_max-", 12) != 0)
		return (true);
	if (!ggml_backend_buffer_is_host(t->buffer))
		return (true);
	const uint8_t	*data = (const uint8_t *)t->data;
	uint32_t	n_kv = (uint32_t)t->ne[0];
	uint32_t	n_head = (uint32_t)t->ne[2];
	uint32_t	n_blocks = (n_kv + st->block_size - 1) / st->block_size;
	if (n_blocks > st->block_scores.size())
		st->block_scores.resize(n_blocks, 0.0);
	for (uint32_t h = 0; h < n_head; h++)
		for (uint32_t i0 = 0; i0 < n_kv; i0++)
			st->block_scores[i0 / st->block_size]
				+= (double)tensor_f32_at(data, t->nb, i0, 0, h, 0);
	return (true);
}

/* Blocks to evict this step: everything resident, currently valid,
 * and not in the policy's keep-set. `resident` tracks blocks not yet
 * evicted (eviction is permanent -- llama_memory_seq_rm cannot be
 * undone, matching real StreamingLLM/H2O-style irrecoverable
 * eviction). */
static std::vector<uint32_t>	blocks_to_evict(quality_policy_t policy,
		std::set<uint32_t> &resident, uint32_t block_size,
		uint32_t current_total_tokens, const std::vector<double> &prev_scores)
{
	std::set<uint32_t>	keep;
	uint32_t			last_block = current_total_tokens
		? (current_total_tokens - 1) / block_size : 0;

	keep.insert(0);	/* sink */
	if (last_block > 0)
		keep.insert(last_block - 1);
	keep.insert(last_block);
	if (policy == quality_policy_t::SLIDING_WINDOW_SINK)
	{
		uint32_t	window_tokens = 256;
		uint32_t	window_blocks = (window_tokens + block_size - 1)
			/ block_size;
		uint32_t	start = (last_block > window_blocks)
			? (last_block - window_blocks) : 0;
		for (uint32_t b = start; b <= last_block; b++)
			keep.insert(b);
	}
	else
	{
		std::vector<std::pair<double, uint32_t>>	ranked;
		for (uint32_t b = 0; b < prev_scores.size(); b++)
			ranked.emplace_back(prev_scores[b], b);
		std::sort(ranked.begin(), ranked.end(),
			[](const auto &a, const auto &b) { return (a.first > b.first); });
		for (uint32_t i = 0; i < 4 && i < ranked.size(); i++)
			keep.insert(ranked[i].second);
	}
	std::vector<uint32_t>	evict;
	for (uint32_t b : resident)
		if (!keep.count(b))
			evict.push_back(b);
	for (uint32_t b : evict)
		resident.erase(b);
	return (evict);
}

struct run_result_t
{
	std::vector<std::vector<float>>	logits;	/* per step, n_vocab */
	std::vector<llama_token>			tokens;
};

static llama_token	greedy_pick(const float *logits, int32_t n_vocab)
{
	llama_token	best = 0;
	float		best_v = logits[0];

	for (int32_t i = 1; i < n_vocab; i++)
		if (logits[i] > best_v)
		{
			best_v = logits[i];
			best = i;
		}
	return (best);
}

static bool	run_decode(llama_model *model, const std::vector<llama_token>
				&prompt_tokens, int gen_steps, quality_policy_t policy,
				bool apply_eviction, uint32_t block_size, run_result_t *out)
{
	llama_context_params	cp = llama_context_default_params();
	cb_state_t				st{};

	cp.n_ctx = (uint32_t)(prompt_tokens.size() + gen_steps + 8);
	cp.n_batch = 256;
	cp.n_threads = 4;
	cp.n_threads_batch = 4;
	cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
	st.block_size = block_size;
	st.active = false;
	if (apply_eviction)
	{
		cp.cb_eval = eval_cb;
		cp.cb_eval_user_data = &st;
	}
	llama_context	*ctx = llama_init_from_model(model, cp);
	if (ctx == NULL)
		return (false);
	const llama_vocab	*vocab = llama_model_get_vocab(model);
	int32_t	n_vocab = llama_vocab_n_tokens(vocab);

	size_t	off = 0;
	while (off < prompt_tokens.size())
	{
		size_t	n = std::min(prompt_tokens.size() - off, (size_t)256);
		if (llama_decode(ctx, llama_batch_get_one(
				(llama_token *)prompt_tokens.data() + off, (int32_t)n)) != 0)
		{
			llama_free(ctx);
			return (false);
		}
		off += n;
	}

	std::set<uint32_t>	resident;
	for (uint32_t b = 0; b * block_size < prompt_tokens.size(); b++)
		resident.insert(b);
	std::vector<double>	prev_scores;
	uint32_t	total_tokens = (uint32_t)prompt_tokens.size();
	llama_memory_t	mem = apply_eviction ? llama_get_memory(ctx) : nullptr;

	for (int step = 0; step < gen_steps; step++)
	{
		llama_token	tok = greedy_pick(llama_get_logits_ith(ctx, -1),
			n_vocab);
		out->tokens.push_back(tok);
		if (apply_eviction && step > 0)
		{
			std::vector<uint32_t>	evict = blocks_to_evict(policy, resident,
				block_size, total_tokens, prev_scores);
			for (uint32_t b : evict)
				llama_memory_seq_rm(mem, 0, (llama_pos)(b * block_size),
					(llama_pos)((b + 1) * block_size));
		}
		st.active = apply_eviction;
		if (!st.block_scores.empty())
			std::fill(st.block_scores.begin(), st.block_scores.end(), 0.0);
		if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0)
		{
			llama_free(ctx);
			return (false);
		}
		st.active = false;
		total_tokens++;
		resident.insert((total_tokens - 1) / block_size);
		prev_scores = st.block_scores;

		const float	*logits = llama_get_logits_ith(ctx, -1);
		out->logits.emplace_back(logits, logits + n_vocab);
	}
	llama_free(ctx);
	return (true);
}

static double	cosine(const std::vector<float> &a, const std::vector<float> &b)
{
	double	dot = 0.0;
	double	na = 0.0;
	double	nb = 0.0;

	for (size_t i = 0; i < a.size(); i++)
	{
		dot += (double)a[i] * b[i];
		na += (double)a[i] * a[i];
		nb += (double)b[i] * b[i];
	}
	if (na <= 0.0 || nb <= 0.0)
		return (0.0);
	return (dot / (std::sqrt(na) * std::sqrt(nb)));
}

static void	softmax(const std::vector<float> &in, std::vector<double> &out)
{
	double	mx = *std::max_element(in.begin(), in.end());
	double	sum = 0.0;

	out.resize(in.size());
	for (size_t i = 0; i < in.size(); i++)
	{
		out[i] = std::exp((double)in[i] - mx);
		sum += out[i];
	}
	for (size_t i = 0; i < out.size(); i++)
		out[i] /= sum;
}

static double	kl_divergence(const std::vector<float> &p_logits,
					const std::vector<float> &q_logits)
{
	std::vector<double>	p;
	std::vector<double>	q;

	softmax(p_logits, p);
	softmax(q_logits, q);
	double	kl = 0.0;
	for (size_t i = 0; i < p.size(); i++)
		if (p[i] > 1e-12)
			kl += p[i] * std::log(p[i] / std::max(q[i], 1e-12));
	return (kl);
}

static std::vector<uint32_t>	top_n_idx(const std::vector<float> &v, int n)
{
	std::vector<uint32_t>	idx(v.size());
	for (uint32_t i = 0; i < idx.size(); i++)
		idx[i] = i;
	std::partial_sort(idx.begin(), idx.begin() + n, idx.end(),
		[&](uint32_t a, uint32_t b) { return (v[a] > v[b]); });
	idx.resize(n);
	return (idx);
}

int	main(int argc, char **argv)
{
	const char	*model_path = NULL;
	std::vector<std::pair<std::string, std::string>>	prompts;
	int			gen_steps = 48;
	int			n_tokens = 512;
	uint32_t	block_size = 32;

	for (int i = 1; i + 1 < argc; i += 2)
	{
		if (strcmp(argv[i], "--model") == 0)
			model_path = argv[i + 1];
		else if (strcmp(argv[i], "--prompt") == 0)
		{
			std::string	spec = argv[i + 1];
			size_t		eq = spec.find('=');
			if (eq != std::string::npos)
				prompts.emplace_back(spec.substr(0, eq),
					spec.substr(eq + 1));
		}
		else if (strcmp(argv[i], "--gen-steps") == 0)
			gen_steps = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "--n-tokens") == 0)
			n_tokens = atoi(argv[i + 1]);
	}
	if (model_path == NULL || prompts.empty())
		return (die("usage: --model M --prompt name=path [--prompt "
			"name=path ...] [--gen-steps N] [--n-tokens N]"));

	llama_backend_init();
	llama_model	*model = llama_model_load_from_file(model_path,
		llama_model_default_params());
	if (model == NULL)
		return (llama_backend_free(), die("model load failed"));
	const llama_vocab	*vocab = llama_model_get_vocab(model);

	printf("model,category,policy,top1_match_rate,top5_overlap_rate,"
		"mean_cosine,mean_kl,first_divergence_step,gen_steps,"
		"generated_text_identical\n");

	for (const auto &pr : prompts)
	{
		std::vector<llama_token>	tokens = build_prompt_tokens(vocab,
			read_file(pr.second.c_str()), n_tokens);
		if (tokens.empty())
		{
			fprintf(stderr, "membrane-kv-attn-quality: skip empty prompt "
				"%s\n", pr.second.c_str());
			continue ;
		}
		run_result_t	baseline;
		if (!run_decode(model, tokens, gen_steps,
				quality_policy_t::SLIDING_WINDOW_SINK, false, block_size,
				&baseline))
		{
			fprintf(stderr, "membrane-kv-attn-quality: baseline decode "
				"failed for %s\n", pr.first.c_str());
			continue ;
		}
		for (quality_policy_t policy : {quality_policy_t::SLIDING_WINDOW_SINK,
				quality_policy_t::TOPK_LAG1_ATTENTION})
		{
			run_result_t	approx;
			if (!run_decode(model, tokens, gen_steps, policy, true,
					block_size, &approx))
			{
				fprintf(stderr, "membrane-kv-attn-quality: approx decode "
					"failed for %s/%s\n", pr.first.c_str(),
					policy_name(policy));
				continue ;
			}
			int		top1_hits = 0;
			int		top5_hits = 0;
			double	cos_sum = 0.0;
			double	kl_sum = 0.0;
			int		first_div = -1;
			bool	identical = true;

			for (int s = 0; s < gen_steps; s++)
			{
				bool	tok_match = baseline.tokens[s] == approx.tokens[s];
				if (tok_match)
					top1_hits++;
				else if (first_div < 0)
					first_div = s;
				if (!tok_match)
					identical = false;
				std::vector<uint32_t>	top5 = top_n_idx(approx.logits[s], 5);
				if (std::find(top5.begin(), top5.end(),
						(uint32_t)baseline.tokens[s]) != top5.end())
					top5_hits++;
				cos_sum += cosine(baseline.logits[s], approx.logits[s]);
				kl_sum += kl_divergence(baseline.logits[s], approx.logits[s]);
			}
			printf("%s,%s,%s,%.4f,%.4f,%.6f,%.6f,%d,%d,%s\n",
				model_path, pr.first.c_str(), policy_name(policy),
				(double)top1_hits / gen_steps, (double)top5_hits / gen_steps,
				cos_sum / gen_steps, kl_sum / gen_steps, first_div,
				gen_steps, identical ? "true" : "false");
			fflush(stdout);
		}
	}
	llama_model_free(model);
	llama_backend_free();
	return (0);
}
