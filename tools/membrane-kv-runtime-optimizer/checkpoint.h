#ifndef MEMBRANE_OPTIMIZER_CHECKPOINT_H
# define MEMBRANE_OPTIMIZER_CHECKPOINT_H

/*
 * Phase 4.2 item 8: checkpoint/resume, factored out of main.cpp so it can
 * be exercised by a fast, model-free test (test_checkpoint.cpp) instead
 * of only through slow end-to-end model runs. Takes plain primitive
 * parameters (no dependency on the tool's metrics_t/slot_t/baseline_t),
 * deliberately -- this keeps the format's correctness testable in
 * isolation from any inference code.
 *
 * A checkpoint file opens with ONE header record per model identifying
 * exactly what produced it -- model SHA-256, the compiled-in llama.cpp
 * commit, a hash over the valid prompt set's paths+contents, and this
 * tool's version -- then one record per live decision (flushed
 * immediately after each), then a completion marker once a tier's
 * search loop actually finishes. Resuming re-verifies all four header
 * fields against the CURRENT run before trusting a single decision; any
 * mismatch is a STALE checkpoint and the run is refused outright (not
 * silently ignored -- a silent ignore could quietly merge decisions
 * from two different models/prompt sets into what looks like one
 * policy). A checkpoint without a completion marker for a tier is
 * never treated as that tier's final policy.
 */

# include <cstdio>
# include <cstring>
# include <string>
# include <vector>

# include "membrane/hash.h"

static inline std::string	ckpt_json_escape(const std::string &s)
{
	std::string	out;

	for (char c : s)
	{
		if (c == '"' || c == '\\')
		{
			out += '\\';
			out += c;
		}
		else if (c == '\n')
			out += "\\n";
		else if (c == '\r')
			out += "\\r";
		else if (c == '\t')
			out += "\\t";
		else if ((unsigned char)c < 0x20)
			continue ;
		else
			out += c;
	}
	return (out);
}

typedef struct s_resume_decision
{
	int			layer;
	bool		is_k;
	bool		accepted;
	double		cosine;
	double		top1;
	double		top5;
	double		kl;
	long		first_divergence;
	double		offline_cosine;
	double		offline_top1;
	std::string	reason;
}	resume_decision_t;

static inline std::string	compute_prompt_set_hash_from_contents(
								const std::vector<std::string> &prompt_paths,
								const std::vector<std::string> &prompt_contents)
{
	std::string	joined;
	uint8_t		digest[MEMBRANE_SHA256_DIGEST_BYTES];
	char		hex[MEMBRANE_SHA256_HEX_LEN + 1];
	size_t		i;

	i = 0;
	while (i < prompt_paths.size())
	{
		joined += prompt_paths[i];
		joined += '\n';
		joined += prompt_contents[i];
		joined += '\n';
		i++;
	}
	membrane_sha256((const uint8_t *)joined.data(), joined.size(), digest);
	membrane_sha256_to_hex(digest, hex);
	return (std::string(hex));
}

static inline bool	write_checkpoint_header(FILE *f, const char *model_name,
					const std::string &model_hash,
					const char *llama_cpp_commit,
					const std::string &prompt_hash, const char *tool_version)
{
	if (f == NULL)
		return (true);
	if (fprintf(f, "{\"record\":\"checkpoint_header\",\"model\":\"%s\","
			"\"model_sha256\":\"%s\",\"llama_cpp_commit\":\"%s\","
			"\"prompt_set_hash\":\"%s\",\"tool_version\":\"%s\"}\n",
			model_name, model_hash.c_str(), llama_cpp_commit,
			prompt_hash.c_str(), tool_version) < 0)
		return (false);
	return (fflush(f) == 0);
}

static inline bool	write_checkpoint_candidate(FILE *f, const char *model_name,
					const char *tier, int layer, bool is_k, bool accepted,
					double cosine, double top1, double top5, double kl,
					long first_divergence, double offline_cosine,
					double offline_top1, const std::string &reason)
{
	if (f == NULL)
		return (true);
	if (fprintf(f, "{\"record\":\"candidate\",\"model\":\"%s\",\"tier\":\"%s\","
			"\"layer\":%d,\"kv\":\"%s\",\"backend\":\"LIVE_RUNTIME\","
			"\"accepted\":%s,\"cosine\":%.6f,\"top1\":%.4f,\"top5\":%.4f,"
			"\"kl\":%.6f,\"first_divergence\":%ld,\"offline_cosine\":%.6f,"
			"\"offline_top1\":%.4f,\"reason\":\"%s\"}\n", model_name, tier,
			layer, is_k ? "K" : "V", accepted ? "true" : "false", cosine,
			top1, top5, kl, first_divergence, offline_cosine, offline_top1,
			ckpt_json_escape(reason).c_str()) < 0)
		return (false);
	return (fflush(f) == 0);
}

static inline bool	write_checkpoint_complete(FILE *f, const char *model_name,
					const char *tier, int evals_used, double search_seconds)
{
	if (f == NULL)
		return (true);
	if (fprintf(f, "{\"record\":\"search_complete\",\"model\":\"%s\","
			"\"tier\":\"%s\",\"evals_used\":%d,\"search_seconds\":%.3f}\n",
			model_name, tier, evals_used, search_seconds) < 0)
		return (false);
	return (fflush(f) == 0);
}

typedef struct s_checkpoint_state
{
	bool							header_present;
	bool							header_matches;
	std::string						mismatch_reason;
	std::vector<resume_decision_t>	decisions;	/* for the requested tier */
	bool							tier_complete;
}	checkpoint_state_t;

/* Reads every record for `model_name` (any tier, to find the header) and
 * `tier` (for decisions/completion), verifying the header against the
 * CURRENT run's real identity. Returns header_matches=false (with
 * mismatch_reason set) rather than silently treating a stale or
 * unrelated checkpoint as usable. */
static inline checkpoint_state_t	load_checkpoint(const char *path,
										const char *model_name,
										const char *tier,
										const std::string &model_hash,
										const char *llama_cpp_commit,
										const std::string &prompt_hash,
										const char *tool_version)
{
	checkpoint_state_t	st;
	FILE				*f;
	char				line[2048];
	char				m[128];
	char				t[64];

	st.header_present = false;
	st.header_matches = false;
	st.tier_complete = false;
	if (path == NULL)
		return (st);
	f = fopen(path, "r");
	if (f == NULL)
		return (st);
	while (fgets(line, sizeof(line), f) != NULL)
	{
		if (strstr(line, "\"record\":\"checkpoint_header\"") != NULL)
		{
			char	mh[128];
			char	lc[128];
			char	ph[128];
			char	tv[128];

			if (sscanf(line, "{\"record\":\"checkpoint_header\","
					"\"model\":\"%127[^\"]\",\"model_sha256\":\"%127[^\"]"
					"\",\"llama_cpp_commit\":\"%127[^\"]"
					"\",\"prompt_set_hash\":\"%127[^\"]"
					"\",\"tool_version\":\"%127[^\"]", m, mh, lc, ph, tv) < 5)
				continue ;
			if (strcmp(m, model_name) != 0)
				continue ;
			st.header_present = true;
			if (model_hash != mh)
				st.mismatch_reason = std::string("model hash: checkpoint=")
					+ mh + " actual=" + model_hash;
			else if (std::string(llama_cpp_commit) != lc)
				st.mismatch_reason = std::string("llama.cpp commit: "
					"checkpoint=") + lc + " actual=" + llama_cpp_commit;
			else if (prompt_hash != ph)
				st.mismatch_reason = std::string("prompt set hash: "
					"checkpoint=") + ph + " actual=" + prompt_hash;
			else if (std::string(tool_version) != tv)
				st.mismatch_reason = std::string("tool version: checkpoint=")
					+ tv + " actual=" + tool_version;
			else
				st.header_matches = true;
			continue ;
		}
		if (strstr(line, "\"record\":\"candidate\"") != NULL)
		{
			char	kv[4];
			int		layer;
			int		acc;
			double	cosine;
			double	top1;
			double	top5;
			double	kl;
			long	first_div;
			double	off_cos;
			double	off_top1;
			const char	*rp;
			const char	*rend;
			std::string	reason;

			if (sscanf(line, "{\"record\":\"candidate\",\"model\":\"%127[^\"]"
					"\",\"tier\":\"%63[^\"]\",\"layer\":%d,\"kv\":\"%3[^\"]",
					m, t, &layer, kv) < 4)
				continue ;
			if (strcmp(m, model_name) != 0 || strcmp(t, tier) != 0)
				continue ;
			acc = strstr(line, "\"accepted\":true") != NULL;
			cosine = 0.0;
			top1 = 0.0;
			top5 = 0.0;
			kl = 0.0;
			first_div = 0;
			off_cos = 0.0;
			off_top1 = 0.0;
			rp = strstr(line, "\"cosine\":");
			if (rp != NULL)
				sscanf(rp, "\"cosine\":%lf", &cosine);
			rp = strstr(line, "\"top1\":");
			if (rp != NULL)
				sscanf(rp, "\"top1\":%lf", &top1);
			rp = strstr(line, "\"top5\":");
			if (rp != NULL)
				sscanf(rp, "\"top5\":%lf", &top5);
			rp = strstr(line, "\"kl\":");
			if (rp != NULL)
				sscanf(rp, "\"kl\":%lf", &kl);
			rp = strstr(line, "\"first_divergence\":");
			if (rp != NULL)
				sscanf(rp, "\"first_divergence\":%ld", &first_div);
			rp = strstr(line, "\"offline_cosine\":");
			if (rp != NULL)
				sscanf(rp, "\"offline_cosine\":%lf", &off_cos);
			rp = strstr(line, "\"offline_top1\":");
			if (rp != NULL)
				sscanf(rp, "\"offline_top1\":%lf", &off_top1);
			reason.clear();
			rp = strstr(line, "\"reason\":\"");
			if (rp != NULL)
			{
				rp += strlen("\"reason\":\"");
				rend = strstr(rp, "\"}");
				if (rend != NULL)
					reason.assign(rp, (size_t)(rend - rp));
			}
			st.decisions.push_back({layer, kv[0] == 'K', acc != 0, cosine,
					top1, top5, kl, first_div, off_cos, off_top1, reason});
			continue ;
		}
		if (strstr(line, "\"record\":\"search_complete\"") != NULL)
		{
			if (sscanf(line, "{\"record\":\"search_complete\","
					"\"model\":\"%127[^\"]\",\"tier\":\"%63[^\"]", m, t) == 2
					&& strcmp(m, model_name) == 0 && strcmp(t, tier) == 0)
				st.tier_complete = true;
		}
	}
	fclose(f);
	return (st);
}

#endif
