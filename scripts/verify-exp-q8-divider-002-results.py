#!/usr/bin/env python3
"""Provenance validator for EXP-FPGA-DIV-002 result artifacts (task item 2).

Two ways to invoke:

  Staging validation (used by run-exp-q8-divider-002.sh's own
  promote_results(), BEFORE anything is copied into the canonical
  experiments/EXP-FPGA-DIV-002/results/ directory):

    verify-exp-q8-divider-002-results.py --staging <dir> \\
        --manifest <dir>/run-manifest.json --phase b4 \\
        --require-canonical=false [--expect-git-commit <sha>]

  Canonical validation (checks the REAL committed results/ directory
  against its own promotion record -- run standalone, or as part of
  item 14's own verification pass):

    verify-exp-q8-divider-002-results.py --canonical --phase b4

Every rejection reason from task item 2's own explicit list is checked
independently and ALL are reported in one pass (not just the first
failure), then the script exits non-zero if any fired.
"""
import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Per-phase minimum combined completed-transaction thresholds. b3's own
# task set a 5,000,000-per-candidate minimum across 6 candidates; b4's own
# task (item 8) sets an 8,000,000-per-candidate minimum across 7 candidates
# (baseline/B1/B2/B3-split/R1/R2/R3-or-R1+R3).
DEFAULT_MIN_TRANSACTIONS = {
	"a": 1,
	"b1": 1,
	"b2": 1,
	"b3": 30_000_000,
	"b4": 56_000_000,
}

REQUIRED_TOOLS = ("yosys", "verilator")


def sha256_of(path):
	h = hashlib.sha256()
	with open(path, "rb") as f:
		for chunk in iter(lambda: f.read(1 << 20), b""):
			h.update(chunk)
	return h.hexdigest()


def load_json(path, reasons, label):
	try:
		return json.loads(Path(path).read_text())
	except Exception as e:  # noqa: BLE001 - report every parse failure, not just the first
		reasons.append(f"malformed JSON: {label} ({path}): {e}")
		return None


def validate_csv_files(paths, reasons):
	for p in paths:
		if not str(p).endswith(".csv"):
			continue
		try:
			with open(p, newline="") as f:
				reader = csv.DictReader(f)
				header = reader.fieldnames
				if not header:
					reasons.append(f"malformed CSV: {p} has no header row")
					continue
				n = 0
				for row in reader:
					n += 1
					if None in row:
						reasons.append(f"malformed CSV: {p} row {n} has more fields than the header")
						break
				if n == 0:
					reasons.append(f"malformed CSV: {p} has a header but 0 data rows")
		except Exception as e:  # noqa: BLE001
			reasons.append(f"malformed CSV: {p}: {e}")


def validate_manifest_fields(m, reasons, *, phase, require_canonical, min_txn, expect_git_commit):
	if m is None:
		return

	run_mode = m.get("run_mode")
	canonical = m.get("canonical")
	if canonical is True and run_mode != "full":
		reasons.append(
			f"quick artifact presented as canonical: canonical=true but run_mode={run_mode!r} (must be 'full')"
		)
	if require_canonical and canonical is not True:
		reasons.append("canonical result without promotion record: manifest's own canonical field is not true")

	completed = m.get("completed_transactions")
	if not isinstance(completed, int) or completed < min_txn:
		reasons.append(
			f"transaction count below required threshold: completed_transactions={completed!r} < required {min_txn}"
		)

	if expect_git_commit is not None and m.get("git_commit") != expect_git_commit:
		reasons.append(
			f"mismatched git commit: manifest has {m.get('git_commit')!r}, expected {expect_git_commit!r}"
		)

	tool_versions = m.get("tool_versions") or {}
	for tool in REQUIRED_TOOLS:
		if not tool_versions.get(tool):
			reasons.append(f"missing tool version: {tool!r} not recorded (or empty) in tool_versions")

	if m.get("status") != "PASS":
		reasons.append(f"incomplete run: status={m.get('status')!r} (must be 'PASS')")
	if not m.get("started_at") or not m.get("completed_at"):
		reasons.append("incomplete run: missing started_at/completed_at timestamp")

	failures = m.get("failures")
	if not isinstance(failures, int) or failures != 0:
		reasons.append(f"failed test count: failures={failures!r} (must be exactly 0)")

	for required in (
		"experiment_id", "phase", "variant", "git_commit", "branch",
		"hostname", "command", "run_id", "seeds", "expected_transactions",
		"source_file_hashes", "result_file_hashes",
	):
		if required not in m:
			reasons.append(f"incomplete run: manifest missing required field {required!r}")

	if m.get("phase") != phase:
		reasons.append(f"incomplete run: manifest phase={m.get('phase')!r} does not match --phase {phase!r}")


def validate_result_hashes(m, reasons):
	if m is None:
		return []
	result_hashes = m.get("result_file_hashes") or {}
	paths = []
	for rel_path, expected_hash in result_hashes.items():
		abs_path = REPO_ROOT / rel_path
		paths.append(abs_path)
		if not abs_path.is_file():
			reasons.append(f"hash mismatch: {rel_path} listed in manifest but missing on disk")
			continue
		actual_hash = sha256_of(abs_path)
		if actual_hash != expected_hash:
			reasons.append(
				f"hash mismatch: {rel_path} sha256={actual_hash} but manifest recorded {expected_hash}"
			)
	return paths


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--staging", help="directory holding a run's staged results + run-manifest.json")
	ap.add_argument("--manifest", help="explicit path to the manifest to validate (defaults to <staging>/run-manifest.json)")
	ap.add_argument("--canonical", action="store_true", help="validate the real committed experiments/EXP-FPGA-DIV-002/results/ directory instead of a staging directory")
	ap.add_argument("--phase", required=True)
	ap.add_argument("--require-canonical", default=None, help="true/false; defaults to the value of --canonical")
	ap.add_argument("--min-transactions", type=int, default=None)
	ap.add_argument("--expect-git-commit", default=None)
	args = ap.parse_args()

	reasons = []
	min_txn = args.min_transactions if args.min_transactions is not None else DEFAULT_MIN_TRANSACTIONS.get(args.phase, 1)

	if args.canonical:
		results_dir = REPO_ROOT / "experiments" / "EXP-FPGA-DIV-002" / "results" / "canonical"
		manifest_path = results_dir / f"{args.phase}-promotion-record.json"
		require_canonical = True if args.require_canonical is None else args.require_canonical.lower() == "true"
		if not manifest_path.is_file():
			print(f"REJECTED: canonical result without promotion record: {manifest_path} does not exist")
			sys.exit(1)
	else:
		if not args.staging:
			print("error: --staging <dir> or --canonical is required", file=sys.stderr)
			sys.exit(2)
		results_dir = Path(args.staging)
		manifest_path = Path(args.manifest) if args.manifest else results_dir / "run-manifest.json"
		require_canonical = False if args.require_canonical is None else args.require_canonical.lower() == "true"
		if not manifest_path.is_file():
			print(f"REJECTED: incomplete run: no manifest found at {manifest_path}")
			sys.exit(1)

	manifest = load_json(manifest_path, reasons, "manifest")
	validate_manifest_fields(
		manifest, reasons,
		phase=args.phase, require_canonical=require_canonical,
		min_txn=min_txn, expect_git_commit=args.expect_git_commit,
	)
	hashed_paths = validate_result_hashes(manifest, reasons)
	validate_csv_files(hashed_paths, reasons)
	for p in hashed_paths:
		if str(p).endswith(".json") and p != manifest_path:
			load_json(p, reasons, str(p))

	if reasons:
		print(f"INVALID ({len(reasons)} issue(s)):")
		for r in reasons:
			print(f"  REJECTED: {r}")
		sys.exit(1)

	print(f"VALID: {manifest_path} (phase={args.phase} canonical={manifest.get('canonical')} completed_transactions={manifest.get('completed_transactions')})")
	sys.exit(0)


if __name__ == "__main__":
	main()
