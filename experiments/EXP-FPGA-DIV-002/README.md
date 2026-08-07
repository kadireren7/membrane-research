# EXP-FPGA-DIV-002 — Q8_0 divider/scheduler research record

Five-phase investigation into the Q8_0 encode/decode datapath's divider
pair and its input-scheduling logic. **No part of this experiment has
been promoted to production.** Every RTL file here is experimental, and
the current status after Phase B4 is `RESEARCH_COMPLETE_NO_PROMOTION` —
see "Status" below.

## Problem

`q8_scale.sv` (production, unmodified throughout this experiment)
computes both `d = amax/127.0` and `id = 127.0/amax` using two full,
wide `membrane_fp_divider` instances — 123,742 ECP5 cells measured at
this integration point (`results/canonical/synthesis.csv`). This
experiment asked, in order: can the two dividers be replaced by
something smaller without losing bit-exactness (Phase A → B1), and once
they are, does the resulting scheduler create new collateral cost on
other in-flight transaction classes, and can that cost be reduced
(Phase B2 → B3 → B4)?

## Baseline

Two parallel `membrane_fp_divider` instances, fixed single-cycle
latency, no handshake, no scheduling contention (`d` and `id` compute
independently). 123,742 ECP5 cells (`results/canonical/synthesis.csv`).
Overall 5.47-5.48 cycles/transaction across the full correctness runs
(`results/canonical/b3-candidate-comparison.md`, `b4-candidate-comparison.md`)
— the number every later phase's "collateral slowdown vs. baseline"
figure is measured against.

## Phase timeline

Exact figures below are drawn directly from `results/canonical/*.md`/
`*.json`/`*.csv`. Where this timeline and any archived phase document's
prose disagree, the canonical artifact is authoritative — see
`methodology.md`.

### Phase A — reciprocal/algebraic sharing feasibility

**Question**: can one divider's result be reused to derive the other
(`d` and `id` are reciprocals of each other, up to the `/127.0`
scale), avoiding a second wide divider entirely?

**Tested**: reconstructing `id` from `1/d`; reconstructing `d` from
`1/id`; a constant-reciprocal multiply (`amax * (1/127)`) in place of
one full division.

**Result**: **not bit-exact**, on all three candidates. 2,050,239
differential cases (`results/canonical/feasibility-differential-full.txt`):
reconstructed-`1/d` mismatched 513,433 cases (25.04%), reconstructed-`1/id`
mismatched 577,474 cases (28.17%), constant-reciprocal multiply mismatched
93,043 cases (4.54%) — all real 1-ULP-or-worse mismatches against the
production reference, concentrated in denormal/boundary/random-amax
cases, not rounding noise that averages out.

**Decision**: reject all three; pursue an exact dual-radix-4-divider
design instead of any shared/reconstructed approach.

### Phase B1 — dual exact radix-4 dividers

**Tested**: `q8_scale_dual_radix4.sv` — two `membrane_fp_divider_radix4`
instances (the same radix-4 divider module already in production for
Q4_0's `id=1/d` path) in place of the two wide `membrane_fp_divider`
instances.

**Exactness**: bit-exact. 4,052,224 differential cases
(`results/canonical/b1-differential.json`), 0 mismatches on `d` or `id`,
0 drops/duplicates/ordering errors, including the pre-existing baseline
negative-zero quirk reproduced exactly (not fixed).

**Area proxy**: **-97.76% ECP5 cells** at the integration point
(123,742 → 2,775, `results/canonical/b1-synthesis.csv`) — below even
the naive 2×-single-instance estimate (3,018), attributed to Yosys/ABC
resource-sharing between the two parallel radix-4 instances.

**Performance**: the divider itself is no longer single-cycle — measured
mean latency 14.888 cycles under random backpressure (max 34), and a
measured initiation interval of 16 cycles (2,000 back-to-back cases, no
backpressure). This single-in-flight-per-divider behavior is what forces
full-serialization scheduling against other in-flight transaction
classes, the problem every subsequent phase addresses.

**Decision**: `PROMOTE_CANDIDATE` — an experiment-branch-internal
research verdict (the design is worth building on), explicitly not a
production merge authorization.

### Phase B2 — scheduler collateral stalls

**Tested**: a bounded, tag-based scheduler (`SHADOW_DEPTH` 1 or 2 hold
slots) allowing a limited number of other-mode transactions to bypass a
blocked Q8_0/Q4_0-encode transaction, instead of Phase B1's unconditional
full-serialization block.

**Improvement**: real and material. 6,250,000 transactions across 4 full
runs, 0 mismatches. 28.5-32.7% overall cycles/transaction better than B1
at the shipped default `SHADOW_DEPTH=1` (32.4-36.6% at depth 2, a modest
further gain), eliminating 67-77% of B1's own collateral stall at
20-25% Q8_0-encode density. At light density (10%), B2 doesn't just meet
but beats the baseline — measured collateral is *negative* (faster than
untouched production) on all three affected modes.

**Remaining limitation**: the strict ≤10% collateral bound (vs.
baseline, at 20-25% Q8_0-encode density) is **not met**: residual
collateral of 16.0-25.7% across Q8_0-decode/Q4_0-encode/Q4_0-decode.
Root cause (`results/canonical/b2-stall-root-cause.md`): with strict
in-order retirement preserved and bounded shadow depth, the residual is
dominated by `IN_FIFO_DEPTH=16`'s own queueing capacity interacting with
Q8_0/Q4_0-encode's inherent single-in-flight service time — not by
shadow-queue depth itself, which is why depth 1→2 only buys 4-6
percentage points, not a step-change.

**Decision**: `CONTINUE`.

### Phase B3 — head-of-line hypothesis

**Hypothesis**: the residual B2 collateral is head-of-line (HOL)
blocking at the input queue — a blocked Q8_0/Q4_0-encode transaction at
the head prevents *any* other-mode transaction behind it from being
issued, even when the scheduler has capacity to accept one. Bounded
lookahead (peek past the head, issue a ready transaction out of order)
should reduce it.

**L2/L4 lookahead result**: **the hypothesis was wrong** — lookahead
made density-sweep collateral *worse*, not better. At every
density/mode measured, 2-entry (`b3l2`) and 4-entry (`b3l4`) lookahead
collateral exceeded B2's own (e.g. 20%-density Q8_0-decode: B2 +12.39%,
`b3l2` +19.73%, `b3l4` +37.28%). Root cause
(`results/canonical/b3-candidate-comparison.md`): shadow-retirement
contention plus the constant per-issue lookahead/compaction overhead
outweighs the head-of-line bypass benefit lookahead was meant to
capture — a real, counter-to-hypothesis result, disclosed rather than
reframed. (On the *separate* adversarial-HOL-pattern metric, lookahead
does show a modest gain over B2, +3.9%/+11.7% — the density-sweep
collateral bound is the metric that regressed.)

**Split-queue result**: mode-split ingress queues (`b3_split` — Q8_0/
Q4_0-encode transactions queue separately from other modes, removing
the head-of-line dependency directly instead of bypassing it) **worked**:
met the ≤10% collateral bound at 20% density on all three modes
(+9.58% Q8_0-dec, +0.59% Q4_0-enc, +9.79% Q4_0-dec), and ran 17.8-21.4%
faster than B2 on the 20-25%-density profiles specifically. 8,042,500
transactions/candidate across all 6 candidates this phase tested, 0
mismatches.

**Decision**: `CONTINUE`, `b3_split` selected as the base for Phase B4.

### Phase B4 — provenance safety and retirement pressure

**Provenance safety (Part A)**: a real, disclosed defect existed in the
experiment driver script — quick-mode runs could silently overwrite
full-mode canonical results under the same output path. Fixed with
run-scoped staging directories, an explicit `--promote-results` step, a
run manifest, and an independent validator
(`scripts/verify-exp-q8-divider-002-results.py`) — committed and
verified (5/5 regression tests) *before* any Part B performance result
was interpreted, per this phase's own explicit sequencing gate. Full
account: `results/canonical/b4-run-provenance.md`.

**Retirement-pressure finding**: a software retirement-state model
(`scripts/b4-retirement-model.py`) found that **strict in-order
retirement, not head-of-line blocking, now dominates** stall cycles on
top of `b3_split`: 65.6-80.5% of stall cycles across density profiles
are retirement-related, not input-queueing-related
(`results/canonical/b4-retirement-analysis.md`).

**Tested**: R1 (one completion slot per transaction class), R2 (2-entry
shared completion queue), R3 (direct-retire bypass — skip completion
storage entirely when the downstream consumer can accept immediately).

**R1/R2 result**: both **real, disclosed regressions**. R1 regressed
-84.3% on the adversarial-retirement pattern (6.667 vs. `b3_split`'s
3.618 cycles/txn) — reducing completion-storage capacity made retirement
pressure sharply worse, not better. R2 regressed a smaller but still
real -3.7% on the same pattern.

**R3 result**: a real, partial improvement. 4.5-4.8% faster than
`b3_split` overall across density profiles, +4.1% faster on the
adversarial-retirement pattern, and the only B4 candidate to meet the
≤10% collateral bound at 20% density (+4.32%/-4.14%/+4.51% across the
three modes) — though not at 25% density (+11.79%/+1.26%/+11.71%).
8,382,500 transactions/candidate across all 7 candidates this phase
tested (baseline/B1/B2/`b3_split`/R1/R2/R3), 0 mismatches.

**Decision**: `CONTINUE` at the time — see "Status" below for how this
cycle was subsequently closed.

## Status

**`RESEARCH_COMPLETE_NO_PROMOTION`** (closed after Phase B4 — see
`ROADMAP.md`'s "EXP-FPGA-DIV-002" entry for the full closure record).
This is not a rejection of the underlying idea: Phase B1's
exact-radix-4-divider result is real and substantial (-97.76% area,
bit-exact), and Phase B4's R3 is a real, if partial, scheduling
improvement on top of it. It is a disclosed decision to stop iterating
on *this* architecture family without having met the strict ≤10%
collateral bound at 25% density, rather than continue indefinitely
(`B5`, `B6`, ...) chasing a target this family's own retirement-pressure
ceiling makes unlikely to reach without a larger reorder structure —
explicitly out of scope for every phase in this experiment. Any further
work on this specific bottleneck is tracked as a **new** experiment ID
in `ROADMAP.md`, not a `B5` continuation.

## What went to production, and what didn't

**Nothing from this experiment has been merged to `kadireren7/membrane`'s
`main`.** The production Q4_0 datapath's own exact-radix-4 divider (a
related but separate experiment, `EXP-FPGA-DIV-001`, promoted via
[kadireren7/membrane#2](https://github.com/kadireren7/membrane/pull/2))
is not the same result as this experiment's Q8_0 work — the two share an
architectural idea (exact radix-4 division replacing a wide combinational
divider) but this experiment evaluates it for the wider, dual-instance
Q8_0 case with genuinely new scheduling questions EXP-FPGA-DIV-001 never
needed to answer (Q4_0 has one divider, not two; no shared-scheduler
collateral-cost question arises there).

## Limitations

No real FPGA board, no vendor place-and-route toolchain, and no measured
Fmax, LUT utilization, or power exist anywhere in this record, in any
phase. Every cell count is a Yosys 0.33 generic or `synth_ecp5`
synthesis-tool proxy result. Full top-level synthesis timed out at every
phase from B2 onward; Phase B4's isolated-wrapper synthesis (trivial
fixed-latency stand-ins for the divider engines) is the only method that
produced any real number for the B3/B4 scheduler candidates, and even
its absolute cell counts are inflated by the `-noshare` flag required to
avoid the same ABC timeout — only cross-candidate *deltas* are treated
as meaningful (see `methodology.md`).

## Result files

Canonical artifacts in `results/canonical/` — differential JSON,
synthesis CSV, retirement-taxonomy CSV, run-manifest/promotion-record
JSON, and narrative `.md` analysis for each phase. See
`results/schemas/README.md` for field docs and each phase's own archived
document (`archive/phase-bN.md`) for full per-phase detail.

## Reproduction

See `reproduction/README.md` for exact `--phase`/`--quick`/`--full`
commands, and `methodology.md` for exactness rules, toolchain, and the
measurement classification (SIMULATED/MEASURED_BY_TOOL/ESTIMATED/
UNAVAILABLE) used throughout.

## Provenance

Migrated to `membrane-research` from `kadireren7/membrane`'s
`experiment/q8-divider-pipeline` branch, commit
`61ce8adc4733512a78dcf5c04844e6b85da04b54` (SHA256-verified, see
`provenance/import-manifest.json` at this repository's root). That
branch is not deleted and remains the authoritative git history.
Production source this experiment compares against
(`membrane_fp_divider_radix4.sv`, `membrane_fp_pkg.sv`, and every other
`rtl/*.sv`/`rtl/tb/*` file the experimental tops instantiate) stays in
`kadireren7/membrane`, `main`, under `rtl/` — not duplicated here.
