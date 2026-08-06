# EXP-FPGA-DIV-002 — Q8_0 divider/scheduler research record (index)

This directory preserves the complete research record of a five-phase
investigation into the Q8_0 encode/decode datapath's own divider pair
and its input-scheduling logic. Unlike EXP-FPGA-DIV-001, **no part of
this experiment has been promoted to production** — every RTL file here
is experimental, and the current decision after Phase B4 is `CONTINUE`,
not a merge recommendation. See "What went to production" below.

## Executive summary

`q8_scale.sv` (production, unmodified throughout this experiment)
computes both `d = amax/127.0` and `id = 127.0/amax` using two full,
wide `membrane_fp_divider` instances (123,742 ECP5 cells measured at
this integration point). This experiment asked, in order: can the two
dividers be replaced by something smaller without losing bit-exactness
(Phase A → B1), and once they are, does the resulting scheduler create
new collateral cost on other in-flight transaction classes, and can that
cost be reduced (Phase B2 → B3 → B4)?

## Phase timeline

(Exact values from each phase's own canonical artifact — see the
"Evidence" column's own file for the full context.)

| Phase | Question | Evidence | Outcome | Decision |
|---|---|---|---|---|
| A (`archive/baseline.md`) | Can one divider + an algebraic/reciprocal transform replace the dual-divider pair? | 2,050,239 differential cases (`results/canonical/feasibility-differential-full.txt`) | **Not bit-exact** — reciprocal reconstruction mismatched 25.04%/28.17% of cases (1 ULP each), constant-reciprocal multiply mismatched 4.54% — all candidates rejected | Pursue an exact dual-radix-4-divider design in Phase B1 |
| B1 (`archive/phase-b1.md`) | Can two exact radix-4 dividers replace the two wide dividers, bit-exact? | 4,052,224 differential cases, 0 mismatches; real ECP5 synthesis | **Exact, and much smaller — but the divider itself is now 16-cycle-latency, not single-cycle**: -97.76% ECP5 cells (123,742 → 2,775) at the `q8_scale` integration point, real collateral slowdown on other in-flight modes (full-serialization scheduling required) | `PROMOTE_CANDIDATE` (experiment-branch-only — this is a research decision, not a production merge) |
| B2 (`archive/phase-b2.md`) | Can a bounded, tag-based scheduler reduce B1's own full-serialization collateral cost? | Mixed-mode traffic, 6,250,000 transactions across 4 full runs, 0 mismatches | **Material, real improvement**: >=25% cycles/transaction better than B1 on every mixed profile (28.5-36.6%), but the strict <=10% collateral-slowdown target was not met at 20-25% Q8_0-encode density (residual 16-26%) | `CONTINUE` |
| B3 (`archive/phase-b3.md`) | Can bounded input lookahead remove the remaining head-of-line blocking? | 8,042,500 transactions/candidate across 6 candidates | **Lookahead (2/4-entry) made collateral WORSE, not better** (a real, counter-to-hypothesis result, disclosed not hidden) — constant per-issue selection overhead outweighed the bypass benefit. **Mode-split ingress queues (B3-split) worked**: met the <=10% bar at 20% density, 17.8-21.4% overall improvement vs. B2 | `CONTINUE`, with B3-split selected as the base for Phase B4 |
| B4 (`archive/phase-b4.md`) | Can shared retirement/completion-storage pressure be reduced safely, without a general reorder buffer? | 8,382,500 transactions/candidate across 7 candidates | A software retirement-pressure model found strict in-order retirement (not head-of-line blocking) now dominates, 65.6-80.5% of stalls. **Direct-retire bypass (R3) helped, partially**: 4.5-4.8% faster than B3-split, met the <=10% collateral bar at 20% density (not 25%). Reducing completion-storage capacity alone (R1/R2) made things **worse**, R1 severely so (-84.3% on the adversarial-retirement pattern) | `CONTINUE`, R3 identified as the base for any future phase |

## What went to production, and what didn't

**Nothing from this experiment has been merged to `kadireren7/membrane`'s
`main`.** Every phase's own decision is `CONTINUE`, `PROMOTE_CANDIDATE`
(the exact-radix-4-divider result, Phase B1), or a rejection (Phase B3's
lookahead candidates) — `PROMOTE_CANDIDATE` here is an
**experiment-branch-internal** research verdict, explicitly not a merge
authorization, per this experiment's own repeated, explicit scope
statement in every phase document.

The production Q4_0 datapath's own exact-radix-4 divider (a related but
**separate** experiment, `EXP-FPGA-DIV-001`, promoted via
[kadireren7/membrane#2](https://github.com/kadireren7/membrane/pull/2))
is not the same result as this experiment's own Q8_0 work — the two
experiments share an architectural idea (exact radix-4 division in place
of a wide combinational divider) but EXP-FPGA-DIV-002 evaluates it for
the wider, dual-instance Q8_0 case with genuinely new scheduling
questions EXP-FPGA-DIV-001 never needed to answer (Q4_0 has one divider,
not two; no shared-scheduler collateral-cost question arises).

## Result files

35 canonical artifacts in `results/canonical/` — differential JSON,
synthesis CSV, retirement-taxonomy CSV, run-manifest/promotion-record
JSON, and narrative `.md` analysis for each phase. See each phase's own
archived document for which files are its own.

## Provenance

- **Migrated to `membrane-research`** during the repository-focus split
  (see `provenance/import-manifest.json` at this repo's root) from
  `kadireren7/membrane`'s `experiment/q8-divider-pipeline` branch,
  commit `61ce8adc4733512a78dcf5c04844e6b85da04b54`. That branch is
  **not deleted** and remains the authoritative git history.
- Experimental RTL (`rtl/experimental/q8_div/`, 11 `.sv`/`.cpp` files
  spanning baseline-dual-radix4 through B2/B3(l2/l4/split)/B4(R1/R2/R3))
  and the phase-specific reproduction/provenance tooling
  (`scripts/run-exp-q8-divider-002.sh`, `scripts/gen-b3-*.py`,
  `scripts/gen-b4-*.py`, `scripts/*-retirement-model.py`,
  `scripts/test-exp-q8-divider-002-provenance.sh`,
  `scripts/verify-exp-q8-divider-002-results.py`) are imported alongside
  this record, byte-for-byte (SHA256-verified, see the import manifest).
- Production source this experiment compares against
  (`membrane_fp_divider_radix4.sv`, `membrane_fp_pkg.sv`, and every
  other `rtl/*.sv`/`rtl/tb/*` file the experimental tops instantiate)
  stays in `kadireren7/membrane`, `main`, under `rtl/` — **not
  duplicated here**; referenced by commit per the two-repository
  contract's single-source-of-truth rule.
- See `reproduction/README.md` for exact commands, and
  `methodology.md` for exactness rules, toolchain, and measurement
  classification (SIMULATED/MEASURED_BY_TOOL/ESTIMATED/UNAVAILABLE) —
  this experiment's own provenance-safety infrastructure
  (`scripts/gen-run-manifest.py`, `scripts/verify-exp-q8-divider-002-results.py`)
  is itself one of its real deliverables (Phase B4 Part A), not just
  process — see `results/canonical/b4-run-provenance.md`.

## Real hardware limitation

No real FPGA board, no vendor place-and-route toolchain, and no measured
Fmax, LUT utilization, or power exist anywhere in this record, in any
phase. Every cell count is a Yosys 0.33 generic or `synth_ecp5`
synthesis-tool proxy result. Isolated-wrapper synthesis in Phase B4 (the
only synthesis method that produced any real number for the B3/B4
scheduler candidates, since full-top synthesis timed out at every phase)
used trivial fixed-latency stand-ins for the actual divider engines — see
`archive/phase-b4.md`'s own "Synthesis" section for exactly what is and
is not real in those specific numbers.
