# Research roadmap

Open tracks for this research record, in priority order. This is a
living index, not a schedule with dates — see each linked document for
the actual detail and current status.

## Track 1 — Real hardware validation

`docs/phase8-hardware-validation-plan.md` plans three gated levels (FPGA
simulation/P&R with no board, a real FPGA board, real CXL/PCIe hardware)
to replace this repository's current Yosys-only, no-P&R synthesis
results with real vendor toolchain and (eventually) physical
measurements. `outreach/hardware-claim-gates.md` governs exactly what
can be said publicly at each stage — nothing before Level A completes
implies a real board exists. Not started as of this repository's
initial population.

## Track 2 — EXP-FPGA-DIV-002 continuation

Currently at `CONTINUE` after Phase B4, with R3 (direct-retire bypass)
identified as the base for any future phase — see
`experiments/EXP-FPGA-DIV-002/README.md`'s phase timeline and
`archive/phase-b4.md`'s own "Decision" section for exactly which targets
(25%-density collateral bound, overall-improvement bound,
adversarial-reduction bound) remain unmet and would define a Phase B5.

## Track 3 — Paper submission

`paper/submission-options.md` and `paper/claim-audit.md` track venue
options and the current state of claim verification
(`paper/scripts/verify-paper.py`). Not yet submitted anywhere.

## Track 4 — Reproduction tooling for the two-repository split

Both FPGA divider experiments' `reproduction/README.md` and this
repository's own root `reproduction.md` disclose the same gap: the
experiment driver scripts (`scripts/run-exp-fp-divider-001.sh`,
`scripts/run-exp-q8-divider-002.sh`) and the KV/CXL tool build
instructions were written when experimental and production RTL, and all
tooling, lived in one working tree. The `MEMBRANE_PRODUCTION_ROOT` and
`MEMBRANE_ROOT`/`-DMEMBRANE_ROOT=` path-resolution conventions
documented in those reproduction guides are the intended fix, but have
not been independently re-verified end to end against a real,
separately-cloned `kadireren7/membrane` checkout since the split. This
is the highest-priority tooling gap in this repository: until it's
verified, "clone both repos and reproduce X" is a documented intent, not
a confirmed working path.

## Track 5 — Outreach

`outreach/target-selection.md` and `outreach/contact-tracker.csv` track
in-progress outreach; `scripts/verify-outreach.py` gates every factual
claim sent externally against a real source artifact before it goes out.
