#!/usr/bin/env python3
"""Re-hash every file in provenance/import-manifest.json and confirm it
still matches the destination_sha256 recorded at migration time -- the
mechanical half of this repository's own provenance guarantee (the
narrative half is provenance/source-map.md).

Also does a lightweight structural pass over every results/canonical/
and results/schemas/ tree under experiments/: every .json file must
parse, every .csv file must have a non-empty header and at least one
data row (a 0-byte or header-only CSV most often means a synthesis
timeout was mistakenly treated as an empty-but-valid result -- see
each experiment's own methodology.md UNAVAILABLE convention).

Exit code: 0 if every check passes, 1 otherwise.
"""
import csv
import hashlib
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
FAILURES = []
CHECK_COUNT = 0


def check(label, ok, detail):
    global CHECK_COUNT
    CHECK_COUNT += 1
    tag = "PASS" if ok else "FAIL"
    print(f"[{tag}] {label}: {detail}")
    if not ok:
        FAILURES.append(label)


def verify_import_manifest():
    manifest_path = REPO_ROOT / "provenance" / "import-manifest.json"
    if not manifest_path.is_file():
        check("provenance/import-manifest.json exists", False, "missing")
        return
    manifest = json.loads(manifest_path.read_text())
    entries = manifest.get("entries", [])
    mismatches = []
    missing = []
    for e in entries:
        dest = REPO_ROOT / e["destination_path"]
        expected = e.get("destination_sha256")
        if not expected:
            continue
        if not dest.is_file():
            missing.append(e["destination_path"])
            continue
        actual = hashlib.sha256(dest.read_bytes()).hexdigest()
        if actual != expected:
            mismatches.append(e["destination_path"])
    check(
        f"{len(entries)} import-manifest entries re-hash to their recorded destination_sha256",
        not mismatches and not missing,
        f"{len(entries) - len(mismatches) - len(missing)} verified, "
        f"{len(mismatches)} hash mismatch(es), {len(missing)} missing on disk",
    )
    for p in mismatches:
        print(f"       mismatch: {p}")
    for p in missing:
        print(f"       missing:  {p}")


def structural_check_results():
    json_bad = []
    json_ok = 0
    csv_bad = []
    csv_ok = 0
    for base in REPO_ROOT.glob("experiments/*/results"):
        for sub in ("canonical", "schemas"):
            d = base / sub
            if not d.is_dir():
                continue
            for f in d.rglob("*.json"):
                try:
                    json.loads(f.read_text())
                    json_ok += 1
                except json.JSONDecodeError as exc:
                    json_bad.append(f"{f.relative_to(REPO_ROOT)}: {exc}")
            for f in d.rglob("*.csv"):
                try:
                    with f.open(newline="") as fh:
                        rows = list(csv.reader(fh))
                    if not rows or not rows[0]:
                        csv_bad.append(f"{f.relative_to(REPO_ROOT)}: no header row")
                    elif len(rows) < 2:
                        csv_bad.append(f"{f.relative_to(REPO_ROOT)}: header only, no data rows")
                    else:
                        csv_ok += 1
                except csv.Error as exc:
                    csv_bad.append(f"{f.relative_to(REPO_ROOT)}: {exc}")
    check(
        f"{json_ok + len(json_bad)} results JSON files parse cleanly",
        not json_bad,
        f"{json_ok} ok, {len(json_bad)} bad" + ("" if not json_bad else f" ({json_bad[0]})"),
    )
    check(
        f"{csv_ok + len(csv_bad)} results CSV files have a header and at least one data row",
        not csv_bad,
        f"{csv_ok} ok, {len(csv_bad)} bad" + ("" if not csv_bad else f" ({csv_bad[0]})"),
    )
    for m in json_bad[1:]:
        print(f"       {m}")
    for m in csv_bad[1:]:
        print(f"       {m}")


def main():
    verify_import_manifest()
    structural_check_results()
    print()
    print(f"{CHECK_COUNT - len(FAILURES)}/{CHECK_COUNT} checks passed")
    sys.exit(1 if FAILURES else 0)


if __name__ == "__main__":
    main()
