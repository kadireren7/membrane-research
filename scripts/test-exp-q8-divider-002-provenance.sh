#!/usr/bin/env bash
#
# Regression tests for the provenance-safety fix in
# scripts/run-exp-q8-divider-002.sh (EXP-FPGA-DIV-002 Phase B4, task
# item 2's own explicit regression-test requirement). Proves, with real
# invocations of the real script (not a simulation of it):
#
#   1. `--quick` leaves the committed canonical results/ files byte-
#      identical (the actual defect Phase B3's own reproduction found).
#   2. `--promote-results` on a quick run is refused before any work
#      starts (no side effects).
#   3. A failed run (failures != 0) cannot pass
#      scripts/verify-exp-q8-divider-002-results.py's own canonical-
#      promotion gate.
#   4. A manifest claiming canonical=true with run_mode=quick is
#      rejected by the same validator ("quick artifact presented as
#      canonical", task item 2's own first listed rejection reason).
#   5. Killing a `--full --promote-results` run before it completes
#      cannot leave a partial write in the canonical directory (no
#      `.incoming-*` staging directory, no modified canonical file).
#
# Exits 0 only if every test passes. Run with:
#   scripts/test-exp-q8-divider-002-provenance.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

RUN_SCRIPT="$REPO_ROOT/scripts/run-exp-q8-divider-002.sh"
VALIDATOR="$REPO_ROOT/scripts/verify-exp-q8-divider-002-results.py"
CANON="$REPO_ROOT/experiments/EXP-FPGA-DIV-002/results"
TMPBASE="$(mktemp -d)"
trap 'rm -rf "$TMPBASE"' EXIT

PASS=0
FAIL=0
ok() { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

canon_hash() {
	# Every file this phase's own promotion could touch, hashed and
	# sorted -- deliberately includes phase-b3.md (hand-written, never
	# promoted) as a canary: if a future change ever widens promotion to
	# touch hand-written docs by accident, this test starts failing too.
	{
		sha256sum "$CANON"/b3-*.csv "$CANON"/b3-*.json "$CANON"/b3-*.md "$REPO_ROOT/experiments/EXP-FPGA-DIV-002/phase-b3.md" 2>/dev/null || true
	} | sort -k2
}

echo "=== test-exp-q8-divider-002-provenance: 5 tests ==="

# ---- Test 1: --quick leaves canonical results byte-identical ----
before1="$(canon_hash)"
RUN_ID_1="prov-test-quick-identical-$$"
if bash "$RUN_SCRIPT" --phase b3 --quick --run-id "$RUN_ID_1" >"$TMPBASE/t1.log" 2>&1; then
	after1="$(canon_hash)"
	if [ "$before1" = "$after1" ]; then
		ok "1: --phase b3 --quick leaves canonical results byte-identical"
	else
		bad "1: canonical results CHANGED after --phase b3 --quick (see $TMPBASE/t1.log)"
		diff <(echo "$before1") <(echo "$after1") || true
	fi
else
	bad "1: --phase b3 --quick itself failed (see $TMPBASE/t1.log) -- cannot evaluate byte-identity"
fi

# ---- Test 2: --promote-results on a quick run is refused up front ----
before2="$(canon_hash)"
RUN_ID_2="prov-test-quick-promote-refused-$$"
set +e
bash "$RUN_SCRIPT" --phase b3 --quick --promote-results --run-id "$RUN_ID_2" >"$TMPBASE/t2.log" 2>&1
rc2=$?
set -e
after2="$(canon_hash)"
if [ "$rc2" -ne 0 ] && [ "$before2" = "$after2" ] && grep -q "requires --full" "$TMPBASE/t2.log"; then
	ok "2: --phase b3 --quick --promote-results refused before any work started, canonical untouched"
else
	bad "2: quick+promote-results was not cleanly refused (rc=$rc2, see $TMPBASE/t2.log)"
fi

# ---- Test 3: a failed run cannot pass the validator's canonical gate ----
STAGE3="$TMPBASE/staged-fail"
mkdir -p "$STAGE3"
echo '{"fake":"result"}' >"$STAGE3/b3-correctness.json"
python3 - "$STAGE3" <<'PYEOF'
import hashlib, json, sys
staging = sys.argv[1]
p = f"{staging}/b3-correctness.json"
h = hashlib.sha256(open(p, "rb").read()).hexdigest()
import os
rel = os.path.relpath(p, start=os.path.dirname(os.path.dirname(os.path.dirname(staging))) if False else None) if False else None
PYEOF
REPO_REL_PATH="$(python3 -c "import os; print(os.path.relpath('$STAGE3/b3-correctness.json', '$REPO_ROOT'))")"
FILEHASH="$(sha256sum "$STAGE3/b3-correctness.json" | awk '{print $1}')"
cat >"$STAGE3/run-manifest.json" <<EOF
{
  "experiment_id": "EXP-FPGA-DIV-002", "phase": "b3", "variant": ["baseline"],
  "run_mode": "full", "canonical": false,
  "git_commit": "0000000000000000000000000000000000000000", "git_dirty": false,
  "branch": "test", "started_at": "2026-01-01T00:00:00Z", "completed_at": "2026-01-01T00:01:00Z",
  "hostname": "test", "tool_versions": {"yosys": "Yosys 0.33", "verilator": "Verilator 5.020"},
  "command": "test", "run_id": "test", "seeds": "test",
  "expected_transactions": 30000000, "completed_transactions": 30000000,
  "failures": 3, "status": "FAIL",
  "source_file_hashes": {}, "source_file_hashes_blob": "",
  "result_file_hashes": {"$REPO_REL_PATH": "$FILEHASH"}, "result_file_hashes_blob": ""
}
EOF
set +e
out3="$(python3 "$VALIDATOR" --staging "$STAGE3" --phase b3 --require-canonical=false 2>&1)"
rc3=$?
set -e
if [ "$rc3" -ne 0 ] && echo "$out3" | grep -q "failed test count"; then
	ok "3: a run with failures=3/status=FAIL is rejected by the validator (failed test count)"
else
	bad "3: validator did NOT reject a failed run as expected (rc=$rc3): $out3"
fi

# ---- Test 4: quick artifact presented as canonical is rejected ----
STAGE4="$TMPBASE/staged-quick-as-canonical"
mkdir -p "$STAGE4"
echo '{"fake":"result"}' >"$STAGE4/b3-correctness.json"
REPO_REL_PATH4="$(python3 -c "import os; print(os.path.relpath('$STAGE4/b3-correctness.json', '$REPO_ROOT'))")"
FILEHASH4="$(sha256sum "$STAGE4/b3-correctness.json" | awk '{print $1}')"
cat >"$STAGE4/run-manifest.json" <<EOF
{
  "experiment_id": "EXP-FPGA-DIV-002", "phase": "b3", "variant": ["baseline"],
  "run_mode": "quick", "canonical": true,
  "git_commit": "0000000000000000000000000000000000000000", "git_dirty": false,
  "branch": "test", "started_at": "2026-01-01T00:00:00Z", "completed_at": "2026-01-01T00:01:00Z",
  "hostname": "test", "tool_versions": {"yosys": "Yosys 0.33", "verilator": "Verilator 5.020"},
  "command": "test", "run_id": "test", "seeds": "test",
  "expected_transactions": 100, "completed_transactions": 100,
  "failures": 0, "status": "PASS",
  "source_file_hashes": {}, "source_file_hashes_blob": "",
  "result_file_hashes": {"$REPO_REL_PATH4": "$FILEHASH4"}, "result_file_hashes_blob": ""
}
EOF
set +e
out4="$(python3 "$VALIDATOR" --staging "$STAGE4" --phase b3 --require-canonical=false 2>&1)"
rc4=$?
set -e
if [ "$rc4" -ne 0 ] && echo "$out4" | grep -q "quick artifact presented as canonical"; then
	ok "4: canonical=true with run_mode=quick is rejected (quick artifact presented as canonical)"
else
	bad "4: validator did NOT reject a quick-as-canonical manifest as expected (rc=$rc4): $out4"
fi

# ---- Test 5: killing a --full --promote-results run early leaves no partial canonical write ----
before5="$(canon_hash)"
RUN_ID_5="prov-test-interrupted-$$"
set +e
timeout -s KILL 3 bash "$RUN_SCRIPT" --phase b3 --full --promote-results --run-id "$RUN_ID_5" >"$TMPBASE/t5.log" 2>&1
rc5=$?
set -e
after5="$(canon_hash)"
incoming_count="$(find "$CANON" -maxdepth 1 -name '.incoming-b3-*' 2>/dev/null | wc -l)"
manifest_exists=0
[ -f "$REPO_ROOT/build-exp-q8-divider-002/b3/full/$RUN_ID_5/run-manifest.json" ] && manifest_exists=1
if [ "$rc5" -ne 0 ] && [ "$before5" = "$after5" ] && [ "$incoming_count" -eq 0 ] && [ "$manifest_exists" -eq 0 ]; then
	ok "5: killing --phase b3 --full --promote-results after 3s left canonical untouched, no .incoming-* leftover, no manifest written (run never reached completion)"
else
	bad "5: interrupted run left unexpected state (rc=$rc5, incoming_count=$incoming_count, manifest_exists=$manifest_exists, canon changed=$([ "$before5" = "$after5" ] && echo no || echo YES))"
fi
rm -rf "$REPO_ROOT/build-exp-q8-divider-002/b3/full/$RUN_ID_5" "$REPO_ROOT/build-exp-q8-divider-002/b3/quick/$RUN_ID_1" "$REPO_ROOT/build-exp-q8-divider-002/b3/quick/$RUN_ID_2"

echo
echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
