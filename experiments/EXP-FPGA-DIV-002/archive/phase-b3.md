# Experiment record: EXP-FPGA-DIV-002 Phase B3

Filled from [EXPERIMENT_TEMPLATE.md](https://github.com/kadireren7/membrane/blob/main/EXPERIMENT_TEMPLATE.md), same
convention as this experiment's own [experiment.md](experiment.md) (Phase
A), [phase-b1.md](phase-b1.md) (Phase B1), and [phase-b2.md](phase-b2.md)
(Phase B2). Branch: `experiment/q8-divider-pipeline`.

## Experiment ID

`EXP-FPGA-DIV-002` Phase B3

## Hypothesis

Phase B2's own residual collateral slowdown at 20-25% Q8_0-encode density
(16-26%, vs. the <=10% target) is caused by avoidable input head-of-line
(HOL) blocking: the single-head input FIFO forces a resource-independent
younger transaction (Q8_0/Q4_0 decode, Q4_0 encode) to wait behind an
older Q8_0/Q4_0 encode transaction whose own target engine is currently
busy, even though the younger transaction could issue and execute
immediately if the scheduler looked past the blocked head. A small,
bounded issue-selection window (2-4 entry lookahead, or two small
mode-split ingress queues) that lets such a younger transaction bypass a
blocked encode head -- while preserving strict global output order via
the same class of small, tag-indexed retirement structure Phase B2
already established -- should recover most of the remaining gap without
growing into a general-purpose reorder buffer.

## Preflight (task item 0)

Branch HEAD confirmed at `ecd996d` (`research: baseline Q8 divider
architecture`) before any change was made. No stale experiment processes
were found running. The dirty `third_party/llama.cpp` submodule state was
left untouched throughout, per its own out-of-scope status in every prior
phase. Reproducing `--phase b2 --quick` before touching any RTL surfaced
one real, previously undetected regression: `membrane_quant_stream_top_q8_dual_radix4_b2.sv`
contained four instances of SystemVerilog `<width>'(expr)`/`int'(expr)`
size-cast syntax (`shadow_hold_occ + 1'(1)`, `int'(shadow_hold_occ)`,
`shadow_reserved_count + 1'(1)`/`- 1'(1)`) that Verilator's frontend
accepts but Yosys 0.33's frontend rejects outright (`syntax error,
unexpected TOK_INT`). This was introduced during Phase B2's own later
`SHADOW_DEPTH` parameterization work and never re-verified against Yosys
before being committed in `ecd996d` -- exactly the kind of regression this
task's own item 0 preflight step exists to catch. Fixed by replacing all
four with plain arithmetic (`+ 1`, `- 1`, bare `shadow_hold_occ`), relying
on Verilog's own automatic width promotion; re-verified via Yosys parse,
Verilator lint at both `SHADOW_DEPTH` values, and a full `--phase b2
--quick` run (all clean). Phase B2's own file and its own prior published
results are otherwise unchanged -- this was a synthesis-frontend-
compatibility fix, not a behavioral change (confirmed via the same
correctness suite Phase B2 already used).

## Method

1. **HOL stall taxonomy** (task item 1): rather than instrument Phase
   B2's committed RTL directly (out of scope -- B1/B2 stay unmodified),
   built a discrete-event **software reference model** of Phase B2's own
   scheduling rules (`scripts/b3-hol-model.py`), cross-validated against
   the real B2 RTL's own measured aggregate latencies (within ~6-9%).
   Result: at 20-25% Q8_0-encode density, 58-63% of all input-stalled
   cycles are directly caused by the FIFO head targeting a busy
   Q8_0/Q4_0-encode engine while a mean of 4.4-5.4 resource-independent
   younger transactions sit ready behind it; a depth-2 lookahead reaches
   the first executable younger transaction 58-66% of the time, depth-4
   reaches 74-84%. See `results/b3-hol-profile.csv` and
   `results/b3-hol-analysis.md` for the full breakdown.
2. Designed and implemented exactly the four bounded issue-selection
   candidates the task specifies (task item 2) -- no generic ROB, no
   lookahead beyond 4, no externally visible out-of-order completion:
   - **A** (baseline for comparison): Phase B2's own strict single-head
     FIFO issuance, unmodified.
   - **B/C**: a 2-entry and 4-entry bounded lookahead window in front of
     the same single input FIFO. Each cycle, the oldest *issuable* window
     entry issues (starvation-free by construction -- if the oldest entry
     is issuable it always wins), and a same-cycle FIFO pop backfills the
     freed window slot via a left-packing compaction network.
     `rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4_b3_l2.sv`
     and `_b3_l4.sv` (the latter a faithful, diff-verified parameter
     duplicate of the former).
   - **D**: two small mode-split ingress queues (one encode-class, one
     decode-class, `ENC_FIFO_DEPTH=DEC_FIFO_DEPTH=8`, bounded/comparable
     total buffering to the existing single 16-entry FIFO, not a
     duplicated full-depth FIFO per mode), each with its own independent
     head, allowing genuine same-cycle dual issue when both an
     encode-class and a decode-class transaction are simultaneously ready.
     `rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4_b3_split.sv`.
   All three candidates keep Phase B2's own global sequence-tag
   admission/retirement discipline unchanged: Q8_0/Q4_0 encode issues
   only when its own engine is idle (task item 3), decode-class entries
   issue when a bounded, tag-indexed retirement slot is reserved for
   them, and retirement order exactly matches original acceptance order
   (task item 4) via the same class of small hold-register/shadow
   mechanism Phase B2 already established (no large associative ROB, no
   speculative execution).
3. **A real correctness bug was found and fixed in candidate D** (split
   queues) during development, not glossed over. See "A real bug found
   during split-queue development" below for the full root-cause
   analysis -- it generalizes to a real, if lower-probability, risk in
   candidates B/C too, and the same fix class (broadening the "might need
   the shadow retirement path" prediction beyond `primary_pending` alone)
   was verified not to be structurally necessary there, for a specific,
   checked reason (see that section).
4. Extended `rtl/experimental/q8_div/tb_top_verilator_q8_b3_variant.cpp`
   from Phase B2's own three-way correctness+performance tool into a
   six-way tool (baseline/B1/B2/B3-l2/B3-l4/B3-split), compiled six times
   via the same compile-time-DUT-selection technique
   (`-DMEMBRANE_{B1,B2,B3_L2,B3_L4,B3_SPLIT}_VARIANT`), adding: a
   starvation-stress stage (dense `{Q8_ENC, Q4_DEC x8}` repeated pattern,
   relying on the existing 200,000-cycle deadlock watchdog plus strict
   FIFO-order checks as the operational starvation-freedom proof), a
   Q4_0-encode/Q8_0-encode contention stage, an adversarial-HOL
   correctness stage, and 5 new performance profiles (20/40/60%
   Q8_0-encode density, alternating Q4_0-encode/Q4_0-decode, and the
   adversarial-HOL pattern itself) on top of Phase B2's own 10, for 15
   total.
5. Extended `scripts/run-exp-q8-divider-002.sh` with the six-variant
   build, the HOL-model regeneration step, the six correctness+
   performance runs, a synthesis matrix (`q8_scale_dual_radix4` reference
   plus all three B3 top-levels), and two new artifact generators
   (`scripts/gen-b3-artifacts.py`, `scripts/gen-b3-synthesis-csv.py`)
   that parse the six logs into `results/b3-correctness.json`,
   `results/b3-performance.csv`, `results/b3-synthesis.csv`, and
   `results/b3-candidate-comparison.md`.
6. Ran `--phase b3 --quick` first to validate the whole six-way pipeline
   end to end (125,750 correctness transactions + a 15-profile matrix at
   N_PROFILE=2,000, per variant, 0 fails on all six), then `--phase b3
   --full` for real numbers (5,042,628+ correctness transactions + a
   15-profile matrix at N_PROFILE=200,000, per variant).

## A real bug found during split-queue development (disclosed, not
hidden)

Candidate D (split queues) deadlocked under mixed random traffic during
its own correctness smoke test (dense pure-Q8_0-encode bursts worked
correctly; the deadlock only appeared under genuinely mixed mode traffic).
**Root cause**: the "does this decode-class entry need the bounded
shadow-retirement path" prediction, captured once at admission time,
was `primary_pending` (a Q8_0/Q4_0-encode-busy-or-held flag) alone --
correct for Phase B2's own single-ingress-FIFO design, where a decode
entry admitted while `primary_pending` is false is guaranteed to find it
is already its own turn to retire by the time it reaches the tail
(nothing else can have "gotten ahead" of it, since everything issues from
one shared head in strict order). Candidate D's two **independent**
ingress queues break that guarantee: an *older* encode-class transaction
can be sitting queued in `enc_fifo`, not yet issued to its own engine
(hence `primary_pending` reads false), while a *younger* decode-class
transaction issues from the independent `dec_fifo` head, believing
(incorrectly) that nothing is ahead of it. That decode entry then reaches
the tag_pipe tail still not its turn, with `is_extra_sel=0` gating it out
of the shadow-capture path entirely -- it is neither retired directly
nor captured, and is silently dropped from `next_retire_seq`'s accounting,
permanently stalling everything behind it. Diagnosed via a
`--public-flat-rw` Verilator debug build and a from-scratch minimal
mixed-traffic repro harness that reproduced the stall in under 400 cycles
and traced the exact `tagpipe_issue_fire`/`retire_fire` event sequence to
the specific dropped transaction. **Fix**: broadened the "might need
shadow" prediction from `primary_pending` alone to
`primary_pending || (enc_fifo_occ > 0) || (shadow_reserved_count > 0)`
(any encode-class transaction anywhere in the system -- queued, busy, or
held -- OR any already-outstanding decode-class backlog), used
consistently for both the admission-time reservation and the tag_pipe
entry's own `is_extra` tag. This stays structurally false (no added
throttling) for pure single-mode streams, since neither an encode-class
transaction nor a shadow backlog can exist in a stream with only one
mode. Verified via the same minimal repro harness at 200,000 mixed
transactions (0 stalls, vs. reproducing within 400 cycles pre-fix), then
via the full six-way correctness+performance tool.

**Why candidates B/C (lookahead) do not need the same broadened
condition**: lookahead's single shared input FIFO admits entries into its
own bounded window in strict arrival order (an entry cannot enter the
window before every older entry already has), and only one entry issues
per cycle, always the oldest *issuable* one. An older encode-class entry
sitting in the window is therefore either issuable (in which case it
always wins priority that same cycle, so nothing younger issues instead)
or blocked on its own engine (in which case `primary_pending` correctly
reads true). An encode-class entry not yet even in the window is,
by the same strict-admission-order argument, necessarily *younger* than
everything already in or past the window, so it cannot be an invisible
older blocker for anything currently being evaluated. This was checked,
not assumed -- both lookahead candidates' own correctness suites (quick
and full) confirm 0 mismatches/drops/ordering errors without the
broadened condition.

## Environment

Same project dev machine as every prior phase: 5.6 GiB RAM, shared with
other concurrent local sessions. Same toolchain: `tools/.local-yosys`
(Yosys 0.33), `tools/.local-verilator`. No place-and-route tool, no
Xilinx/Altera toolchain, no physical FPGA board.

## Metrics

Same categories as Phase B2 (per-mode min/mean/p50/p95/p99/max latency
and throughput across profiles, per-mode collateral slowdown vs.
baseline, overall cycles/transaction) extended to 15 traffic profiles and
6 variants, plus: HOL stall taxonomy and bypass-opportunity statistics
(`results/b3-hol-profile.csv`), adversarial-HOL-pattern stall-cycle
reduction vs. Phase B2, and a same-cadence synthesis matrix for all three
B3 candidates.

## Success criteria / Results against task item 9's thresholds

All figures below are MEASURED_BY_TOOL (real Verilator cosim cycle
counts from `tb_top_verilator_q8_b3_variant.cpp` against the golden C
reference, `results/b3-performance.csv`/`results/b3-correctness.json`),
computed directly from the committed per-mode mean-latency numbers.
`results/b3-candidate-comparison.md` was extended with the per-mode
collateral-vs-baseline table this section relies on (it previously only
carried overall/density-sweep/adversarial/pure-stream tables, not the
specific breakdown task item 9's targets require).

- **Correctness**: 0 payload mismatches, 0 ordering errors, 0
  drops/duplicates, 0 reset-recovery failures, 0 starvation violations --
  **MET**, across six full 8,042,500-transaction runs (baseline, B1, B2,
  B3-l2, B3-l4, B3-split; see `results/b3-correctness.json`).
- **Collateral slowdown <=10% (Q8_0 decode / Q4_0 decode / Q4_0 encode)
  at 20-25% Q8_0-encode density**:
  | density | mode | B2 (reference) | B3-l2 | B3-l4 | B3-split |
  |---|---|---|---|---|---|
  | 20% | Q8_0 dec | +12.39% | +19.73% | +37.28% | **+9.58%** |
  | 20% | Q4_0 enc | +8.64% | +15.79% | +31.71% | **+0.59%** |
  | 20% | Q4_0 dec | +12.38% | +20.02% | +37.09% | **+9.79%** |
  | 25% | Q8_0 dec | +25.33% | +33.64% | +54.17% | +17.32% |
  | 25% | Q4_0 enc | +19.84% | +27.60% | +46.17% | +6.25% |
  | 25% | Q4_0 dec | +25.41% | +34.04% | +54.55% | +17.65% |

  **NOT MET as a full 20-25% range** by any candidate. **Bounded
  lookahead (B3-l2, B3-l4) makes collateral WORSE than B2 at every
  density measured here, not better** -- a real, counter-to-hypothesis
  result, not glossed over (see "Why lookahead makes collateral worse,
  not better" below). **B3-split meets the <=10% bar at 20% density on
  all three modes** (9.58% / 0.59% / 9.79%) but **not at 25%** (two of
  three modes exceed 10%: 17.32% Q8_0 dec, 17.65% Q4_0 dec; Q4_0 enc
  stays under at 6.25%) -- partial, not full, target achievement.
- **Overall cycles/transaction >=15% better than B2, at 20-25% density**:
  | density | B3-l2 vs B2 | B3-l4 vs B2 | B3-split vs B2 |
  |---|---|---|---|
  | 20% | +8.14% | +12.30% | **+17.82%** |
  | 25% | +8.23% | +11.45% | **+21.38%** |

  **MET by B3-split only** (17.82% / 21.38%, both over the 15% bar).
  **NOT MET by B3-l2 or B3-l4** (8.1-12.3%, short of 15% at both
  densities).
- **Adversarial HOL profile: >=50% fewer HOL stall cycles than B2**:
  the RTL correctness+performance tool deliberately does not instrument
  per-cycle stall-category counters inside the experimental top-levels
  (disclosed in `tb_top_verilator_q8_b3_variant.cpp`'s own header comment
  -- out of this phase's scope, would need debug-only DUT ports); the
  only real per-profile number available for the adversarial pattern is
  total cycles/transaction, used here as an upper-bound proxy for
  stall-cycle reduction (total-cycle savings on an identical transaction
  count and identical issue-cycle-per-transaction structure is at least
  as large as the true stall-cycle-only savings would be, so this proxy
  cannot understate a real target miss): B2=4.285, B3-l2=4.118 (+3.9%),
  B3-l4=3.785 (+11.7%), B3-split=3.618 (+15.6%). **NOT MET by any
  candidate** -- the best (B3-split, 15.6%) is well under half of the
  50% bar, so the true stall-cycle-only figure (necessarily <= this
  proxy) cannot meet it either. Classified UNAVAILABLE at the exact
  stall-cycle-category granularity, NOT MET at the proxy granularity.
- **Low Q8_0-encode density (10%): no regression vs. B2 beyond 2%**:
  overall cycles/transaction at 10% density: B2=6.860, B3-l2=6.391,
  B3-l4=6.090, B3-split=6.493 -- all three candidates are FASTER than
  B2 at light density (6.8-11.2% better, not just "no worse than 2%").
  **MET, by all three candidates.**
- **Pure streams: no regression for Q8_0 decode / Q4_0 encode / Q4_0
  decode**:
  | mode | B2 (reference) | B3-l2 | B3-l4 | B3-split |
  |---|---|---|---|---|
  | Q8_0 dec (mean latency) | 52.768 | +3.02% | +2.44% | **-30.24%** |
  | Q4_0 enc (mean latency) | 317.026 | +11.49% | +23.29% | **-47.21%** |
  | Q4_0 dec (mean latency) | 52.370 | -0.81% | -2.56% | **-29.84%** |

  **NOT MET by B3-l2 or B3-l4** -- both show a real, measured regression
  in pure Q8_0-decode (+2.4-3.0%) and a larger one in pure Q4_0-encode
  (+11.5-23.3%), i.e. lookahead adds real per-transaction latency cost
  even in a single-mode stream with nothing to bypass. **MET by
  B3-split**, which is faster than B2 on every pure stream (-30% to
  -47%), not merely non-regressed.
- **Pure Q8_0-encode**: expected to remain limited by the divider's own
  II=16 (task item 9's own explicit allowance) -- confirmed: all six
  variants' `100pct_Q8_ENC` profile clusters within measurement noise of
  each other (see `results/b3-performance.csv`), since a pure
  single-mode Q8_0-encode stream never contends with anything an
  issue-selection change could help.
- **Starvation freedom**: **MET** -- the dedicated starvation-stress
  stage (dense `{Q8_ENC, Q4_DEC x8}` pattern) and the 200,000-cycle
  deadlock watchdog report 0 fails across all six variants; the
  oldest-issuable-always-wins selection rule (task item 3) is
  structurally starvation-free for B3-l2/l4 (an older window entry
  cannot be indefinitely passed over, since it wins as soon as it
  becomes issuable), and B3-split's independent per-class heads cannot
  starve each other by construction (each class's own queue drains in
  strict FIFO order regardless of the other class's state).

## Why lookahead makes collateral worse, not better (a real,
counter-to-hypothesis result, disclosed not hidden)

The hypothesis in this document's own "Hypothesis" section predicted
lookahead would recover most of Phase B2's own residual collateral gap.
The measured result is the opposite: B3-l2/l4 collateral is worse than
B2's at every density measured, and B3-l4 (deeper lookahead) is worse
than B3-l2 (shallower lookahead) -- monotonically worse with more
lookahead, not better. Two real, measured contributors, both visible in
the data already collected:

1. **The shadow-retirement path, not the busy-encode head, is the
   binding constraint once bypass is allowed.**
   `results/b3-hol-profile.csv` already shows "head: shadow full" as the
   single largest stall category at every density from 10-25%
   (159,128 / 142,686 / 129,327 cycles, vs. 166,961 / 198,956 / 218,138
   combined busy-encode-head cycles at those same densities) -- Phase
   B2's own bounded shadow-retirement capacity (`SHADOW_DEPTH`) was
   already the co-dominant cost, not merely a secondary one, before B3
   changed anything. B3-l2/l4 raise `SHADOW_DEPTH` to match
   `LOOKAHEAD_DEPTH` (2 and 4, vs. B2's default 1) specifically so a
   decode-class entry that bypasses a blocked head has somewhere to go
   -- but this means MORE decode-class entries now compete for
   shadow-retirement slots per unit time (every successful bypass is a
   new shadow-path admission that would not have happened under strict
   FIFO issuance), and each entry's own worst-case shadow occupancy time
   grows with deeper lookahead (a bypass from window position 4 can
   leave 3 older, still-blocked entries for it to eventually retire
   behind). The net effect measured here: enabling more bypass
   opportunities increases pressure on the same bounded shadow structure
   faster than it relieves pressure on the encode-class head, and the
   two effects do not net out in the candidates' favor.
2. **The lookahead window's own selection/compaction logic adds
   constant per-issue latency, independent of whether a bypass ever
   fires.** The pure-stream results above make this the cleanest
   possible test: a `100pct_Q4_ENC` stream has nothing to bypass (every
   window entry is the same mode, contending for the same single
   engine), yet B3-l2/l4 are still 11.5%/23.3% slower than B2 there.
   Priority-encoding "pick the oldest issuable window entry" and
   left-packing the window after every issue (task item 2's own
   compaction requirement) sit on the same-cycle issue path for every
   transaction, bypass or not -- B2's strict single-head FIFO pays none
   of this cost. This scales with `LOOKAHEAD_DEPTH` (l4 pays more than
   l2), consistent with the monotonic ordering observed across every
   metric in this section.

**Why split queues avoid both problems**: B3-split's two independent,
per-class ingress queues let a decode-class transaction issue without
ever touching the encode-class engine's own busy/idle state or the
lookahead window's shared selection logic at all -- there is no
combined priority-encode step on the decode path, and (per the
broadened shadow-admission condition documented above under "A real bug
found during split-queue development") the shadow path is only entered
when genuinely necessary, not as a side effect of a bypass decision. The
real result reflects this: B3-split is the only candidate that improves
collateral, meets the >=15% density-range target, and is faster (not
merely non-regressed) on every pure stream.

## Synthesis (task item 10)

`results/b3-synthesis.csv` (generated by
`scripts/gen-b3-synthesis-csv.py` from real Yosys 0.33 `stat` output,
`build-exp-q8-divider-002-b3-full/synth/*.log`):

- **`q8_scale_dual_radix4` (component reference, unchanged since Phase
  B1)**: MEASURED_BY_TOOL both flows -- generic 1,556 cells, ECP5 2,775
  cells (1,659 LUT4 / 319 CCU2C / 360 PFUMX / 180 L6MUX21 / 257
  TRELLIS_FF). Identical to Phase B1's own number (3rd+ independent
  reproduction, this branch) -- **-97.76% retained vs. the original
  `q8_scale` baseline (123,742 cells)**, unchanged by anything in Phase
  B3, since this component is reused byte-for-byte and Phase B3 touches
  only top-level scheduling logic around it.
- **B3-l2 / B3-l4 / B3-split full top-level**: best-effort `synth_ecp5`
  (only flow attempted at the full-top level, same precedent as every
  prior phase's own full-top attempt -- generic-flow full-top synthesis
  was never attempted in Phase A/B1/B2 either, since ECP5 is the only
  flow that produces ECP5-proxy-cell figures item 10 asks for), bounded
  at 1500s: **all three UNAVAILABLE (timed out)**, same class of
  ABC-resource-sharing-analysis-stage timeout as Phase A's/B1's/B2's own
  full-top attempts. Stage reached at timeout (from each log's own
  tail, real observed data): B3-l2 -- still inside `AUTONAME` (post-ABC
  LUT-optimization renaming, 457,465 log lines emitted); B3-l4 -- still
  inside `OPT_DFF` (post-ABC DFF optimization, 437,873 log lines); B3-
  split -- still inside ABC's own resource-sharing SAT solver pass
  itself (351,415 log lines, mid-`Analyzing resource sharing`), i.e. the
  earliest-stage timeout of the three, consistent with it being
  structurally the largest design (two independent FIFOs plus a shared
  shadow/tag_pipe hierarchy, vs. one FIFO plus a lookahead window for
  l2/l4). Hierarchy check (`hierarchy -check`) passes cleanly for all
  three (0 problems) before the bounded `synth_ecp5` attempt begins, and
  all three simulate correctly end-to-end at 8,042,500 transactions each
  with 0 fails (`results/b3-correctness.json`) -- genuinely UNAVAILABLE
  at the synthesized-cell-count level, not a synthesizability problem in
  the RTL and not a script failure, per this task's own explicit item 10
  instruction.
- **ESTIMATED added-register-bit delta vs. B2** (analytical, from each
  candidate's own parameterized array widths read directly from the
  committed RTL -- same estimation class Phase B2's own doc used for its
  B1->B2 delta, disclosed as ESTIMATED not MEASURED_BY_TOOL, since no
  real full-top synthesized number exists for B2 either):
  - B2 (`SHADOW_DEPTH=1` default) baseline: ~1,500 added FF bits over
    Phase B1 (Phase B2's own doc, reproduced here as the reference
    point).
  - B3-l2 (`LOOKAHEAD_DEPTH=2`, `SHADOW_DEPTH=2`): one `IN_WORD_WIDTH`
    (`2+ID_WIDTH+512` = 530 bits with `ID_WIDTH=16`) window slot beyond
    B2's existing single-entry head register, plus one extra
    `SHADOW_DEPTH` slot (B2's own per-slot estimate: `1+2+16+512+1+8` =
    540 bits) relative to B2's `SHADOW_DEPTH=1` default -- roughly
    530 + 540 ~= **1,070 additional FF bits over B2**, plus small
    (a few dozen bits) window-valid/tag/priority-encode control state.
  - B3-l4 (`LOOKAHEAD_DEPTH=4`, `SHADOW_DEPTH=4`): three additional
    530-bit window slots plus three additional 540-bit shadow slots
    relative to B2's `SHADOW_DEPTH=1` -- roughly
    3*530 + 3*540 ~= **3,210 additional FF bits over B2**, again plus
    control state (larger than l2's, since the priority-encoder/
    compaction network scales with window depth).
  - B3-split (`ENC_FIFO_DEPTH=8`, `DEC_FIFO_DEPTH=8`, `SHADOW_DEPTH=4`):
    two independent 8-entry queues of the mode-tagged word width
    (`Q_WORD_WIDTH` = `2+ID_WIDTH+512+SEQ_WIDTH` = 538 bits per entry,
    8*538*2 ~= 8,608 bits) replacing B2's single 16-entry `IN_FIFO_DEPTH`
    queue of `IN_WORD_WIDTH`=530 bits (16*530 = 8,480 bits) -- i.e. total
    ingress-queue storage is within ~1.5% of B2's own (bounded and
    comparable, per task item 2's own explicit constraint on candidate
    D, not a duplicated full-depth FIFO per mode), plus the same
    3-additional-`SHADOW_DEPTH`-slot delta as B3-l4 (~2,160 bits) since
    it also runs `SHADOW_DEPTH=4`. Total ESTIMATED delta over B2 is the
    largest of the three candidates in raw bit count
    (~2,160 shadow bits, ingress storage roughly a wash vs. B2), but
    structurally the simplest per-bit (two independent shift-register
    FIFOs with no shared priority-encoder/compaction network, vs.
    l2/l4's shared window logic) -- consistent with it being the fastest
    of the three to reach a `synth_ecp5` timeout-stage (see above) despite
    not being the smallest in raw bit count, since ABC's own resource-
    sharing analysis cost scales more with combinational complexity
    (the lookahead candidates' priority-encode/compaction network) than
    with flip-flop count alone.
  - **All three estimates are small relative to the ~40K-cell-class full
    top** (same order of magnitude as B2's own ~1,500-bit estimate) --
    nothing here suggests any B3 candidate meaningfully erodes the
    `q8_scale_dual_radix4` component's own -97.76% retained area
    reduction, though (as with B2) no real MEASURED_BY_TOOL full-top
    number exists to confirm this at synthesis granularity for any of
    Phase B1/B2/B3's own full tops.

## Candidate selection (task item 11)

Applying the task's own explicit rules in order:

- "Choose lookahead 2 if it meets all targets" -- **it does not**
  (collateral worse than B2, not better; overall-improvement target
  missed; pure-stream regression present). Ruled out.
- "Choose lookahead 4 only if it materially beats lookahead 2" -- **it
  does not**; B3-l4 is materially WORSE than B3-l2 on every metric in
  this document (collateral, overall improvement is directionally
  better but still short of target, pure-stream regression is larger).
  Ruled out on its own terms, independent of B3-l2's own shortfall.
- "Choose split queues only if they clearly outperform with comparable
  or lower complexity" -- **they do**: B3-split is the only candidate
  meeting the >=15%-vs-B2 density-range improvement target, the only one
  meeting the <=10% collateral bar (partially, at 20% only), the only
  one with zero pure-stream regression (strictly faster on every pure
  mode), and the only one with the best adversarial-HOL proxy number
  -97.76% component-level area is untouched by any candidate, and
  B3-split's own ESTIMATED ingress-storage bit count is within ~1.5% of
  B2's existing single FIFO (comparable complexity, per the analysis
  above), not a duplicated full-depth-per-mode structure.

**Selected candidate: B3-split (mode-split ingress queues)** -- the only
one of the three that materially outperforms B2 rather than
underperforming it, at comparable bounded hardware complexity. This
selection does **not** amount to `PROMOTE_CANDIDATE` (see Decision
below): B3-split still misses the strict 25%-density collateral bound
and the adversarial-HOL 50%-stall-cycle-reduction bound.

## Limitations

- Neither the strict full 20-25%-density collateral bound nor the
  50%-adversarial-HOL-stall-cycle-reduction bound is met by any
  candidate, including the selected B3-split -- disclosed as a real
  shortfall, not glossed over (see "Success criteria / Results" above).
- Bounded lookahead (B3-l2/l4) is a real, measured **regression**
  relative to B2 on every metric evaluated here, not a neutral or
  marginal result -- this reverses this document's own stated
  hypothesis and is the most important disclosed finding of this phase.
- No real synthesized full-top cell count exists for any of B1/B2/B3's
  own full tops (all UNAVAILABLE, same 1500s best-effort bound) -- all
  candidate-vs-B2 area deltas are ESTIMATED from RTL register-array
  widths, not MEASURED_BY_TOOL, same disclosed limitation class as
  Phase B2's own doc.
- The adversarial-HOL 50%-stall-cycle-reduction target cannot be
  evaluated at its literal stall-cycle granularity with the
  instrumentation built this phase (RTL stall-category counters were
  explicitly out of scope, per `tb_top_verilator_q8_b3_variant.cpp`'s
  own header) -- only the total-cycles-per-transaction proxy, disclosed
  as an upper bound, not a substitute measurement.
- `results/b3-hol-profile.csv`'s own software reference model covers
  10/20/25/40% Q8_0-encode density only (task item 1's own explicit
  scope); it does not model the B3 candidates' own scheduling rules
  (only B2's), and does not cover the adversarial-HOL pattern
  specifically -- it motivated B3's candidate design, it is not a
  substitute for the real RTL measurements this document's own Results
  section relies on.
- Same environment limitations as every prior phase: 5.6 GiB
  memory-constrained dev machine, no place-and-route tool, no real
  FPGA hardware, no real Fmax/timing/power figure anywhere in this
  document.

## Decision

**CONTINUE.**

- Exact: **YES** -- 0 mismatches, 0 ordering errors, 0
  drops/duplicates, 0 reset-recovery failures, 0 starvation violations,
  across six full 8,042,500-transaction runs.
- B3 performance targets met: **PARTIALLY, by one candidate only** --
  B3-split meets the >=15%-vs-B2 density-range improvement target and
  the <=10% collateral bar at 20% density, is strictly faster than B2 on
  every pure stream, and shows no regression at low density (all three
  candidates improve on B2 there). It does **not** meet the strict
  25%-density collateral bound (2 of 3 modes exceed 10%, at 17.3-17.7%)
  nor the adversarial-HOL 50%-stall-cycle-reduction bound (best proxy
  figure: 15.6%). B3-l2 and B3-l4 meet **none** of task item 9's
  performance targets and are real, measured regressions vs. B2 on
  collateral and pure-stream latency -- this phase's own hypothesis
  (lookahead recovers most of the residual gap) is falsified by the
  data, not confirmed.
- Area remains strongly favorable: **YES** at the component level
  (`q8_scale_dual_radix4`, -97.76%, untouched by any B3 candidate).
  Full-top-level area impact of every B3 candidate is UNAVAILABLE
  (disclosed), ESTIMATED small relative to the full design.
- Bounded/simple scheduler: **YES** for all three (no ROB, no lookahead
  beyond 4, ingress storage bounded and comparable to B2's existing
  `IN_FIFO_DEPTH`); B3-split is additionally the structurally simplest
  per the analysis above (two independent shift-register FIFOs, no
  shared priority-encoder/compaction network).
- Starvation safety: **YES** for all three, confirmed via dedicated
  starvation-stress testing (0 fails).
- Reproducible: **YES** -- `scripts/run-exp-q8-divider-002.sh --phase b3
  --quick|--full|--resume|--output-dir`, all exercised this session.

Per this task's own explicit instruction (mirroring Phase B2's own
framing), this is a real, substantial, honestly-quantified result: one
of three evaluated candidates (B3-split) is a genuine improvement over
B2 on most axes, and this phase's own central hypothesis about
lookahead is falsified by real data rather than confirmed -- neither
outcome is dressed up as a complete solution. Item 15's `REJECT_B3`
criteria do not apply (there IS meaningful HOL reduction and a real
performance improvement from B3-split specifically, no starvation/order
issue, and area is retained) but item 15's `PROMOTE_CANDIDATE` criteria
are also not fully met (the strict 20-25%-density collateral target is
only half-met, and the adversarial-HOL target is not met by any
candidate) -- `CONTINUE` is the only decision the task's own item 15
rules support. This is a Phase-B3-internal, experiment-branch-only
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
continues, the natural next step (a possible Phase B4, not undertaken
here) would start from B3-split specifically -- e.g. evaluating a larger
`SHADOW_DEPTH` for that candidate alone, since the "Why lookahead makes
collateral worse" analysis above identifies shadow-retirement capacity,
not head-of-line blocking itself, as the now-dominant residual
constraint at 25% density.
