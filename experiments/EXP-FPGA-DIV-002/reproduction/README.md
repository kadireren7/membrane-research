# EXP-FPGA-DIV-002 — reproduction

All commands run from this repository's root
(`kadireren7/membrane-research`), using this repository's own
`rtl/experimental/q8_div/` (imported from `kadireren7/membrane`'s
`experiment/q8-divider-pipeline` branch — see the top-level README's
"Provenance" section for the exact commit and hash verification).

## Requirements

Yosys 0.33 and Verilator (see `provenance/source-map.md` at this repo's
root for exact version strings recorded at import time), plus a local
checkout of `kadireren7/membrane` for the shared production RTL
(`rtl/membrane_fp_pkg.sv`, `rtl/membrane_fp_divider_radix4.sv`, and every
other `rtl/*.sv`/`rtl/tb/*` file the experimental tops instantiate) —
this repository does not duplicate that source.

## Commands (all phases and candidates)

```bash
git clone https://github.com/kadireren7/membrane-research
cd membrane-research
export MEMBRANE_PRODUCTION_ROOT=/path/to/kadireren7/membrane

# Quick smoke (small transaction counts, elaboration-only synthesis):
scripts/run-exp-q8-divider-002.sh --phase a  --quick
scripts/run-exp-q8-divider-002.sh --phase b1 --quick
scripts/run-exp-q8-divider-002.sh --phase b2 --quick
scripts/run-exp-q8-divider-002.sh --phase b3 --quick
scripts/run-exp-q8-divider-002.sh --phase b4 --quick

# Full research scale (real transaction counts this record's own
# results/canonical/ artifacts were produced at -- multi-minute to
# multi-hour, see each phase's own archived document):
scripts/run-exp-q8-divider-002.sh --phase b4 --full --run-id <your-run-id>
```

`--phase b4`'s own provenance-safety flags
(`--run-id`, `--promote-results`, `--force-promote`, `--dry-run`,
`--print-output-dir`, `--resume`) work as documented in
`archive/phase-b4.md`'s own "Part A: provenance safety" section — a
`--full --promote-results` run re-validates and re-publishes this
directory's own `results/canonical/b4-*` files from a fresh run, the
same mechanism that produced them originally.

**Note (same disclosed gap as EXP-FPGA-DIV-001)**: this script was
written when the experimental and production RTL trees lived in the
same repository. `MEMBRANE_PRODUCTION_ROOT` above is the intended path-
resolution fix for the now-split layout; verifying it end-to-end against
a real `membrane` checkout is tracked as open reproduction-tooling work,
not yet independently re-verified after the split — see
`provenance/source-map.md`'s own "known gaps" section and
`ROADMAP.md` Track 4 at this repository's root.

## Expected output

Deterministic seeds throughout (see `methodology.md`) — a fresh full run
should reproduce the exact transaction/cycle counts already committed in
`results/canonical/`, not merely numbers in the same range. Every
`PASS:` line format, and the retirement-taxonomy/synthesis CSV schemas,
are documented in `results/schemas/`.
