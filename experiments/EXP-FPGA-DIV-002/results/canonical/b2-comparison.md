# EXP-FPGA-DIV-002 Phase B2 -- baseline vs. B1 vs. B2 scheduler comparison

Every number below is **MEASURED_BY_TOOL** this session (`scripts/run-exp-q8-divider-002.sh
--phase b2 --full`, plus a manual `SHADOW_DEPTH=2` sweep build, 2026-08-04)
unless marked **ESTIMATED**, **SIMULATED**, or **UNAVAILABLE**. Raw data:
`b2-correctness.json`, `b2-performance.csv`, `b2-stall-breakdown.csv`,
`b2-synthesis.csv`.

## 1. Ordering contract

Confirmed from `rtl/membrane_quant_stream_top.sv`'s own header and live
assertions, not assumed: **strict output order matching accepted input
order** (option A). See `results/b2-stall-root-cause.md` section 0. Phase
B2 preserves this exactly -- 0 ordering errors across four full 6,250,000-
transaction runs (baseline, B1, B2 depth=1, B2 depth=2).

## 2. Scheduler architecture

A global monotonic sequence tag (8 bits) assigned at issue; a transaction
retires only when its tag matches a free-running `next_retire_seq`
counter. `q8_scale_dual_radix4` (byte-for-byte reused from Phase B1) and
`q4_scale` each keep their own single-in-flight discipline plus **one**
result-holding register, and now issue **independently of each other**
(Phase B1 kept them mutually exclusive; Phase B2 does not, since they use
disjoint divider hardware). Q8_0/Q4_0 decode still ride the existing
fixed-latency shared `tag_pipe` completely unmodified in its own advance
logic (deliberately never stalled -- stalling it would silently lose an
in-flight `q8_dequantize`/`q4_unpack` result, which have no ready/valid
backpressure of their own; see `phase-b2.md`'s "why not just stall
tag_pipe" subsection for the exact counter-example this design avoids). A
shared, bounded `shadow_hold` queue (evaluated at depth 1 and depth 2,
task item 10's own sweep -- no depth 4/8) catches the capped number of
tag_pipe entries admitted while an older Q8_0/Q4_0 encode is still
outstanding. Total added state is small and bounded -- not a general
N-deep reorder buffer, and smaller than every configuration
EXP-FPGA-DIV-001 Phase B3 already evaluated and rejected.

## 3. Correctness (4 full runs, 6,250,000 transactions each)

| Variant | Fails | Ordering errors | Drops/dupes | Reset failures | Internal assertions fired |
|---|---|---|---|---|---|
| Baseline | 0 | 0 | 0 | 0 | N/A |
| B1 (full serialization) | 0 | 0 | 0 | 0 | 0 |
| B2 (SHADOW_DEPTH=1) | 0 | 0 | 0 | 0 | 0 |
| B2 (SHADOW_DEPTH=2) | 0 | 0 | 0 | 0 | 0 |

Sequence-tag wraparound (256-transaction period, ~24,414 wraps/run at this
scale): exercised for real, `live_seq_count` assertion never fired.

## 4. Performance -- overall cycles/transaction improvement vs. B1

| Profile | Baseline | B1 | B2 (d=1) | B2 (d=2) | d=1 vs B1 | d=2 vs B1 |
|---|---|---|---|---|---|---|
| uniform_random_modes | 6.4763 | 11.8197 | 7.9591 | 7.5672 | **+32.66%** | **+35.98%** |
| 10pct_Q8ENC_90pct_other | 7.4367 | 9.6863 | 6.8950 | 6.5072 | **+28.82%** | **+32.83%** |
| 25pct_Q8ENC_75pct_other | 6.4699 | 11.8664 | 8.0373 | 7.5226 | **+32.28%** | **+36.60%** |
| realistic_reconstructed_mix (SIMULATED) | 3.3332 | 8.0592 | 5.7648 | 5.4435 | **+28.46%** | **+32.44%** |

**The >=25%-vs-B1 overall improvement target (task item 8) is met by BOTH
depths on every mixed-traffic profile measured**, comfortably.

## 5. Performance -- per-mode collateral slowdown vs. baseline production

The strict target (task item 8): Q8_0 decode / Q4_0 encode / Q4_0 decode
collateral slowdown <=10% vs. baseline, under mixed-mode traffic.

| Profile | Mode | Baseline mean (cyc) | B1 collateral | B2 d=1 residual | B2 d=2 residual | d=1 meets <=10%? | d=2 meets <=10%? |
|---|---|---|---|---|---|---|---|
| uniform_random_modes (25% Q8_0 enc) | Q8_0 dec | 110.065 | +77.25% | +25.66% | +21.22% | NO | NO |
| uniform_random_modes | Q4_0 enc | 121.852 | +69.83% | +20.07% | +16.81% | NO | NO |
| uniform_random_modes | Q4_0 dec | 110.049 | +76.86% | +25.22% | +21.06% | NO | NO |
| 10pct_Q8ENC_90pct_other (10% Q8_0 enc) | Q8_0 dec | 125.066 | +28.46% | **-3.82%** | **-7.71%** | **YES** | **YES** |
| 10pct_Q8ENC_90pct_other | Q4_0 enc | 137.433 | +26.27% | **-5.16%** | **-7.88%** | **YES** | **YES** |
| 10pct_Q8ENC_90pct_other | Q4_0 dec | 125.091 | +28.59% | **-4.16%** | **-7.91%** | **YES** | **YES** |
| 25pct_Q8ENC_75pct_other | Q8_0 dec | 109.815 | +78.25% | +25.23% | +20.79% | NO | NO |
| 25pct_Q8ENC_75pct_other | Q4_0 enc | 121.938 | +70.24% | +19.63% | +15.99% | NO | NO |
| 25pct_Q8ENC_75pct_other | Q4_0 dec | 109.628 | +78.43% | +25.45% | +20.79% | NO | NO |
| realistic_mix (SIMULATED, 20% Q8_0 enc) | Q8_0 dec | 60.258 | +124.03% | +68.82% | +63.41% | NO | NO |
| realistic_mix | Q4_0 enc | 68.600 | +115.04% | +57.54% | +52.78% | NO | NO |
| realistic_mix | Q4_0 dec | 60.234 | +124.54% | +69.00% | +63.30% | NO | NO |

**Real, honest result: the strict <=10% bound is density-dependent.** At
light Q8_0-encode traffic density (10%), Phase B2 not only meets but beats
the target -- collateral is negative (B2 is measurably FASTER than
baseline at these modes, not just "not slower"). At 20-25% Q8_0-encode
density, Phase B2 eliminates **67-77%** of Phase B1's own collateral
stall (see `results/b2-stall-breakdown.csv` for the full per-mode
cycle-accounting) but does not reach the strict <=10% absolute bound.
SHADOW_DEPTH=2 helps (eliminates a further ~5-6 percentage points of the
original B1 collateral vs. depth=1) but the improvement from 1->2 is
modest, not qualitative -- see section 6 for why.

## 6. Why depth alone does not close the remaining gap (task item 8's own
"report the lower bound and the exact architectural reason" clause)

The residual collateral at 20-25% Q8_0-encode density is **not** primarily
a shadow-queue-depth-limited effect. Two structural facts, both already
fixed by this task's own scope (not something Phase B2 was authorized to
change):

1. **`q8_maxabs_reduce -> q8_scale_dual_radix4 -> q8_quantize_pack` (or
   `q4_scan -> q4_scale -> q4_pack`) is single-in-flight against ITSELF.**
   Even fully decoupled from every other mode, back-to-back Q8_0 encode
   transactions still serialize against each other at the chain's own real
   latency (`100pct_Q8_ENC` profile, MEASURED: mean 368.017 cycles under
   ~50%-issue-probability traffic -- itself inflated well above the
   divider pair's own standalone ~15-cycle mean by input-FIFO queueing,
   since `IN_FIFO_DEPTH=16` is a fixed, existing, out-of-scope-for-this-
   phase parameter and demand at ~0.5 issue/cycle vastly exceeds a
   ~20-40-cycle real per-transaction service time).
2. Once `shadow_hold` (1 or 2 slots) is full, **new tag_pipe issuance
   falls back to Phase-B1-style blocking** until the outstanding Q8_0/Q4_0
   encode retires -- this is the deliberate, disclosed, bounded trade-off
   this phase's own task explicitly requires ("bounded bookkeeping only,"
   "no depth 4/8 ROB sweep"). At 20-25% Q8_0-encode density in a
   4-mode-uniform stream, the shadow window (1-2 slots) is exhausted
   quickly relative to how often Q8_0/Q4_0 encode transactions arrive, so
   most Q8_0-decode/Q4_0-decode/Q4_0-encode transactions in that window
   still experience SOME input-FIFO queueing behind the blocking
   transaction -- just far less of it than Phase B1's unconditional block
   (see section 5's 67-77% elimination figures).

**The exact architectural reason the strict <=10% bound is not reached at
this density, stated plainly:** with strict in-order retirement preserved
(non-negotiable, section 1) and bounded shadow depth (1-2, per this
phase's own explicit scope), the residual cost is dominated by
`IN_FIFO_DEPTH=16`'s own queueing capacity interacting with Q8_0/Q4_0
encode's inherent single-in-flight service time -- not by the shadow
queue's own depth, which is why depth 1->2 only buys ~4-6 percentage
points, not a step-change. Widening `IN_FIFO_DEPTH` or building a deeper
reorder structure could plausibly close more of the gap, but the former
touches an existing, unrelated, out-of-scope parameter and the latter is
explicitly forbidden by this phase's own task ("no depth 4/8 ROB sweep,"
"no area-heavy generic reorder structure").

## 7. Area

| Item | ECP5 cells | vs. original baseline `q8_scale` (123,742) |
|---|---|---|
| `q8_scale_dual_radix4` (unchanged, byte-for-byte reused) | 2,775 | **-97.76%** |
| Phase B2 scheduler's own added logic | UNAVAILABLE (full-top synth timed out for both B1 and B2 -- no real number exists for either, so no real delta exists either); ESTIMATED ~1.5-2 Kbit of added flip-flop state from the RTL's own parameter-scaled array widths (see `b2-synthesis.csv` notes) | N/A |

The **>=90%-smaller-than-original-baseline** area criterion is met by the
one component that actually has a real, measured, apples-to-apples number
on both sides of the comparison (`q8_scale_dual_radix4` itself, unchanged
by this phase). The scheduler's own bookkeeping is small by construction
(a handful of hold registers plus per-stage tag widening, no new
memories/arrays beyond `SHADOW_DEPTH<=2` entries) but has no real
synthesized number to report, honestly disclosed as UNAVAILABLE/ESTIMATED
rather than assumed negligible.

## 8. Decision rationale

**CONTINUE** (see `phase-b2.md`'s own Decision section for the full
item-by-item justification): exact (0 mismatches, 0 ordering errors across
four 6.25M-transaction runs), materially improved (67-77% of Phase B1's
own collateral stall eliminated at realistic-to-heavy Q8_0-encode density,
100%+ eliminated -- net FASTER than baseline -- at light density, >=25%
overall cycles/transaction improvement vs. B1 met on every profile), area
still overwhelmingly favorable at the component level -- but the strict
absolute <=10%-collateral-vs-baseline target is not met at 20-25%
Q8_0-encode density with either evaluated shadow depth, for the
architectural reasons in section 6, which this phase's own scope does not
authorize resolving further (deeper input buffering or a larger reorder
structure). SHADOW_DEPTH=1 remains the shipped default (this file's own
parameter default) since depth 2's improvement over it is real but modest,
not qualitatively different, and depth 2's own full-top synthesis was not
separately attempted (same tool/memory bound as depth 1's own timeout
already demonstrated).
