# EXP-FPGA-DIV-001 Phase B3 -- scheduling decoupling comparison

All numbers below are labeled MEASURED (real Verilator cosimulation or real
Yosys synthesis output, this session), ESTIMATED (a combined/summed
projection from two separately-measured numbers, explicitly flagged), or
UNAVAILABLE (not obtainable in this environment). No number here is measured
silicon or vendor place-and-route output. B3 reuses B2's divider
(`fp32_div_iterative_exact`) and Q4_0 scale unit (`q4_scale_b2.sv`)
unmodified -- this phase changes ONLY the top-level scheduling/retirement
logic (`membrane_quant_stream_top_b3.sv` + the new
`membrane_completion_reorder.sv`).

## 1. Bit-exactness / correctness

| | Baseline | B1 | B2 | B3 (d1/d2/d4/d8) |
|---|---|---|---|---|
| Component differential cases | -- | 2,204,128 | 2,456,685 | **2,456,685 (reused from B2, divider unchanged)** |
| Component mismatches | -- | 0 | 0 | **0** |
| Full-datapath transactions (this phase's workload) | 1,110,000 | 1,110,000 | 1,110,000 | **1,110,000 each** |
| Full-datapath fails | 0 | 0 | 0 | **0 at every depth** |
| Dropped / duplicated | 0 / 0 | 0 / 0 | 0 / 0 | **0 / 0 at every depth** |
| Deadlock / timeout (200,000-cycle watchdog) | none | none | none | **none at every depth** |
| Reset-while-queues-non-empty (B3-specific stage) | n/a | n/a | n/a | **pass at every depth, `outstanding` correctly returns to 0** |

All MEASURED (`results/b3-full-datapath.json`). This phase's workload
(1,110,000 transactions, including 3 dense adversarial stages: long Q4_0
encode bursts, alternating Q4_0-encode/Q8_0-encode, dense random-mode) is
deliberately larger and harsher than Phase B2's own committed 520,000-txn
number -- baseline/B1/B2 were RE-RUN against this identical new workload so
every column below is directly comparable. Do not compare the B2 numbers in
this document to `phase-b2.md`'s own 22.842-cycle Q8_ENC figure, which used
a smaller, less adversarial workload.

## 2. Full-datapath throughput (MEASURED, `results/b3-full-datapath.json`)

| | Baseline | B1 | B2 | B3 d=1 | B3 d=2 | B3 d=4 | B3 d=8 |
|---|---|---|---|---|---|---|---|
| Overall cycles/transaction | 2.812 | 2.812 | 11.395 | 15.704 | 12.051 | **10.936** | **10.856** |
| Q8_ENC mean latency | 27.607 | 27.607 | 71.229 | 185.990 | 116.164 | 65.579 | 64.606 |
| Q8_DEC mean latency | 17.889 | 17.889 | 37.127 | 166.101 | 93.528 | 36.590 | 35.538 |
| Q4_ENC mean latency | 40.905 | 40.905 | 442.465 | 467.070 | 439.114 | 435.370 | 434.885 |
| Q4_DEC mean latency | 17.987 | 17.987 | 37.237 | 166.067 | 93.554 | 36.667 | 35.443 |
| Q8_ENC collateral slowdown vs. baseline | 1.00x | 1.00x | 2.580x | 6.737x | 4.208x | **2.376x** | **2.341x** |
| Q8_DEC collateral slowdown vs. baseline | 1.00x | 1.00x | 2.075x | 9.285x | 5.229x | **2.045x** | **1.987x** |
| Q4_DEC collateral slowdown vs. baseline | 1.00x | 1.00x | 2.070x | 9.233x | 5.202x | **2.039x** | **1.971x** |

**The honest headline finding: REORDER_DEPTH 1 and 2 are WORSE than B2 on
every single metric**, not an improvement. `membrane_completion_reorder`
adds a flat +1-cycle latency to every transaction (registered write-then-
drain, see that module's own header); at depth 1-2 the concurrency window
this buys is too small, relative to this workload's Q4_0-encode frequency,
to offset that added latency -- the buffer fills almost immediately behind
any in-flight Q4_0 encode and then stalls issuance for most of its ~430+
cycle lifetime anyway (depth=1: 93.63% of all cycles are a depth-bound
stall; depth=2: 32.26%), so transactions end up waiting longer overall than
under B2's simpler full-serialization scheme.

**Only depth >= 4 is a real, measured improvement over B2**: overall
cycles/transaction -4.03% at depth=4, -4.73% at depth=8, with Q8_ENC's
collateral slowdown falling from B2's 2.580x to 2.376x (depth=4) / 2.341x
(depth=8), and Q8_DEC/Q4_DEC's falling below 2x for the first time at
depth=8 (1.987x, 1.971x). At depth=4/8 the depth-bound stall (buffer full)
drops to 6.75%/1.08% of all cycles, while the structurally-unavoidable
"the one physical Q4_0 divider is busy" stall rises to 76.50%/79.73% --
i.e. most of the remaining stall at depth>=4 is no longer a scheduling
artifact, which is also why depth=8 only buys a small additional gain over
depth=4 (diminishing returns, exactly as the scheduler's own design would
predict: a bigger buffer cannot help once the bottleneck is the single
divider itself, not buffer capacity).

## 3. Scheduler internals (MEASURED, `results/b3-full-datapath.json`)

| | B3 d=1 | B3 d=2 | B3 d=4 | B3 d=8 |
|---|---|---|---|---|
| Reorder buffer high-water mark | 1 | 2 | 4 | 8 |
| Issue-stall %, depth-bound | 93.63% | 32.26% | 6.75% | 1.08% |
| Issue-stall %, Q4_0 divider busy | 0.00% | 59.44% | 76.50% | 79.73% |
| Issue-stall %, output-FIFO slot | 0.00% | 0.00% | 0.00% | 0.00% |
| Q4_0 divider busy %, any reason | 56.64% | 73.79% | 81.33% | 81.96% |
| Cycles with simultaneous completions (both reorder ports firing) | 0 | 0 | 629 | 296 |

The buffer's high-water mark exactly equals its configured depth at every
setting -- confirming the bound is real, reached under this workload, and
not oversized. Simultaneous completions (a fast tag_pipe-mode transaction
finishing on the exact same cycle as a long-running Q4_0 encode -- the
scenario B2's own invariant explicitly forbade) were observed directly at
depth 4 and 8, confirming the two-port arbitration in
`membrane_completion_reorder.sv` was genuinely exercised under realistic
load, not merely possible in theory.

## 4. Area (MEASURED Yosys 0.33 generic + ECP5, `results/b3-synthesis.csv`)

| | Depth 1 | Depth 2 | Depth 4 | Depth 8 |
|---|---|---|---|---|
| `membrane_completion_reorder` generic cells | 1,095 | 5,894 | 13,901 | 28,850 |
| `membrane_completion_reorder` ECP5 cells | 1,091 | **17,611** | 14,959 | 19,800 |
| ECP5 FF | 541 | 1,076 | 2,141 | 4,272 |
| Payload storage bits (531 bits x depth) | 531 | 1,062 | 2,124 | 4,248 |

**A real, measured, non-monotonic anomaly**: depth=2's ECP5 cell count
(17,611) is HIGHER than depth=4's (14,959), driven by a large L6MUX21 spike
(3,196 at depth=2 vs. 3 at depth=4) -- almost certainly an ABC technology-
mapping heuristic picking a worse wide-multiplexer decomposition for that
specific bit-width, not a real complexity increase (the PRE-mapping generic
cell count IS monotonic: 5,894 < 13,901 < 28,850). Reported as measured,
not smoothed over.

**System-level area estimate** (ESTIMATED -- sum of two separately-
synthesized standalone numbers; whole-top synthesis, which would show any
real cross-module sharing ABC might find, is UNAVAILABLE at every depth
tested, see section 5): `q4_scale_b2` (2,268 ECP5 cells, unchanged from B2)
+ `membrane_completion_reorder`:

| Depth | Estimated combined ECP5 cells | vs. baseline `q4_scale` (74,382) | vs. B2 alone (2,268, -96.9%) |
|---|---|---|---|
| 1 | ~3,359 | -95.5% | area advantage nearly matched, but throughput is WORSE than B2 (section 2) |
| 2 | ~19,879 | -73.3% | area advantage substantially eroded, AND throughput still worse than B2 |
| **4** | **~17,227** | **-76.8%** | **area advantage measurably eroded (77% vs. 97% reduction) but still large; throughput IS better than B2** |
| 8 | ~22,068 | -70.3% | more area than depth=4 for only a small further throughput gain |

This is the real trade-off this phase surfaces: B2's area win at the
`q4_scale` level itself is completely untouched (same 2,268 cells), but the
NEW scheduling logic needed to safely exploit concurrency is not free --
`membrane_completion_reorder`'s wide (531-bit) per-slot payload storage
costs more silicon, at every depth tested, than the entire iterative-
divider-based `q4_scale_b2` unit B2 already shrank. The area advantage over
baseline is still large at every depth (70-96%), but it is NOT "B2's
advantage, preserved" in the tight sense -- a genuinely leaner reorder
design (storing a tag/pointer instead of the full 531-bit payload, reading
the wide data from the existing per-chain output registers only at drain
time) would very plausibly recover most of this cost, but that redesign is
out of this phase's scope.

## 5. Whole-top synthesis (UNAVAILABLE, same precedent as baseline/B1/B2)

| Depth | Result |
|---|---|
| 1 | UNAVAILABLE -- `synth_ecp5` timed out at the 300s soft bound |
| 2 | UNAVAILABLE -- timed out at 300s |
| 4 | UNAVAILABLE -- timed out at 300s |
| 8 | not attempted (skipped by design -- a 4th 300s timeout would add no new information) |

Consistent with baseline/B1/B2's own whole-top attempts (`results/synthesis.csv`,
`whole_top_level` rows), never completed on this session's 5.6 GiB RAM
development machine. Peak memory was not successfully captured for these
runs (the `/usr/bin/time -v` wrapper's summary line did not survive the
`timeout`-induced kill in this environment) -- reported as UNAVAILABLE, not
fabricated. The whole design's real BEHAVIOR (as opposed to synthesized
cell count) at every depth IS fully exercised by the 1,110,000-transaction
Verilator cosimulation in section 1/2.

## 6. Selected depth and bottom line

**REORDER_DEPTH=4 is the selected depth.** It is the smallest depth tested
that beats B2 on every throughput metric (section 2), its depth-bound stall
(6.75%) is already small relative to the structurally-unavoidable divider-
busy stall (81.33%), and depth=8's further gain is marginal (-0.73% overall
cycles/transaction, more area) for real additional area cost. Depths 1 and 2
are excluded outright: they are strictly worse than B2 on throughput while
saving little to nothing on area (section 4).

Phase B3 proves the scheduling-decoupling concept is CORRECT and BOUNDED
(0 fails/drops/duplicates/deadlocks across 7 x 1,110,000 real transactions,
ordering preserved exactly, reset-safe including the new queues-non-empty
scenario) and delivers a real, measured throughput improvement over B2 at
depth>=4 -- but that improvement is modest (4-5%), and the area cost of the
buffer needed to achieve it measurably erodes (does not preserve intact)
B2's own area advantage. See `phase-b3.md` section 9 for why this is decided
CONTINUE, not PROMOTE_CANDIDATE.
