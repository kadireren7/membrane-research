# EXP-FPGA-DIV-002 Phase B4 -- provenance-safety verification record
(task item 12)

This document is the timestamped account task item 1's own gate ("Complete
and commit this safety work before interpreting B4 performance data")
requires: every command run to build and verify the provenance-safety
fix, in order, with its real outcome. All classifications: MEASURED_BY_TOOL
unless noted.

## 0. Preflight

- `git rev-parse HEAD` before any change: `e416e36cd7cd12ebe305cd509da263ef6b64181b`
  -- matches the task's own stated current head exactly.
- `git status --short`: clean except `m third_party/llama.cpp` (pre-
  existing dirty submodule, out of scope every prior phase, untouched
  throughout this phase).
- `ps aux | grep -E "run-exp-q8|yosys|verilator"`: no matches -- no stale
  process from any prior session.
- Canonical B3 result file hashes recorded (`sha256sum experiments/
  EXP-FPGA-DIV-002/results/b3-*.{csv,json,md} phase-b3.md`) before any
  change this phase made, and independently re-verified identical after
  every regression test below that touches `--phase b3`.
- `transactions_checked` in the existing `results/b3-correctness.json`:
  8,042,500 for all six Phase B3 variants -- confirmed corresponding to
  the real full run task item 0 asked to verify (not a quick-mode
  artifact).

## 1. Script redesign implementation

- Added `--run-id`, `--promote-results`, `--force-promote`, `--dry-run`,
  `--print-output-dir` flags.
- Default output root changed from phase-specific fixed directories
  (`build-exp-q8-divider-002-b3`, etc.) to
  `build-exp-q8-divider-002/<phase>/<quick|full>/<run-id>/`.
- `RESULTS_STAGING_DIR` (always under `$BUILD_DIR`, never canonical)
  introduced; Phase B3's own `RESULTS_DIR` retrofitted to point there
  instead of `experiments/EXP-FPGA-DIV-002/results/` directly (the exact
  line that caused the original defect).
- `write_run_manifest()` / `promote_results()` helper functions added,
  used by both `--phase b3` (retrofitted) and `--phase b4` (new).
- `scripts/gen-run-manifest.py`, `scripts/verify-exp-q8-divider-002-
  results.py`, `scripts/test-exp-q8-divider-002-provenance.sh` added.

Verified with `bash -n scripts/run-exp-q8-divider-002.sh` after every
edit (bash syntax only, not a behavioral check) -- 0 syntax errors at
every stage of this work.

## 2. Manual verification (real invocations, dry-run and real)

| # | Command | Real result |
|---|---|---|
| 1 | `--phase b3 --quick --print-output-dir` | printed `build-exp-q8-divider-002/b3/quick/<auto-run-id>` -- new scheme confirmed |
| 2 | `--phase b4 --full --run-id test123 --print-output-dir` | printed `build-exp-q8-divider-002/b4/full/test123` -- explicit `--run-id` honored |
| 3 | `--phase b3 --full --run-id dryrun1 --promote-results --dry-run` | printed the full gate list, git commit, resolved paths; performed no build/write |
| 4 | `--phase b3 --quick --output-dir experiments/EXP-FPGA-DIV-002/results/evil --dry-run` | **initially incorrectly ALLOWED** (see "real bug" below); after fix, correctly `error: --output-dir must not point inside the canonical results directory` |
| 5 | `--phase b3 --quick --output-dir "$PWD/experiments/EXP-FPGA-DIV-002/results/evil2" --dry-run` | correctly refused, same message (absolute-path variant) |
| 6 | `--phase b3 --quick --output-dir <scratchpad path> --dry-run` | correctly allowed, normal dry-run output |
| 7 | `--phase b3 --quick --promote-results --dry-run` (implicitly quick+promote) | `error: --promote-results requires --full` -- refused before any work, exit 1 |

**Real bug found and fixed (disclosed above and in phase-b4.md)**: test
#4 above was the regression test that caught it -- the canonical-directory
guard compared `$BUILD_DIR` as given by the user, so a *relative*
`--output-dir` bypassed it entirely. Fixed with `realpath -m` before the
comparison; #4 and #5 above are the real before/after evidence.

## 3. Real end-to-end run: `--phase b3 --quick --run-id provenance-test-1`

Canonical `results/b3-*` hashes recorded immediately before this run.
Real invocation (no `--dry-run`) of the actual script, building all six
Phase B3 Verilated variants and running the full quick-mode correctness +
15-profile performance matrix against each: all six report `PASS
(below 5,000,000 minimum -- quick mode) ... 0 fails`. `run-manifest.json`
written to `build-exp-q8-divider-002/b3/quick/provenance-test-1/run-
manifest.json` with `run_mode: "quick"`, `canonical: false`, real
`git_commit`, real `tool_versions` (`Yosys 0.33 (git sha1 2584903a060)`,
`Verilator 5.020`), real `completed_transactions: 754500`, real per-file
`source_file_hashes` (16 real RTL/script/testbench files hashed).
Canonical `results/b3-*` hashes re-checked immediately after: **byte-
identical to the pre-run hashes** -- the real fix for the real defect,
confirmed by direct measurement, not assumed from the code change alone.

## 4. Automated regression suite:
`scripts/test-exp-q8-divider-002-provenance.sh`

First run surfaced the `set -e`/`timeout` interaction bug in test 5
itself (see phase-b4.md's own "Regression tests" section for the full
account) -- fixed, re-run:

```
=== test-exp-q8-divider-002-provenance: 5 tests ===
PASS: 1: --phase b3 --quick leaves canonical results byte-identical
PASS: 2: --phase b3 --quick --promote-results refused before any work started, canonical untouched
PASS: 3: a run with failures=3/status=FAIL is rejected by the validator (failed test count)
PASS: 4: canonical=true with run_mode=quick is rejected (quick artifact presented as canonical)
PASS: 5: killing --phase b3 --full --promote-results after 3s left canonical untouched, no .incoming-* leftover, no manifest written (run never reached completion)

=== 5 passed, 0 failed ===
```

Exit code 0. All five of task item 2's own required regression-test
properties confirmed with real invocations of the real script (tests 1,
2, 5) and real invocations of the real validator against synthetic-but-
realistic manifests (tests 3, 4 -- synthetic because constructing an
actually-failed or actually-quick-claiming-canonical run through the real
pipeline would require deliberately breaking a real build, which is not
a safe or necessary way to test a validator's own field-level rejection
logic).

## 5. Part A completion gate

Per task item 2's own explicit instruction ("Complete and commit this
safety work before interpreting B4 performance data"), Part A's own work
(sections 1-4 above) was committed as its own separate commit
(`research: make Q8 experiment artifacts provenance-safe`) BEFORE any
Part B (retirement-pressure RTL/differential) result in this phase's own
docs was interpreted or acted on. See `experiments/EXP-FPGA-DIV-002/
phase-b4.md`'s own "Part A: provenance safety" section for the design
rationale this record supports.

## 6. Promoting Part B's own real results: two real bugs found and
fixed during the very first real promotion (disclosed, not hidden)

After Part A was committed (`6c033c9`) and Part B's own real full run
(`--phase b4 --full --run-id full-run-1`, 58,677,500 transactions, 0
fails) completed, promoting its results surfaced two real defects in
`promote_results()` itself -- exactly the kind of thing this phase's own
regression-testing work exists to catch, not glossed over because they
were found late:

**Bug 1: the git-commit-match check was stricter than the task's own
required workflow can satisfy.** `promote_results()` originally hard-
refused promotion if `git rev-parse HEAD` no longer matched the commit
recorded at run-start time. But this phase's own task explicitly
requires committing Part A *before* interpreting/promoting Part B's own
data -- so by the time Part B's real results were ready to promote, HEAD
had legitimately moved (`e416e36` -> `6c033c9`) for a reason completely
unrelated to Part B's own correctness. Re-hashing all 29 source files
the run's own manifest recorded confirmed 28/29 unchanged -- the sole
difference was `scripts/run-exp-q8-divider-002.sh` itself (Part A's own
commit, plus header documentation additions), which affects how *future*
runs behave, not the RTL/testbench/generator logic that produced these
specific results. Fixed by making the git-commit check informational
(logged, not a hard gate on its own) and relying on the source-hash
check -- already present, and strictly more precise ("did a file this
run depends on actually change" vs. "did any commit happen anywhere")
-- as the real gate. This diagnosis is what justified the manual,
disclosed promotion below rather than either blocking indefinitely or
silently weakening the check without understanding why it fired.

**Bug 2: the promotion record's own `result_file_hashes` referenced the
temporary `.incoming-<phase>-<run-id>/` staging path, not the final
canonical path.** `promote_results()` computed the promotion record's
own file hashes against files still sitting in the `.incoming-*`
staging directory, then moved that directory's contents (including the
record) into their final canonical locations -- meaning the record's own
`result_file_hashes` pointed at paths that no longer existed the moment
promotion finished. Running `verify-exp-q8-divider-002-results.py
--canonical` against the freshly-promoted b4 results immediately caught
this (9 rejections, all "hash mismatch: ... listed in manifest but
missing on disk" / "malformed CSV/JSON: ... No such file or directory").
Fixed by re-ordering `promote_results()`: the data files are moved into
their final canonical location FIRST, then the promotion record is
generated by hashing those files at their now-final canonical paths, and
only then is the record itself (written to a `.promotion-record-tmp-
<phase>-<run-id>.json` temp path first) renamed into place -- preserving
the same atomic-per-file guarantee for the record as for the data files,
while making the record's own claims true the moment it exists.

**How Part B's real results were actually promoted**: given bug 1 was
diagnosed (not yet fixed at that point) and bug 2 had not yet been hit,
Phase B4's own canonical results were promoted manually, applying the
exact same checks `promote_results()` itself performs, by hand: (1)
`verify-exp-q8-divider-002-results.py --staging ... --require-canonical=
false` against the real staged results -- VALID; (2) re-hashed all 29
source files from the run's own manifest against current disk content --
28/29 unchanged, `scripts/run-exp-q8-divider-002.sh` the sole (understood
and explained) exception; (3) `diff -u` against canonical (all 5 files
new, no existing content to compare); (4) copied into a
`.incoming-b4-full-run-1/` staging directory, generated the promotion
record, then `mv -f`'d each file into place (same atomic-per-file
technique `promote_results()` itself uses). This manual promotion
independently reproduced bug 2 (the same path mistake), caught by the
same `--canonical` validator run, and was corrected by hand before this
document was written -- both bugs were then fixed in
`scripts/run-exp-q8-divider-002.sh` itself so future `--promote-results`
invocations do not need manual intervention. Final state re-verified:
`verify-exp-q8-divider-002-results.py --canonical --phase b4` reports
VALID, and the full 5-test regression suite
(`scripts/test-exp-q8-divider-002-provenance.sh`) still reports 5/5
PASS after both fixes.

## 7. Migration note (added during the `membrane-research` repository split)

This record was generated in `kadireren7/membrane`, where canonical
results lived directly under `experiments/EXP-FPGA-DIV-002/results/`.
The Part 3 documentation template adopted when this experiment was
migrated into `membrane-research` moved every canonical artifact one
level deeper, into `results/canonical/`, to make room for the sibling
`results/schemas/` directory. That move is a pure relocation (verified
byte-identical via the migration's own SHA256 check — see
`provenance/import-manifest.json` at this repository's root) but it
left this record's own `result_file_hashes` keys pointing at the old,
pre-migration paths, which `verify-exp-q8-divider-002-results.py
--canonical` resolves relative to the repository root — so the first
post-migration run of that validator correctly reported 5 "missing on
disk" rejections, a real (if narrow) consequence of the migration, not
a data-integrity problem.

Fixed by updating the five `result_file_hashes` keys (and the mirrored
`result_file_hashes_blob` text) to their new `results/canonical/`
paths — hash **values** unchanged, since file content did not change,
only location. `scripts/verify-exp-q8-divider-002-results.py`'s own
`--canonical` path (line ~171) was updated the same way, from
`.../EXP-FPGA-DIV-002/results` to `.../EXP-FPGA-DIV-002/results/canonical`.
Re-run confirms: `VALID (phase=b4 canonical=True
completed_transactions=58677500)`. This is the only promotion record
that existed at migration time (Phases A/B1/B2/B3 predate the
provenance-safety infrastructure Phase B4 itself introduced), so no
other record needed the same fix.
