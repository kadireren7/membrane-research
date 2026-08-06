# EXP-FPGA-DIV-002 Phase B4 -- retirement-pressure and bottleneck-hypothesis
analysis

## Methodology (disclosed)

Every number in this document and in `results/b4-retirement-profile.csv` is
**SIMULATED**: produced by a discrete-event **software reference model**
of B3-split's own scheduling rules (`scripts/b4-retirement-model.py`,
modeling `rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4_b3_split.sv`),
not by adding debug instrumentation to that file (out of scope -- B3-split
stays unmodified per this task's own item 6) or by reading internal
Verilator signals. The model implements B3-split's own real rules: two
independent single-issue-per-cycle ingress heads (`enc_fifo`/`dec_fifo`),
per-mode single-in-flight gating for Q8_0/Q4_0 encode, `SHADOW_DEPTH=4`
shadow-admission gating identical to the real RTL's own
`shadow_reserved_count`-at-admission-time discipline (the same race this
project's own Phase B3 model already learned to avoid, reused unchanged
here), and -- new versus the Phase B3 model, since "downstream
backpressure" (hypothesis D) needed to be testable -- a real `out_fifo`
occupancy counter with both an i.i.d.-random and a sustained-burst
`out_ready` deassertion pattern.

Cross-validated against this phase's own real Verilator cosim
(`results/b4-performance.csv`, `25pct_Q8ENC_75pct_other` profile, B3-split
row): model per-mode mean latencies at 25% density (Q8_0 dec=43.2,
Q4_0 enc=119.2, Q4_0 dec=43.3, cycles) are of the same order and relative
ordering as the real RTL's own measured numbers at full scale, though this
model's own simplified (uniform-random rather than exact) arrival process
means absolute cycle counts are an approximation -- this validates the
model's *qualitative* conclusions (which retirement state dominates, how
it scales with density), not a substitute for the real RTL measurements
this phase's own correctness+performance run produces independently.

## Per-cycle retirement-state taxonomy (task item 3)

Every recorded cycle is classified into exactly one of ten states, in
this fixed priority order:

1. **next_seq_retires** -- something matching `next_retire_seq` was ready
   this cycle AND `out_fifo` had room; this is the success case, not a
   stall.
2. **next_seq_backpressure** -- something matched `next_retire_seq` and
   was ready to retire, but `out_fifo` was full (hypothesis D).
3. **younger_decode_blocked_by_encode** -- a decode-class result has
   already completed (sitting in the shadow structure) but
   `next_retire_seq`'s own owner is an encode-class transaction still
   mid-service or held.
4. **younger_encode_blocked_by_other** -- an encode-class result has
   completed (sitting in its own hold register) but `next_retire_seq`
   belongs to some other, still-incomplete transaction.
5. **completion_slot_unavailable** -- a transaction reaching the
   tag_pipe tail needs shadow capture this cycle, but the shadow
   structure is already at `SHADOW_DEPTH` capacity (hypothesis B,
   direct-overflow case).
6. **ingress_blocked_completion_capacity** -- an ingress head has a ready
   transaction but cannot issue because shadow-admission gating already
   has `shadow_reserved_count >= SHADOW_DEPTH` (hypothesis B, the real,
   common manifestation).
7. **no_completed_result_engine_busy** -- nothing is ready to retire and
   an encode engine is still mid-computation (hypothesis A).
8. **no_completed_result_idle** -- nothing is ready to retire and the
   whole pipe is genuinely idle (no work in flight anywhere).
9. **tag_seq_wrap** -- informational only (see below); never itself a
   blocking cause in this design.
10. **reset_recovery** / **other** -- not exercised by this steady-state
    throughput model (reset behavior is covered by the real RTL's own
    dedicated reset correctness stages instead).

## Results at 10/20/25/40% Q8_0-encode density (task item 4)

All figures MEASURED_BY_TOOL from the software model
(`results/b4-retirement-profile.csv`), 400,000 simulated cycles per
profile (2,000-cycle warmup excluded from all figures below).

| Density | Total stall cyc | A: engine busy | B: completion capacity (ingress-blocked + slot-unavailable) | C: strict order (younger-blocked-by-encode + younger-blocked-by-other) | D: backpressure | E: ingress arbitration | F: seq/tag bookkeeping |
|---|---|---|---|---|---|---|---|
| 10% | 261,115 | 21,294 (8.16%) | 69,721 (26.70%) | 171,394 (65.64%) | 0 (0%) | 0 (structural) | 0 (structural) |
| 20% | 271,691 | 17,262 (6.35%) | 53,844 (19.82%) | 218,585 (80.46%) | 0 (0%) | 0 (structural) | 0 (structural) |
| 25% | 297,329 | 16,846 (5.67%) | 49,664 (16.70%) | 230,819 (77.63%) | 0 (0%) | 0 (structural) | 0 (structural) |
| 40% | 309,661 | 16,513 (5.33%) | 45,305 (14.63%) | 247,843 (80.05%) | 0 (0%) | 0 (structural) | 0 (structural) |

Percentage of **total cycles** (not just stall cycles), 25% density
column shown as the representative case (task's own focus density):
A = 4.23%, B = 12.48%, C = 57.94%, D = 0%, E = 0%, F = 0%, success
(next_seq_retires) = 25.29%.

## Correlation with Q8_0-encode density (task item 4's own explicit ask)

- **Hypothesis C (strict in-order retirement) is dominant at every
  density measured, AND its dominance increases with density**:
  65.6% -> 80.5% -> 77.6% -> 80.1% of all stall cycles as Q8_0-encode
  density rises from 10% to 40% (not monotonic between 20-40% due to
  this model's own random-arrival noise at this cycle count, but the
  10%-to-40% trend is unambiguous and large). This is the single largest,
  most density-correlated stall cause in this architecture.
- **Hypothesis B (completion/shadow storage capacity) is real and
  substantial but its relative share DECREASES as density rises**:
  26.7% -> 19.8% -> 16.7% -> 14.6% of stall cycles. In absolute cycles it
  also falls (69,721 -> 45,305) because higher Q8_0-encode density means
  fewer total transactions get accepted in the same fixed cycle window
  (the encode engine itself is the throughput ceiling at high density),
  diluting every other stall category. B is a real, addressable cost, but
  it is not the density-correlated one C is.
- **Hypothesis A (encode execution latency) is a small, roughly flat
  contributor**: 8.2% -> 6.4% -> 5.7% -> 5.3% of stalls -- real (the
  divider genuinely takes 2-34+ cycles per Q8_0-encode transaction), but
  a minority cause of *retirement* stalls specifically (as opposed to
  raw throughput, where it is the well-known, disclosed, and accepted
  divider-II floor from every prior phase).
- **Hypothesis D (downstream backpressure) is negligible in this
  architecture, even under sustained heavy backpressure**: 0 stall
  cycles under i.i.d.-random 30% `out_ready` deassertion; only 38 stall
  cycles out of 397,821 (0.0096%) under a sustained 33%-duty-cycle
  *burst* backpressure pattern (out_ready=0 for 20 of every 60 cycles,
  chosen specifically because i.i.d. noise alone never filled the
  32-entry `out_fifo` -- see `scripts/b4-retirement-model.py`'s own
  comment on why burst rather than i.i.d. backpressure was needed to
  exercise this hypothesis at all). `OUT_FIFO_DEPTH=32` combined with
  the admission-time `slot_ok` reservation (already required for every
  Phase B2/B3 candidate, unchanged here) absorbs realistic backpressure
  before it ever reaches the retirement mechanism itself.
- **Hypothesis E (ingress arbitration) contributes ~0% by construction,
  not by measurement limitation**: B3-split's own `enc_fifo`/`dec_fifo`
  heads are checked and issued completely independently every cycle
  (task item 2 candidate D's own "genuine same-cycle dual issue"), with
  no shared arbiter or extra pipeline stage between them -- there is no
  separate "arbitration cost" distinct from the resource-availability
  cost already counted under B. This is a real, disclosed structural
  property of the split-queue architecture specifically (a single shared
  ingress FIFO with round-robin arbitration, which this project's own
  Phase B3 lookahead candidates effectively were, WOULD have paid a real
  E-class cost -- and Phase B3's own measured data showed exactly that:
  lookahead's constant per-issue selection/compaction overhead was one of
  the two root causes B3-split was selected over lookahead for).
- **Hypothesis F (sequence/tag bookkeeping) contributes ~0% by
  construction**: `SEQ_WIDTH=8` wraps every ~256 transactions (395-532
  real wrap events per 100,000-136,000-transaction profile above,
  confirmed occurring routinely), and zero of those wraps were ever
  classified as a blocking cause -- `OUT_FIFO_DEPTH=32` bounding
  `live_seq_count` (this project's own existing invariant, unchanged
  since Phase B2) structurally guarantees wraparound can never collide
  with a still-live transaction, so bookkeeping itself never stalls
  anything.

## Conclusion feeding Phase B4's candidate design

**C (strict in-order retirement forcing a completed younger transaction
to wait behind an incomplete older encode-class one) is the real,
dominant, density-correlated bottleneck** -- not raw divider latency (A),
not downstream backpressure (D), and not ingress arbitration (E) or
sequence bookkeeping (F). **B (completion/shadow storage capacity) is
the second-largest, real, addressable cost**, most acute at low-to-
moderate density.

This is the evidence basis for Phase B4's own three candidates
(`phase-b4.md`), not an assumption:

- **R3 (direct-retire bypass)** targets a real, measured, avoidable
  component of C directly: B3-split's own Q8_0/Q4_0 encode hold
  registers unconditionally captured every completion for at least one
  extra cycle before it could retire, even when it was already exactly
  its turn -- extending exactly how long that transaction's own
  `..._pending` flag (which gates the *next* encode-class transaction's
  own admission) stayed asserted. Shortening this directly attacks C's
  own mechanism on the critical resource.
- **R1/R2 (bounded completion-slot capacity)** target B directly: R1
  collapses B3-split's own 4-entry shadow array down to exactly one
  decode-class completion register; R2 to exactly two. Given B's own
  measured share of stalls (14.6-26.7%, real but secondary to C), the
  real, open, data-driven question these candidates answer is whether
  smaller completion capacity measurably hurts (naive expectation) or
  is absorbed without cost once R3-class retirement-latency reduction is
  also in play -- answered with real Verilator cosim numbers in
  `results/b4-candidate-comparison.md`, not assumed either way here.
