# EXP-FPGA-DIV-001 -- promotion audit

**Status: B1+B4 production integration was merged through PR #2.** The
rest of this document was written before that merge, while `feature/
q4-radix4-divider` was still an open candidate branch -- left unchanged
below (per this experiment's own disclosed-not-rewritten convention) as
the audit record that justified the merge, not edited to read as if it
always described `main`'s current state.

File-by-file classification of every file in `main..experiment/fp-divider-pipeline`
(38 files, all additions -- no production file was modified by this experiment;
confirmed by `git diff --stat main..experiment/fp-divider-pipeline`, see below).
This is an audit only: it does not itself change any file on either branch.
See `promotion-plan.md` for how the `PROMOTE` set is actually integrated into
production RTL, and `decision.md`/`experiment.md` for the experiment's own
per-phase decisions this audit is built on.

## Scope recap (per the promotion task, not re-litigated here)

Candidate for `main`: **B1's constant power-of-two scale shortcut** + **B4's
exact radix-4 iterative divider**, plus the minimal Q4 scheduling/handshake
change B4's own top level requires (which is architecturally B2's, unchanged
by B3 or B4 -- see `decision.md`'s per-phase table). **Not** candidates: B2's
radix-2 divider (superseded by B4), B3's reorder buffer and its scheduling
(REJECT_ARCHITECTURE), the B1/B2 intermediate top-level/q4_scale integration
variants (superseded by the B4 integration), and the multi-variant research
harness/testbench files (kept for reproducibility, not needed in production
form).

## Classification legend

- **PROMOTE** -- becomes (the basis of) a production file on `feature/q4-radix4-divider`.
- **KEEP_EXPERIMENT_ONLY** -- stays on `experiment/fp-divider-pipeline` /
  under `experiments/EXP-FPGA-DIV-001/`, not touched by the promotion.
- **DROP_FROM_INTEGRATION** -- real, correct research artifact, but explicitly
  out of scope for `main` (B2/B3 architecture, superseded intermediate variants).
- **NEEDS_CLEANUP** -- contains material that IS needed in production, but not
  in its current form (multi-variant test harnesses that need trimming to a
  single production target).

## Documentation and results (`experiments/EXP-FPGA-DIV-001/`)

| Path | Purpose | Classification | Rationale | Dependency | Test coverage |
|---|---|---|---|---|---|
| `baseline.md` | Phase A baseline characterization | KEEP_EXPERIMENT_ONLY | Historical record of the pre-experiment state; not production code | none | n/a (doc) |
| `experiment.md` | Running experiment record, all phases | KEEP_EXPERIMENT_ONLY | Research narrative/decision trail, per-phase addenda; `promotion-plan.md`/`promotion-comparison.md` summarize what's needed for `main` instead of moving this | none | n/a (doc) |
| `decision.md` | Per-phase decision log (CONTINUE/REJECT_ARCHITECTURE/PROMOTE_CANDIDATE) | KEEP_EXPERIMENT_ONLY | Authoritative research decision trail; referenced by this audit, not duplicated into it | none | n/a (doc) |
| `phase-b1.md` | B1 design/results/decision | KEEP_EXPERIMENT_ONLY | Source material for `fp32_scale_neg_pow2`'s promoted rationale (carried into the production module's own header instead) | none | n/a (doc) |
| `phase-b2.md` | B2 design/results/decision | KEEP_EXPERIMENT_ONLY | Documents the radix-2 divider and full-serialization scheduling, both DROP or superseded-by-B4 respectively; kept as the origin record for the scheduling scheme B4 (and this promotion) reuses | none | n/a (doc) |
| `phase-b3.md` | B3 design/results/decision | KEEP_EXPERIMENT_ONLY | Documents the rejected reorder-buffer architecture; explicitly not promoted, kept for the historical record of why | none | n/a (doc) |
| `phase-b3-root-cause.md` | B3 root-cause analysis of B2's collateral slowdown | KEEP_EXPERIMENT_ONLY | Research analysis, not production code | none | n/a (doc) |
| `phase-b4.md` | B4 design/results/decision (PROMOTE_CANDIDATE) | KEEP_EXPERIMENT_ONLY | Primary source for this promotion's technical rationale; summarized into `promotion-plan.md`/module headers, not moved | none | n/a (doc) |
| `results/*.md`, `results/*.json`, `results/*.csv`, `results/baseline-synthesis.txt` | Raw/derived measurement data for every phase | KEEP_EXPERIMENT_ONLY | Point-in-time research measurements (some for DROP'd phases B2/B3); `promotion-comparison.md` re-derives the subset relevant to B1/B4 on the clean branch instead of copying these files forward | none | n/a (data) |

## RTL -- promoted

| Path | Purpose | Classification | Rationale | Dependency | Test coverage |
|---|---|---|---|---|---|
| `rtl/experimental/fp_div/fp32_scale_neg_pow2.sv` | Exact `a * -(2^-SHIFT)` shortcut (B1), replaces `membrane_fp_divider` for Q4's constant `mx/-8.0f` | **PROMOTE** | Exact by construction (zero-remainder division property), 2,204,128/2,204,128 differential cases vs. real divider, 0 mismatches; standalone ECP5 cells -99.8% | none (uses only `valid_delay_line`) | `rtl/tb/tb_fp32_scale_neg_pow2.cpp` (2.2M+ cases) |
| `rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv` | Exact radix-4 (2 bits/cycle) iterative FP32 divider (B4), replaces `membrane_fp_divider` for Q4's variable `1/d` | **PROMOTE** | PROMOTE_CANDIDATE decision; 4,456,685/4,456,685 3-way differential cases, 0 mismatches vs. both the real divider and B2's radix-2; -32.13% cycles/transaction vs. B2 at only +25.0% ECP5 cells | none (self-contained FSM) | `rtl/tb/tb_fp32_div_iterative_radix4_exact.cpp` (4.4M+ cases, NEEDS_CLEANUP for the 2-way production form -- see below) |
| `rtl/experimental/fp_div/q4_scale_b4.sv` | Q4_0 scale wrapper: B1's shortcut for `u_div_d`, B4's radix-4 divider for `u_div_id`, single hold register, `busy` output | **PROMOTE** (as the basis for a rewritten `rtl/q4_scale.sv`, not copied verbatim -- see `promotion-plan.md`) | Byte-for-byte identical to `q4_scale_b2.sv` except the divider instance; this IS the integration point being promoted | `fp32_scale_neg_pow2`, `fp32_div_iterative_radix4_exact` | Exercised indirectly by the full-datapath test; no dedicated standalone `q4_scale_b4` unit test exists upstream -- production gets one (adapted `rtl/tb/tb_q4_scale.sv`) |
| `rtl/experimental/fp_div/membrane_quant_stream_top_b4.sv` | Top-level variant: B2-style `q4enc_inflight` full-serialization + direct-retire Q4 encode path, `q4_scale_b4` instantiated | **PROMOTE** (as the basis for editing `rtl/membrane_quant_stream_top.sv` in place -- same module name/ports, not a new file -- see `promotion-plan.md`) | Structurally identical to `membrane_quant_stream_top_b2.sv` except the one instance swap; this is the minimal scheduling change B4's variable-latency divider requires | `q4_scale_b4`, all other unchanged `q4_*`/`q8_*` modules | 1,110,000/1,110,000 full-datapath transactions, 0 fails/drops/duplicates (`results/b4-full-datapath.json`); production gets `rtl/tb/tb_membrane_quant_stream_top.sv` (unchanged, black-box) + a new focused Verilator test |

## RTL -- experiment-only / superseded (not promoted)

| Path | Purpose | Classification | Rationale | Dependency | Test coverage |
|---|---|---|---|---|---|
| `rtl/experimental/fp_div/fp32_div_iterative_exact.sv` | B2's radix-2 (1 bit/cycle) iterative divider | DROP_FROM_INTEGRATION | Superseded by B4's radix-4 divider (-32.13% cycles/transaction, only +25.0% area) per `decision.md`'s own recommendation; B4 does not instantiate this module (verified: B4's iteration is self-contained, not built from this module) | none | `rtl/tb/tb_fp32_div_iterative_exact.cpp` (DROP, see below) |
| `rtl/experimental/fp_div/q4_scale_b1.sv` | B1-only Q4 scale wrapper (constant shortcut only, `u_div_id` still the general divider) | DROP_FROM_INTEGRATION | Intermediate research checkpoint; B4's integration (`q4_scale_b4.sv`) supersedes it by also fixing `u_div_id` -- promoting B1 alone was never the target architecture per the task's own scope | `fp32_scale_neg_pow2`, `membrane_fp_divider` | none dedicated (exercised via `membrane_quant_stream_top_b1`'s full-datapath run only) |
| `rtl/experimental/fp_div/q4_scale_b2.sv` | B2 Q4 scale wrapper (radix-2 divider) | DROP_FROM_INTEGRATION | Superseded by `q4_scale_b4.sv` (same file, radix-4 divider, better cycles/area trade) | `fp32_scale_neg_pow2`, `fp32_div_iterative_exact` | none dedicated |
| `rtl/experimental/fp_div/membrane_quant_stream_top_b1.sv` | B1-only top-level variant | DROP_FROM_INTEGRATION | Intermediate checkpoint, same reasoning as `q4_scale_b1.sv` | `q4_scale_b1` | full-datapath run only (part of B1's own 520,000-txn comparison) |
| `rtl/experimental/fp_div/membrane_quant_stream_top_b2.sv` | B2 top-level variant (radix-2 divider + full-serialization scheduling) | DROP_FROM_INTEGRATION | Superseded by `_b4.sv` (structurally identical except the divider); the SCHEDULING pattern this file introduces is what's promoted, not this file itself | `q4_scale_b2` | full-datapath run only (part of B2's own 520,000-txn comparison, reused by B3/B4 as the "B2" baseline column) |
| `rtl/experimental/fp_div/membrane_quant_stream_top_b3.sv` | B3 top-level variant (decoupled issuance + completion reorder buffer) | DROP_FROM_INTEGRATION | `decision.md`: **REJECT_ARCHITECTURE** -- correct at every depth tested, but the depth that helps (4) costs more area (14,959 ECP5 cells) than the entire unit it protects (2,268 cells) for only a 4-5% throughput gain | `q4_scale_b2`, `membrane_completion_reorder` | 1,110,000-txn full-datapath run (0 fails -- correctness is not in question, area/throughput trade is) |
| `rtl/experimental/fp_div/membrane_completion_reorder.sv` | B3's bounded direct-mapped completion reorder buffer | DROP_FROM_INTEGRATION | Same REJECT_ARCHITECTURE reasoning as `membrane_quant_stream_top_b3.sv`; this is the module whose area cost drove that rejection | none | standalone synthesis only (`results/b3-synthesis.csv`), no dedicated differential/unit testbench upstream |

## RTL testbenches / C++ harnesses

| Path | Purpose | Classification | Rationale | Dependency | Test coverage |
|---|---|---|---|---|---|
| `rtl/tb/tb_fp32_scale_neg_pow2.cpp` | Component differential test: `fp32_scale_neg_pow2` vs. real `membrane_fp_divider` (`b_in=-8.0`) | **PROMOTE** | Directly reusable as-is for the promoted module (only the DUT module name changes on rebuild, source untouched) -- this is task item 7's "focused B1 differential test" | Verilator-built `membrane_fp_divider`/`fp32_scale_neg_pow2` | 2,204,128 cases, 0 mismatches |
| `rtl/tb/tb_fp32_div_iterative_radix4_exact.cpp` | 3-way component differential test: `membrane_fp_divider` vs. B2 radix-2 vs. B4 radix-4 | NEEDS_CLEANUP | Contains the exact test vectors/categories needed for task item 7's "focused B4 differential test", but its 3-way structure references B2's divider (DROP'd, not present in the clean tree) -- needs trimming to a 2-way (`membrane_fp_divider` vs. the new production radix-4 module) comparison before promotion | Verilator-built `membrane_fp_divider` + B2's `fp32_div_iterative_exact` + B4's `fp32_div_iterative_radix4_exact` | 4,456,685 cases, 0 mismatches (both pairings) |
| `rtl/tb/tb_fp32_div_iterative_exact.cpp` | Component differential test for B2's radix-2 divider alone | DROP_FROM_INTEGRATION | Tests a DROP'd module; not needed once B2's divider is not part of production | Verilator-built `membrane_fp_divider`/`fp32_div_iterative_exact` | 2,456,685 cases, 0 mismatches (B2-only concern) |
| `rtl/experimental/fp_div/tb_top_verilator_variant.cpp` | Shared, compile-time-variant-selected (`-DMEMBRANE_B1_VARIANT` etc.) full-datapath Verilator testbench used by baseline/B1/B2/B3/B4 | NEEDS_CLEANUP | This is the actual source of task item 7's "production full-datapath test" and the reset/backpressure/order tests, but its multi-variant `#ifdef` structure (5 variants, 3 of them DROP'd) is exactly the kind of "duplicate test harness" the task asks to sadeleştir (simplify) before promotion -- needs to become a single-variant test built only against the production top level | all variant top-level modules | 520,000-1,110,000 txn per variant across phases, 0 fails/drops/duplicates in every phase |
| `rtl/tb/tb_membrane_quant_stream_top.sv` | Existing production Icarus smoke test (black-box valid/ready streaming, mode-agnostic, no fixed-latency assumption) | **PROMOTE (unchanged)** | Drives/checks purely through the `in_*`/`out_*` handshake and per-id/mode ordering -- does not assume any particular internal latency scheme, so it needs zero changes to keep working against the new scheduling; confirmed by inspection (no `L_MAX`-derived timing assumption in the driver/checker tasks) | `membrane_quant_stream_top` (whichever RTL is under it) | 300/mode + 400 mixed transactions, randomized backpressure both directions |
| `rtl/tb/tb_q4_scale.sv` | Existing production Icarus unit test for `q4_scale` | NEEDS_CLEANUP | Currently drives `valid_in` back-to-back every cycle, assuming the old fixed `DIV_DELAY`-based II=1 pipe; the new `q4_scale` is a variable-latency, single-in-flight module with a `busy` output (violating this discipline trips the module's own `` `ifndef SYNTHESIS `` assertion) -- needs the driver updated to gate on `busy`, same discipline `membrane_quant_stream_top_b4.sv`'s `q4enc_inflight` already uses | `q4_scale` | 20,000 blocks, currently no busy-gating |

## Scripts

| Path | Purpose | Classification | Rationale | Dependency | Test coverage |
|---|---|---|---|---|---|
| `scripts/run-exp-fp-divider-001.sh` | Full research reproduction harness, all 4 phases (`--phase b1\|b2\|b3\|b4`), `--quick`/`--full` | KEEP_EXPERIMENT_ONLY | Exists to reproduce the FULL research record (including DROP'd B2/B3 phases) for auditability; not a production CI script -- `promotion-plan.md`/task item 7 calls for a new, separate, bounded production verification script instead of repurposing this one | all RTL/tb files above | n/a (driver script) |

## Summary counts

- **PROMOTE**: 7 files (2 new production RTL modules, 2 production RTL files edited in place, 1 differential test reused as-is, 1 existing smoke test unchanged, plus the B4 full-datapath/synthesis result set re-derived on the clean branch in `promotion-comparison.md`).
- **NEEDS_CLEANUP**: 3 files (2 testbenches need trimming from multi-variant to single-target, 1 existing unit test needs busy-gating added).
- **DROP_FROM_INTEGRATION**: 7 RTL files (B2's divider + both its wrapper/top variants, B3's reorder buffer + its wrapper/top variant, B1's intermediate wrapper/top variants) + 2 testbenches (B2's dedicated differential test, and implicitly the B1/B2/B3-variant code paths inside the shared harness).
- **KEEP_EXPERIMENT_ONLY**: the remaining 21 documentation/results/script files, unchanged, left on `experiment/fp-divider-pipeline`.

No file in this diff modifies `third_party/llama.cpp`, any `rtl/q8_*.sv` file, or any file outside `experiments/EXP-FPGA-DIV-001/`, `rtl/experimental/fp_div/`, `rtl/tb/`, and `scripts/` -- confirmed by `git diff --stat main..experiment/fp-divider-pipeline` (38 files changed, 9,804 insertions(+), 0 deletions, 0 files outside those four directories).
