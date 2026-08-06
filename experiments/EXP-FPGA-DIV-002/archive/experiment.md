# Experiment record: EXP-FPGA-DIV-002

Filled from [EXPERIMENT_TEMPLATE.md](https://github.com/kadireren7/membrane/blob/main/EXPERIMENT_TEMPLATE.md).
Branch: `experiment/q8-divider-pipeline`.

## Experiment ID

`EXP-FPGA-DIV-002`

## Hypothesis

`rtl/q8_scale.sv`'s two parallel `membrane_fp_divider` instances
(`d = amax/127.0`, `id = 127.0/amax`) are, by direct measurement now
available from EXP-FPGA-DIV-001's own promoted Q4_0 precedent
(`experiments/EXP-FPGA-DIV-001/`), this datapath's single largest
remaining disclosed FPGA-synthesis risk: `q8_scale.sv`'s own header
comment (lines 14-19) already discloses "two divider instances run in
parallel... rather than time-multiplexing a single divider," and
`membrane_fp_divider.sv`'s own header (lines 32-37) already discloses
the underlying divider is "a single, wide combinational... operator,"
unpipelined, with an unquantified real-Fmax risk. Following
EXP-FPGA-DIV-001's own Phase A methodology exactly (characterize
first, do not design an alternative yet), this falsifiable claim is
tested in a **future Phase B**, not yet tested here: *at least one of
the two `q8_scale` divider instances can likely be replaced by an
alternative construction (shared, dual-small, or constant-optimized)
using meaningfully fewer synthesized cells and/or a shorter
combinational critical path, without changing the datapath's bit-exact
behavior.*

**Phase A scope (this record): characterize the baseline and evaluate
candidate directions on paper/differentially — no alternative divider
is designed, written, or synthesized in this phase.** See `baseline.md`
section 4 for the six candidate directions analyzed (not implemented).

## Baseline tag/commit

Tag `v0.1.0-research`, commit `8298e953b792c78aa8604c7558ef701b2b862b28`
(the current stable public release). This experiment branch,
`experiment/q8-divider-pipeline`, was created from `main` at commit
`9dbbede255dccf025cc3ecad7f17cd9f52f384a8` (the current `main` HEAD at
experiment start, several commits ahead of the tag — unrelated
CodeRabbit/CodeQL/CI-infrastructure work, no RTL changes — see
`docs/repository-boundary.md`/`docs/research-release-freeze.md` for why
`main` and the release tag are allowed to differ).

## Method

Phase A is characterization plus a **differential feasibility study**
(no new RTL divider variant written or synthesized — an explicit
constraint of this phase, unlike EXP-FPGA-DIV-001 whose later Phase B1-B4
sub-phases did write and synthesize new RTL):

1. Read `rtl/q8_scale.sv`, `rtl/membrane_fp_divider.sv`,
   `rtl/membrane_quant_stream_top.sv`, `rtl/q8_maxabs_reduce.sv`, and
   the C reference (`src/quant/quant_simd.c`) to derive the two
   divider operations' exact roles, their mathematical/floating-point
   relationship, and every edge case (zero/NaN/Inf/subnormal) —
   `results/baseline-dataflow.md`.
2. Re-ran `yosys` (generic and `synth_ecp5`) on the standalone
   `membrane_fp_divider` (cross-checking against EXP-FPGA-DIV-001's own
   published numbers), **and**, going further than EXP-FPGA-DIV-001's
   own Phase A (which had to kill this run), completed real
   `synth_ecp5` runs for `q8_scale` standalone and the full
   `membrane_quant_stream_top` — see `baseline.md` section 5 and
   `results/synthesis.csv` for whether the full-top run completed or
   was bounded/killed.
3. Re-ran `hierarchy -check -top membrane_quant_stream_top` to confirm
   the whole design still elaborates cleanly (unchanged from
   EXP-FPGA-DIV-001).
4. Re-ran the existing 520,000-transaction Verilator cosimulation
   (`rtl/tb/tb_top_verilator.cpp`) against the unmodified RTL.
5. Wrote a new differential feasibility tool,
   `rtl/tb/tb_q8_scale_feasibility.cpp`, that drives the REAL Verilated
   `membrane_fp_divider`/`membrane_fp_multiplier` RTL (never a
   hand-written approximation) to measure, at scale, whether three
   candidate shortcuts (`1/d`, `1/id`, and a constant-reciprocal
   multiply) agree bit-exactly with the current production `d`/`id`
   values, across edge cases, uniform-random `amax` bit patterns, and a
   realistic Q8 runtime `amax` distribution sample.
6. Re-ran the project's existing CI-equivalent local verification
   (Debug/Release/ASan+UBSan/TSan ctest, ggml quant parity,
   `verify-results.py`, `verify-paper.py`, `verify-outreach.py`) plus a
   CodeQL/CodeRabbit config sanity check, to confirm nothing else in
   the repository regressed.

No production RTL file was modified. No new divider variant was
written or synthesized anywhere in this phase — `tb_q8_scale_feasibility.cpp`
only re-drives the existing, unmodified `membrane_fp_divider`/
`membrane_fp_multiplier` RTL with different operand sequences.

## Environment

This project's own development machine: 5.6 GiB RAM, shared with other
concurrent local sessions/processes during this run (unlike
EXP-FPGA-DIV-001's presumably more isolated run — disclosed because it
measurably affected wall-clock time and required deliberate
sequencing of memory-heavy steps, though not peak-RSS-per-job, which
matched EXP-FPGA-DIV-001's own numbers closely). `tools/.local-yosys`
(Yosys 0.33, git sha1 2584903a060) and `tools/.local-verilator`
(locally-extracted Verilator, no root install) — the same toolchain
versions EXP-FPGA-DIV-001 and `docs/phase5-synthesizable-fpga.md` used.
No place-and-route tool, no Xilinx/Altera toolchain, no physical FPGA
board — unchanged from every prior phase's disclosure.

## Model/dataset

Not applicable in the LLM-checkpoint sense — this is a pure RTL/
synthesis-tooling experiment. The Verilator full-datapath cosimulation
uses the existing deterministic, fixed-seed golden vectors generated by
`rtl/tb/gen_top_x_vectors.c` and friends (120,000 blocks per format/
direction). The new feasibility differential
(`tb_q8_scale_feasibility.cpp`) uses (a) a full `amax` exponent/mantissa
boundary sweep plus named edge cases, (b) uniform-random 31-bit
magnitude patterns (matching `amax`'s own always-non-negative
invariant), and (c) a synthetic Q8 runtime `amax` distribution sample
built the same way `tb_membrane_fp_divider_radix4.cpp`'s own "q4
runtime d-distribution sample" stage does (32 random F16 elements per
block, `amax = max(|x|)`) — not a captured real-model trace, a
structurally-representative synthetic sample, same technique this
project's own prior divider experiment already established as
sufficient for this class of question.

## Metrics

- Divider instance count/role per call site, and their exact
  floating-point relationship (reciprocal in exact math, not
  necessarily bit-exact after independent rounding).
- Per-unit latency (cycles) and initiation interval.
- Yosys generic and ECP5-mapped cell counts: standalone divider,
  `q8_scale`, and (best-effort) the full `membrane_quant_stream_top`.
- Differential feasibility results per candidate: exact-match count,
  mismatch count, mismatch categories, max ULP, first mismatch
  examples.
- Verilator cosimulation transaction count and fail count.
- Local test-suite pass/fail counts (Debug/Release/ASan+UBSan/TSan) and
  the three `verify-*.py` script pass counts, plus ggml quant parity.

All sourced in `baseline.md`, `results/baseline-dataflow.md`, and
`results/synthesis.csv`, each number labeled MEASURED, SIMULATED,
ESTIMATED, or UNAVAILABLE.

## Success criteria (Phase A)

- The two divider operations and their exact mathematical/rounding
  relationship are correctly derived and cross-checked against at
  least two independent sources (RTL source comments + the C
  reference).
- Real (not estimated) Yosys synthesis results exist for the standalone
  divider and `q8_scale`, in both generic and technology-mapped form.
- The differential feasibility tool runs to completion at the
  requested scale (2,000,000+ cases) and reports real exact-match/
  mismatch counts for every candidate, without asserting or forcing a
  particular outcome.
- The existing 520,000-transaction Verilator cosimulation still passes
  at 0 fails against the unmodified RTL.
- Existing local verification (Debug/Release/ASan+UBSan/TSan ctest, all
  three `verify-*.py` scripts, ggml quant parity) still passes
  unchanged.

## Failure criteria (Phase A)

- Any call site or edge case (zero/NaN/Inf/subnormal handling) missed
  or mischaracterized relative to the actual RTL/C-reference source.
- Yosys synthesis failing to complete or reporting elaboration errors
  for the standalone divider or `q8_scale` (the full top-level is
  explicitly best-effort, per the task's own "mümkünse" scope — a
  bounded/killed full-top run is a disclosed limitation, not a failure).
- The feasibility tool crashing, hanging, or failing to reach its
  planned case count.
- The Verilator cosimulation reporting any of the 520,000 transactions
  as a mismatch (would indicate this characterization work itself
  introduced a regression, since no production RTL was supposed to
  change).
- Any existing test or verification script regressing.
- Presenting any exact-mismatching candidate as production-ready, or
  any generic/technology-independent cell count as if it were a real
  LUT count.

## Resource budget

Expected: a few hours of wall time (larger than EXP-FPGA-DIV-001's own
"well under an hour" Phase A budget, because this phase's synthesis
scope is larger — `q8_scale` standalone plus a full-top attempt, both
new relative to DIV-001 — and the differential tool's `--full` scope is
2,000,000+ cases against real Verilated RTL, run twice per case
through the divider). Actual figures recorded per-step in
`results/baseline-synthesis.txt` and this file's own "Results" section
below.

## Checkpoints

`scripts/run-exp-q8-divider-002.sh --resume` skips any already-built
Verilator object dir or already-produced log file it finds in
`--output-dir`, matching `scripts/verify-q4-radix4-divider.sh`'s own
`--resume` convention — relevant here because the full synthesis
matrix and the full 2M+-case differential run are each independently
resumable/skippable without re-running the other.

## Results

See `baseline.md` (full characterization + feasibility analysis),
`results/baseline-dataflow.md` (dataflow derivation),
`results/baseline-synthesis.txt` (raw synthesis summary),
`results/synthesis.csv` (structured synthesis matrix), and
`results/feasibility-differential-full.txt` (raw differential output).
Headline numbers:

- `q8_scale` = 2 parallel `membrane_fp_divider` instances (`u_div_d`,
  `u_div_id`), both `DIV_DELAY=1`. Latency 1 cycle, II 1 (parallel, not
  chained). Whole datapath: 7-cycle uniform latency (`L_MAX`), II 1.
- `amax/127` and `127/amax` are exact reciprocals in real arithmetic but
  NOT guaranteed bit-exact after independent IEEE-754 rounding — now
  quantified, not just asserted (see feasibility results below).
- Yosys: standalone `membrane_fp_divider` generic 10,234 / ECP5 73,629
  cells (3rd independent reproduction, unchanged). `q8_scale` standalone
  generic 21,800 / **ECP5 123,742 cells — NEW real measurement**,
  correcting EXP-FPGA-DIV-001's own ~75-80K extrapolation upward. Full
  top-level `synth_ecp5`: attempted, killed after a 25-minute bound,
  UNAVAILABLE.
- Verilator cosim: 520,000/520,000 transactions, 0 fails (incl. 120,000
  Q8 encode + 120,000 Q8 decode focused stages).
- Differential feasibility (2,050,239 cases each): reciprocal
  reconstruction `1/d`↔`id` 74.96% exact, `1/id`↔`d` 71.83% exact (every
  ordinary mismatch exactly 1 ULP, plus a real zero-case +Inf bug if
  unguarded); constant-reciprocal `amax*(1/127)`↔`d` 95.46% exact (also
  exactly 1 ULP per mismatch, no categorical failures).
- Local verification: Debug 28/28, Release 28/28, ASan+UBSan 30/30,
  TSan 30/30, ggml quant parity PASS, `verify-results.py` 13/13,
  `verify-paper.py` 11/11, `verify-outreach.py` 17/17.
- Decision: **NEXT_DUAL_RADIX4** (see `baseline.md` section 7).

## Limitations

- No real Fmax/timing-closure number exists for any configuration (no
  P&R tool in this environment) — unchanged from every prior phase.
- The differential feasibility tool measures **bit-value agreement**
  only; it says nothing about the relative synthesized cost of
  candidates B/C (a multiplier is generally cheaper than a divider, but
  this experiment does not synthesize a full alternative `q8_scale`
  built on any candidate — that is explicitly Phase B work).
  Candidates D/E/F (shared/dual/algebraic architectures) are evaluated
  by analysis only, not by a working alternative implementation.
- This is a characterization-and-feasibility-only phase; it makes no
  claim about whether any Phase B alternative will actually reduce cell
  count or improve timing closure in production RTL — that is exactly
  what a future Phase B would need to test, same disclosed boundary
  EXP-FPGA-DIV-001's own Phase A drew before its later Phase B1-B4 work.
- No real FPGA hardware, board, or vendor toolchain (Vivado/Quartus)
  was used anywhere in this experiment.

## Decision

**NEXT_DUAL_RADIX4.** Candidate B (reciprocal reconstruction) is
empirically rejected (~25-28% bit-mismatch rate, measured). Candidate C
(constant-reciprocal multiply) is measurably better but still not exact
(~4.5% mismatch) — kept on the list for a future dedicated Phase B, not
promoted now. Candidate E (dual exact radix-4 dividers) is the
strongest candidate: EXP-FPGA-DIV-001 Phase B4 already proved
`membrane_fp_divider_radix4` bit-exact (4,456,685 cases, 0 mismatches)
and measured its standalone cost at 1,509 ECP5 cells (-97.9% vs. this
experiment's own freshly-confirmed 73,629-cell baseline), so applying
two instances carries zero new bit-exactness risk — the open question
for a future Phase B is the real two-instance-parallel area/latency
cost, not measured this phase. See `baseline.md` section 7 for the full
reasoning, including why candidate D (shared single divider) ranks
below E.

## Promotion status

`not proposed` — this remains on `experiment/q8-divider-pipeline`,
pushed to the public repository per this project's open-development
policy (`docs/open-development-policy.md`), but **not merged into
`main`**. No pull request has been opened, per this task's own explicit
scope. Per `docs/research-release-freeze.md`, nothing here is a
verified public claim of the `v0.1.0-research` release; it is
disclosed, research-in-progress work on a public branch.

## Phase B1 (follow-up)

This Phase A record above is unchanged and historical. Phase A's own
`NEXT_DUAL_RADIX4` decision (section "Decision" above) was carried out as
a separate, later phase: see [phase-b1.md](phase-b1.md) for the full
record. Headline: `rtl/membrane_fp_divider_radix4` (reused unmodified) in
a new `q8_scale_dual_radix4` variant is bit-exact with baseline `q8_scale`
across 4,052,224 differential cases (0 mismatches), reduces `q8_scale`'s
own ECP5 footprint by -97.76% (123,742 -> 2,775 cells, a real integration
measurement), at a measured 16x initiation-interval cost and a real,
disclosed collateral slowdown on other in-flight-serialized modes.
Phase B1's own decision: `PROMOTE_CANDIDATE` (experiment-branch-only, not
a `main`-merge authorization).

## Phase B2 (follow-up)

Phase B1's own disclosed collateral slowdown (Q8_0 decode +48.3%, Q4_0
decode +47.5%, Q4_0 encode +8.2%) was addressed as a separate, later
phase: see [phase-b2.md](phase-b2.md) for the full record. Headline: a
bounded, sequence-number-tagged scheduler
(`rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4_b2.sv`,
`q8_scale_dual_radix4.sv` reused unmodified from Phase B1) lets Q8_0/Q4_0
decode and Q4_0 encode issue independently of an in-flight Q8_0/Q4_0
encode while preserving strict output ordering (confirmed the real
contract, not assumed) via one hold register per encode engine plus a
shared, small (depth 1 or 2, both evaluated) hold queue for the decode
classes -- explicitly smaller than every reorder-buffer configuration
EXP-FPGA-DIV-001 Phase B3 already rejected. Real result across four full
6,250,000-transaction runs (baseline/B1/B2 depth=1/B2 depth=2): 0
mismatches, 0 ordering errors; >=25%-vs-B1 overall cycles/transaction
improvement met on every mixed-traffic profile (28.5-36.6%); the strict
<=10%-collateral-vs-baseline target is met (and exceeded) at light
Q8_0-encode density but not at 20-25% density (residual 16-26%, having
eliminated 67-77% of Phase B1's own collateral cost), for a real,
disclosed architectural reason (input-FIFO queueing behind Q8_0/Q4_0
encode's own single-in-flight service time, not shadow-queue depth,
dominates once `IN_FIFO_DEPTH=16` is the binding constraint). Phase B2's
own decision: `CONTINUE` (experiment-branch-only).

## Phase B3 (follow-up)

Phase B2's own residual collateral slowdown at 20-25% Q8_0-encode
density (16-26%) was targeted as a separate, later phase: see
[phase-b3.md](phase-b3.md) for the full record. A software reference
model of Phase B2's own scheduling rules
(`scripts/b3-hol-model.py`, cross-validated within ~6-9% of the real
RTL's own measured latencies) quantified the root cause: 58-63% of all
input-stalled cycles at 20-25% density are directly caused by the FIFO
head targeting a busy Q8_0/Q4_0-encode engine while a mean of 4.4-5.4
resource-independent younger transactions sit ready behind it
(`results/b3-hol-profile.csv`, `results/b3-hol-analysis.md`). Three
bounded issue-selection candidates were built on top of this evidence
and evaluated against baseline/B1/B2 at real scale (six full
8,042,500-transaction Verilator cosim runs, 0 mismatches/ordering
errors/drops/duplicates/reset failures/starvation violations across all
six):
`rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4_b3_l2.sv`
(2-entry bounded lookahead),
`..._b3_l4.sv` (4-entry bounded lookahead), and
`..._b3_split.sv` (independent mode-split ingress queues, `q8_scale_dual_radix4.sv`
reused unmodified from Phase B1 in all three). Real, counter-to-hypothesis
result: **bounded lookahead (l2/l4) makes collateral WORSE than Phase B2,
not better** -- at 20-25% density it exceeds B2's own residual slowdown
on every mode measured, and even regresses pure single-mode streams
(+2.4-23.3%) where there is nothing to bypass, because enabling bypass
increases pressure on the same bounded shadow-retirement structure Phase
B2 already used, and the lookahead window's own priority-encode/
compaction logic adds constant per-issue latency independent of whether
a bypass ever fires (see phase-b3.md's own "Why lookahead makes
collateral worse, not better" section for the full analysis). **Split
ingress queues avoid both problems** and are the one candidate that
performs better than B2: it meets the <=10% collateral bar at 20%
density (not 25%), meets the >=15%-vs-B2 density-range improvement
target (17.8%/21.4% at 20%/25%), and is strictly faster than B2 on every
pure stream (-30% to -47%) -- but it does not meet the strict full
20-25%-density collateral bound, nor the adversarial-HOL
50%-stall-cycle-reduction bound (best available proxy: 15.6%). Phase
B3's own decision: `CONTINUE` (experiment-branch-only), with B3-split
identified as the one candidate worth building on if this line of work
continues, and the shadow-retirement structure (not head-of-line
blocking itself) identified as the now-dominant residual constraint at
25% density.

## Phase B4 (follow-up)

Two independent parts, in order (Part A committed and verified before
Part B's own performance data was interpreted, per this phase's own
explicit task requirement): see [phase-b4.md](phase-b4.md) for the full
record.

**Part A (provenance safety)**: fixed the real defect Phase B3's own
reproduction found -- `--phase b3 --quick` had silently overwritten the
committed 8,042,500-transaction canonical results with a 125,750-
transaction quick-mode run, because `scripts/run-exp-q8-divider-002.sh`'s
own `RESULTS_DIR` pointed straight at the canonical
`experiments/EXP-FPGA-DIV-002/results/` directory unconditionally, every
run, regardless of mode. Redesigned so every run's own artifacts live
under a run-scoped staging directory
(`build-exp-q8-divider-002/<phase>/<quick|full>/<run-id>/`), never the
canonical directory directly; canonical promotion is now a separate,
explicit `--promote-results` step, hard-gated on run mode, test results,
real transaction counts, source-file integrity, and independent schema
validation, publishing a diff before any canonical write and swapping
files in atomically (per-file). A provenance manifest
(`scripts/gen-run-manifest.py`) and independent validator
(`scripts/verify-exp-q8-divider-002-results.py`) back every run; a
5-test regression suite (`scripts/test-exp-q8-divider-002-provenance.sh`)
proves the fix against the real script, not a simulation of it. Real
bugs found and fixed during this work (disclosed in phase-b4.md and
results/b4-run-provenance.md): a `set -e`/`timeout` interaction in the
regression-test script itself, a relative-path bypass of the canonical-
directory guard, an overly strict git-commit check that could not
accommodate this task's own required "commit Part A, then promote Part
B" sequencing, and a promotion-record path bug found while promoting
Part B's own real results for the first time.

**Part B (retirement pressure)**: a software reference model of
B3-split's own scheduling rules
(`scripts/b4-retirement-model.py`) quantified the real bottleneck behind
Phase B3's own remaining 20-25%-density collateral gap: **strict in-order
retirement dominates at every density measured (65.6-80.5% of stall
cycles, increasing with density)**, not head-of-line blocking (already
addressed by B3-split's own mode-split queues), not downstream
backpressure (negligible even under sustained heavy backpressure), and
not ingress arbitration or sequence bookkeeping (both ~0 by
construction in this architecture). Reading B3-split's own RTL found the
real, addressable mechanism: its Q8_0/Q4_0 encode hold registers
unconditionally captured every completion for at least one extra cycle
before checking if it was already its turn to retire, extending the
same engine's own effective back-to-back service time. Three bounded
candidates were built on B3-split (task's own required base, kept
unchanged) and evaluated at real scale (seven full 8,382,500-transaction
Verilator cosim runs, 0 mismatches/ordering errors/drops/duplicates/
reset failures/starvation violations across all seven):
`rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4_b4_r1.sv`
(single decode-class completion register, replacing B3-split's own
4-entry shadow array), `..._b4_r2.sv` (fixed 2-entry completion queue),
`..._b4_r3.sv` (direct-retire bypass for the encode hold registers,
storage unchanged). Real result: **R3 is a genuine, if partial,
improvement over B3-split** (4.5-4.8% faster overall at 20-25% density,
meets the <=10% collateral bar at 20% density on all three modes, the
closest of any candidate to the 25%-density bar, zero pure-stream
regression) but does not meet the strict 25%-density collateral bound,
the >=10%-overall-improvement bound, or the >=35%-adversarial-reduction
preferred bound. **R1 and R2 are real, measured regressions**, not
improvements -- R1 severely so (a measured -84.3% adversarial-retirement
regression: removing shadow capacity down to one slot saturates
immediately under dense mixed traffic). R1+R3 was not built as a
follow-up candidate: R1 alone is a severe regression, not a
neutral-or-mild one, so there is no plausible mechanism by which
stacking it under R3 would beat R3 alone (disclosed reasoning, not an
assumption). Isolated-wrapper synthesis (stubbed dividers, real
scheduler logic) found R1/R2/R3's relative area delta vs. B3-split
physically coherent with each candidate's own storage change (R1 -5,240
cells, R2 -3,632, R3 +1,138, ESTIMATED from a disclosed non-
representative absolute baseline). `q8_scale_dual_radix4`'s own -97.76%
area reduction is untouched by any B4 candidate. Phase B4's own
decision: `CONTINUE` (experiment-branch-only), with R3 (direct-retire
bypass) identified as the one candidate worth building on if this line
of work continues.
