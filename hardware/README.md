# hardware/

Physical-validation planning and vendor-integration interface materials
for MEMBRANE's FPGA datapath (`rtl/`). **Nothing in this directory
claims a real board result** — no FPGA board, synthesis toolchain with
place-and-route, or CXL hardware exists in this project's development
environment as of this writing. See
`docs/phase8-hardware-validation-plan.md` for the plan this directory
supports, and `outreach/hardware-claim-gates.md` for exactly which
claims are and aren't allowed at each stage.

## Contents

- **`board-targets.md`** — candidate FPGA boards, vendor toolchains,
  PCIe/DMA framework options, clock targets, resource budgets, and the
  known divider/pipeline-replication constraints from this project's
  existing yosys-only synthesis check.
- **`vendor-wrapper/`** — interface-only RTL skeletons (AXI4-Stream
  adapter, AXI-Lite control-register adapter, DMA command/completion
  abstraction, platform-independent core wrapper) that a real board
  integration would wire `rtl/membrane_quant_stream_top.sv` behind. See
  that directory's own README for what's a skeleton vs. a spec-only
  placeholder.
- **`experiment-protocol.md`** — the exact sequence of tests to run once
  a board is actually obtained, each with command/input/metric/expected
  invariant/artifact path/pass-fail rule.
- **`results-schema.json`** — the JSON schema every real hardware
  measurement must conform to, plus one clearly-labeled documented
  example fixture (not a real result).
- **`risk-register.md`** — the real, disclosed risks to this validation
  plan (RTL resource use, timing closure, board/CXL availability, tool
  licensing, etc.), each with probability/impact/mitigation/evidence-needed.

## Why vendor-IP-free

This project does not vendor, copy, or rewrite any vendor PCIe IP or CXL
controller. The rationale is practical, not just legal caution: vendor
IP is board- and toolchain-specific, usually under a restrictive
redistribution license, and would make this repository's RTL
non-portable across the candidate boards in `board-targets.md`. Instead,
`vendor-wrapper/` defines plain interface boundaries
(AXI4-Stream/AXI-Lite/DMA-command signal lists) that a real integration
wires to whatever vendor shell/IP the target board actually provides —
see `vendor-wrapper/README.md` for the exact signal tables and
integration checklist.

## What this directory does NOT contain

- A working bitstream for any board.
- A real Fmax, timing-closure, power, or throughput number (those exist
  only as clearly-labeled *target assumptions* inherited from
  `docs/phase5-synthesizable-fpga.md` §8, never presented as measured).
- Any vendor IP core, driver, or board-support package.
- Any real `hardware/results-schema.json`-conformant result record —
  only the schema and one documented example fixture, kept in a
  separate, clearly-labeled file so it can never be mistaken for real
  data (see `results-schema.json`'s own header comment).
