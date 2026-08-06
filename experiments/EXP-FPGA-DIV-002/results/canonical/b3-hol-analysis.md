# EXP-FPGA-DIV-002 Phase B3 -- head-of-line (HOL) blocking analysis

## Methodology (disclosed)

Every number in this document and in `results/b3-hol-profile.csv` is
**SIMULATED**: produced by a compact discrete-event **software reference
model** of Phase B2's own scheduling rules
(`rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4_b2.sv`),
not by adding debug instrumentation to that file (out of scope -- B1/B2
stay unmodified per this task's own item 5) or by reading internal
Verilator signals. The model implements the exact same rules as the real
RTL: strict single-head FIFO issuance, per-mode single-in-flight gating
for Q8_0/Q4_0 encode, and `shadow_reserved_count` gated admission for the
decode classes (incremented at ADMISSION time, not at tail-capture time --
an earlier draft of this model used tail-capture occupancy only and, in
software, reproduced the exact class of race the real RTL's own design
deliberately avoids; caught and fixed before any number below was
produced, via a hard assertion that zero shadow-capacity overflow events
ever occur). Per-transaction service-time distributions are drawn from
this project's own real measured numbers (`results/b1-differential.json`'s
own standalone dual-radix4-divider latency range, 2-34 cycles). **Cross-
validated** against the real B2 RTL's own measured aggregate mean
latencies (`results/b2-performance.csv`, `25pct_Q8ENC_75pct_other`
profile): model gives Q8_0 dec mean=129.11, Q4_0 dec mean=129.51, Q4_0 enc
mean=133.86 vs. the real RTL's own depth=1 measurement of 137.52, 137.53,
145.87 respectively -- within ~6-9%, a reasonable match given the model's
simplified (uniform-random rather than exact) service-time distribution
and arrival process. This validates the model's *qualitative* conclusions
(fraction of stall from busy-encode-head, bypass counts, useful lookahead
depth) even though its absolute cycle counts are an approximation, not a
substitute for the real RTL measurements Phase B3's own RTL candidates are
later validated against.

## Stall taxonomy (task item 1)

Every blocked-head cycle is classified into exactly one category. "FIFO
empty" never occurred in these runs (the model drives dense-ish ~70%
arrival pressure, matching this experiment's own established convention);
"blocked by output-order capacity" and "downstream backpressure"/"reset/
recovery" are not modeled here (they are real, but orthogonal to input
HOL blocking specifically, and are already covered by this project's
existing correctness testbenches' own backpressure/reset stages -- adding
them to this SPECIFIC head-of-line model would not change the HOL
conclusion, so they are omitted from this model by design, not
overlooked).

| Q8_0-encode density | Total stall cyc | FIFO empty | Head busy (Q8_0 enc) | Head busy (Q4_0 enc) | Head: shadow full | **Fraction of stall from a busy-encode head** |
|---|---|---|---|---|---|---|
| 10% | 326,089 | 0 | 35,727 | 131,234 | 159,128 | **51.2%** |
| 20% | 341,642 | 0 | 107,459 | 91,497 | 142,686 | **58.2%** |
| 25% | 347,465 | 0 | 143,719 | 74,419 | 129,327 | **62.8%** |
| 40% | 361,013 | 0 | 238,562 | 39,391 | 83,060 | **77.0%** |

**Root cause, quantified**: at 20-25% Q8_0-encode density (the range
Phase B2's own residual collateral slowdown was measured at), **58-63% of
all input-stalled cycles are directly caused by the FIFO head targeting a
currently-busy Q8_0 or Q4_0 encode engine** while resource-independent
younger transactions sit ready behind it -- this is precisely the
avoidable head-of-line blocking Phase B3 targets. The remaining stall
(shadow-queue-full) is the SAME class of cost Phase B2 already
characterized and partially addressed; Phase B3's lookahead/split
candidates target the "busy encode head" category specifically, since
that is where a resource-independent younger transaction genuinely could
have proceeded had the scheduler looked past the head.

## Bypass opportunity (task item 1: "independently executable younger
transactions behind the blocked head", "distance to first executable")

| Density | Mean # bypassable younger entries behind a blocked head | Max observed | Mean distance to first executable younger | Frac. reachable within lookahead=2 | Frac. reachable within lookahead=4 |
|---|---|---|---|---|---|
| 10% | 4.43 | 15 | 3.45 | 57.9% | 74.2% |
| 20% | 5.08 | 15 | 2.76 | 64.4% | 82.4% |
| 25% | 5.26 | 15 | 2.69 | 65.7% | 83.4% |
| 40% | 5.35 | 15 | 2.72 | 64.9% | 83.9% |

**Maximum useful lookahead depth (task item 1)**: a depth-2 lookahead
would reach the first executable younger transaction in **58-66%** of
blocked-head cycles across the measured density range; a depth-4
lookahead reaches **74-84%**. This is real, quantitative evidence
motivating Phase B3's own choice to evaluate exactly depth 2 and depth 4
(no more, per this phase's own explicit scope) -- depth 4 is expected to
meaningfully outperform depth 2 (not just marginally), a prediction Phase
B3's own RTL differential/performance run (`results/b3-performance.csv`)
either confirms or corrects with real measured numbers.

## Blocked-head / bypassable-mode distribution

At 25% density (representative): blocked heads are Q8_0 encode
143,719 times and Q4_0 encode 74,419 times (Q8_0 encode blocks more often
-- it is the majority of encode-class traffic at this density AND has the
longer real service time, both compounding). Bypassable younger entries
behind a blocked head are drawn from all three OTHER modes roughly in
proportion to their own traffic share (no structural bias toward any one
mode being "more bypassable" -- the model confirms this is purely a
function of arrival mix, not an artifact of the scheduling rules
themselves).

## Conclusion feeding Phase B3's candidate design

The dominant, addressable cost is a blocked encode-class head sitting in
front of a majority-bypassable queue (mean 4.4-5.4 independently
executable younger entries at any given stall). A bounded lookahead of 2
already reaches roughly 3 in 5 of these opportunities; a bounded lookahead
of 4 reaches roughly 4 in 5. This is the evidence basis for Phase B3's own
architecture choices (`phase-b3.md`), not an assumption.
