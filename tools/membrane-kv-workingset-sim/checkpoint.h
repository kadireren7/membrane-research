#ifndef MEMBRANE_WSSIM_CHECKPOINT_H
#define MEMBRANE_WSSIM_CHECKPOINT_H

/*
 * Phase 6.2 item 13: atomic checkpoint/resume for the working-set
 * sweep, following the same design Phase 4.2's checkpoint.h
 * established (tools/membrane-kv-runtime-optimizer/checkpoint.h) --
 * a header record identifying exactly what produced the file (here:
 * a hash of every real trace file used plus a hash of the sweep's own
 * config), then one record per completed scenario flushed
 * immediately, then a completion marker. Resuming re-verifies the
 * header against the CURRENT run before trusting any record; a
 * mismatch is refused outright, not silently ignored (the same
 * reasoning as Phase 4.2: silently reusing decisions from a different
 * trace/config would quietly corrupt the result). A new schema (this
 * tool's own, not reused byte-for-byte) because Phase 4.2's format is
 * shaped around K/V-layer accept/reject decisions, not
 * (policy, eviction, block_size, cache_bytes) scenarios.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "membrane/hash.h"

namespace wssim
{

inline std::string	sha256_hex_of_string(const std::string &s)
{
	uint8_t	digest[MEMBRANE_SHA256_DIGEST_BYTES];
	char	hex[MEMBRANE_SHA256_HEX_LEN + 1];

	membrane_sha256((const uint8_t *)s.data(), s.size(), digest);
	membrane_sha256_to_hex(digest, hex);
	return (std::string(hex));
}

inline std::string	sha256_hex_of_file(const std::string &path)
{
	char	hex[MEMBRANE_SHA256_HEX_LEN + 1];

	if (membrane_sha256_file(path.c_str(), hex) != MEMBRANE_OK)
		return (std::string());
	return (std::string(hex));
}

struct checkpoint_writer_t
{
	FILE	*f = nullptr;

	bool	open(const std::string &path, const std::string &trace_hash,
			const std::string &config_hash, bool append)
	{
		f = fopen(path.c_str(), append ? "a" : "w");
		if (f == nullptr)
			return (false);
		if (!append)
		{
			fprintf(f, "{\"record\":\"header\",\"trace_hash\":\"%s\","
				"\"config_hash\":\"%s\"}\n", trace_hash.c_str(),
				config_hash.c_str());
			fflush(f);
		}
		return (true);
	}

	void	write_scenario(const std::string &id, const std::string &csv_row)
	{
		if (f == nullptr)
			return ;
		fprintf(f, "{\"record\":\"scenario\",\"id\":\"%s\",\"row\":\"%s\"}\n",
			id.c_str(), csv_row.c_str());
		fflush(f);
	}

	void	write_complete()
	{
		if (f == nullptr)
			return ;
		fprintf(f, "{\"record\":\"sweep_complete\"}\n");
		fflush(f);
	}

	~checkpoint_writer_t()
	{
		if (f != nullptr)
			fclose(f);
	}
};

struct checkpoint_state_t
{
	bool						header_present = false;
	bool						header_matches = false;
	std::string					mismatch_reason;
	std::unordered_set<std::string>	completed_ids;
	std::vector<std::string>	completed_rows;	/* in file order */
	bool						sweep_complete = false;
};

inline checkpoint_state_t	load_checkpoint(const std::string &path,
								const std::string &trace_hash,
								const std::string &config_hash)
{
	checkpoint_state_t	st;
	FILE				*f;
	char				line[4096];

	f = fopen(path.c_str(), "r");
	if (f == nullptr)
		return (st);
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		if (strstr(line, "\"record\":\"header\"") != nullptr)
		{
			char	th[128];
			char	ch[128];

			st.header_present = true;
			th[0] = '\0';
			ch[0] = '\0';
			const char	*p = strstr(line, "\"trace_hash\":\"");
			if (p != nullptr)
				sscanf(p, "\"trace_hash\":\"%127[^\"]", th);
			p = strstr(line, "\"config_hash\":\"");
			if (p != nullptr)
				sscanf(p, "\"config_hash\":\"%127[^\"]", ch);
			if (trace_hash != th)
				st.mismatch_reason = "trace_hash mismatch";
			else if (config_hash != ch)
				st.mismatch_reason = "config_hash mismatch";
			else
				st.header_matches = true;
			continue ;
		}
		if (strstr(line, "\"record\":\"scenario\"") != nullptr)
		{
			char	id[512];

			id[0] = '\0';
			const char	*p = strstr(line, "\"id\":\"");
			if (p != nullptr)
				sscanf(p, "\"id\":\"%511[^\"]", id);
			if (id[0] != '\0')
			{
				st.completed_ids.insert(id);
				char	row[4096];
				row[0] = '\0';
				p = strstr(line, "\"row\":\"");
				if (p != nullptr)
					sscanf(p, "\"row\":\"%4095[^\"]", row);
				st.completed_rows.emplace_back(row);
			}
			continue ;
		}
		if (strstr(line, "\"record\":\"sweep_complete\"") != nullptr)
			st.sweep_complete = true;
	}
	fclose(f);
	return (st);
}

}	/* namespace wssim */

#endif
