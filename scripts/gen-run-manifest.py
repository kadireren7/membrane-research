#!/usr/bin/env python3
"""Write a provenance manifest for one scripts/run-exp-q8-divider-002.sh
invocation (task item 2 -- run-manifest.json / <phase>-promotion-record.json).

Not phase-specific: called by the shell script's own write_run_manifest()
and promote_results() helpers with the fields those functions already
computed (git commit/branch, tool versions, transaction counts, etc). This
script's only job is to assemble them into one schema-stable JSON document
and parse the "hash  path" hash-list files produced by the shell script's
own hash_tree() function into both a structured dict (for
verify-exp-q8-divider-002-results.py) and a raw blob string (for the
shell script's own byte-identical before/after comparisons at promotion
time -- see promote_results()'s source- and result-file-integrity checks).
"""
import argparse
import json
import sys


def parse_bool(s):
	if s.lower() in ("true", "1", "yes"):
		return True
	if s.lower() in ("false", "0", "no"):
		return False
	raise argparse.ArgumentTypeError(f"expected true/false, got {s!r}")


def parse_hash_file(path):
	text = open(path).read()
	d = {}
	for line in text.splitlines():
		line = line.rstrip("\n")
		if not line.strip():
			continue
		h, p = line.split("  ", 1)
		d[p] = h
	return d, text


def parse_kv_list(items):
	d = {}
	for item in items:
		k, _, v = item.partition("=")
		d[k] = v
	return d


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--out", required=True)
	ap.add_argument("--experiment-id", required=True)
	ap.add_argument("--phase", required=True)
	ap.add_argument("--variants", required=True, help="space-separated variant list")
	ap.add_argument("--run-mode", required=True, choices=["quick", "full"])
	ap.add_argument("--canonical", required=True, type=parse_bool)
	ap.add_argument("--git-commit", required=True)
	ap.add_argument("--git-dirty", required=True, type=parse_bool)
	ap.add_argument("--branch", required=True)
	ap.add_argument("--started-at", required=True)
	ap.add_argument("--completed-at", required=True)
	ap.add_argument("--hostname", required=True)
	ap.add_argument("--tool-versions", required=True, nargs="+")
	ap.add_argument("--command", required=True)
	ap.add_argument("--run-id", required=True)
	ap.add_argument("--expected-transactions", required=True, type=int)
	ap.add_argument("--completed-transactions", required=True, type=int)
	ap.add_argument("--failures", required=True, type=int)
	ap.add_argument("--status", required=True, choices=["PASS", "FAIL"])
	ap.add_argument("--source-hashes-file", required=True)
	ap.add_argument("--result-hashes-file", required=True)
	ap.add_argument(
		"--seeds",
		default="mt19937(0xC0FFEE) transaction-stream, mt19937(0xC0FFEE) mode-select stream (tb_top_verilator_q8_b3_variant.cpp's own fixed, deterministic seeds -- same for every candidate run so 'identical seeds and traffic' is real, not assumed)",
	)
	args = ap.parse_args()

	source_hashes, source_blob = parse_hash_file(args.source_hashes_file)
	result_hashes, result_blob = parse_hash_file(args.result_hashes_file)

	manifest = {
		"experiment_id": args.experiment_id,
		"phase": args.phase,
		"variant": args.variants.split(),
		"run_mode": args.run_mode,
		"canonical": args.canonical,
		"git_commit": args.git_commit,
		"git_dirty": args.git_dirty,
		"branch": args.branch,
		"started_at": args.started_at,
		"completed_at": args.completed_at,
		"hostname": args.hostname,
		"tool_versions": parse_kv_list(args.tool_versions),
		"command": args.command,
		"run_id": args.run_id,
		"seeds": args.seeds,
		"expected_transactions": args.expected_transactions,
		"completed_transactions": args.completed_transactions,
		"failures": args.failures,
		"source_file_hashes": source_hashes,
		"source_file_hashes_blob": source_blob,
		"result_file_hashes": result_hashes,
		"result_file_hashes_blob": result_blob,
		"status": args.status,
	}

	with open(args.out, "w") as f:
		json.dump(manifest, f, indent=2, sort_keys=True)
		f.write("\n")
	print(f"wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
	main()
