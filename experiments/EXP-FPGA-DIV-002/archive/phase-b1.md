# Experiment record: EXP-FPGA-DIV-002 Phase B1

Filled from [EXPERIMENT_TEMPLATE.md](https://github.com/kadireren7/membrane/blob/main/EXPERIMENT_TEMPLATE.md), same convention as
this experiment's own [experiment.md](experiment.md) (Phase A). Branch:
`experiment/q8-divider-pipeline`.

## Experiment ID

`EXP-FPGA-DIV-002` Phase B1

## Hypothesis

Following Phase A's own `NEXT_DUAL_RADIX4` decision (`baseline.md` section 7):
replacing `rtl/q8_scale.sv`'s two parallel `membrane_fp_divider` instances
(wide, single-cycle combinational, 123,742 ECP5 cells measured in Phase A)
with two parallel `membrane_fp_divider_radix4` instances (the same exact,
already-production-proven iterative divider used by `rtl/q4_scale.sv`'s
`id = 1/d` path) will produce a bit-exact `q8_scale` replacement at a
substantially smaller real synthesized footprint, at the cost of a real,
previously-unmeasured latency/throughput trade-off for this specific
two-instance-parallel configuration (Phase A's own candidate-E estimate,
`baseline.md` section 5, was explicitly an *extrapolation*, not a
measurement).

## Baseline tag/commit

Tag `v0.1.0-research`, commit `8298e953b792c78aa8604c7558ef701b2b862b28`
(current stable public release, unchanged from Phase A). This phase's own
starting point is Phase A's own completed work on
`experiment/q8-divider-pipeline`, commit `c26e83467d225c2383587d95a57628902811c27` (`research: baseline Q8 divider architecture`).
`main` HEAD at experiment start: `9dbbede255dccf025cc3ecad7f17cd9f52f384a8`
(unrelated CI/CodeQL/CodeRabbit infrastructure work, no RTL changes).

## Method

1. Verified `rtl/membrane_fp_divider_radix4.sv` (the production module
   already promoted for Q4_0's `id=1/d` call site) is safe to reuse
   **unmodified** for `q8_scale`'s general operand shapes (`amax`/`127.0`
   and `127.0`/`amax` in both numerator and denominator positions): its own
   ports carry no hardcoded assumption that either operand is the constant
   `1.0` (unlike its one existing production call site, which is a
   caller-side choice), its special-case decode is copied verbatim from
   `membrane_fp_divider.sv`'s own (same NaN/Inf/zero tests, same priority
   chain, same `CANON_NAN`, same sign rule), and it was already
   differentially exercised with BOTH operands independently random
   (EXP-FPGA-DIV-001 Phase B4's own `random_general` category, part of that
   phase's 4,456,685-case, 0-mismatch run) -- not just with the numerator
   pinned to `1.0`. No fork or copy was made; the experimental wrapper
   instantiates the real production file directly.
2. Wrote `rtl/experimental/q8_div/q8_scale_dual_radix4.sv`: two parallel
   `membrane_fp_divider_radix4` instances (`u_div_d`, `u_div_id`), atomic
   common-input acceptance (`in_ready` = AND of both instances' own
   `in_ready`), result rendezvous via each instance's own `S_DONE`-holds
   behavior (`out_ready` = AND driven by `both_done && out_ready`, no extra
   holding register needed), and a bit-exact reproduction of baseline
   `q8_scale.sv`'s own all-zero/negative-zero masking quirk (see
   `results/b1-differential.json`'s `all_zero_behavior` section).
3. Wrote `rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4.sv`:
   the production top with ONLY `q8_scale` replaced by
   `q8_scale_dual_radix4`. Q8_0 encode becomes single-in-flight/
   variable-latency (same structural class as Q4_0 encode's own
   `q4_scale`), so it now fully serializes against every other mode
   (simplest-correct scheduling, per this phase's own explicit scope --
   no reorder buffer).
4. Wrote `rtl/tb/tb_q8_scale_dual_radix4.cpp`: RTL-vs-RTL differential test
   (real Verilated `q8_scale` vs real Verilated `q8_scale_dual_radix4`),
   4,052,224 cases (Phase A's own 2,050,239-case feasibility scope + this
   phase's own +2,000,000 additional random `amax`, per task item 4),
   random reset-mid-computation recovery test, random `out_ready`
   backpressure, and a dedicated back-to-back throughput/II measurement.
5. Wrote `rtl/experimental/q8_div/tb_top_verilator_q8_variant.cpp`: one C++
   source, compiled twice (`-DMEMBRANE_Q8DUAL_VARIANT`) against production
   `membrane_quant_stream_top` and the experimental
   `membrane_quant_stream_top_q8_dual_radix4`, 1,310,000 transactions per
   variant (>=250,000 each of Q8_0 encode/decode, Q4_0 encode/decode, plus
   mixed-mode interleave and three dense adversarial patterns: Q8_0 encode
   burst, alternating Q8_0/Q4_0 encode, dense random-mode mixed), randomized
   input gaps and `out_ready` backpressure, explicit reset-mid-stream and
   reset-while-Q8-divider-busy stages, deadlock/timeout watchdog
   (200,000-cycle bound).
6. Extended `scripts/run-exp-q8-divider-002.sh` with a `--phase b1` mode
   (`--quick`/`--full`/`--resume`/`--output-dir`, matching the existing
   `--phase a` conventions exactly) that builds and runs all of the above,
   plus a 5-way synthesis matrix (A. baseline `membrane_fp_divider`
   standalone, B. `membrane_fp_divider_radix4` standalone, C. baseline
   `q8_scale`, D. `q8_scale_dual_radix4`, E. best-effort full experimental
   top), plus the existing local CI-equivalent verification suite
   (`--full` only).
7. Ran `scripts/run-exp-q8-divider-002.sh --phase b1 --quick` first (smoke:
   44,224 differential cases, 2,500 full-datapath transactions/variant,
   elaboration-only synthesis smoke) to validate the whole pipeline before
   committing to the full-scale run.
8. Ran `scripts/run-exp-q8-divider-002.sh --phase b1 --full` for this
   phase's real, reported numbers (results below).

## Environment

Same project dev machine as Phase A: 5.6 GiB RAM, shared with other
concurrent local sessions (browser/editor) during this run -- disclosed
because available RAM dropped as low as ~160 MiB during the synthesis
matrix stage (no OOM kill observed, confirmed via `journalctl`). Same
toolchain as every prior phase: `tools/.local-yosys` (Yosys 0.33, git sha1
2584903a060), `tools/.local-verilator` (locally-extracted Verilator). No
place-and-route tool, no Xilinx/Altera toolchain, no physical FPGA board.

## Model/dataset

Not applicable in the LLM-checkpoint sense. Differential test case
composition: 223 edge/boundary cases + 2,000,000 + 2,000,000 uniform-random
`amax` (F16) + 50,000 real Q8-runtime-`amax`-distribution-sample blocks (same
synthetic-but-structurally-representative technique as Phase A's own
feasibility tool) + 1 reset-recovery case + 2,000 back-to-back throughput
cases = 4,052,224 total. Full-datapath golden vectors: existing
deterministic `rtl/tb/gen_top_x_vectors.c`-family generators (250,000
blocks/format/direction for `--full`), same external ordering contract as
`rtl/tb/tb_top_verilator.cpp`.

## Metrics

Same categories as Phase A (`experiment.md`'s own Metrics section) plus:
d/id bit-exact mismatch counts (dual-radix4 vs baseline), accepted/retired/
drop/duplicate/order-error counts, latency (min/mean/max cycles, with and
without backpressure) and measured initiation interval, per-mode
full-datapath latency and collateral slowdown, and the 5-way synthesis
matrix (generic + ECP5 cell counts, cell-type breakdown, divider instance
count, synthesizability).

## Success criteria (Phase B1)

- `q8_scale_dual_radix4` is bit-exact with baseline `q8_scale` (`d` and
  `id`) across >=4,000,000 differential cases, 0 mismatches.
- The experimental full top-level passes its own 1,310,000-transaction
  Verilator cosimulation, 0 fails, with no drop/duplicate/order error and
  no internal assertion firing.
- Real (not estimated) Yosys ECP5 synthesis numbers exist for both new
  variants (B: standalone radix-4 divider, D: `q8_scale_dual_radix4`).
- All-zero/negative-zero behavior is bit-exact with the current production
  quirk (not "fixed" and not silently different).
- Local verification (Debug/Release/ASan+UBSan/TSan ctest, all `verify-*.py`
  scripts, ggml quant parity) still passes unchanged.

## Failure criteria (Phase B1)

- Any d/id mismatch, drop, duplicate, or ordering error.
- Any Verilator assertion firing (rendezvous/atomic-acceptance/mutual-
  exclusion invariants).
- Yosys elaboration or ECP5 synthesis reporting a hard error (as opposed to
  a disclosed, best-effort UNAVAILABLE timeout for the full top-level).
- Any existing test or verification script regressing.
- Presenting the best-effort full-top synthesis timeout as a "failure"
  rather than the same disclosed UNAVAILABLE-on-timeout precedent Phase A
  already established for the baseline top.

## Resource budget

Actual: full differential test 11.6s wall time (4,052,224 cases); full
full-datapath cosimulation 34.9s (baseline) + 60.8s (experimental), 1.31M
transactions each; synthesis matrix (A/B/C/D real, E best-effort) plus the
local verification suite: complete run finished in well under an hour of
wall time (script start 22:19, finished before the next scheduled check),
smaller than Phase A's own "few hours" budget despite doing strictly more
(this phase's own differential+full-datapath stages are both
GPU/CPU-bound Verilator simulation, which turned out to be fast; the
dominant real cost was the E best-effort top-level synth_ecp5 bound,
1500s/25min, which did time out as expected).

## Checkpoints

`scripts/run-exp-q8-divider-002.sh --phase b1 --resume` skips any
already-built Verilator object dir or already-produced artifact it finds in
`--output-dir`, same convention as `--phase a`.

## Results

See `results/b1-differential.json`, `results/b1-full-datapath.json`,
`results/b1-synthesis.csv`, and `results/b1-comparison.md` for full detail.
Headline numbers (all **MEASURED** this session unless noted):

- **Divider reuse**: `membrane_fp_divider_radix4` reused unmodified, no
  fork -- confirmed general-purpose (item 1 of the governing task),
  confirmed by this phase's own 4M+-case differential (not just by
  inspection).
- **Differential (baseline `q8_scale` vs `q8_scale_dual_radix4`)**:
  4,052,224 cases, **0 mismatches (d)**, **0 mismatches (id)**, 0
  reset-recovery fails, 0 drops/duplicates/order errors. All-zero and
  negative-zero edge cases included and bit-exact (including baseline's own
  pre-existing negative-zero `id`=-Infinity quirk, faithfully reproduced,
  not fixed).
- **Latency**: min=2, mean=14.888, max=34 cycles (with random `out_ready`
  backpressure across all 4M+ cases); no-backpressure min=2, mean=14.599,
  max=15 (n=2,875,557). Roughly matches (does not exceed) a single
  radix-4 divider's own structural latency, confirming (by measurement, not
  assumption) that the two parallel instances complete together for
  `q8_scale`'s own operand shape, as this phase's own RTL header comment
  argues structurally.
- **Initiation interval**: **16 cycles** (measured, 2,000 back-to-back
  general-path cases, no backpressure) -- vs. baseline `q8_scale`'s own
  II=1. **Max in-flight: 1** (single-in-flight by construction, verified
  live via Verilator `--assert` across the full differential run, never
  fired).
- **Full-datapath cosimulation**: baseline 1,310,000/1,310,000 transactions,
  0 fails (34.9s); experimental dual-radix4 1,310,000/1,310,000
  transactions, 0 fails (60.8s). Overall cycles/transaction: baseline
  6.732, experimental 12.151 (**+80.5%**).
- **Collateral slowdown** (task item 7): Q8_0 encode's own mean latency
  goes from 38.5/54.5 to 333.5 cycles (expected, direct effect); Q8_0
  decode and Q4_0 decode -- which never touch the new divider -- still see
  **+48.3%** and **+47.5%** mean-latency increases respectively, and Q4_0
  encode (already single-in-flight/serialized in production) sees **+8.2%**,
  entirely because the full-serialization scheduling (this phase's own
  "simplest correct" choice, no reorder buffer) blocks ALL other issuance
  for the whole, now much longer, Q8_0-encode in-flight window. Real,
  measured, disclosed -- not estimated.
- **Synthesis (ECP5)**: standalone `membrane_fp_divider_radix4` (B) =
  **1,509 cells** (matches EXP-FPGA-DIV-001 Phase B4 exactly, 2nd
  reproduction); `q8_scale_dual_radix4` (D) = **2,775 cells**, a
  **-97.76%** reduction vs. baseline `q8_scale` (C) = 123,742 cells, and
  **below** both the naive 2x-single-instance estimate (3,018) and Phase
  A's own ESTIMATED ~3,000-4,000-cell extrapolation. Full experimental
  top-level (E): **UNAVAILABLE** (best-effort `synth_ecp5` timed out at
  1500s inside ABC's resource-sharing SAT analysis, same disclosed
  precedent as Phase A's own baseline full-top attempt -- hierarchy check
  and Verilator elaboration both pass cleanly, this is a tooling
  time/memory bound, not a synthesizability failure).
- **Performance estimates** (ESTIMATED clock frequencies, MEASURED cycle
  counts, no real Fmax/timing-closure anywhere): baseline `q8_scale`
  @100MHz/200MHz = 100M/200M ops/s (II=1); `q8_scale_dual_radix4`
  @100MHz/200MHz = 6.25M/12.5M ops/s (II=16, measured). Area-throughput
  proxy @100MHz: baseline ~808 ops/s per ECP5 cell; dual-radix4 ~2,252
  ops/s per ECP5 cell -- **dual-radix4 is ~2.79x more area-efficient**
  despite 16x lower raw per-instance throughput, because its footprint
  shrank ~44.6x.
- **Local verification**: Debug 28/28, Release 28/28, ASan+UBSan 30/30,
  TSan 30/30, ggml quant parity PASS, `verify-results.py` 13/13,
  `verify-paper.py` 11/11, `verify-outreach.py` 17/17 -- all unchanged.

## Limitations

- No real Fmax/timing-closure number exists for any configuration -- no P&R
  tool in this environment, unchanged from every prior phase. The
  structurally correct statement is: "both single-cycle wide combinational
  Q8 divides were structurally removed; vendor timing closure remains
  unverified" -- not "faster" or "closes timing," which are not claims this
  phase makes.
- The full experimental top-level's own real ECP5 cell count remains
  UNAVAILABLE (best-effort, time/memory-bounded on this project's own
  constrained dev machine) -- the D-level (`q8_scale_dual_radix4` standalone
  vs. baseline `q8_scale` standalone) comparison is the real, apples-to-apples
  area number this phase actually has; the full-system number would also
  need to account for ABC's own cross-module resource sharing (e.g. with
  `q4_scale`'s own radix-4 divider and the various `membrane_fp_multiplier`
  instances), which this phase's D-level number does not capture.
- This phase's own scheduling choice (full serialization, no reorder
  buffer) is deliberately the simplest correct option, not the best
  possible one -- the measured collateral slowdown on Q8_0 decode/Q4_0
  paths (48/47/8 percent) is a real, disclosed cost of that choice, and a
  smarter scheduler letting those paths continue issuing around an
  in-flight Q8_0 encode is explicitly out of scope for this phase (a
  natural CONTINUE-class follow-up if pursued).
- No real FPGA hardware, board, or vendor toolchain was used anywhere in
  this experiment.

## Decision

**PROMOTE_CANDIDATE** (as an experiment-branch research result -- see
`results/b1-comparison.md` section "Decision rationale" for the full
weighing of exactness vs. the measured 16x latency/throughput cost).
Reasoning, ranked by what this phase actually measured:

- d/id exact parity: **YES**, 4,052,224/4,052,224 cases, 0 mismatches,
  including all-zero/negative-zero edge cases and the pre-existing
  production quirk, faithfully reproduced.
- Full datapath clean: **YES**, both variants 1,310,000/1,310,000
  transactions, 0 fails, no internal assertion fired.
- Both combinational dividers removed: **YES** -- `q8_scale_dual_radix4`
  contains zero `membrane_fp_divider` instances; only
  `membrane_fp_divider_radix4` (already production-proven for Q4_0).
- Large mapped-area reduction: **YES** -- -97.76% ECP5 cells at the
  `q8_scale` level (123,742 -> 2,775), a real integration measurement, not
  an extrapolation.
- Latency/throughput trade-off acceptable and documented: the II going
  from 1 to 16 cycles, and the measured 48/47/8 percent collateral
  slowdown on Q8_0 decode/Q4_0 encode/decode, are real, disclosed costs --
  this phase judges them an ACCEPTABLE, DOCUMENTED trade for a -97.76%
  area reduction (same qualitative trade-off precedent
  EXP-FPGA-DIV-001 already established and promoted for Q4_0's own
  `id=1/d` path), not something silently glossed over.
- Reproducible: **YES** -- `scripts/run-exp-q8-divider-002.sh --phase b1
  --quick|--full`, both exercised this session.

This is a Phase-B1-internal, experiment-branch-only decision. It does
**not** authorize merging any experimental file into production RTL --
that would require its own separate authorization, exactly like every
prior phase's own precedent.

## Promotion status

`not proposed` -- remains on `experiment/q8-divider-pipeline`, pushed to
the public repository per this project's open-development policy, **not
merged into `main`**, no pull request opened, per this task's own explicit
scope. Nothing here is a verified public claim of the `v0.1.0-research`
release.
