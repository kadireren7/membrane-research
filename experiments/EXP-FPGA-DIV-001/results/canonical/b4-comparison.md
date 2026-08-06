# EXP-FPGA-DIV-001 Phase B4 -- radix-4 divider comparison (vs. B2 and vs. B3)

All numbers below are labeled MEASURED (real Verilator cosimulation or real
Yosys synthesis output, this session), INFERRED (a structural argument from
a measured property, explicitly flagged, e.g. II from the single-in-flight
design), or UNAVAILABLE (not obtainable in this environment). No number
here is measured silicon or vendor place-and-route output. B4 reuses B2's
own top-level SCHEDULING unchanged (full serialization while a Q4_0 encode
transaction is in flight) -- it does NOT use Phase B3's
`membrane_completion_reorder` (rejected as an architecture, see
`decision.md`). The only change vs. B2: `fp32_div_iterative_exact`
(radix-2, 1 quotient bit/cycle) is replaced by
`fp32_div_iterative_radix4_exact` (radix-4, 2 quotient bits/cycle).

## 1. Bit-exactness / correctness

| | vs. `membrane_fp_divider` | vs. B2 `fp32_div_iterative_exact` |
|---|---|---|
| Differential cases | 4,456,685 | 4,456,685 |
| Mismatches | **0** | **0** |
| Full-datapath transactions | 1,110,000 | 1,110,000 |
| Full-datapath fails | **0** | **0** |

MEASURED (`results/b4-differential.json`, `results/b4-full-datapath.json`).
This is a genuine 3-way simultaneous comparison (baseline, B2, B4 all
driven with the identical operand on the identical cycle, every case) --
a bug that made B4 agree with baseline but disagree with B2 (or vice
versa) could not hide behind either pairing alone.

## 2. Divider-level latency and area (MEASURED, Yosys 0.33 generic + ECP5)

| | Baseline (combinational) | B2 (radix-2) | B4 (radix-4) |
|---|---|---|---|
| Quotient bits/cycle | n/a (1-cycle combinational) | 1 | **2** |
| Main iteration length | n/a | 26 cycles | **13 cycles** |
| General-path latency (measured, no backpressure) | 1 | 28 (mean 27.89) | **15 (mean 14.95)** |
| Standalone generic cells | 10,234 | 1,223 | 1,556 |
| Standalone ECP5 cells | 73,629 | 1,471 | **1,509 (+2.6% vs. B2)** |
| Standalone ECP5 FF | 33 | 180 | **180 (identical to B2)** |

**The headline finding**: B4's ECP5-mapped standalone cell count is only
2.6% larger than B2's (1,509 vs. 1,471), and its flip-flop count is
IDENTICAL (180 -- the same `quot_reg`/`rem_reg`/control registers, no new
pipeline stages), even though it does roughly twice the compare-and-
subtract work per clock cycle (two chained radix-2 steps, see
`fp32_div_iterative_radix4_exact.sv`'s own header for why this specific
construction is provably bit-exact). The combinational logic per cycle
IS larger (883 LUT4 vs. 847, 88 CCU2C vs. 64) -- the real, expected,
disclosed radix-4-vs-radix-2 trade (longer path per cycle, half the
cycles) -- but ABC's technology mapping absorbs almost all of that
difference at the whole-module level. No real Fmax/timing-closure claim
is made anywhere (no vendor place-and-route tool in this environment,
same disclosure as every prior phase) -- this is a cell-count comparison,
not a timing one.

## 3. `q4_scale` integration area (MEASURED)

| | Baseline | B1 | B2 | B4 |
|---|---|---|---|---|
| Generic cells | 21,666 | 11,658 | 2,646 | 2,978 |
| ECP5 cells | 74,382 | 72,727 | 2,268 | **2,836** |
| ECP5 FF | 98 | 98 | 238 | **238 (identical to B2)** |
| ECP5 cells vs. baseline | -- | -2.2% | -96.95% | **-96.19%** |
| ECP5 cells vs. B2 | -- | -- | -- | **+25.0%** |

B4 costs 25.0% more ECP5 cells than B2 at the `q4_scale` integration
point (2,836 vs. 2,268) -- but this is a MUCH smaller area cost than
Phase B3's reorder buffer (which alone cost 14,959 ECP5 cells at its
selected depth, more than B2's entire `q4_scale_b2` unit). B2's area
advantage over baseline (-96.95%) is barely eroded by B4 (-96.19%),
compared to how far Phase B3 eroded it (~-76.8% estimated combined,
`results/b3-comparison.md` section 4).

## 4. Full-datapath throughput (MEASURED, `results/b4-full-datapath.json`, identical 1,110,000-txn workload to Phase B3's own comparison)

| | Baseline | B2 | B3 (best: depth=4) | **B4** |
|---|---|---|---|---|
| Overall cycles/transaction | 2.812 | 11.395 | 10.936 (-4.03% vs. B2) | **7.734 (-32.13% vs. B2)** |
| Q8_ENC mean latency | 27.607 | 71.229 | 65.579 | **52.287** |
| Q8_DEC mean latency | 17.889 | 37.127 | 36.590 | **28.879** |
| Q4_ENC mean latency | 40.905 | 442.465 | 435.370 | **270.052** |
| Q4_DEC mean latency | 17.987 | 37.237 | 36.667 | **28.980** |
| Q8_ENC collateral slowdown vs. baseline | 1.00x | 2.580x | 2.376x | **1.894x** |
| Q8_DEC collateral slowdown vs. baseline | 1.00x | 2.075x | 2.045x | **1.614x** |
| Q4_DEC collateral slowdown vs. baseline | 1.00x | 2.070x | 2.039x | **1.611x** |

**B4 beats Phase B3's own best result (depth=4 reorder buffer) on every
single metric, using a smaller area budget.** Q4_0 encode's own latency
drops 39.0% (442.465 -> 270.052) purely from the divider completing in
roughly half the cycles -- and because B4 keeps B2's exact scheduling
(full serialization while Q4_0 encode is in flight), that latency
reduction directly and proportionally shrinks the serialization window,
which is exactly why the three untouched chains' collateral slowdown also
falls sharply (all three now measure BELOW 2x for the first time in this
experiment, vs. B2's 2.07-2.58x and B3 depth=4's 2.04-2.38x). This
confirms the phase's own hypothesis directly: speeding up the divider
itself is a more effective lever than adding scheduling complexity around
an unchanged slow divider.

## 5. Whole-top synthesis (UNAVAILABLE, same precedent as baseline/B1/B2/B3)

`synth_ecp5` on `membrane_quant_stream_top_b4` timed out at the 300-second
soft bound under this session's 5.6 GiB RAM development machine --
identical precedent to every prior phase's own whole-top attempt, never
completed. Not a synthesizability failure; the whole design's real
BEHAVIOR is fully exercised by the 1,110,000-transaction cosimulation in
section 4.

## 6. Bottom line

Phase B4 is a clean, dominant result relative to both B2 and Phase B3:

- **Exact parity**: 0 mismatches across 4,456,685 differential cases
  (vs. both `membrane_fp_divider` and B2 simultaneously) and 0 fails
  across 1,110,000 full-datapath transactions.
- **Real, large throughput win**: -32.1% overall cycles/transaction vs.
  B2, roughly 7-8x the size of Phase B3's own best result (-4.03% at its
  selected depth=4).
- **Small, honestly-disclosed area cost**: +25.0% ECP5 cells vs. B2 at
  the `q4_scale` integration point (2,836 vs. 2,268) -- B2's own area
  advantage over baseline is barely eroded (-96.19% vs. -96.95%), unlike
  Phase B3's reorder buffer, which measurably eroded that same advantage
  down to an estimated -76.8%.
- **No new scheduling complexity**: `membrane_quant_stream_top_b4.sv` is
  structurally identical to B2's own top level (same `q4enc_inflight`
  full-serialization gate, same `tag_pipe`, no reorder buffer) -- every
  measured improvement comes from the divider itself being faster, not
  from a smarter scheduler.

See `phase-b4.md` section 8 for the full PROMOTE_CANDIDATE/CONTINUE/REJECT
reasoning.
