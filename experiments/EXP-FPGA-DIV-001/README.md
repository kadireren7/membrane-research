# EXP-FPGA-DIV-001 -- Q4_0 divider research record (index)

This directory preserves the complete research record behind the Q4_0
divider change now in `main`'s FPGA datapath (`rtl/membrane_fp_scale_neg_pow2.sv`,
`rtl/membrane_fp_divider_radix4.sv`, `rtl/q4_scale.sv`,
`rtl/membrane_quant_stream_top.sv`). It is documentation only -- no RTL,
test, or benchmark file lives under this directory; the production source
these documents describe is in `rtl/`.

## Problem

`membrane_fp_divider.sv` (the FPGA quant/dequant datapath's general-purpose
FP32 divider, `docs/phase5-synthesizable-fpga.md` §7) is a single-cycle,
wide combinational integer divide. That is a disclosed timing-closure risk
on real FPGA fabric, unquantified because no vendor place-and-route tool
exists in this project's development environment. This experiment
characterized that risk's four actual call sites and evaluated whether any
could be replaced by an alternative construction using meaningfully fewer
synthesized cells, without changing the datapath's bit-exact behavior.

## Baseline

Two call sites inside `q4_scale`: a constant-divisor `mx/-8.0f` and a
variable-divisor `1/d`, both using `membrane_fp_divider.sv`. (The other
two call sites, `q8_scale`'s `d=amax/127.0` and `id=127.0/amax`, are a
related but separate investigation — see `EXP-FPGA-DIV-002`.) Phase A's
real synthesis plus a 520,000-transaction cosimulation confirmed both
`q4_scale` sites unchanged and identified 4 candidate directions for
Phase B, before any design work began (`archive/baseline.md`,
`results/canonical/synthesis.csv`).

## Key measurements

The headline result promoted to production: **-32.13% cycles/transaction
vs. Phase B2's own already-working radix-2 divider, for +25.0% ECP5
cells** at the `q4_scale` integration point — exact parity maintained
throughout (4,456,685 differential cases, 0 mismatches, Phase B4 vs. both
the original combinational divider and B2 simultaneously). Full
per-phase numbers are in the table below; nothing here is rounded past
what `results/canonical/` actually records.

## Phase summary

| Phase | Scope | Result |
|---|---|---|
| A (`archive/baseline.md`) | Characterize the baseline divider and its 4 call sites; no design work | Accepted as complete -- real synthesis + 520,000-transaction cosimulation confirmed the baseline unchanged, 4 candidate directions identified for Phase B |
| B1 (`archive/phase-b1.md`) | Replace `q4_scale`'s constant-divisor `mx/-8.0f` with an exact power-of-two shortcut | **Exact, but a limited mapped-area win**: 2.2M+ differential cases, 0 mismatches, but the real `q4_scale`-level ECP5 cell reduction was only -2.2% -- ABC's technology mapper was already sharing most of the cost between `q4_scale`'s two divider instances in the baseline |
| B2 (`archive/phase-b2.md`) | Replace the remaining variable-divisor `1/d` with an exact iterative (radix-2) divider | **Large area win, high throughput cost**: -96.9% ECP5 cells at `q4_scale`, but full-serialization scheduling (needed because the new divider is multi-cycle) collaterally slowed 3 untouched chains ~1.9-2.6x -- a queueing cost, not a divider-speed cost |
| B3 (`archive/phase-b3.md`, `archive/phase-b3-root-cause.md`) | Decouple the 3 unaffected chains from Q4_0 encode's in-flight status via a bounded completion reorder buffer | **Correct, but REJECT_ARCHITECTURE** (`archive/decision.md`): 0 fails/drops/duplicates/deadlocks across 7.77M+ transaction-checks at every depth tested, but the only depth that helped at all (4) cost MORE ECP5 area (14,959 cells) than the entire divider unit (2,268 cells) it was protecting, for only a 4-5% throughput gain |
| B4 (`archive/phase-b4.md`) | Replace B2's radix-2 divider with an exact radix-4 (2 quotient bits/cycle) divider, keeping B2's own scheduling, no reorder buffer | **PROMOTE_CANDIDATE**: exact parity vs. both the real divider and B2 simultaneously (4,456,685 cases, 0 mismatches), -32.13% cycles/transaction vs. B2 for only +25.0% ECP5 cells at the `q4_scale` integration point -- the strongest result of any phase |

Full per-phase reasoning, numbers, and reproduction instructions are in
each phase's own document; `archive/decision.md` is the running per-phase decision
log (including the note recording why Phase B3's own original CONTINUE
call was later revised to REJECT_ARCHITECTURE).

## What went to production, and what didn't

**Merged into `main` (PR [#2](https://github.com/kadireren7/membrane/pull/2),
squash commit `f96c695`)**: B1's constant power-of-two scale shortcut and
B4's exact radix-4 iterative divider, plus the minimal Q4_0-encode
issue-serialization scheduling B4's variable-latency divider requires
(architecturally B2's own scheme, carried through B3 and B4 unchanged).
`q8_scale.sv` and `membrane_quant_stream_top`'s external interface are
unchanged. See `archive/promotion-audit.md` (file-by-file classification),
`archive/promotion-plan.md` (integration plan), and `archive/promotion-comparison.md`
(main/experiment/clean-branch comparison) in this directory for the full
detail -- all three were written before the merge and are left as the
record that justified it (each carries its own "Status" note pointing
here).

**Not promoted, preserved only on `experiment/fp-divider-pipeline`**:

- Phase B2's radix-2 iterative divider (`rtl/experimental/fp_div/fp32_div_iterative_exact.sv`) -- superseded by B4's radix-4 divider.
- Phase B3's completion reorder buffer (`rtl/experimental/fp_div/membrane_completion_reorder.sv`) and its decoupled-issuance top-level variant -- REJECT_ARCHITECTURE.
- The B1/B2/B3-intermediate top-level and `q4_scale` integration variants -- superseded by the B4 integration actually promoted.
- The multi-variant (`#ifdef`-selected) Verilator testbench harness used to compare these alternatives during research -- not carried into the production test suite (which uses focused, single-target differential tests instead, `archive/promotion-audit.md`'s NEEDS_CLEANUP items).

## Result files

- `results/canonical/synthesis.csv`, `results/canonical/baseline-synthesis.txt` -- Phase A baseline synthesis.
- `results/canonical/b1-comparison.md` -- Phase B1 numeric comparison.
- `results/canonical/b2-comparison.md`, `results/canonical/b2-differential.json`, `results/canonical/b2-full-datapath.json` -- Phase B2.
- `results/canonical/b3-comparison.md`, `results/canonical/b3-full-datapath.json`, `results/canonical/b3-performance.csv`, `results/canonical/b3-synthesis.csv` -- Phase B3.
- `results/canonical/b4-comparison.md`, `results/canonical/b4-differential.json`, `results/canonical/b4-full-datapath.json`, `results/canonical/b4-synthesis.csv` -- Phase B4.

See `methodology.md` for test design, toolchain, exactness rules, and
measurement classification.

## Reproduction

See `reproduction/README.md` for exact `--phase`/`--quick`/`--full`
commands (A/B1/B2/B3/B4, all of them — including B4, the phase that was
promoted) against this repository's own `rtl/experimental/fp_div/`,
compared against a real `kadireren7/membrane` checkout's production RTL
via `MEMBRANE_PRODUCTION_ROOT`. For a quicker check of just the promoted
result as it exists in production today (no experimental-RTL checkout
needed), use `kadireren7/membrane`'s own `docs/reproduction.md` Level 1.4
instead — the two paths exercise the same production RTL from opposite
directions (this one also runs the rejected/superseded B2/B3 alternatives
alongside it, for comparison).

## Provenance

- **Migrated to `membrane-research`** during the repository-focus split
  (see `provenance/import-manifest.json` at this repo's root) from
  `kadireren7/membrane`'s `experiment/fp-divider-pipeline` branch,
  commit `4c22efa5bc29f42579f2ea641dd6c9458dec988c`. That branch was
  **never merged to `main`** and is **not deleted** -- it remains the
  authoritative git history for this experiment; this migration copies
  its content (verified by SHA256, see the import manifest) rather than
  replacing it.
- The RTL/testbench evidence this experiment produced
  (`rtl/experimental/fp_div/*.sv`, `rtl/tb/tb_fp32_*.cpp`) previously
  existed **only** on that branch -- `experiments/EXP-FPGA-DIV-001/`'s
  own docs were merged to `membrane`'s `main` (PR #3,
  `docs: preserve EXP-FPGA-DIV-001 research record`) without the code
  that produced them. This migration reunites the two under
  `rtl/experimental/fp_div/` and `rtl/tb/` in this repository.
- Clean promotion PR:
  [kadireren7/membrane#2](https://github.com/kadireren7/membrane/pull/2)
  (`rtl: replace Q4 combinational dividers with exact radix-4 datapath`),
  squash-merged as `f96c695` -- **this is the authoritative record of
  what reached production**, unaffected by this migration.
- Production source (`membrane_fp_scale_neg_pow2.sv`,
  `membrane_fp_divider_radix4.sv`, `q4_scale.sv`,
  `membrane_quant_stream_top.sv`) is in `kadireren7/membrane`, `main`,
  under `rtl/` -- **not duplicated here**; this repository references it
  by commit, per the two-repository contract's single-source-of-truth
  rule.
- Rejected/superseded RTL (B2's radix-2 divider, B3's completion reorder
  buffer, and the intermediate B1/B2/B3 top-level wiring) is preserved
  in this repository under `rtl/experimental/fp_div/`, exactly as it
  existed on `experiment/fp-divider-pipeline` -- see `reproduction/README.md`
  for how to build and run it here.

## Real hardware limitation

No real FPGA board, no vendor place-and-route toolchain (Vivado/Quartus),
and no measured Fmax exist anywhere in this record, in any phase, for
either the baseline divider or any evaluated alternative -- unchanged by
the promotion to `main`. Every cell count cited above and in every linked
document is a Yosys 0.33 generic or `synth_ecp5` synthesis result, a proxy
for real FPGA resource usage, not a measurement of it. `q8_scale.sv`'s two
`membrane_fp_divider` instances remain completely untouched by this
experiment and its promotion, carrying the same disclosed,
unquantified timing-closure risk Phase A first raised for them.
