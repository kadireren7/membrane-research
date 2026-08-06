# EXP-FPGA-DIV-001 Phase B3 -- scheduler root-cause analysis

Branch `experiment/fp-divider-pipeline`. This document is task item 1 of
Phase B3: a root-cause analysis of Phase B2's collateral slowdown, derived
directly from reading `rtl/experimental/fp_div/membrane_quant_stream_top_b2.sv`
and `rtl/experimental/fp_div/q4_scale_b2.sv` this session -- every claim below
is either a structural fact (checkable by inspection/`grep` of those two
files, cited by line-area) or a MEASURED number already on record in
`phase-b2.md`/`results/b2-full-datapath.json`. Nothing here is estimated.
Predictions about what Phase B3 will measure are explicitly flagged
"PREDICTION" and are re-checked against real Verilator/Yosys runs in
`phase-b3.md`, not asserted as fact here.

## 1. Current state machine (Phase B2)

`membrane_quant_stream_top_b2.sv` has exactly two retirement mechanisms
running side by side, not one:

1. **`tag_pipe`** (line ~158-176): a fixed-depth (`L_MAX=7`) shift register
   carrying `{valid, mode, id}`. Used by Q8_0 encode, Q8_0 decode, and Q4_0
   decode -- the three chains whose RTL is byte-identical to the production
   file and whose latency is a compile-time constant. A transaction shifts
   in at `tag_pipe[0]` on issue and falls out of `tag_pipe[L_MAX-1]` exactly
   `L_MAX` cycles later, asserting `retire_fire`. Because issuance into this
   pipe is one-at-a-time and every entry takes the identical fixed latency,
   entries retire in the same relative order they entered -- in-order by
   construction, no reorder logic needed among these three modes alone.
2. **Q4_0 encode's own direct path**: `q4_scale_b2` (containing the
   variable-latency `fp32_div_iterative_exact`) followed by `q4_pack`.
   Its completion signal is `q4_pack_valid`, wired straight to
   `q4enc_direct_retire` (line ~404-405) -- it never enters `tag_pipe` at
   all.

Both mechanisms write into the **same single-word-per-cycle output FIFO
port** (`out_fifo_in_valid`/`out_fifo_in_word`, line ~470-473). A single
write port cannot accept two simultaneous retirements, so B2 needs some
invariant that guarantees `retire_fire` and `q4enc_direct_retire` are never
both true on the same cycle. That invariant is asserted directly in the RTL
(line ~494-496) and is the load-bearing correctness property of the whole
design.

## 2. Issue path (where the global stall signal is set)

`issue_fire` (line ~138-150) is the single point where a transaction moves
from the input FIFO into the compute datapath. Its logic, exactly as
written:

```
slot_ok = (out_fifo_occ + in_flight + (q4enc_inflight ? 1 : 0)) < OUT_FIFO_DEPTH;
if (mode_pop == MODE_Q4_ENC)
    issue_fire = in_fifo_out_valid && slot_ok && !q4enc_inflight && (in_flight == 0);
else
    issue_fire = in_fifo_out_valid && slot_ok && !q4enc_inflight;
```

**The global stall signal is `q4enc_inflight`.** It gates the `else` branch
too -- i.e. it blocks issuance of Q8_0 encode, Q8_0 decode, AND Q4_0 decode,
not just other Q4_0 encodes. `q4enc_inflight` (line ~189-196) is set the
cycle a Q4_0 encode transaction issues and is held high for the transaction's
**entire** latency (measured 3-473 cycles, `phase-b2.md` section 3/5),
clearing only when `q4_pack_valid` finally pulses.

So: while one Q4_0 encode transaction is anywhere between issue and
retirement, `issue_fire` is forced false for every mode, every cycle,
regardless of `tag_pipe`'s own occupancy or the output FIFO's free space.
Nothing else in the datapath is the bottleneck -- `q4_scale_b2`, `q4_pack`,
`tag_pipe`, and both FIFOs all have spare capacity during this window; the
single boolean `q4enc_inflight` is the entire blocking mechanism.

A secondary, narrower gate exists for Q4_0 encode specifically:
`(in_flight == 0)` -- a Q4_0 encode transaction is only issued once
`tag_pipe` has fully drained (no fixed-latency-mode transaction currently
outstanding). This is NOT the collateral-slowdown source (it only delays
*starting* a Q4_0 encode, and only by up to `L_MAX=7` cycles); the
collateral cost comes from the reverse direction -- everything else being
blocked *while* Q4_0 encode runs, which can be up to 473 cycles.

## 3. Completion path

- `tag_pipe[L_MAX-1]` -> `retire_fire`, `mode_sel`, `id_sel` -> muxed into
  `result_data`/`result_error` from whichever of the three fixed-latency
  chains' own output register is live that cycle -> `out_fifo_in_*`.
- `q4_pack_valid` -> `q4enc_direct_retire` -> a separate mux arm writing
  `{MODE_Q4_ENC, q4enc_id_hold, {368'h0, q4_packed}, q4enc_final_err}`
  straight to the same `out_fifo_in_*` port.
- `out_fifo_in_valid = retire_fire || q4enc_direct_retire` (line 470): a
  plain OR, not an arbiter -- this is only safe because issue-side gating
  (section 2) already guarantees the two operands are never both true.

## 4. Credit / accounting

Two independent counters exist, not one:

- `in_flight` (line ~178-185): incremented when `tagpipe_issue_fire` fires
  (issuance into `tag_pipe` specifically -- Q4_0 encode issuance does NOT
  increment this), decremented on `retire_fire`. Bounded `0..L_MAX`
  structurally (asserted at line ~488-490).
- `q4enc_inflight` (section 2): a single bit, not a counter -- at most one
  Q4_0 encode transaction may be outstanding at any time. This is a
  structural resource constraint as much as a scheduling one: there is
  exactly one instance of `fp32_div_iterative_exact`, and that module's own
  `in_ready` is asserted only from its `IDLE` state (single-in-flight by
  its own design, `phase-b2.md` section 2) -- so `q4enc_inflight` cannot be
  relaxed to "more than one at a time" without instantiating a second
  divider, which is out of scope for this phase (task explicitly asks to
  keep the divider itself untouched).
- `slot_ok` reserves one output-FIFO slot per outstanding transaction
  across BOTH mechanisms combined (`out_fifo_occ + in_flight +
  q4enc_inflight`), preventing output FIFO overflow -- this part of the
  design is orthogonal to the serialization choice and does not need to
  change for B3.

## 5. Output ordering: global or mode-local?

**Global**, not mode-local -- confirmed from `rtl/membrane_quant_stream_top.sv`'s
own header (section "ordering guarantee", lines 31-40 of that file, unchanged
by B1/B2/B3):

> "Because issue order is preserved by the input FIFO and every transaction
> takes the identical fixed latency regardless of mode, results necessarily
> retire in the same order they were issued -- output ordering is preserved
> by construction, not by an explicit reorder buffer."

This is a whole-datapath invariant across all four modes combined, not
per-mode. It is also exactly what the existing testbench
(`rtl/experimental/fp_div/tb_top_verilator_variant.cpp`, `run_mode()`,
lines ~322-416) actually checks: a single FIFO of `{mode, id}` expected
tuples is pushed on every accepted issue (any mode) and popped/compared on
every retirement (any mode) -- i.e. the test already encodes a strict
**global**, cross-mode FIFO ordering contract, not a per-mode one. Any
scheduling change for B3 must keep satisfying this exact check unmodified
(task item 1 asks explicitly what ordering contract the existing testbench
expects -- this is it, confirmed by reading the check itself, not inferred).

## 6. Why B2 chose full serialization (the actual constraint)

Given section 5's global-order requirement and Q4_0 encode's now-variable
(usually much longer than `L_MAX`) latency, B2 faced a real problem: if
Q8_0/Q4_0-decode transactions issued *after* a Q4_0 encode were allowed to
keep flowing through `tag_pipe` and retiring at their normal fixed
`L_MAX=7` latency, they would very often complete and want to retire
*before* the earlier-issued, still-computing Q4_0 encode transaction --
violating global order. B2's documented choice (`phase-b2.md` section 5) was
the simplest correct fix: prevent that situation from ever arising by
blocking all issuance while a Q4_0 encode is outstanding, so nothing can
ever be "behind" it in the global order while it's still computing. This is
correct (0 ordering failures, 520,000/520,000) but pays for correctness with
throughput, because it stalls three chains that share no compute resource
at all with the divider.

## 7. Same invariant, verified false requirement

Section 6's constraint is about *retirement* order, not *issue or compute*
order. Nothing requires that Q8_0/Q4_0-decode transactions issued after a
Q4_0 encode be prevented from being **issued and computed** concurrently
with it -- only that their **results not be written to the output FIFO**
ahead of the older, still-in-flight Q4_0 encode's result. B2 conflates "must
not retire out of turn" with "must not issue or compute at all", which is
the actual, avoidable source of the collateral slowdown. Decoupling issuance
from `q4enc_inflight` and adding a small buffer to hold early completions
until their turn (instead of blocking issuance so no early completion can
ever occur) is therefore a targeted fix, not a redesign of the whole
retirement scheme -- `tag_pipe` itself, the three unchanged chains, and the
single-in-flight Q4_0 divider all stay exactly as they are.

## 8. Measured collateral slowdown (already on record, re-cited not re-measured here)

From `phase-b2.md` section 5 / `results/b2-full-datapath.json` (520,000
transactions, MEASURED, B2 variant):

| Mode | Baseline/B1 mean latency (cycles) | B2 mean latency (cycles) | Ratio |
|---|---|---|---|
| Q8_ENC | 12.009 | 22.842 | 1.90x |
| Q8_DEC | 11.973 | 22.814 | 1.91x |
| Q4_ENC | 11.998 | 473.225 | 39.4x (expected, not the problem) |
| Q4_DEC | 11.955 | 22.803 | 1.91x |
| Overall cycles/transaction | 3.006 | 9.589 | 3.19x |

The three fixed-latency chains' ~1.9x mean-latency rise, despite
byte-identical RTL to production, is caused entirely by section 2's
`q4enc_inflight` issue gate -- not by any change to their own logic, not by
output-FIFO contention, and not by `in_flight`/credit-accounting overhead
(all confirmed unchanged/untouched by inspection above).

## 9. Correctness invariants that must survive Phase B3

1. Global in-order retirement across all four modes (section 5) --
   verified by the existing, unmodified testbench check.
2. No output FIFO overflow -- some form of section 4's slot reservation
   must still bound total outstanding transactions against
   `OUT_FIFO_DEPTH`.
3. At most one Q4_0 encode transaction physically in flight at a time --
   a structural fact about `fp32_div_iterative_exact`'s single-in-flight
   design (section 4), unrelated to the ordering fix and not something B3
   is asked to change (task explicitly keeps the divider itself untouched).
4. No transaction is ever dropped or duplicated (existing testbench check,
   unmodified).
5. Reset (mid-stream, and specifically while a Q4_0 encode divider is
   busy) must still flush cleanly with no stale `out_valid` and a clean
   `in_ready` recovery (existing testbench stages, unmodified).
6. Bounded resources only: whatever new buffering Phase B3 adds must have a
   fixed, small, parameterized depth -- not an unbounded queue.

## 10. Direction this points to (task item 2, decided in `phase-b3.md`, not here)

Sections 6-9 together rule out "just remove the `q4enc_inflight` gate" (would
break invariant 1, section 9) and rule out "route Q4_0 encode through
`tag_pipe` too" (its latency is not a compile-time constant, `tag_pipe`'s
whole mechanism assumes one). They point at: decouple issuance of the three
fixed-latency modes from `q4enc_inflight` (removing the actual stall
source, section 2), keep `tag_pipe` and the single-in-flight Q4_0 divider
completely unchanged (invariant 3), and add a small, bounded completion
buffer that lets a fast completion which arrives "out of turn" (behind an
older, still-computing Q4_0 encode) wait for its turn instead of either
stalling issuance (B2's cost) or retiring early (breaks invariant 1). This
is Option B from the task's preference order ("small shared request
FIFO/tagged completion reorder buffer"), scoped down to the actual two
completion sources this design has (not a general N-way scheduler) -- full
design in `phase-b3.md` section 2.
