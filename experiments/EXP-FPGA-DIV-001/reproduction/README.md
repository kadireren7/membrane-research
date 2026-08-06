# EXP-FPGA-DIV-001 — reproduction

All commands run from this repository's root
(`kadireren7/membrane-research`), using this repository's own
`rtl/experimental/fp_div/` and `rtl/tb/tb_fp32_*` (imported from
`kadireren7/membrane`'s `experiment/fp-divider-pipeline` branch —
see the top-level README's "Provenance" section for the exact commit
and hash verification).

## Requirements

- Yosys 0.33 and Verilator, same versions the maintained `membrane`
  repository's own `tools/.local-yosys` / `tools/.local-verilator`
  pin (see `provenance/source-map.md` at this repo's root for exact
  version strings recorded at import time).
- A checkout of `kadireren7/membrane` at or after `f96c695` (the commit
  this experiment's own promoted result was merged as) for the
  production RTL files (`rtl/membrane_fp_divider_radix4.sv`, etc.) that
  `scripts/run-exp-fp-divider-001.sh` compares candidates against —
  this repository does not duplicate that source, per the
  single-source-of-truth rule in `provenance/repository-contract.md`.

## Quick reproduction (per phase)

```bash
git clone https://github.com/kadireren7/membrane-research
cd membrane-research

# Point at a local checkout of the maintained membrane repository for
# the production RTL this experiment's candidates are compared against:
export MEMBRANE_PRODUCTION_ROOT=/path/to/kadireren7/membrane

scripts/run-exp-fp-divider-001.sh --phase b1 --quick
scripts/run-exp-fp-divider-001.sh --phase b2 --quick
scripts/run-exp-fp-divider-001.sh --phase b3 --quick
scripts/run-exp-fp-divider-001.sh --phase b4 --quick
```

Replace `--quick` with `--full` for the real research-scale run (see
each phase's own archived document, e.g. `../archive/phase-b4.md`, for
the exact transaction counts and expected wall-clock time — full runs
took real, disclosed multi-minute-to-multi-hour time originally, not a
quick smoke test).

**Note**: `scripts/run-exp-fp-divider-001.sh` was written when this
experiment's own RTL and the production RTL it compares against lived
in the same repository (`membrane`, on the `experiment/fp-divider-pipeline`
branch). Now that the experimental RTL and the production RTL it's
compared against live in two separate repositories, the script needs a
small path-resolution update (`MEMBRANE_PRODUCTION_ROOT` above) before
it will run unmodified here — tracked as a real, disclosed gap, not
silently worked around; see `provenance/source-map.md`'s own "known
gaps" section.

## Expected output

Each phase's own PASS/FAIL and exact transaction/cycle counts are
recorded, already measured, in `../results/canonical/` (e.g.
`b4-differential.json`, `b4-full-datapath.json`,
`b4-synthesis.csv`) — a fresh run should reproduce those numbers exactly
(deterministic seeds throughout, per this project's own established
convention), not merely numbers "in the same range."
