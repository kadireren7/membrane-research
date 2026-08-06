# Hardware claim gates

Which experiment must pass before which sentence can be used, anywhere
in this repository (`README.md`, `paper/`, `outreach/`, `docs/`). This
is the enforcement reference for `scripts/verify-outreach.py`'s
prohibited-claim scan — if a claim below is marked prohibited before its
gate, it should be flagged as a violation wherever it appears unhedged.

As of Phase 7.3, **no gate past "RTL synthesizability (yosys)" has been
passed** — every claim requiring place-and-route, a real board, real
DMA, or real CXL hardware is currently prohibited project-wide.

## Gate 0: RTL cosimulation (PASSED)

- **Required artifact**: `rtl/tb/tb_top_verilator.cpp` run log.
- **Required test**: 520,000-transaction Verilator cosimulation vs. the
  CPU reference, 0 mismatches.
- **Allowed claim**: "cosimulated bit-exact against the CPU reference,"
  "simulation bit-exact," "520,000-transaction Verilator cosimulation."
- **Prohibited claim**: "hardware bit-exact," "FPGA-verified,"
  "board-verified."

## Gate 1: RTL synthesizability, no P&R (PASSED)

- **Required artifact**: yosys 0.33 elaboration log
  (`docs/phase5-synthesizable-fpga.md` §5, and this phase's
  `hardware/vendor-wrapper/` elaboration).
- **Required test**: full hierarchy elaborates under yosys with no
  errors; per-module `synth_ecp5` cell counts obtained for at least the
  core arithmetic modules.
- **Allowed claim**: "synthesizable RTL," "elaborates cleanly under
  yosys," "real, measured cell counts (yosys/ECP5 stand-in target)."
- **Prohibited claim**: "FPGA-deployed," "runs on an FPGA," any specific
  Fmax/clock-frequency claim, any Xilinx/Altera-specific LUT/DSP/BRAM
  count (only ECP5 numbers exist).

## Gate 2: Place-and-route + timing closure (NOT PASSED)

- **Required artifact**: a vendor tool's (Vivado/Quartus) place-and-route
  report showing timing closure at a specific, real clock frequency.
- **Required test**: `docs/phase8-hardware-validation-plan.md` Level A,
  steps 2-4; post-route simulation matching the pre-synthesis golden
  vectors bit-exact.
- **Allowed claim** (once passed): "closes timing at `<N>` MHz on
  `<real device part number>`," "real place-and-route result."
- **Prohibited claim before this gate**: any specific clock frequency
  stated as achieved (only the 100/200/300 MHz *assumption* from
  `docs/phase5-synthesizable-fpga.md` §10 may be cited, and only
  labeled explicitly as an assumption, never as a result); "timing
  closes," "runs at `<N>` MHz."

## Gate 3: Real board bring-up, known-vector match (NOT PASSED)

- **Required artifact**: `hardware/experiment-protocol.md` steps 1-6
  logs, board identification, bitstream hash.
- **Required test**: known-vector test passes with 0 unexplained
  mismatches on real hardware.
- **Allowed claim**: "brought up on `<real board model>`," "known-vector
  test passes on real hardware."
- **Prohibited claim before this gate**: "runs on an FPGA board,"
  "hardware-validated," "deployed."

## Gate 4: Hardware bit-exactness (NOT PASSED)

- **Required artifact**: `hardware/experiment-protocol.md` step 7's
  100K-random-block parity result, `hardware/results-schema.json`-
  conformant record with `result_label: "REAL_HARDWARE"` and
  `parity_failures: 0`.
- **Required test**: 100,000+ randomized blocks, 0 unexplained
  mismatches, on real silicon.
- **Allowed claim**: "hardware bit-exact," "bit-exact on real FPGA
  hardware, 100,000+ blocks."
- **Prohibited claim before this gate**: "hardware bit-exact" used
  unqualified (only "simulation bit-exact" is earned so far — Gate 0).

## Gate 5: Real DMA throughput/latency (NOT PASSED)

- **Required artifact**: `hardware/experiment-protocol.md` steps 8-10
  logs, `hardware/results-schema.json`-conformant records with real
  `throughput_bytes_per_sec` and `latency_ns` fields.
- **Required test**: sustained throughput measurement, queue-depth and
  batch-size scaling, both completed on real hardware with a real DMA
  engine.
- **Allowed claim** (once passed): "measured `<N>` GB/s sustained
  throughput on `<real board>`," "real p50/p95/p99 latency: `<values>`."
- **Prohibited claim before this gate**: **any throughput or latency
  number presented as a hardware result.** Every throughput/latency
  number in this project today is either a discrete-event simulation
  output or a cosimulation transaction count — neither is a throughput
  claim and must not be phrased as one (e.g. "520,000 transactions in
  7.3 seconds" describes simulation wall-clock time, not hardware
  throughput, and must never be requoted as "processes N GB/s").

## Gate 6: Real FPGA power/thermal (NOT PASSED)

- **Required artifact**: `hardware/experiment-protocol.md` steps 12-13
  logs, real vendor-sensor readings.
- **Required test**: power/thermal measurement under sustained load on
  real hardware.
- **Allowed claim** (once passed): "measured `<N>`W board power under
  sustained load."
- **Prohibited claim before this gate**: any power number presented as
  measured (Level A's vendor-tool pre-bitstream power *estimate*, if
  obtained, must always be labeled "estimated, pre-bitstream," never
  "measured").

## Gate 7: Real CXL platform integration (NOT PASSED)

- **Required artifact**: `docs/phase8-hardware-validation-plan.md`
  Level C logs, a real CXL Type-3 device or emulation platform's
  measured bandwidth/p99.
- **Required test**: memory-tier integration smoke test + multi-sequence
  workload replay against real CXL hardware.
- **Allowed claim** (once passed): "validated against a real CXL Type-3
  device," "CXL near-memory architecture, hardware-validated."
- **Prohibited claim before this gate**: **"real CXL acceleration,"
  "CXL-accelerated," or any claim that a real CXL device was used.**
  The only currently-allowed phrasing is **"CXL architecture
  simulation"** or **"CXL near-memory appliance simulation, calibrated
  from real traces"** — always with "simulation"/"simulated" present,
  never omitted.

## Summary table (current state, Phase 7.3)

| Gate | Status | What can be said today |
|---|---|---|
| 0. RTL cosimulation | **PASSED** | "simulation bit-exact," "520,000-transaction Verilator cosimulation" |
| 1. Synthesizability (yosys, no P&R) | **PASSED** | "synthesizable RTL," real ECP5 cell counts |
| 2. Place-and-route / timing closure | not passed | "assumed 100-300 MHz (not measured)" only |
| 3. Real board bring-up | not passed | no board claim of any kind |
| 4. Hardware bit-exactness | not passed | "simulation bit-exact" only, never "hardware bit-exact" |
| 5. Real DMA throughput/latency | not passed | no throughput/latency claim of any kind on real hardware |
| 6. Real power/thermal | not passed | no power/thermal claim of any kind on real hardware |
| 7. Real CXL integration | not passed | "CXL architecture simulation" only, never "CXL acceleration" |

This table itself is checked for internal consistency by
`scripts/verify-outreach.py`'s claim-gate check — see that script for
exactly how "gate not passed => claim not used anywhere" is enforced
automatically rather than by discipline alone.
