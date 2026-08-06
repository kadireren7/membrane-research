# Experiment record: EXP-FPGA-DIV-002 Phase B4

Filled from [EXPERIMENT_TEMPLATE.md](https://github.com/kadireren7/membrane/blob/main/EXPERIMENT_TEMPLATE.md), same
convention as this experiment's own [experiment.md](experiment.md) (Phase
A), [phase-b1.md](phase-b1.md) (Phase B1), [phase-b2.md](phase-b2.md)
(Phase B2), and [phase-b3.md](phase-b3.md) (Phase B3). Branch:
`experiment/q8-divider-pipeline`.

## Experiment ID

`EXP-FPGA-DIV-002` Phase B4

## Hypothesis

Two independent hypotheses, addressed in order (task's own explicit gate:
part A must be complete and committed before part B's own performance
data is interpreted):

**A. Provenance safety.** Phase B3's own reproduction of `--phase b3
--quick` silently overwrote the committed 8,042,500-transaction canonical
results with a 125,750-transaction quick-mode run, because
`RESULTS_DIR` pointed straight at the canonical `experiments/
EXP-FPGA-DIV-002/results/` directory unconditionally, in every mode, on
every run. This is a structural defect in the reproduction script itself,
not a one-off mistake -- fixing it requires the canonical directory to be
unreachable from a normal run at all, with promotion as a separate,
explicitly-gated, validated, atomic step.

**B. Retirement pressure.** Phase B3's own residual collateral slowdown
at 20-25% Q8_0-encode density that even B3-split (this phase's own
selected architectural base) does not fully resolve is caused by shared
retirement/completion-storage pressure, not head-of-line blocking itself
(already addressed by B3-split's own mode-split ingress queues) -- a
completed younger transaction is forced to wait behind an incomplete
older encode-class transaction (strict in-order retirement, real and
unavoidable given the public ordering contract), and B3-split's own
encode-class hold registers add an avoidable extra cycle of retirement
latency even when a completion is already exactly its turn. A small,
bounded completion-storage/direct-retire change -- not a general
reorder buffer, not a change to B3-split's own already-selected ingress
architecture -- should recover a meaningful fraction of the remaining
gap.

## Preflight (task item 0)

Branch HEAD confirmed at `e416e36` (`research: reduce Q8 input
head-of-line blocking`) before any change was made. Working tree
confirmed clean except the pre-existing dirty `third_party/llama.cpp`
submodule (untouched throughout this phase, per its own out-of-scope
status in every prior phase). No stale monitor/watch/build process was
found running (`ps aux` checked for `run-exp-q8-divider-002.sh`,
`yosys`, `verilator` before any change). Every committed Phase B3 result
file's hash was recorded before this phase touched anything
(`sha256sum experiments/EXP-FPGA-DIV-002/results/b3-*.{csv,json,md}
phase-b3.md`) and independently re-confirmed unchanged after this
phase's own provenance-safety regression tests exercised `--phase b3
--quick` against the real script (task item 2's own required
regression-test property #1) -- see "Provenance safety" below for the
full account, including a real defect this phase's own testing process
found and fixed in the regression-test script itself (a `set -e`
interaction that would have made a failing/killed-run test abort the
whole suite instead of reporting a clean PASS/FAIL).

## Part A: provenance safety (task items 1-2)

### Root cause (confirmed, not assumed)

`scripts/run-exp-q8-divider-002.sh`'s own `--phase b3` block set
`RESULTS_DIR="$REPO_ROOT/experiments/EXP-FPGA-DIV-002/results"`
unconditionally at the top of the phase, then wrote
`results/b3-hol-profile.csv` directly (every run, regardless of mode) and
called `gen-b3-artifacts.py`/`gen-b3-synthesis-csv.py` with that same
canonical path as their own output directory. Nothing in the script
distinguished "this run's own artifacts" from "the committed public
record" -- they were the same path.

### Fix: run-scoped staging + explicit, gated promotion

- **Every run's own artifacts** (build outputs, logs, and any
  phase-generated "results" files) now live under
  `build-exp-q8-divider-002/<phase>/<quick|full>/<run-id>/`, a fresh
  directory per invocation (`--run-id`, auto-generated from a UTC
  timestamp + PID if not given). `--output-dir` can still override this,
  but is now hard-refused if it resolves (via `realpath -m`, catching
  relative-path attempts too -- a real bug caught and fixed during this
  phase's own testing, see below) to anywhere inside the canonical
  `experiments/EXP-FPGA-DIV-002/results/` directory.
- **A phase's own "results" files are staged, never written directly to
  the canonical directory**: `RESULTS_STAGING_DIR="$BUILD_DIR/results-
  staging"`, and `RESULTS_DIR` (both Phase B3's own block, retrofitted,
  and this phase's own new B4 block) is now always set to that staging
  path. This is the direct, structural fix for the real defect described
  above -- verified, not just asserted (see "Regression tests" below).
- **Canonical promotion is a separate, explicit, hard-gated step**:
  `--promote-results`, refused outright unless `--full` (task item 1's
  own explicit "quick artifacts can never be canonical"). When
  requested, `promote_results()` (in the run script) checks, in order:
  run mode is `full`; all tests this run passed (`FAILS == 0`); the
  run's own `completed_transactions` (recomputed from the real staged
  `*-correctness.json`, not assumed) meets the phase's own minimum
  (5,000,000/candidate x 6 for `b3`, 8,000,000/candidate x 7 for `b4`);
  `git rev-parse HEAD` at promotion time still matches the commit
  recorded when the run started (refuses if the repo moved to a
  different commit mid-run); a fresh hash of every source file this run
  depended on still matches what was hashed into the run's own manifest
  at completion time (refuses if a source file changed after the run
  started -- the one check `--force-promote` may bypass, loudly, and
  the *only* one); a fresh hash of the staged result files themselves
  still matches too (never bypassable, even with `--force-promote` --
  protects against something else touching the staging directory
  between run completion and promotion); and the staged results pass
  `scripts/verify-exp-q8-divider-002-results.py`'s own independent
  schema/consistency check. Only if every one of those passes does it
  print a `diff -u` of staged-vs-canonical for every file about to
  change, then perform the actual publish.
- **Publish is atomic per file, disclosed honestly as not a single
  filesystem transaction across the whole set**: every target file is
  first copied into a `experiments/EXP-FPGA-DIV-002/results/.incoming-
  <phase>-<run-id>/` staging directory (including a new
  `<phase>-promotion-record.json`, itself validated the same way, marked
  `canonical: true`), *then* each file is `mv`'d (a single `rename(2)`,
  atomic on the same filesystem) into its final canonical location. A
  crash between two of those individual `mv`s could in principle leave a
  mixed canonical set -- POSIX has no multi-file transaction primitive
  this project depends on -- but every byte is validated *before* any
  canonical file is touched, and no partial state is possible before
  that point (confirmed by regression test #5, below).

### Provenance manifest (task item 2)

Every run (`a`/`b1`/`b2`/`b3`/`b4`, all phases, not just b3/b4 which have
canonical results to promote) writes `<build-dir>/run-manifest.json` via
the new `scripts/gen-run-manifest.py`, with exactly the fields task item
2 specifies: `experiment_id`, `phase`, `variant`, `run_mode`, `canonical`,
`git_commit`, `git_dirty`, `branch`, `started_at`/`completed_at`,
`hostname`, `tool_versions`, `command`, `run_id` (extra, not in the
task's own list but needed to correlate a manifest back to its own build
directory), `seeds` (the real fixed deterministic seeds
`tb_top_verilator_q8_b4_variant.cpp` itself uses, `0xC0FFEE` for both RNG
streams -- not a placeholder), `expected_transactions`,
`completed_transactions`, `failures`, `source_file_hashes`/
`result_file_hashes` (both a structured dict and a raw blob string used
for the shell script's own byte-identical before/after comparisons), and
`status`. Phases `a`/`b1`/`b2` do not auto-generate canonical results
files (their own real numbers are hand-transcribed into their own phase
docs, unchanged by this phase) -- `--promote-results` is a documented
no-op for those phases, disclosed at runtime, not silently accepted.

### Validator (task item 2)

`scripts/verify-exp-q8-divider-002-results.py` independently checks every
rejection reason task item 2 lists: quick artifact presented as canonical
(`canonical: true` with `run_mode != "full"`); transaction count below
the phase's own required threshold; mismatched git commit (when
`--expect-git-commit` is given); missing tool version; incomplete run
(missing `started_at`/`completed_at`, missing required fields, or wrong
`phase`); failed test count (`failures != 0`); malformed CSV (no header,
short/long rows, zero data rows) or malformed JSON (parse failure,
reported per-file, not just the first); hash mismatch (every path in
`result_file_hashes` is re-hashed from disk and compared); and canonical
result without promotion record (`--canonical` mode requires
`<phase>-promotion-record.json` to exist and itself validate). Can run
against a staging directory (used by `promote_results()` itself, before
any canonical write) or against the real canonical directory directly
(`--canonical`, usable standalone as part of item 14's own verification
pass).

### Regression tests (task item 2's own explicit requirement)

`scripts/test-exp-q8-divider-002-provenance.sh`, five tests, all against
the **real script**, not a simulation of it:

1. `--phase b3 --quick` leaves the committed canonical `results/b3-*`
   files byte-identical (sha256, before vs. after) -- **PASS**, the real
   fix for the real defect.
2. `--phase b3 --quick --promote-results` is refused before any work
   starts, canonical untouched -- **PASS**.
3. A synthetic manifest with `failures=3, status=FAIL` is rejected by the
   validator ("failed test count") -- **PASS**.
4. A synthetic manifest with `canonical=true, run_mode=quick` is rejected
   ("quick artifact presented as canonical") -- **PASS**.
5. Killing `--phase b3 --full --promote-results` after 3 seconds (well
   before it could reach its own manifest-writing step, let alone
   promotion) leaves canonical results untouched, no `.incoming-*`
   directory left behind, no manifest written -- **PASS**.

**A real bug found and fixed in the test script itself, disclosed, not
hidden**: test 5's own `timeout -s KILL 3 ...` command returns a non-zero
exit status by design (the process was killed), and the test script's own
`set -euo pipefail` initially caused THAT to abort the whole test suite
before it could even record a result for test 5 -- tests 1-4 had already
correctly wrapped their own fallible commands in `set +e`/`set -e`, test
5 had not. Fixed by adding the same wrapping; re-run confirmed all 5
tests pass cleanly. This is exactly the kind of self-referential bug
class this phase's own regression-testing work exists to catch, so it is
disclosed here rather than quietly fixed.

**A second real bug found and fixed during manual verification** (not
part of the automated suite, found while checking the canonical-directory
guard by hand): the check that rejects an `--output-dir` inside the
canonical results directory originally compared `$BUILD_DIR` as given,
so a *relative* path like `experiments/EXP-FPGA-DIV-002/results/evil`
was not caught (only an absolute path was). Fixed by resolving
`$BUILD_DIR` through `realpath -m` before the comparison (and using that
resolved, absolute path as `$BUILD_DIR` from then on, which also makes
every later manifest/log path absolute and unambiguous). Re-verified with
both a relative and an absolute attempt.

All five regression tests, the manual relative-path fix, and a real
`--phase b3 --quick` run (not a synthetic test) were used together to
confirm the fix; see `results/b4-run-provenance.md` for the full,
timestamped account of every command run and its outcome.

**Two more real bugs in `promote_results()` itself, found while
promoting Part B's own real results** (after this section's own commit
had already landed): the git-commit-match check was stricter than this
task's own required "commit Part A, then interpret Part B" sequencing
can satisfy, and the promotion record's own file hashes referenced a
temporary staging path that no longer existed once promotion finished.
Both are real, disclosed, fixed in the same commit as Part B (the fix
itself is motivated by, and only discoverable via, promoting Part B's
own real data) -- full account, including the manual promotion procedure
used before the fix landed, in `results/b4-run-provenance.md` section 6.

## Part B: retirement pressure

### Method

1. **Retirement-pressure taxonomy** (task items 3-4): rather than
   instrument B3-split's own committed RTL directly (out of scope, task
   item 6), built a discrete-event **software reference model** of
   B3-split's own scheduling rules (`scripts/b4-retirement-model.py`),
   classifying every cycle into one of ten primary retirement states and
   quantifying all six bottleneck hypotheses (A-F) at 10/20/25/40%
   Q8_0-encode density, including a new (vs. Phase B3's own model)
   `out_fifo`-occupancy-based downstream-backpressure simulation, both
   i.i.d.-random and sustained-burst. Result: **strict in-order
   retirement (hypothesis C) dominates at every density measured, 65.6-
   80.5% of all stall cycles, and its share increases with density**;
   completion/shadow storage capacity (B) is the real second-largest
   cause (14.6-26.7% of stalls, decreasing with density); encode
   execution latency (A), downstream backpressure (D), ingress
   arbitration (E), and sequence/tag bookkeeping (F) are all minor or
   structurally ~0. See `results/b4-retirement-profile.csv` and
   `results/b4-retirement-analysis.md` for the full breakdown and the
   real, RTL-grounded mechanism behind each conclusion.
2. Read B3-split's own committed RTL in full to find the REAL,
   addressable mechanism behind hypothesis C's own dominance (not just
   its existence): B3-split's Q8_0/Q4_0 encode hold registers
   (`q8enc_hold_*`/`q4enc_hold_*`) unconditionally captured every
   completion into a register for at least one full cycle before
   `..._can_retire` could even be checked -- even in the common case
   where the completion was already exactly `next_retire_seq`'s turn and
   could have retired the SAME cycle. Since that same hold-register-
   occupied flag (`q8enc_hold_valid`/`q4enc_hold_valid`) also gates the
   *next* encode-class transaction's own admission
   (`q8enc_pending`/`q4enc_pending` -> `enc_issue_fire`), this is a real,
   measurable, avoidable extension of the single-in-flight encode
   engine's own effective back-to-back service time -- on exactly the
   resource this experiment's own collateral-slowdown measurements keep
   identifying as the critical path.
3. Designed and implemented exactly the three bounded candidates task
   item 5 specifies, all starting from B3-split
   (`rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4_b3_split.sv`,
   NOT modified by this file) as the one required architectural base
   (task item 6: "keep B3-split ingress architecture unchanged unless
   instrumentation proves an ingress defect" -- it did not; hypothesis E
   is ~0 by construction, see above):
   - **R1** (`..._b4_r1.sv`): B3-split's own shared `SHADOW_DEPTH`-entry
     decode-class shadow array (a for-loop scan across N slots) replaced
     with exactly one decode-class completion register (`dec_hold_*`),
     one direct comparison, no loop, no associative search (task item
     5's own explicit bar). Q8_0/Q4_0 encode hold registers unchanged
     (they were already effectively single-slot in B3-split).
   - **R2** (`..._b4_r2.sv`): the same mechanism B3-split already used
     (a small array + scan), fixed at compile time to exactly 2 entries
     (task item 5's own "exactly 2 entries," no longer a sweepable
     module parameter) -- a real, distinct design point between R1's one
     slot and B3-split's own four, not a relabeled B3-split.
   - **R3** (`..._b4_r3.sv`): direct-retire bypass added to BOTH Q8_0 and
     Q4_0 encode's own hold-register paths (`q8enc_direct_can_retire`/
     `q4enc_direct_can_retire`) -- when a completion's own issue-time
     sequence tag already equals `next_retire_seq` that same cycle, it
     retires directly, the hold register is never written, and the
     *next* encode-class transaction can be admitted one cycle sooner.
     Falls back to the hold register exactly as B3-split already did
     otherwise. B3-split's own decode-class shadow array is untouched
     (still `SHADOW_DEPTH=4`) -- R3 targets only the encode-hold-register
     mechanism found in step 2 above. Verified mutually exclusive from
     the hold-register retire path both by construction (the single-
     in-flight engine cannot admit a new transaction while its own hold
     register is still occupied, so a fresh completion and an occupied
     hold register can never coincide) and by a new simulation assertion
     (task item 7).
   R1+R3 combined was deliberately NOT built as a fourth RTL file up
   front (task item 11's own "combine only if independently justified")
   -- see "Candidate selection" below for whether the real R1/R2/R3
   results this phase measured justify building it as a follow-up.
4. Extended `rtl/experimental/q8_div/tb_top_verilator_q8_b4_variant.cpp`
   (a NEW file, copied from Phase B3's own `tb_top_verilator_q8_b3_
   variant.cpp`, which is NOT modified by this phase -- same convention
   Phase B3 itself used relative to Phase B2's own tool) into a
   seven-way tool (baseline/B1/B2/B3-split/R1/R2/R3; the B3 lookahead=2/
   lookahead=4 variants are not carried forward, since this phase's own
   comparison set per task item 8 is baseline/B1/B2/B3-split/R1/R2/R3,
   not the lookahead candidates B3 itself already rejected as
   regressions), compiled seven times via the same compile-time-DUT-
   selection technique, adding three new correctness scenarios task item
   8 requires beyond everything Phase B3's own tool already covered:
   - **Adversarial retirement**: one long Q8_0 encode followed by a dense
     run of younger decode-class transactions, combined with sustained
     heavy (75%) output backpressure throughout the stage -- approximates
     task item 8's own "output backpressure begins at divider completion"
     (this black-box testbench has no visibility into the exact internal
     cycle a divider completes without adding debug ports to the DUT, out
     of scope; heavy backpressure across the stage's own many repetitions
     covers that window with high probability instead, disclosed as an
     approximation, not a precisely-timed pulse).
   - **Reset coincident with completion**: issues a Q8_0 encode
     transaction, lets it run to approximately its own worst-case service
     latency, then asserts reset -- swept across 40 different timing
     offsets to land reset's own falling edge on or near the exact cycle
     a completion would otherwise become ready to retire.
   - **Sequence-tag wraparound with pending results**: resets first (so
     `issue_seq_ctr`/`next_retire_seq` are known to start at exactly 0),
     then drives >256 transactions under heavy (60%) backpressure,
     guaranteeing several transactions are genuinely in-flight at the
     exact cycle the 8-bit sequence counters wrap from 255 back to 0
     (not merely wrapping between otherwise-idle windows, which this
     project's own multi-million-transaction correctness scope already
     does incidentally, every phase, without a dedicated scenario).
   Also raised this tool's own PASS threshold from Phase B3's 5,000,000
   to 8,000,000 transactions (task item 8's own explicit minimum).
5. Extended `scripts/run-exp-q8-divider-002.sh` with a `--phase b4` mode
   (`--quick`/`--full`/`--resume`/`--output-dir`/`--run-id`/
   `--promote-results`/`--dry-run`), building all seven correctness+
   performance binaries, running the retirement-taxonomy model, an
   isolated-synthesis-wrapper matrix (see "Synthesis" below), and
   reusing the existing local-verification-suite stage (now also running
   `scripts/test-exp-q8-divider-002-provenance.sh` as part of `--full`).
6. Ran `--phase b4 --quick` first (twice -- the first attempt surfaced a
   real memory/time hazard in the synthesis stage, see "A real hazard
   found and fixed during isolated-wrapper synthesis development" below)
   to validate the whole seven-way pipeline end to end, then `--phase b4
   --full` for real numbers.

### A real hazard found and fixed during isolated-wrapper synthesis
development (disclosed, not hidden)

The first isolated-wrapper synthesis attempt (generic flow, Yosys's own
default `synth` command, which includes a `share` pass) ran Yosys's own
SAT-based resource-sharing analysis across the wrapper's own real
F16-conversion modules (`q4_pack`/`q4_unpack`/`q8_dequantize`/
`q8_quantize_pack`/`q8_maxabs_reduce`, all still real and unmodified in
every wrapper -- only the two divider engines are stubbed, see
"Synthesis" below) -- the same expensive SAT-query-heavy pattern already
disclosed as a full-top synthesis cost in Phase B1/B2/B3's own docs, not
something this phase introduced. Left unbounded, a manual test of this
exact command consumed 1.7+ GB RSS and pushed this project's own
memory-constrained dev machine (5.6 GiB total, [[dev-machine-memory-
constraints]]) down to under 200 MiB free while a SECOND, independent
Yosys process (the actual `--phase b4 --quick` run's own synthesis stage,
launched to validate the pipeline in parallel) was starting its own
synthesis at the same time -- a real, live near-OOM condition, caught and
resolved by killing both processes immediately, not by letting either run
to a crash. Fixed two ways: (1) `synth -top $top -noshare` for the
generic flow (Yosys's own documented flag to skip the SAT-based sharing
pass entirely -- confirmed via `yosys -h synth` that no equivalent flag
exists for `synth_ecp5`, which does not use nearly as much memory/time in
practice for this design, disclosed as an asymmetry, not silently
assumed equivalent); (2) the wrapper synthesis stage now only runs the
real, potentially multi-minute `synth`/`synth_ecp5` flows under `--full`,
bounded at `WRAP_SYNTH_TIMEOUT_S=900` per wrapper per flow with the same
UNAVAILABLE-on-timeout convention as every prior phase's own full-top
attempt -- `--quick` does a fast hierarchy-check-only elaboration
instead, matching the established convention Phase A/B1/B2/B3 already
used for their own quick-mode synthesis stages (a convention this
phase's own first attempt had NOT yet applied to the new wrapper stage,
which is the root cause this hazard traces back to).

### Synthesis (task item 10)

Because full-top `synth_ecp5` has timed out for every scheduler top-level
this experiment has ever attempted (Phase A/B1/B2/B3, each larger than
the last), this phase measures scheduler/completion logic **in
isolation** instead of repeating that same class of timeout a fifth time:
each candidate's own REAL, unmodified top-level file (B3-split/R1/R2/R3)
is synthesized as-is, with only the two divider engines it instantiates
(`q8_scale_dual_radix4`, `q4_scale`) swapped for trivial fixed-latency
stand-ins of the identical port shape and payload width
(`rtl/experimental/q8_div/q8_scale_dual_radix4_synth_stub.sv`,
`q4_scale_synth_stub.sv` -- see each file's own header for exactly what
is/is not real). Every other module a candidate instantiates (ingress
queues, hold registers, shadow_hold/dec_hold, tag_pipe, the retirement
mux, `q4_pack`/`q4_unpack`/`q8_dequantize`/`q8_quantize_pack`/
`q8_maxabs_reduce`) is that candidate's own real, unmodified, already-
tested source -- this is NOT a hand-written synthetic wrapper module
that might drift from the real scheduler's own behavior, it is the real
scheduler with only the two large FP-math engines (already measured
accurately elsewhere, see below) replaced.

The stand-in divider engines are disclosed as ESTIMATED-class isolation,
never claimed as real divider area/timing -- what IS real and
MEASURED_BY_TOOL is the surrounding scheduler/completion logic's own
synthesized cell count, which is what this task item actually asks to
measure. `q8_scale_dual_radix4` itself (untouched since Phase B1) is
synthesized separately and accurately, unmodified and un-stubbed, as this
experiment's own existing area-reduction reference point
(`results/b4-synthesis.csv`'s own `ref-q8scale-dual-radix4` row) -- the
two measurements are not meant to be summed into a false "total," and
this document does not do so.

See `results/b4-synthesis.csv` for the real per-candidate cell/FF/LUT4/
CCU2C counts (or `UNAVAILABLE` with the timeout disclosed, per candidate
and per flow, if `WRAP_SYNTH_TIMEOUT_S=900` was hit) and the "Results"
section below for the analysis.

## Environment

Same project dev machine as every prior phase: 5.6 GiB RAM, shared with
other concurrent local sessions ([[dev-machine-memory-constraints]]).
Same toolchain: `tools/.local-yosys` (Yosys 0.33), `tools/.local-
verilator`. No place-and-route tool, no Xilinx/Altera toolchain, no
physical FPGA board.

## Metrics

**Part A**: pass/fail on all 5 provenance regression tests; confirmed
byte-identical canonical results before/after a real `--phase b3
--quick` run; confirmed refusal (not silent acceptance, not a crash) on
every one of task item 2's own listed rejection reasons, exercised either
via the real script or via a synthetic manifest against the validator
directly.

**Part B**: per-mode min/mean/p50/p95/p99/max latency and throughput
across the same 15 traffic profiles Phase B3 already used, per-mode
collateral slowdown vs. baseline, overall cycles/transaction vs.
B3-split (this phase's own reference point, not B2), retirement-state
stall taxonomy with real quantified hypothesis-A-F percentages
(`results/b4-retirement-profile.csv`), adversarial-retirement stall-cycle
proxy reduction vs. B3-split, direct-retire hit rate (R3/R1+R3 only), and
a same-cadence isolated-synthesis matrix for R1/R2/R3.

## Success criteria / Results against task item 9's thresholds

All figures MEASURED_BY_TOOL from `results/b4-performance.csv`/
`results/b4-candidate-comparison.md` (real Verilator cosim,
8,382,500 transactions per candidate, 0 fails, all seven variants).

### Overall cycles/transaction, all traffic combined

| variant | overall cycles/txn | vs B3-split |
|---|---|---|
| B3-split (reference) | 9.133 | -- |
| R1 | 9.823 | +7.55% (worse) |
| R2 | 9.390 | +2.81% (worse) |
| R3 | 8.721 | **-4.51% (better)** |

### Overall cycles/transaction at 20%/25% density (**Required: >=10% better
than B3-split**)

| density | B3-split | R1 vs B3-split | R2 vs B3-split | R3 vs B3-split |
|---|---|---|---|---|
| 20% | 6.196 | +25.79% (worse) | +12.44% (worse) | **-4.76%** |
| 25% | 6.264 | +25.46% (worse) | +12.53% (worse) | **-4.66%** |

**NOT MET by any candidate.** R3 is the only real improvement (4.7-4.8%),
well short of the required 10%. R1/R2 are real, substantial
*regressions* relative to B3-split, not improvements -- reducing
completion-storage capacity below B3-split's own 4 slots costs more
throughput than it saves, at both density points measured.

### Collateral slowdown vs. baseline at 20%/25% Q8_0-encode density
(**Required: <=10%** for Q8_0 dec / Q4_0 dec / Q4_0 enc)

| density | mode | B3-split | R1 | R2 | R3 |
|---|---|---|---|---|---|
| 20% | Q8_0 dec | +9.72% (met) | +20.10% (fail) | +12.28% (fail) | **+4.32% (met)** |
| 20% | Q4_0 enc | +0.97% (met) | +9.59% (met) | +3.26% (met) | **-4.14% (met)** |
| 20% | Q4_0 dec | +9.84% (met) | +20.31% (fail) | +12.41% (fail) | **+4.51% (met)** |
| 25% | Q8_0 dec | +17.44% (fail) | +28.39% (fail) | +20.39% (fail) | +11.79% (fail, closest) |
| 25% | Q4_0 enc | +6.35% (met) | +15.62% (fail) | +9.16% (met) | **+1.26% (met)** |
| 25% | Q4_0 dec | +17.32% (fail) | +28.25% (fail) | +20.23% (fail) | +11.71% (fail, closest) |

**MET by R3 at 20% density on all three modes** (the only candidate to do
so). **NOT MET by R3 at 25% density** on Q8_0 dec/Q4_0 dec (11.7-11.8%,
over the 10% bound but the closest of any candidate by a wide margin --
B3-split itself is at 17.3-17.4% there). **R1 and R2 make collateral
WORSE than B3-split at every density/mode combination measured** except
R2's Q4_0-encode figures -- consistent with the retirement-pressure
analysis above: removing completion-storage capacity without also
shortening the encode-hold-register's own retirement latency (R3's own
mechanism) does not help, it actively hurts.

### Retirement-stall reduction: adversarial pattern (**Preferred: >=35%
fewer stall cycles than B3-split**)

No literal per-category stall-cycle count exists for the RTL runs (same
disclosed limitation as Phase B3's own adversarial-HOL proxy -- black-box
testbench, no internal DUT stall counters, out of scope). Using total
cycles/transaction on the adversarial-retirement profile as the same
kind of upper-bound proxy Phase B3 used:

| variant | cycles/txn | vs B3-split |
|---|---|---|
| B3-split (reference) | 3.618 | -- |
| R1 | 6.667 | **-84.3% (severe regression)** |
| R2 | 3.751 | -3.7% (regression) |
| R3 | 3.468 | +4.1% (improvement) |

**NOT MET by any candidate** -- R3's own 4.1% improvement is far short of
the 35% preferred target (and, being a proxy, the true stall-cycle-only
figure is necessarily <= this, so it cannot meet it either). **R1 is a
severe, real regression here specifically** (-84.3%, i.e. adversarial-
pattern throughput roughly halved): with only one decode-class completion
slot, R1's own adversarial-retirement stage (a long Q8_0 encode
immediately followed by a dense run of younger decode-class
transactions, task item 8's own new required scenario) saturates that
single slot almost immediately, forcing every subsequent decode-class
transaction to block at ingress until the encode-class transaction
retires -- exactly `ingress_blocked_completion_capacity`
(hypothesis B) at its worst, real and measured, not merely
theorized.

### Direct-retire hit rate (**Preferred: >=50% when downstream is
ready**)

**UNAVAILABLE** at the literal RTL-measured level -- computing this
requires observing internal DUT signals
(`q8enc_direct_can_retire`/`q4enc_direct_can_retire`) this black-box
testbench does not expose (same disclosed limitation as every other
internal-signal metric this phase and Phase B3 both encountered). The
mechanism's *effect* is real and measured indirectly: R3's own real
improvement over B3-split on every pure-stream and most density-sweep
profiles (see above) is only explainable by the direct-retire path
firing routinely, since R3 changes nothing else.

### Pure streams (**Required: no regression >2%** vs. B3-split)

| mode | R1 | R2 | R3 |
|---|---|---|---|
| Q8_0 dec | +1.2% (met) | **-3.0% (VIOLATES -- >2% regression)** | +2.5% (met) |
| Q4_0 enc | -0.0% (met) | -0.0% (met) | +5.4% (met) |
| Q4_0 dec | +0.5% (met) | +0.9% (met) | +3.5% (met) |

**MET by R1 and R3. NOT MET by R2** -- a real, if small (3.0%), measured
regression on the pure Q8_0-decode stream, the one case in this whole
phase where R2 fails a target R1 does not.

### 40-60% density (honest report, no requirement to match baseline --
divider II is a real fundamental limit)

| density | baseline | B3-split | R1 | R2 | R3 |
|---|---|---|---|---|---|
| 40% | 5.451 | 6.918 | 8.495 | 7.740 | 6.596 |
| 60% | 3.984 | 8.007 | 9.563 | 8.961 | 7.645 |

R3 remains the best of the four B3/B4 candidates at every density
measured, including 40-60%; R1/R2 remain worse than B3-split throughout.
None approach baseline at high density -- expected, since baseline has
no scheduling overhead at all and Q8_0-encode's own divider II is the
same real, unavoidable, previously-disclosed floor for every candidate
here (unchanged since Phase B1).

### Isolated synthesis (task item 10)

`results/b4-synthesis.csv`. `ref-q8scale-dual-radix4` unchanged: 1,556
generic / 2,775 ECP5 cells, **-97.76% retained vs. the original `q8_scale`
baseline** (123,742 cells) -- untouched by any B4 candidate.

Every isolated-wrapper **ECP5** attempt (B3-split/R1/R2/R3) timed out at
`WRAP_SYNTH_TIMEOUT_S=900`, UNAVAILABLE, same class of cost as every
prior phase's own full-top attempt (the wrapper still includes every
real F16-conversion module the scheduler surrounds -- only the two
divider engines are stubbed, see "Synthesis" above).

The **generic** (`-noshare`) flow completed for all four, but at a scale
(836,981-843,359 cells) wildly disproportionate to this experiment's own
prior component-level measurements (for comparison: the ORIGINAL
production `q8_scale`, with two real full dividers, was 123,742 ECP5
cells) -- `-noshare` disables Yosys's own resource-sharing pass
entirely, and the design's real F16-conversion modules
(`q4_pack`/`q4_unpack`/`q8_dequantize`/`q8_quantize_pack`/
`q8_maxabs_reduce`, identical across all four wrappers) apparently rely
on that pass for a large fraction of their own normal size reduction.
**These absolute counts are disclosed as NOT representative of real
synthesized area** -- reporting them as "candidate area" would be
misleading, so this document does not do so.

**What IS real and usable**: since the F16-conversion modules are
byte-for-byte identical across all four wrappers (only the top-level
scheduler differs), the **delta** between candidates cancels that shared
bloat and isolates the scheduler's own real relative size:

| candidate | generic cells | delta vs B3-split | storage change |
|---|---|---|---|
| B3-split (reference) | 842,221 | -- | 4-entry shadow array |
| R1 | 836,981 | **-5,240** | 1-entry register (no array) |
| R2 | 838,589 | **-3,632** | 2-entry (fixed, no sweep) |
| R3 | 843,359 | **+1,138** | unchanged (4-entry array) + 2 direct-retire comparators |

This is physically coherent, not noise: R1 (the smallest completion
structure) shows the largest reduction, R2 (2 entries) a smaller
reduction, and R3 (storage unchanged, two new equality comparators
added) shows a small increase -- in the exact rank order each
candidate's own RTL change predicts. Classified ESTIMATED (a delta
between two non-representative-in-isolation absolute numbers, not a
directly synthesized "candidate area"), not MEASURED_BY_TOOL, disclosed
as such.

## Candidate selection (task item 11)

Applying the task's own explicit rules in order:

- "Choose R3 if direct-retire alone meets targets" -- **it does not,
  fully**: it meets the 20%-density collateral bound and the pure-stream
  bound, but not the 25%-density collateral bound, the >=10%-overall-
  improvement bound, or the >=35%-adversarial-reduction preferred bound.
  It is nonetheless the only candidate that is a real, consistent
  improvement over B3-split on nearly every measure in this document.
- "Choose R1+R3 if it materially improves R3 with modest fixed-state
  cost" -- **not built as a fourth RTL variant**, and this is a
  data-driven decision, not a scope shortcut: R1 *alone* is not a
  neutral-or-mildly-negative candidate that combining might rescue, it
  is a severe, real regression on its own terms (+7.55% worse overall,
  worse on every density-sweep and collateral figure measured, and a
  measured -84.3% catastrophic regression on the adversarial-retirement
  pattern specifically, task item 8's own new required scenario). R1's
  own mechanism (removing shadow capacity down to one slot) and R3's own
  mechanism (shortening encode-hold-register dwell time) are orthogonal
  -- there is no plausible mechanism by which stacking R1's own
  measured harm underneath R3's own measured benefit would beat R3
  alone, and the retirement-pressure analysis above (hypothesis B's own
  real, if secondary, share of stall cycles) is consistent with that:
  R1's capacity reduction costs more than R3's latency reduction saves.
- "Choose R2 only if it beats R1+R3 by >=5%" -- moot (R1+R3 not built,
  per above) and also directly contradicted on its own terms: R2 alone
  underperforms B3-split (+2.81% worse overall) and violates the
  pure-stream no-regression bound R1 and R3 both satisfy.
- "Otherwise retain B3-split" -- **does not apply either**: R3 is a
  real, measured, if partial, improvement over B3-split, not a
  regression, so retaining B3-split unchanged would discard a real gain.

**Selected candidate: R3 (direct-retire bypass)** -- the smallest change
of the three (no new storage, two comparators), the only one that is a
consistent real improvement over B3-split, and the only one worth
building on if this line of work continues. R1 and R2 are both
concluded **not useful** on their own (real regressions, most severely
R1) -- a real, disclosed, negative result task item 5's own
"evaluate exactly these candidates" scope specifically asked for, not
glossed over because the outcome was unfavorable.

## Limitations

- R3 does not meet the full 20-25%-density collateral bound (25% density
  Q8_0/Q4_0 decode remain at 11.7-11.8%, over the 10% bound, though the
  closest any candidate in this experiment's own history has come at
  that density) nor the >=10%-overall-improvement or
  >=35%-adversarial-reduction bounds -- disclosed as real shortfalls,
  not glossed over.
- R1 is a real, severe regression on the adversarial-retirement pattern
  specifically (-84.3%) -- a genuine, if negative, finding about
  completion-storage capacity at its minimum bound, not a defect in this
  phase's own methodology.
- No real synthesized full-top or fully-representative isolated-wrapper
  cell count exists for any B3/B4 candidate (ECP5 always UNAVAILABLE;
  generic only available in a disclosed non-representative `-noshare`
  form) -- candidate-vs-B3-split area deltas are ESTIMATED from that
  non-representative form's own differencing, not MEASURED_BY_TOOL in
  absolute terms.
- Direct-retire hit rate (task item 9's own preferred target) is
  UNAVAILABLE at the literal signal level -- this black-box testbench
  does not expose internal DUT signals, same disclosed limitation as
  every other internal-state metric this phase and Phase B3 both
  encountered.
- The adversarial-retirement backpressure pattern approximates task item
  8's own "output backpressure begins at divider completion" with
  sustained heavy backpressure across the whole stage rather than a
  precisely-timed pulse (disclosed in the Method section above) -- a
  real, disclosed scope simplification, not a claim of exact timing
  fidelity.
- Same environment limitations as every prior phase: 5.6 GiB
  memory-constrained dev machine (a real near-OOM condition was hit and
  resolved during this phase's own isolated-wrapper-synthesis
  development, see above), no place-and-route tool, no real FPGA
  hardware, no real Fmax/timing/power figure anywhere in this document.

## Decision

**CONTINUE.**

- Provenance safety: **COMPLETE** -- Part A's own work is committed
  separately (`research: make Q8 experiment artifacts provenance-safe`)
  and verified (5/5 regression tests, real before/after byte-identical
  canonical results) before this Part B performance data was interpreted
  or acted on, per the task's own explicit gate.
- Exact: **YES** -- 0 mismatches, 0 ordering errors, 0 drops/duplicates,
  0 reset-recovery failures, 0 starvation violations, 0 internal
  assertion failures, across seven full 8,382,500-transaction runs
  (baseline/B1/B2/B3-split/R1/R2/R3).
- Architecture improved: **YES, via R3 specifically** -- a real,
  measured, consistent (if partial) improvement over B3-split: faster
  overall at every density measured (4.5-4.8% at 20-25%), meets the
  20%-density collateral bound on all three modes (the only candidate
  to do so), closest of any candidate to the 25%-density bound, the
  only candidate with zero pure-stream regression alongside R1, and a
  real (if modest, 4.1%) adversarial-pattern improvement. R1 and R2 are
  real, disclosed **regressions**, not improvements -- this phase's own
  data does not support building on either.
- Performance targets: **PARTIALLY met, by R3 only** -- the strict
  25%-density collateral bound, the >=10%-overall-improvement bound, and
  the >=35%-adversarial-reduction preferred bound all remain unmet.
- Bounded/simple architecture: **YES** -- R3 adds no new storage (two
  equality comparators only), the smallest possible change matching
  task item 11's own priority order ("direct-retire fast path" ranked
  above "fixed per-resource slots" above "two-entry completion queue").
- Large divider-area win retained: **YES** -- `q8_scale_dual_radix4`'s
  own -97.76% reduction vs. the original `q8_scale` baseline is
  untouched by any B4 candidate (component-level, real, unchanged since
  Phase B1).
- Reproducible: **YES** -- `scripts/run-exp-q8-divider-002.sh --phase b4
  --quick|--full|--resume|--output-dir|--run-id|--promote-results|
  --dry-run`, all exercised this session.

Per this task's own explicit framing (mirroring Phase B2/B3's own),
this is a real, substantial, honestly-quantified result: R3 is a
genuine, if partial, improvement over B3-split, and R1/R2 are real,
disclosed regressions rather than quietly omitted or reframed as
neutral. Item 15's `PROMOTE_CANDIDATE` criteria are not met (the strict
20-25%-density collateral target is only half-met, and neither the
overall-improvement nor the adversarial-reduction bound is met). Item
15's `REJECT_B4`/`RETAIN_B3` criteria do not apply either (R3 IS a
material, measured improvement, provenance safety IS reliable, and area
is retained) -- `CONTINUE` is the only decision the task's own item 15
rules support. This is a Phase-B4-internal, experiment-branch-only
decision. It does **not** authorize merging any experimental file into
production RTL.

## Promotion status

`not proposed` -- remains on `experiment/q8-divider-pipeline`, pushed to
the public repository per this project's open-development policy
(`docs/open-development-policy.md`), but **not merged into `main`**. No
pull request has been opened, per this task's own explicit scope. Per
`docs/research-release-freeze.md`, nothing here is a verified public
claim of the `v0.1.0-research` release; it is disclosed,
research-in-progress work on a public branch. If this line of work
continues, the natural next step (a possible Phase B5, not undertaken
here) would start from R3 specifically -- e.g. a bounded, small (2-3
entry) direct-retire-aware completion structure that keeps R3's own
encode-hold-register latency reduction while addressing hypothesis C's
own remaining dominant share at 25% density more directly than R1/R2's
own (measured, unhelpful) capacity-reduction approach did.
