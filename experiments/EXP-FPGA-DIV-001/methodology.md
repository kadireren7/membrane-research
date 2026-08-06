# EXP-FPGA-DIV-001 — methodology

## Test design

Every phase (A, B1-B4) followed the same two-part evidence requirement
before a candidate could be considered:

1. **Differential exactness**: the candidate divider/datapath variant's
   output compared bit-for-bit against the real, unmodified production
   RTL (`membrane_fp_divider`, `q4_scale`, or the full
   `membrane_quant_stream_top`) over millions of random and edge-case
   inputs (denormals, zero, infinities/NaN via the F16 special-value
   convention, extrema). Any single mismatch rejects the candidate
   outright — there is no "close enough" bar for a datapath claiming
   bit-exact quantization parity.
2. **Real synthesis**: every candidate was run through the same Yosys
   0.33 generic and `synth_ecp5` flows as the baseline, at the same
   integration point (standalone divider, `q4_scale`, or full top-level
   where synthesizable within the toolchain's own time/memory bounds),
   so cell-count deltas are apples-to-apples.

## Exactness rules

- "Exact" means bit-identical output for every tested input, not
  statistically close. Phase B1/B4's own promoted candidates hit 0
  mismatches across 2.2M–4.46M differential cases each.
- A candidate that is exact but only conditionally so (e.g. exact for
  one call site's fixed divisor but not the general case) is disclosed
  as such, not generalized past what was tested.

## Toolchain

- **Yosys 0.33** (`tools/.local-yosys` in the maintained `membrane`
  repository) — generic synthesis (`synth`) and ECP5-targeted synthesis
  (`synth_ecp5`), both from source, pinned version.
- **Verilator** (`tools/.local-verilator`) — RTL/C++ cosimulation
  against a golden reference model, used for every full-datapath
  transaction-count check in this record.
- No vendor place-and-route tool (Vivado/Quartus/Diamond) and no
  physical FPGA board were used anywhere in this experiment — see each
  phase document's own "Real hardware limitation" section and the
  index README's own closing section.

## Synthesis methodology

- Standalone-module synthesis (divider alone) and integration-point
  synthesis (`q4_scale`, which instantiates the divider) are reported
  separately — a standalone number does not imply the same delta at the
  integration point, since Yosys/ABC can share logic across nearby
  instances in ways that change once a module is embedded in a larger
  design.
- Full top-level (`membrane_quant_stream_top`) synthesis was attempted
  where the toolchain's own bounded timeout allowed; when it did not
  complete, that is disclosed as `UNAVAILABLE`, not silently omitted or
  estimated without saying so.

## Measurement classification

Every number in this experiment's own documents is one of:

- **MEASURED_BY_TOOL** — a real Yosys `stat` cell count or a real
  Verilator cosimulation transaction count/cycle count, reproducible by
  re-running the same command.
- **SIMULATED** — a real tool run, but one that models timing/behavior
  the tool itself does not claim to be a physical measurement (e.g.
  Verilator cycle counts are a real simulated clock-cycle count, not a
  physical Fmax).
- **ESTIMATED** — a number derived analytically from another measured
  number (e.g. a percentage change computed from two measured cell
  counts), not itself independently re-measured.
- **UNAVAILABLE** — explicitly not obtained (most commonly: full
  top-level synthesis that timed out under this project's own bounded
  synthesis budget).

No number in this experiment claims real FPGA LUT utilization, Fmax,
timing closure, or power — Yosys generic and `synth_ecp5` cell counts
are synthesis-tool proxy results only.
