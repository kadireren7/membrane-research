# EXP-FPGA-DIV-002 — methodology

## Test design

Every phase used the same core discipline: a real Verilator cosimulation
tool, compiled once per candidate variant via a compile-time `-D` flag
(never a runtime config switch, so each build is a genuinely distinct
binary), all candidates driven from the **same deterministic seed**
(`mt19937(0xC0FFEE)`, two independent streams — one for cycle-level
backpressure/timing decisions, one for transaction-mode selection,
deliberately separated so different DUT timing never causes the same
seed to produce a different transaction sequence across candidates).
Every candidate's output is checked against a golden C reference model
with strict FIFO-order id/mode/payload checks — a single mismatch, a
single out-of-order retirement, or a single drop/duplicate fails the
whole run.

Phase-specific additions:

- **Phase B3** added a software discrete-event reference model
  (`scripts/b3-hol-model.py`) of the real scheduler's own rules, to
  classify blocked cycles into a stall-cause taxonomy the RTL testbench
  itself does not instrument (no debug ports were added to the
  committed RTL — out of scope by explicit design).
- **Phase B4** added an equivalent retirement-state taxonomy model
  (`scripts/b4-retirement-model.py`), extended to also test hypothesis D
  (downstream backpressure) via a real `out_fifo`-occupancy simulation,
  and a full provenance-safety layer (run manifests, an independent
  result validator, atomic canonical-result promotion) that is itself
  one of that phase's own real deliverables — see
  `results/canonical/b4-run-provenance.md`.

## Exactness rules

Identical to EXP-FPGA-DIV-001: bit-identical output for every tested
input is the only bar for "exact." Every phase's own correctness run
checked millions of transactions (2,050,239 in Phase A up to 8,382,500
per candidate in Phase B4) with 0 tolerance for mismatch.

## Toolchain

Yosys 0.33 and Verilator, same pinned versions as EXP-FPGA-DIV-001 (see
`provenance/source-map.md` at this repo's root). No vendor P&R tool, no
physical FPGA board, in any phase.

## Synthesis methodology

- Phase B1's own component-level (`q8_scale_dual_radix4`) synthesis is a
  real, clean Yosys run — this is the one number in the whole experiment
  with no caveats beyond "synthesis-tool proxy, not physical."
- Full top-level synthesis timed out at every phase from B2 onward (the
  scheduler's own control logic, combined with the F16-conversion
  modules it surrounds, is large enough to make Yosys's own ABC
  resource-sharing analysis impractically slow within this project's own
  bounded time/memory budget) — disclosed as `UNAVAILABLE`, not silently
  omitted.
- Phase B4 introduced **isolated-wrapper synthesis** specifically to get
  *some* real number for the scheduler despite that limit: each
  candidate's own real, unmodified top-level file is synthesized with
  only its two divider-engine instantiations swapped for trivial
  fixed-latency stand-ins of the same port shape (disclosed, not a
  full-top substitute — see `archive/phase-b4.md`). Even this reduced
  design still required Yosys's own `-noshare` flag to avoid the same
  ABC bottleneck, and still frequently timed out on the ECP5-targeted
  flow specifically (`synth_ecp5` has no equivalent `-noshare` escape).

## Measurement classification

Same four-way classification as EXP-FPGA-DIV-001
(MEASURED_BY_TOOL / SIMULATED / ESTIMATED / UNAVAILABLE) — see that
experiment's own `methodology.md` for the definitions, reused unchanged
here. One addition specific to this experiment: Phase B4's isolated-
wrapper synthesis cell-count **deltas** between candidates are
classified ESTIMATED even though each individual absolute count is
MEASURED_BY_TOOL, because the absolute counts themselves are disclosed
as non-representative of real area (the `-noshare` flag inflates them
by roughly an order of magnitude versus a normal synthesis flow) — only
the delta between two equally-inflated numbers, sharing the identical
non-scheduler logic, is treated as meaningful.
