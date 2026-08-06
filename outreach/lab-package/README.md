# MEMBRANE lab evaluation package

If you're a lab evaluating whether to help validate MEMBRANE on real
FPGA/CXL hardware, this directory is the self-contained starting point.
After cloning the repository, read these files in this order:

1. **`quick-start.md`** — what to look at and which command to run
   first (no hardware required for this step).
2. **`required-hardware.md`** — exactly what hardware/toolchain access
   would let you help, at three increasing levels of commitment.
3. **`experiment-checklist.md`** — the concrete, ordered list of tests
   to run once hardware is available.
4. **`expected-artifacts.md`** — what a completed run should produce,
   and where.
5. **`collaboration-scope.md`** — what's being asked of your lab, what's
   not, and what Kadir provides in return.

## The short version

- **What to look at**: `paper/main.md` (the manuscript),
  `docs/phase8-hardware-validation-plan.md` (the 3-level plan this
  package supports), `outreach/hardware-claim-gates.md` (exactly which
  claims are and aren't allowed yet).
- **What command to run**: `scripts/demo.sh --quick` (~25–50 seconds
  depending on cache state, no model download, no hardware needed) to
  confirm the software side works before considering any hardware
  commitment.
- **What hardware is needed**: see `required-hardware.md` — in short,
  an FPGA board + Vivado/Quartus (Level A/B), and/or a CXL Type-3 device
  or emulation platform (Level C).
- **What's needed from Kadir**: nothing beyond what's already in this
  repository — the RTL, test vectors, and protocol are all here. He's
  available to answer integration questions.
- **What's needed from the lab**: hardware/toolchain access (even
  time-boxed or remote), and someone to run
  `hardware/experiment-protocol.md`'s steps and report the results back
  (real, not filtered — negative results are exactly as valuable to
  this project as positive ones).
