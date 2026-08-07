# Research roadmap

This is a living index, not a schedule with dates — see each linked
document for the actual detail and current status.

## Closed experiments

### EXP-FPGA-DIV-002 — `RESEARCH_COMPLETE_NO_PROMOTION`

Closed after Phase B4. **This experiment is not continuing under this
ID** — no Phase B5, B6, or further R-candidate work happens here. Any
future work on the bottleneck below is a **new experiment ID** (see R5).

- **Best known candidate**: Phase B4's R3 (direct-retire bypass, on top
  of Phase B3's `b3_split`). Real, if partial, improvement: 4.5-4.8%
  faster than `b3_split` overall, +4.1% on the adversarial-retirement
  pattern, met the ≤10% collateral bound at 20% Q8_0-encode density on
  all three affected modes.
- **Exact reason promotion did not happen**: this experiment's own
  success criteria required meeting the ≤10% collateral bound at
  *both* 20% and 25% density, plus a ≥10% overall-improvement bound and
  a preferred ≥35% adversarial-reduction bound. R3 met the 20%-density
  bound but not 25% (+11.79%/+1.26%/+11.71% across the three modes) and
  fell short of both the overall-improvement and adversarial-reduction
  bounds. No candidate across any phase met the full set simultaneously.
- **Exact unresolved bottleneck**: a software retirement-state model
  (`scripts/b4-retirement-model.py`) found that strict in-order
  retirement — not head-of-line input blocking, which Phase B3 already
  solved — now dominates stalls: 65.6-80.5% of stall cycles across
  density profiles (`experiments/EXP-FPGA-DIV-002/results/canonical/b4-retirement-analysis.md`).
  R1 (fewer completion slots) and R2 (a small shared completion queue)
  both made this *worse*, not better (R1: -84.3% on the
  adversarial-retirement pattern). The bottleneck is architectural: a
  larger reorder/completion structure would plausibly close more of the
  gap, but every phase in this experiment explicitly excluded that
  ("bounded bookkeeping only," "no depth 4/8 ROB sweep," "no area-heavy
  generic reorder structure") — the ceiling documented here is a
  ceiling *of that constraint*, not of the underlying idea.

Full record: [`experiments/EXP-FPGA-DIV-002/README.md`](experiments/EXP-FPGA-DIV-002/README.md).

## Near-term roadmap

### R1 — Repository split + ownership

**Definition of done**: `membrane` and `membrane-research` are both
clean and correctly scoped; `membrane`'s README is understandable by a
recruiter in about 90 seconds; the maintained build is reproducible from
a clean clone without reading research history; `career/MEMBRANE_INTERVIEW_GUIDE.md`
is complete. Status: in progress (Gate C/D of the repository-focus
migration).

### R2 — Production promotion exercise

Pick one already-mature research result and take it through the full
research → PR → review → merge process end to end, with Kadir doing the
review himself (not delegating it), to build that muscle on a low-risk
change before it's needed on a higher-stakes one. `EXP-FPGA-DIV-001`'s
own promotion (`kadireren7/membrane#2`) is the template to repeat, not
a new architectural bet — the goal is process fluency, not a new result.
Not started.

### R3 — External reproduction

Have at least one experiment in this repository reproduced by someone
other than the original agent session that produced it (Kadir himself,
running `experiments/EXP-FPGA-DIV-001/reproduction/README.md` or
`EXP-FPGA-DIV-002/reproduction/README.md` end to end on his own, counts).
This is also the first real end-to-end test of the Track-4 tooling gap
below. Not started.

### R4 — Vendor tooling / hardware validation preparation

Only if real hardware/tool access exists — see
`docs/phase8-hardware-validation-plan.md`'s three gated levels (FPGA
simulation/P&R with no board, a real FPGA board, real CXL/PCIe
hardware) and `outreach/hardware-claim-gates.md` for exactly what can be
said publicly at each stage. Possible concrete steps: a real
nextpnr/vendor place-and-route flow, an actual FPGA board, real
LUT/FF/Fmax/power numbers. **No result is claimed before it is
measured** — this entry stays a plan until Level A of that document
actually completes. Not started.

### R5 — Broader memory-system experiment

The next architectural attempt at EXP-FPGA-DIV-002's unresolved
retirement-pressure bottleneck (or any other new research direction)
gets its **own new experiment ID** (e.g. `EXP-FPGA-DIV-003`), not a
`B5`/`B6` continuation of a closed experiment. Not yet scoped or started.

## Also tracked

- **Paper submission**: `paper/submission-options.md` and
  `paper/claim-audit.md` track venue options and the current state of
  claim verification (`paper/scripts/verify-paper.py`). Not yet
  submitted anywhere.
- **Track 4 — reproduction tooling for the two-repository split**: both
  FPGA divider experiments' `reproduction/README.md` and this
  repository's own root `reproduction.md` disclose the same gap: the
  experiment driver scripts and KV/CXL tool build instructions were
  written when experimental and production RTL, and all tooling, lived
  in one working tree. The `MEMBRANE_PRODUCTION_ROOT` and
  `MEMBRANE_ROOT`/`-DMEMBRANE_ROOT=` path-resolution conventions
  documented in those reproduction guides are the intended fix, but
  have not been independently re-verified end to end against a real,
  separately-cloned `kadireren7/membrane` checkout since the split —
  R3 above is this gap's first real test.
- **Outreach**: `outreach/target-selection.md` and
  `outreach/contact-tracker.csv` track in-progress outreach;
  `scripts/verify-outreach.py` gates every factual claim sent externally
  against a real source artifact before it goes out.
