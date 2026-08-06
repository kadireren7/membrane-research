/*
 * membrane-kv-exact-sim-verify: Phase 6.5 item 11's standalone
 * results-integrity checker/repair tool for a (CSV, checkpoint) pair
 * produced by membrane-kv-exact-sim. Independent of the sweep binary
 * itself -- run any time (mid-sweep, after an interruption, after a
 * full completion) to check for:
 *
 *   - duplicate checkpoint records for the same scenario id
 *   - a checkpoint record whose row fails its own completion_checksum
 *     (truncated/corrupted mid-write)
 *   - a CSV row with no backing checkpoint record ("stale" -- the CSV
 *     is supposed to be entirely derived FROM the checkpoint, see
 *     main.cpp's rebuild-on-start logic; any CSV row that isn't is a
 *     sign the CSV was hand-edited or written by an old binary)
 *   - a checkpoint record with no corresponding CSV row ("missing")
 *   - a checkpoint whose header doesn't match the CURRENT native
 *     trace files / config (stale checkpoint -- re-derives both
 *     hashes fresh from disk, does not trust anything cached)
 *
 * `--repair` rewrites both files: the checkpoint deduplicated (keep
 * the LAST record for any duplicated id) with corrupted/truncated
 * lines dropped, and the CSV regenerated fresh from the repaired
 * checkpoint (the same "CSV is a derived artifact" invariant
 * main.cpp's own startup rebuild already relies on).
 */

#define _DEFAULT_SOURCE

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "checkpoint.h"
#include "membrane/block.h"

using wssim::sha256_hex_of_file;
using wssim::sha256_hex_of_string;

namespace
{

/* Mirrors main.cpp's config_hash literal EXACTLY -- Phase 6.5
 * deliberately left this unchanged from Phase 6.4 (see main.cpp's
 * top-of-file comment) so existing checkpoints stay valid; this tool
 * must recompute the SAME string to judge staleness correctly. */
const char	*kConfigHashInput =
	"phase6.4-unified-v1;target_steps=130560;concurrency=512;"
	"comparisons=7;precisions=3;host_sizes=5;device_sizes=3";

struct raw_record_t
{
	std::string	id;
	std::string	row;
	bool		checksum_ok;
	size_t		line_no;
};

/* Re-parses the checkpoint file directly (not via checkpoint.h's
 * load_checkpoint, which silently skips malformed lines and does not
 * expose duplicates) so this tool can report exactly what
 * load_checkpoint would hide. */
std::vector<raw_record_t>	parse_checkpoint_raw(const std::string &path,
								bool *header_present,
								std::string *header_trace_hash,
								std::string *header_config_hash,
								bool *sweep_complete,
								size_t *truncated_lines)
{
	std::vector<raw_record_t>	out;
	FILE						*f = fopen(path.c_str(), "r");
	char						line[8192];
	size_t						line_no = 0;

	*header_present = false;
	*sweep_complete = false;
	*truncated_lines = 0;
	if (f == nullptr)
		return (out);
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		line_no++;
		if (strstr(line, "\"record\":\"header\"") != nullptr)
		{
			char	th[128] = {0};
			char	ch[128] = {0};
			const char	*p = strstr(line, "\"trace_hash\":\"");

			if (p != nullptr)
				sscanf(p, "\"trace_hash\":\"%127[^\"]", th);
			p = strstr(line, "\"config_hash\":\"");
			if (p != nullptr)
				sscanf(p, "\"config_hash\":\"%127[^\"]", ch);
			*header_present = true;
			*header_trace_hash = th;
			*header_config_hash = ch;
			continue ;
		}
		if (strstr(line, "\"record\":\"sweep_complete\"") != nullptr)
		{
			*sweep_complete = true;
			continue ;
		}
		if (strstr(line, "\"record\":\"scenario\"") == nullptr)
			continue ;

		char	id[512] = {0};
		char	row[4096] = {0};
		const char	*p = strstr(line, "\"id\":\"");

		if (p != nullptr)
			sscanf(p, "\"id\":\"%511[^\"]", id);
		p = strstr(line, "\"row\":\"");
		if (p != nullptr)
			sscanf(p, "\"row\":\"%4095[^\"]", row);
		if (id[0] == '\0' || row[0] == '\0')
		{
			(*truncated_lines)++;
			continue ;
		}

		/* Verify the row's own trailing completion_checksum (see
		 * main.cpp's finalize_row): the last comma-separated field is
		 * an 8-hex-char CRC32 of everything before it. */
		std::string	row_s(row);
		size_t		last_comma = row_s.find_last_of(',');
		bool		checksum_ok = false;

		if (last_comma != std::string::npos)
		{
			std::string	body = row_s.substr(0, last_comma);
			std::string	stored = row_s.substr(last_comma + 1);
			uint32_t	recomputed = membrane_block_checksum(
				(const uint8_t *)body.data(), body.size());
			char		expect[16];

			snprintf(expect, sizeof(expect), "%08x", recomputed);
			checksum_ok = (stored == expect);
		}
		if (!checksum_ok)
			(*truncated_lines)++;
		out.push_back({std::string(id), row_s, checksum_ok, line_no});
	}
	fclose(f);
	return (out);
}

std::vector<std::string>	read_csv_data_lines(const std::string &path)
{
	std::vector<std::string>	out;
	FILE						*f = fopen(path.c_str(), "r");
	char						line[8192];
	bool						first = true;

	if (f == nullptr)
		return (out);
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		if (first)
		{
			first = false;
			continue ;	/* header */
		}
		std::string	s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
			s.pop_back();
		if (!s.empty())
			out.push_back(s);
	}
	fclose(f);
	return (out);
}

std::string	id_from_csv_row(const std::string &row)
{
	size_t	c[5];
	size_t	pos = 0;

	for (int i = 0; i < 5; i++)
	{
		c[i] = row.find(',', pos);
		if (c[i] == std::string::npos)
			return (std::string());
		pos = c[i] + 1;
	}
	return ("unified|" + row.substr(0, c[0]) + "|"
		+ row.substr(c[0] + 1, c[1] - c[0] - 1) + "|"
		+ row.substr(c[1] + 1, c[2] - c[1] - 1) + "|"
		+ row.substr(c[2] + 1, c[3] - c[2] - 1) + "|"
		+ row.substr(c[3] + 1, c[4] - c[3] - 1));
}

}	/* anonymous namespace */

int	main(int argc, char **argv)
{
	std::string	csv_path;
	std::string	ckpt_path;
	std::string	trace_135m_long;
	std::string	trace_360m_long;
	bool		repair = false;

	for (int i = 1; i + 1 <= argc; i++)
	{
		if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc)
			csv_path = argv[++i];
		else if (strcmp(argv[i], "--checkpoint") == 0 && i + 1 < argc)
			ckpt_path = argv[++i];
		else if (strcmp(argv[i], "--trace-135m-long") == 0 && i + 1 < argc)
			trace_135m_long = argv[++i];
		else if (strcmp(argv[i], "--trace-360m-long") == 0 && i + 1 < argc)
			trace_360m_long = argv[++i];
		else if (strcmp(argv[i], "--repair") == 0)
			repair = true;
	}
	if (csv_path.empty() || ckpt_path.empty())
	{
		fprintf(stderr, "usage: membrane-kv-exact-sim-verify --csv CSV "
			"--checkpoint PATH [--trace-135m-long P] "
			"[--trace-360m-long P] [--repair]\n");
		return (2);
	}

	bool		header_present = false;
	std::string	header_trace_hash;
	std::string	header_config_hash;
	bool		sweep_complete = false;
	size_t		truncated_lines = 0;
	std::vector<raw_record_t>	records = parse_checkpoint_raw(ckpt_path,
		&header_present, &header_trace_hash, &header_config_hash,
		&sweep_complete, &truncated_lines);

	int	problems = 0;

	fprintf(stderr, "== membrane-kv-exact-sim-verify: %s + %s ==\n",
		csv_path.c_str(), ckpt_path.c_str());
	fprintf(stderr, "checkpoint: header_present=%d sweep_complete=%d "
		"scenario_records=%zu truncated/corrupt_lines=%zu\n",
		(int)header_present, (int)sweep_complete, records.size(),
		truncated_lines);
	if (truncated_lines > 0)
	{
		problems++;
		fprintf(stderr, "  [PROBLEM] %zu checkpoint line(s) truncated or "
			"failed their own completion_checksum\n", truncated_lines);
	}

	if (!trace_135m_long.empty())
	{
		std::string	trace_hash_input = sha256_hex_of_file(trace_135m_long);

		if (!trace_360m_long.empty())
			trace_hash_input += sha256_hex_of_file(trace_360m_long);
		std::string	expect_trace_hash = sha256_hex_of_string(
			trace_hash_input);
		std::string	expect_config_hash = sha256_hex_of_string(
			kConfigHashInput);

		if (!header_present)
		{
			problems++;
			fprintf(stderr, "  [PROBLEM] no header record -- cannot "
				"confirm checkpoint identity\n");
		}
		else if (header_trace_hash != expect_trace_hash)
		{
			problems++;
			fprintf(stderr, "  [PROBLEM] STALE checkpoint: trace_hash "
				"%s does not match the current native trace file(s) "
				"(%s)\n", header_trace_hash.c_str(),
				expect_trace_hash.c_str());
		}
		else if (header_config_hash != expect_config_hash)
		{
			problems++;
			fprintf(stderr, "  [PROBLEM] STALE checkpoint: config_hash "
				"%s does not match the current sweep config (%s)\n",
				header_config_hash.c_str(), expect_config_hash.c_str());
		}
		else
			fprintf(stderr, "  header matches current trace/config -- "
				"checkpoint is NOT stale\n");
	}

	/* Duplicate id detection -- keep the LAST occurrence, exactly what
	 * main.cpp's dedupe-on-rebuild logic does. */
	std::map<std::string, std::string>	by_id;
	std::vector<std::string>			order;
	std::map<std::string, int>			id_count;

	for (const raw_record_t &r : records)
	{
		if (!r.checksum_ok)
			continue ;	/* already counted as truncated above */
		id_count[r.id]++;
		if (by_id.find(r.id) == by_id.end())
			order.push_back(r.id);
		by_id[r.id] = r.row;
	}
	int	dup_ids = 0;
	for (const auto &kv : id_count)
		if (kv.second > 1)
			dup_ids++;
	if (dup_ids > 0)
	{
		problems++;
		fprintf(stderr, "  [PROBLEM] %d scenario id(s) have duplicate "
			"checkpoint records\n", dup_ids);
	}

	/* Cross-check against the CSV. */
	std::vector<std::string>	csv_rows = read_csv_data_lines(csv_path);
	std::set<std::string>		csv_ids;

	for (const std::string &row : csv_rows)
	{
		std::string	id = id_from_csv_row(row);

		if (id.empty())
		{
			problems++;
			fprintf(stderr, "  [PROBLEM] CSV row failed to parse an "
				"id (truncated?): %.80s...\n", row.c_str());
			continue ;
		}
		csv_ids.insert(id);
		if (by_id.find(id) == by_id.end())
		{
			problems++;
			fprintf(stderr, "  [PROBLEM] STALE CSV row (no backing "
				"checkpoint record): %s\n", id.c_str());
		}
	}
	int	missing_from_csv = 0;
	for (const std::string &id : order)
		if (csv_ids.find(id) == csv_ids.end())
			missing_from_csv++;
	if (missing_from_csv > 0)
	{
		problems++;
		fprintf(stderr, "  [PROBLEM] %d checkpoint record(s) have no "
			"matching CSV row\n", missing_from_csv);
	}

	fprintf(stderr, "== %d problem(s) found; %zu unique scenarios in "
		"checkpoint, %zu CSV data rows ==\n", problems, order.size(),
		csv_rows.size());

	if (!repair)
		return (problems > 0 ? 1 : 0);

	fprintf(stderr, "-- repairing --\n");
	std::string	tmp_ckpt = ckpt_path + ".repair.tmp";
	FILE		*out = fopen(tmp_ckpt.c_str(), "w");

	if (out == nullptr)
	{
		fprintf(stderr, "failed to open %s for writing\n",
			tmp_ckpt.c_str());
		return (1);
	}
	if (header_present)
		fprintf(out, "{\"record\":\"header\",\"trace_hash\":\"%s\","
			"\"config_hash\":\"%s\"}\n", header_trace_hash.c_str(),
			header_config_hash.c_str());
	for (const std::string &id : order)
		fprintf(out, "{\"record\":\"scenario\",\"id\":\"%s\",\"row\":\"%s\"}\n",
			id.c_str(), by_id.at(id).c_str());
	if (sweep_complete)
		fprintf(out, "{\"record\":\"sweep_complete\"}\n");
	fclose(out);
	if (rename(tmp_ckpt.c_str(), ckpt_path.c_str()) != 0)
	{
		fprintf(stderr, "failed to replace %s\n", ckpt_path.c_str());
		return (1);
	}

	std::string	tmp_csv = csv_path + ".repair.tmp";
	FILE		*csvf = fopen(tmp_csv.c_str(), "w");

	if (csvf == nullptr)
	{
		fprintf(stderr, "failed to open %s for writing\n", tmp_csv.c_str());
		return (1);
	}
	/* Header line is preserved from the original CSV (this tool
	 * doesn't know the schema well enough to reconstruct it, only the
	 * data rows -- if the header itself is missing/corrupt that's a
	 * separate, more serious problem outside this tool's scope). */
	{
		FILE	*orig = fopen(csv_path.c_str(), "r");
		char	line[8192];

		if (orig != nullptr && fgets(line, sizeof(line), orig) != nullptr)
			fputs(line, csvf);
		if (orig != nullptr)
			fclose(orig);
	}
	for (const std::string &id : order)
		fprintf(csvf, "%s\n", by_id.at(id).c_str());
	fclose(csvf);
	if (rename(tmp_csv.c_str(), csv_path.c_str()) != 0)
	{
		fprintf(stderr, "failed to replace %s\n", csv_path.c_str());
		return (1);
	}

	fprintf(stderr, "-- repaired: %zu unique scenario records written to "
		"both files --\n", order.size());
	return (0);
}
