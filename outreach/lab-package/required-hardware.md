# Required hardware

Three increasing levels of commitment, matching
`docs/phase8-hardware-validation-plan.md`. A lab can help at any one
level without committing to the others.

## Level A — FPGA synthesis + place-and-route (lowest commitment)

- **Hardware**: none required — synthesis/P&R targets a device part
  number, no physical board needed for this level alone.
- **Software**: Vivado (Xilinx/AMD path) or Quartus Prime (Intel/Altera
  path), whichever your lab already has a license for.
- **Time**: a few days for a clean run; longer if the FP32 divider needs
  re-pipelining to close timing (a real, disclosed, expected risk — see
  `hardware/board-targets.md`).
- **What we're asking**: run place-and-route on the existing RTL
  (`rtl/*.sv` + `hardware/vendor-wrapper/`), report the real Fmax/
  utilization/timing result — whichever way it comes out.

## Level B — Real FPGA board (moderate commitment)

- **Hardware**: an AMD/Xilinx Alveo U250/U280/U55C, or an accessible
  equivalent your lab already has (see `hardware/board-targets.md` for
  the tradeoffs — board choice should follow what you already have, not
  require a new purchase).
- **Software**: the matching XRT/board-support-package (Xilinx) or OPAE
  (Intel) DMA stack.
- **Time**: 1-2 weeks for bring-up + known-vector parity test; 1-2
  additional weeks for the full throughput/scaling/power suite.
- **What we're asking**: run `hardware/experiment-protocol.md`'s steps
  1-15 and report the results (via `hardware/results-schema.json`-
  conformant records) — again, whichever way they come out.

## Level C — CXL platform (highest commitment, most valuable if available)

- **Hardware**: a real CXL Type-3 memory device, or an accessible CXL
  emulation/prototyping platform.
- **Software**: a recent Linux kernel with CXL core driver support.
- **Time**: highly platform-dependent, realistically 2-6 weeks.
- **What we're asking**: check whether this project's near-memory/CXL
  discrete-event simulation (`docs/phase6-cxl-near-memory.md`) resembles
  real CXL device behavior at all — this is the single most valuable
  open question this project cannot answer without external hardware
  access, since CXL hardware availability is by far this plan's largest
  risk (see `hardware/risk-register.md`).

## None of these is an all-or-nothing ask

Level A alone, with a real pass-or-fail timing result reported back,
would already be a meaningful contribution — see
`outreach/collaboration-scope.md` (in this same directory) for what's
explicitly NOT being asked for (funding, exclusivity, co-authorship
obligations).
