#!/usr/bin/env bash
# Phase 6.5 item 14: automated interrupted/resumed sweep + corrupted-
# checkpoint rejection test, against the REAL membrane-kv-exact-sim
# and membrane-kv-exact-sim-verify binaries and a REAL (small, already
# in-repo) native trace file -- not a mock. Kept fast by exploiting
# the two analytical comparisons (full-scan-cxl, compressed-full-scan-
# cxl), which complete near-instantly with no calibrate() call, so
# this never has to wait out a real 130560-step calibration to prove
# the interruption/resume/integrity machinery works.
set -euo pipefail

EXACT_SIM_BIN="$1"
VERIFY_BIN="$2"
NATIVE_TRACE="$3"
REPO_ROOT="$4"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

# main.cpp's out-of-core synthetic trace path
# (benchmarks/cxl-sim/traces/<model>-unified-128k.attntrace3) is
# relative to the CURRENT WORKING DIRECTORY, not configurable via CLI
# -- ctest itself runs from the build directory, not the repo root,
# so this must cd there first (the same way every manual invocation
# in this project's docs/history already assumed running from repo
# root).
cd "$REPO_ROOT"

CSV="$SCRATCH/sweep.csv"
TAIL_CSV="$SCRATCH/tail.csv"
CKPT="$SCRATCH/sweep.ckpt"

echo "== phase 1: run briefly, kill before any real (non-analytical) scenario forces long compute =="
"$EXACT_SIM_BIN" --trace-135m-long "$NATIVE_TRACE" \
	--out "$CSV" --tail-out "$TAIL_CSV" --checkpoint "$CKPT" \
	--workers 1 --memory-budget-mib 256 --chunk-steps 512 \
	> "$SCRATCH/run1.log" 2>&1 &
PID=$!
# The two analytical comparisons x 3 precisions = 6 rows complete
# almost immediately on a Release build once the ~431 MiB out-of-core
# synthetic trace is generated -- but that generation step's real wall
# time depends on the machine (slower under ASan/TSan interceptors on
# every zlib call, and a real, measured CI-runner-vs-dev-machine gap:
# a fixed `sleep 45` here reliably produced 0 completed scenario
# records on GitHub's hosted runner even under a plain Debug build,
# not just under sanitizers). Poll for the first real checkpoint
# record instead of guessing a wall-clock constant, so this test is
# correct on whatever hardware it runs on rather than tuned to one
# machine.
POLL_TIMEOUT_S=240
elapsed=0
found=0
while [ "$elapsed" -lt "$POLL_TIMEOUT_S" ]; do
	if [ -s "$CKPT" ] && grep -q '"record":"scenario"' "$CKPT" 2>/dev/null; then
		found=1
		break
	fi
	if ! kill -0 "$PID" 2>/dev/null; then
		echo "FAIL: process exited on its own before any scenario completed -- see log below"
		cat "$SCRATCH/run1.log"
		exit 1
	fi
	sleep 1
	elapsed=$((elapsed + 1))
done
if [ "$found" -eq 0 ]; then
	echo "FAIL: no scenario record appeared within ${POLL_TIMEOUT_S}s -- see log below"
	cat "$SCRATCH/run1.log"
	kill -KILL "$PID" 2>/dev/null || true
	wait "$PID" 2>/dev/null || true
	exit 1
fi
# Give a brief extra moment to let a few more of the near-instant
# analytical rows land (there are 6 total per model), then kill hard
# (SIGKILL, the worst case -- no graceful shutdown at all) to prove
# the checkpoint/CSV survive an ungraceful death, not just a clean
# exit.
sleep 2
kill -KILL "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

if [ ! -s "$CKPT" ]; then
	echo "FAIL: checkpoint is empty after phase 1 -- nothing to resume from"
	exit 1
fi
completed_after_kill=$(grep -c '"record":"scenario"' "$CKPT" || true)
echo "checkpoint has $completed_after_kill scenario record(s) after SIGKILL"
if [ "$completed_after_kill" -lt 1 ]; then
	echo "FAIL: expected at least 1 completed scenario before the kill"
	exit 1
fi

echo "== phase 2: verify the CSV survived the SIGKILL too (the real fflush bug this test guards against) =="
csv_rows=$(($(wc -l < "$CSV") - 1))
if [ "$csv_rows" -ne "$completed_after_kill" ]; then
	echo "FAIL: CSV has $csv_rows data row(s) but checkpoint has $completed_after_kill -- the CSV did not durably capture what the checkpoint has (a real bug this exact scenario caught during Phase 6.5 development)"
	exit 1
fi

echo "== phase 3: resume and confirm it recognizes prior progress, doesn't restart from 0 =="
"$EXACT_SIM_BIN" --trace-135m-long "$NATIVE_TRACE" \
	--out "$CSV" --tail-out "$TAIL_CSV" --checkpoint "$CKPT" \
	--workers 1 --memory-budget-mib 256 --chunk-steps 512 \
	> "$SCRATCH/run2.log" 2>&1 &
PID2=$!
# Same reasoning as phase 1: poll for the startup line instead of a
# fixed sleep, since how long a fresh process takes to open the
# checkpoint/trace and print its startup line is machine-dependent.
elapsed=0
while [ "$elapsed" -lt 60 ]; do
	if grep -q "already complete)" "$SCRATCH/run2.log" 2>/dev/null; then
		break
	fi
	if ! kill -0 "$PID2" 2>/dev/null; then
		break
	fi
	sleep 1
	elapsed=$((elapsed + 1))
done
kill -TERM "$PID2" 2>/dev/null || true
wait "$PID2" 2>/dev/null || true

if ! grep -q "already complete)" "$SCRATCH/run2.log"; then
	echo "FAIL: resumed run did not print its usual already-complete-count startup line"
	cat "$SCRATCH/run2.log"
	exit 1
fi
if grep -qE "\((0|) already complete\)" "$SCRATCH/run2.log"; then
	echo "FAIL: resumed run reports 0 already complete -- checkpoint was not honored"
	exit 1
fi
echo "resume correctly reported prior progress:"
grep "already complete)" "$SCRATCH/run2.log"

echo "== phase 4: integrity tool reports zero problems on the real (interrupted-then-resumed) artifacts =="
if ! "$VERIFY_BIN" --csv "$CSV" --checkpoint "$CKPT" --trace-135m-long "$NATIVE_TRACE"; then
	echo "FAIL: integrity tool found real problems in artifacts that should be clean"
	exit 1
fi

echo "== phase 5: corrupt one checkpoint line's completion_checksum, confirm the tool catches it and --repair fixes it =="
python3 - "$CKPT" << 'PYEOF'
import sys
path = sys.argv[1]
with open(path) as f:
	lines = f.readlines()
for i, line in enumerate(lines):
	if '"record":"scenario"' in line and line.rstrip().endswith('"}'):
		# flip the last hex digit of the trailing completion_checksum
		idx = line.rfind('"}')
		corrupted = line[:idx-1] + ('0' if line[idx-1] != '0' else '1') + line[idx:]
		lines[i] = corrupted
		break
with open(path, 'w') as f:
	f.writelines(lines)
PYEOF

if "$VERIFY_BIN" --csv "$CSV" --checkpoint "$CKPT" --trace-135m-long "$NATIVE_TRACE"; then
	echo "FAIL: integrity tool did not detect the deliberately corrupted checksum"
	exit 1
fi
echo "corruption correctly detected (tool exited nonzero, as expected)"

"$VERIFY_BIN" --csv "$CSV" --checkpoint "$CKPT" --trace-135m-long "$NATIVE_TRACE" --repair
if ! "$VERIFY_BIN" --csv "$CSV" --checkpoint "$CKPT" --trace-135m-long "$NATIVE_TRACE"; then
	echo "FAIL: --repair did not produce a clean pair"
	exit 1
fi
echo "repair correctly restored a clean (CSV, checkpoint) pair"

echo "== ALL PHASES PASSED =="
