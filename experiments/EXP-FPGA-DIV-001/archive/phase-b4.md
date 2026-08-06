# EXP-FPGA-DIV-001 Phase B4 -- exact radix-4 iterative Q4 divider

Branch `experiment/fp-divider-pipeline`. Builds on Phase B2 (exact radix-2
iterative Q4 divider, `phase-b2.md`) and responds directly to Phase B3's
own outcome: B3's bounded completion reorder buffer proved correct but was
**rejected as an architecture** (`decision.md`) because its area cost
(14,959 ECP5 cells at the only depth that helped) was far larger than the
entire `q4_scale_b2` unit it existed to protect (2,268 cells), for only a
~4-5% throughput gain. Phase B4 tests the alternative hypothesis: instead
of adding scheduling complexity around an unchanged, slow divider, make
the divider itself faster, with zero new scheduling logic.

## 1. Architecture

`rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv` -- an exact,
synthesizable, multi-cycle FP32 divider producing TWO quotient bits per
clock cycle instead of Phase B2's one, halving the main iteration count
(13 cycles instead of 26 for the same 26-bit fixed-point quotient). No
`/` operator, no `real`/`shortreal`/DPI anywhere in the file.

**How it's built, and why this is honestly "radix-4"**: by definition, a
radix-4 divider produces a quotient digit from {0,1,2,3} (2 bits) per
iteration -- this module does exactly that, every ITER cycle. The
INTERNAL construction is deliberately the simplest one provably correct
by direct construction: each ITER cycle chains TWO of Phase B2's own
radix-2 restoring-division steps (shift, compare, conditional-subtract)
combinationally, registering the result once. This is a standard,
non-redundant radix-4 restoring-division construction -- NOT an SRT
divider (no redundant digit set, no carry-save arithmetic, which is what
typically underlies real high-speed radix-4 SRT dividers). The real,
disclosed trade-off: the combinational path per ITER cycle is roughly
twice as deep as B2's per-cycle path (two chained compare-and-subtract
steps instead of one) -- the well-known radix-4-vs-radix-2 trade (fewer
cycles, longer path per cycle). No real Fmax/timing-closure claim is made
anywhere (no vendor place-and-route tool exists in this environment, same
disclosure as every prior phase).

Every special-case decode/priority chain and the rounding tail
(normalize/guard/round/sticky, round-to-nearest-even, flush-to-zero/
flush-to-infinity) are copied VERBATIM from `fp32_div_iterative_exact.sv`
-- they operate purely on the final `quot_reg`/`rem_reg` values, which are
unaffected by processing 2 bits/cycle instead of 1, so no re-derivation
was needed or attempted.

## 2. Iterations

- Main iteration: **13 cycles** (`PAIR_ITER_WIDTH = MANT_ITER_WIDTH/2 =
  26/2`), each cycle two chained radix-2 steps -- vs. B2's 26 single-step
  cycles.
- `EARLY_OUT_SPECIAL` (default on, same as B2): NaN/Inf/zero denominators
  skip the iteration entirely, landing in `S_ROUND` with a 2-cycle
  latency (matches B2's own `min=2`/`min=3`-class early-out path).
- Same FSM shape as B2: `IDLE -> ITER -> ROUND -> [DRAIN] -> DONE ->
  IDLE`, same single-in-flight discipline (`in_ready` only asserted from
  `IDLE`), same asynchronous unconditional reset.

## 3. Reference behavior (task item 3)

Matches `membrane_fp_divider.sv`'s exact bit behavior (not idealized
floating-point math) -- every special-case branch, sign rule, rounding
convention, and the two disclosed non-IEEE simplifications (subnormal
operands treated as normal, flush-to-zero instead of gradual underflow)
are the SAME ones `phase-b2.md` section 1 already documents in full, since
this file's decode/rounding logic is a verbatim copy of B2's own. Not
re-derived here.

## 4. Differential verification (task item 4)

`rtl/tb/tb_fp32_div_iterative_radix4_exact.cpp` -- a genuine 3-way
simultaneous comparison (`membrane_fp_divider` vs. B2's
`fp32_div_iterative_exact` vs. B4's `fp32_div_iterative_radix4_exact`,
all driven with the identical operand on the identical cycle every case).

**Scope** (exceeds every task item 4 requirement): 4,456,685 total cases
-- reproduces Phase B2's own full 2,456,685-case category structure
(boundary sweep, specials cross product, powers of two, real Q4 runtime
d-distribution, reset recovery, throughput) with the random-denominator
pool raised from B2's 2,200,000 to 4,200,000 (+2,000,000, task item 4's
explicit ask), plus random reset timing (randomized offset into B4's own
shorter iteration), random `out_ready` backpressure on both candidates
independently, back-to-back throughput measurement, and a 10,000-cycle
per-case timeout/deadlock bound (never hit).

**Result (MEASURED, `results/b4-differential.json`)**:

| | Count |
|---|---|
| Total cases | 4,456,685 |
| Mismatches, baseline vs. B2 | **0** |
| Mismatches, baseline vs. B4 | **0** |
| Mismatches, B2 vs. B4 | **0** |
| Reset-recovery fails | 0 |
| Deadlock/timeout | none |

**Latency (MEASURED)**: B2 mean 28.139 (no-bp: min=2, mean=27.890,
max=28); B4 mean 15.194 (no-bp: min=2, mean=14.945, max=15) -- B4's
general-path latency is essentially half of B2's, exactly as the halved
iteration count predicts. **Initiation interval**: B2 measured directly at
29.000 cycles (2,000 back-to-back, no backpressure). B4's own II was not
independently isolated in this specific shared-loop stage (the loop paces
on whichever DUT is slower, i.e. B2) but is INFERRED at 15 cycles from the
same single-in-flight structural argument used for B2 (`in_ready` only
from `IDLE` => II == general-path latency), consistent with the
independently-measured no-backpressure general-path latency (min=max=15,
n=3,350,018 real cases across the whole run, not just the throughput
stage) -- see `results/b4-differential.json` for the full disclosure of
this distinction.

## 5. Q4 integration (task item 5)

New files only: `rtl/experimental/fp_div/q4_scale_b4.sv` (byte-for-byte
identical to `q4_scale_b2.sv` except `u_div_id` instantiates
`fp32_div_iterative_radix4_exact` instead of `fp32_div_iterative_exact`;
`u_div_d`, Phase B1's constant power-of-two shortcut, unchanged) and
`rtl/experimental/fp_div/membrane_quant_stream_top_b4.sv` (structurally
identical to `membrane_quant_stream_top_b2.sv` -- same `q4enc_inflight`
full-serialization gate, same shared `tag_pipe`, same direct-retire path
for Q4_0 encode -- with `q4_scale_b2` replaced by `q4_scale_b4`). **Phase
B3's `membrane_completion_reorder` is NOT used anywhere in this variant**
-- confirmed by inspection (no B3 file is even referenced by B4's build).
`q8_scale.sv` is completely untouched. No production RTL file was
modified.

## 6. Full datapath verification (task item 6)

Identical 1,110,000-transaction workload to Phase B3's own comparison
(200,000 x 4 single-mode + 100,000 mixed + 3 x 70,000 dense adversarial:
long Q4-encode burst, alternating Q4/Q8, dense random-mode), randomized
backpressure, 4 reset-safety stages, 200,000-cycle deadlock watchdog.
Baseline/B1/B2 numbers reproduced from Phase B3's own run (nothing about
those 3 variants changed); B4 is new.

**Result (MEASURED, `results/b4-full-datapath.json`)**: 0 fails, 0
dropped, 0 duplicated, 0 deadlocks, at every variant including B4.

| | Baseline | B2 | **B4** |
|---|---|---|---|
| Overall cycles/transaction | 2.812 | 11.395 | **7.734 (-32.13% vs. B2)** |
| Q4_ENC mean latency | 40.905 | 442.465 | **270.052 (-38.98% vs. B2)** |
| Q8_ENC mean latency | 27.607 | 71.229 | **52.287 (-26.60% vs. B2)** |
| Q8_DEC mean latency | 17.889 | 37.127 | **28.879 (-22.20% vs. B2)** |
| Q4_DEC mean latency | 17.987 | 37.237 | **28.980 (-22.18% vs. B2)** |

No new ROB or scheduling complexity was added beyond swapping the divider
-- the entire improvement traces directly to the divider's own halved
cycle count, exactly as intended.

## 7. Synthesis matrix (task item 7)

Full table: `results/b4-synthesis.csv`. Standalone divider: B4 costs
+2.6% ECP5 cells vs. B2 (1,509 vs. 1,471) for identical FF count (180).
`q4_scale` integration: B4 costs +25.0% ECP5 cells vs. B2 (2,836 vs.
2,268), for identical FF count (238) -- both real, measured, and small
relative to B2's own area advantage over baseline (`q4_scale` ECP5:
baseline 74,382 -> B2 2,268 [-96.95%] -> B4 2,836 [-96.19%]). Whole-top
synthesis (`membrane_quant_stream_top_b4`) timed out at the 300-second
soft bound (UNAVAILABLE, same precedent as every prior phase's own
whole-top attempt, not a synthesizability failure).

## 8. Decision

**PROMOTE_CANDIDATE.**

Every PROMOTE_CANDIDATE criterion this phase's own task spec lists is met,
with real measured numbers, not estimates:

- **Exact parity**: 0 mismatches across 4,456,685 differential cases,
  simultaneously against both `membrane_fp_divider` AND B2.
- **Full datapath clean**: 0 fails/drops/duplicates/deadlocks across
  1,110,000 transactions, including adversarial patterns and reset-safety
  stages.
- **Latency reduces meaningfully vs. B2**: Q4_0 encode's own mean latency
  falls 39.0% (442.465 -> 270.052 cycles), a direct, structural
  consequence of the divider processing 2 quotient bits/cycle instead of
  1 (not an estimate -- measured both at the component level, section 4,
  and the full-datapath level, section 6).
- **cycles/transaction improves clearly**: -32.13% vs. B2 (11.395 ->
  7.734) -- roughly 7-8x the size of Phase B3's own best result (-4.03%
  at its selected depth).
- **Far lower area cost than Phase B3's ROB**: +25.0% ECP5 cells vs. B2
  at the `q4_scale` integration point (2,836 vs. 2,268), compared to
  Phase B3's reorder buffer alone costing MORE area (14,959 cells) than
  the entire unit it was protecting.
- **Baseline area advantage substantially preserved**: `q4_scale`-level
  ECP5 reduction vs. baseline is -96.19% (B4) vs. -96.95% (B2) -- barely
  eroded, in sharp contrast to Phase B3's estimated combined -76.8%.
- **Reproducible**: `scripts/run-exp-fp-divider-001.sh --phase b4
  --quick|--full`.

No REJECT criterion applies: no parity issue, no integration issue, and
throughput/area both improve relative to B2 rather than regress. This is
the strongest result of any Phase B sub-phase in this experiment to date.

**What "PROMOTE_CANDIDATE" means here, precisely**: this experiment's own
decision remains internal to the `experiment/fp-divider-pipeline` branch
(see "Promotion status" in `experiment.md`/`decision.md`) -- it is a
recommendation that this design is READY to be considered for promotion
to `main` in a future, separate, explicitly-authorized step, not an
authorization to merge it now. This phase does not open a PR, does not
merge to `main`, and does not touch the `v0.1.0-research` tag, per this
phase's own explicit constraints.

## 9. Reproduction

`scripts/run-exp-fp-divider-001.sh --phase b4 --quick` (fast smoke: small
3-way differential run, small full-datapath run for baseline/B1/B2/B4,
elaboration-only synthesis check) or `--phase b4 --full` (the exact
numbers in this document: 4,456,685 differential cases, 1,110,000-
transaction full-datapath test per variant, complete generic+ECP5
synthesis matrix at the standalone and `q4_scale` levels, whole-top
attempt). `--resume` skips rebuilding already-built binaries and
regenerating correctly-sized golden vectors; `--output-dir` redirects
build/output artifacts.
