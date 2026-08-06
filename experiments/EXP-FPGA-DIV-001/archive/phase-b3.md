# EXP-FPGA-DIV-001 Phase B3 -- decoupled scheduling for the iterative Q4 divider

Branch `experiment/fp-divider-pipeline`. Builds on `phase-b2.md` (which
replaced `q4_scale`'s remaining variable-divisor `1/d` division with an
exact, multi-cycle, iterative divider, `fp32_div_iterative_exact.sv`, at a
real, measured cost: Q8_0 encode/decode and Q4_0 decode -- three chains
whose own RTL B2 never touched -- slowed ~1.9-2.6x because
`membrane_quant_stream_top_b2.sv` blocked ALL issuance while any Q4_0
encode transaction was in flight, the simplest correct way B2 found to keep
global in-order retirement). Phase B3's task: reduce that collateral cost
with a correct, bounded scheduling/queueing redesign, without touching the
divider itself, `q4_scale_b2.sv`, or `q8_scale.sv`.

Full root-cause analysis (task item 1): `phase-b3-root-cause.md`.

## 1. Root cause (summary; full detail in `phase-b3-root-cause.md`)

B2's ENTIRE stall mechanism is a single boolean, `q4enc_inflight`, which
gates issuance of every mode (not just Q4_0 encode) and stays high for a
Q4_0 encode transaction's full, now-variable (3-473+ cycle) latency. This
exists solely to guarantee B2's two completion sources -- the shared
fixed-latency `tag_pipe` and Q4_0 encode's own direct retire path -- never
both fire in the same cycle into the single-word output-FIFO port, which
is what the design needs to keep the external interface's GLOBAL (all four
modes together), in-order retirement contract, confirmed by reading
`membrane_quant_stream_top.sv`'s own header AND the existing testbench's
own ordering check (a single cross-mode expected-order queue, not four
separate per-mode ones). The real, avoidable mistake: B2 conflates "must
not RETIRE out of turn" with "must not ISSUE OR COMPUTE at all" -- nothing
requires the latter.

## 2. Design (task item 2)

**Chosen: Option B (small shared completion reorder buffer), scoped down to
this design's actual two completion sources** -- not a general N-way
scheduler, not separate per-mode issue queues (Option A was unnecessary:
the three fixed-latency modes already share one correctly-ordered queue,
`tag_pipe`, among themselves; only Q4_0 encode's variable latency needs
reconciling against that).

- `rtl/experimental/fp_div/membrane_completion_reorder.sv`: a bounded
  (power-of-two depth: 1, 2, 4, or 8 tested), direct-mapped completion
  buffer with two write ports (one per completion source) and one
  in-order drain port. Every issued transaction (any mode) gets a global
  sequence number; completions are written into the buffer at
  `seq mod DEPTH` and drained strictly in sequence order. Full design
  rationale (why direct-mapped indexing is safe given a
  `outstanding < DEPTH` issue gate, why it costs a flat +1 cycle per
  transaction, reset behavior) is in that file's own header.
- `rtl/experimental/fp_div/membrane_quant_stream_top_b3.sv`: B2's top level
  with `q4enc_inflight` REMOVED as an issuance gate for the three
  fixed-latency modes (they now issue purely on `issue_allow`
  [`outstanding < REORDER_DEPTH`] and `slot_ok` [output-FIFO space] --
  identical logic for every mode). Q4_0 encode keeps exactly one gate
  beyond those two: `!q4enc_inflight`, a STRUCTURAL constraint (one
  physical divider instance, single-in-flight by its own design), not a
  scheduling choice. `tag_pipe`, the three unchanged chains, and
  `q4_scale_b2`/`fp32_div_iterative_exact` are byte-identical to B2.

No `membrane_issue_queue.sv` was written -- the three fixed-latency modes
never needed a separate issue queue (they already share `tag_pipe`
correctly); introducing one would have been unused complexity for a
problem this design doesn't have.

## 3. Ordering contract (task item 3)

Confirmed (not assumed) by reading both the production header comment and
the existing testbench's check: **global, not per-mode** in-order
completion (`phase-b3-root-cause.md` section 5). B3 does not change this
contract -- transactions may now be issued and COMPUTED concurrently
across modes, but the reorder buffer still enforces strict global-order
DRAINING to the output FIFO. Transaction tag: an 8-bit sequence number
(`SEQ_WIDTH=8`), assigned at issue, wrapping mod 256; only its low
`log2(DEPTH)` bits are ever used for buffer addressing, which the
`outstanding < DEPTH` invariant guarantees is alias-free regardless of the
upper bits' wraparound (proven in `membrane_completion_reorder.sv`'s own
header, checked at runtime by that module's `ifndef SYNTHESIS` assertions).
Reset clears the buffer, both sequence pointers, and `outstanding` to 0 --
safe because a reset also flushes the entire datapath (existing
reset-mid-stream test) and the buffer only ever holds already-completed
payloads (nothing mid-computation is discarded unsafely). Duplicate/drop
detection: the existing testbench's per-transaction, cross-mode
expected-order queue (unchanged) already catches drops, duplicates, and
misordering without needing the RTL to expose the sequence number at the
external interface -- it never was part of the production contract, and
B3 does not add it there either.

## 4. Experimental top-level (task item 4)

`rtl/experimental/fp_div/membrane_quant_stream_top_b3.sv`,
`rtl/experimental/fp_div/membrane_completion_reorder.sv` -- new files only.
No production RTL file (`rtl/q4_scale.sv`, `rtl/membrane_quant_stream_top.sv`,
`rtl/membrane_fp_divider.sv`, `rtl/q8_scale.sv`) or B2 experimental file
(`q4_scale_b2.sv`, `fp32_div_iterative_exact.sv`) was modified. The B3 top
adds 6 debug-only OUTPUT ports (`dbg_outstanding`, `dbg_q4enc_inflight`,
`dbg_stall_depth_o`, `dbg_stall_q4busy_o`, `dbg_stall_outfifo_o`,
`dbg_simultaneous_completion_o`) beyond the shared `in_*`/`out_*` production
contract, purely so the Verilator testbench could observe scheduler
internals as plain ports (robust across Verilator versions) rather than via
`` `verilator public ``-attributed internal signals (which this session
found do NOT reliably surface as accessible C++ class members on the local
Verilator 5.020 build without extra flags that changed access patterns
further -- plain output ports sidestepped that entirely). These add no
behavioral effect on the datapath; a real integration would simply leave
them unconnected.

## 5. Queue-size study (task item 5)

Depths 1, 2, 4, 8 all built, synthesized, and run against the identical
1,110,000-transaction workload. Full table: `results/b3-performance.csv` /
`results/b3-full-datapath.json`. Headline:

| Depth | Overall cycles/txn | vs. B2 | Correctness |
|---|---|---|---|
| 1 | 15.704 | +37.8% (WORSE) | 0 fails |
| 2 | 12.051 | +5.8% (WORSE) | 0 fails |
| **4** | **10.936** | **-4.03% (better)** | 0 fails |
| 8 | 10.856 | -4.73% (better) | 0 fails |

Depths 1 and 2 are real, measured REGRESSIONS vs. B2 (worse on every
per-mode metric, `results/b3-comparison.md` section 2) -- the buffer's flat
+1-cycle tax on every transaction is not offset by a concurrency window
that small, given how frequently this workload issues Q4_0 encode
transactions. Depth 4 is the smallest depth that is a genuine improvement;
depth 8 was evaluated (per the task's own "only if the first three are
genuinely capacity-bound" rule) because depth=4's own depth-bound stall was
a real 6.75% of all cycles -- depth 8 reduces that to 1.08% but only
improves overall throughput a further 0.73%, confirming diminishing
returns once the bottleneck shifts to the structurally-unavoidable
single-divider-busy cost (81.33%/81.96% of all cycles at depth 4/8).
**Depth 8 was not evaluated for deadlock/correctness beyond the same clean
520,000+-transaction, 0-fail result already shown above -- no depth showed
any ordering, drop, duplicate, or deadlock issue.**

## 6. Full datapath verification (task item 6)

`rtl/experimental/fp_div/tb_top_verilator_variant.cpp`, extended this phase
with: a `MEMBRANE_B3_VARIANT` compile path; per-cycle scheduler
instrumentation (queue high-water mark, 3-way stall-reason breakdown,
divider-busy-cycle count, simultaneous-completion-cycle count); a
200,000-cycle no-progress deadlock/timeout watchdog (active for every
variant, never triggered); dense long-Q4-encode-burst, dense-alternating-
Q4/Q8, and dense-random-mode adversarial workload generators (all reused,
unmodified, for baseline/B1/B2 too -- confirming Q8 chains are unaffected
under adversarial load, not just ordinary random load); and a new
reset-while-multiple-queues-non-empty stage (B3-specific -- meaningless for
baseline/B1/B2, which never have more than one transaction outstanding
across modes).

**Scope**: 1,110,000 transactions per variant (200,000 x 4 single-mode +
100,000 mixed + 3 x 70,000 dense adversarial), run against baseline, B1,
B2, and B3 at depths 1/2/4/8 -- 7,770,000 transaction-checks total, plus
2,456,685 component-level differential cases (B2's own, reused unmodified
since the divider itself did not change).

**Result (MEASURED, `results/b3-full-datapath.json`)**: 0 fails, 0 dropped,
0 duplicated, 0 reorder errors, 0 deadlocks/timeouts, at every depth,
across every stage including the 4 reset-safety stages. Queue high-water
mark exactly equals configured depth at every setting (real, exercised
bound, not oversized). Simultaneous completions (both reorder ports firing
the same cycle -- the scenario B2's own invariant forbade) were directly
observed: 629 cycles at depth 4, 296 at depth 8 (0 at depth 1/2, where the
buffer is too small for this to occur). Full stall breakdown and per-mode
latency/throughput: `results/b3-comparison.md` sections 2-3.

## 7. Performance comparison (task item 7)

Full tables: `results/b3-comparison.md`. Baseline/B1 unaffected (2.812
cycles/txn, unchanged from Phase A/B1's own byte-identical chains). B2 vs.
B3 depth=4: overall cycles/transaction -4.03% (11.395 -> 10.936); Q8_ENC
collateral slowdown 2.580x -> 2.376x; Q8_DEC 2.075x -> 2.045x; Q4_DEC
2.070x -> 2.039x. Depth=8 pushes these to -4.73% overall, 2.341x/1.987x/
1.971x (Q8_DEC and Q4_DEC drop below 2x collateral slowdown for the first
time). Q4_0 encode's OWN latency was never a target and moved negligibly
(442.465 -> ~435 cycles, a side effect of the reorder buffer's own +1-cycle
tax being nearly offset by slightly less contention, not a divider change).

## 8. Synthesis matrix (task item 8)

Full table: `results/b3-synthesis.csv`. `membrane_completion_reorder`
standalone, all 4 depths, real Yosys 0.33 generic + ECP5:

| Depth | Generic cells | ECP5 cells | ECP5 FF |
|---|---|---|---|
| 1 | 1,095 | 1,091 | 541 |
| 2 | 5,894 | **17,611** | 1,076 |
| 4 | 13,901 | 14,959 | 2,141 |
| 8 | 28,850 | 19,800 | 4,272 |

Depth=2's ECP5 cell count is a measured, real, non-monotonic anomaly
(higher than depth=4's) -- almost certainly an ABC wide-multiplexer
mapping-heuristic artifact (see `results/b3-comparison.md` section 4 for
the L6MUX21 evidence), not re-run or smoothed over. `q4_scale`-level
synthesis is UNCHANGED from Phase B2 (`q4_scale_b2.sv` not modified this
phase) -- reproduced by reference in `results/b3-synthesis.csv`, not
re-synthesized. Whole-top `synth_ecp5` was attempted for depths 1/2/4 and
timed out at a 300-second soft bound every time (same precedent as
baseline/B1/B2's own whole-top attempts never completing on this session's
5.6 GiB RAM machine) -- marked UNAVAILABLE, not a synthesizability failure;
depth=8 whole-top was not attempted (would add no new information). An
ESTIMATED combined system-level number (`q4_scale_b2` + reorder buffer,
summed since whole-top sharing data is UNAVAILABLE) is in
`results/b3-comparison.md` section 4: roughly -70% to -96% vs. baseline
depending on depth, a real but measurable erosion of B2's own -96.9%
`q4_scale`-level reduction.

## 9. Decision

**CONTINUE.**

Meets every correctness/reproducibility bar cleanly: 0 fails, drops,
duplicates, or deadlocks across 7,770,000+ real transaction-checks
(including new adversarial and reset-safety coverage this phase adds), the
global in-order retirement contract verified unchanged, the single-in-flight
divider-resource invariant held and asserted, bounded queue design (no
unbounded growth possible by construction), fully reproducible via
`scripts/run-exp-fp-divider-001.sh --phase b3`.

**Not promoted**, for two real, measured, disclosed reasons -- exactly the
"queue depth/scheduling still needs tuning" and "area overhead too high" /
"throughput benefit limited" CONTINUE criteria, not a REJECT-class defect:

1. **The throughput win is real but modest, not dramatic**, and only
   materializes at REORDER_DEPTH>=4 -- depths 1 and 2 are measured
   REGRESSIONS vs. B2 (section 5/6). A queue depth has to be chosen
   correctly for this design to help at all, which is itself a tuning
   burden the task's own success criteria flag.
2. **The reorder buffer's own area cost measurably erodes B2's area
   advantage.** `membrane_completion_reorder` at the selected depth (4)
   costs 14,959 ECP5 cells standalone -- MORE than the entire
   `q4_scale_b2` unit B2 already shrank to 2,268 cells. The combined,
   ESTIMATED system-level area reduction vs. baseline drops from B2's own
   measured -96.9% to roughly -76.8% at depth 4. Still a large win over
   baseline, but not "B2's advantage, preserved" in the tight sense the
   task's own PROMOTE_CANDIDATE bar asks for. A leaner reorder design
   (storing a tag/pointer rather than the full 531-bit payload per slot,
   reading wide data from the existing per-chain output registers only at
   drain time) would plausibly recover most of this cost, but that
   redesign is future work, not attempted this phase.

Neither issue is a correctness defect, an ordering bug, a deadlock, or
unbounded behavior -- none of the REJECT criteria apply. This is a genuine,
partial, honestly-quantified engineering win: the scheduling-decoupling
CONCEPT is proven correct and bounded; the specific
`membrane_completion_reorder` IMPLEMENTATION is not yet area-efficient
enough, at the depth needed for a real throughput win, to call this ready
for promotion.

**Selected depth if this design is carried forward: REORDER_DEPTH=4** --
the smallest depth that beats B2 on every measured metric, per section 5.

## 10. Reproduction

`scripts/run-exp-fp-divider-001.sh --phase b3 --quick` (fast smoke: small
mixed-mode workload, depth 1/2/4 builds, elaboration-only synthesis check --
depth 8 skipped for speed) or `--phase b3 --full` (the exact numbers in this
document: 1,110,000-transaction workload x 7 variants including depth 8,
complete generic+ECP5 synthesis matrix for `membrane_completion_reorder` at
all 4 depths, whole-top attempts for depths 1/2/4). `--resume` skips
rebuilding already-built binaries and regenerating golden vectors already
sized correctly for the requested `N_PER_MODE`; `--output-dir` redirects
build/output artifacts. The divider differential test is reused from Phase
B2 unmodified (the divider itself is not touched this phase).
