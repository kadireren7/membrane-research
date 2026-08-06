# EXP-FPGA-DIV-002 -- baseline + feasibility: `q8_scale`'s divider pair

Phase A only: characterize the existing `q8_scale` divider pair as-is,
plus a **differential feasibility study** of candidate replacements
(measured, not implemented). No production RTL behavior changed, no new
divider variant written. Every number below is labeled **MEASURED**,
**SIMULATED**, **ESTIMATED**, or **UNAVAILABLE** -- this project's
existing REAL/SIMULATED/EXTRAPOLATED/ORACLE/ASSUMED disclosure
convention, adapted to hardware-synthesis terms, unchanged from
EXP-FPGA-DIV-001.

## 1. The two divider operations and their location

`rtl/q8_scale.sv` instantiates `rtl/membrane_fp_divider.sv` twice,
`u_div_d` (`d = amax/127.0`) and `u_div_id` (`id = 127.0/amax`), both
`DIV_DELAY=1`, both driven by the same `amax_f32`. Full derivation,
cross-checked against two independent sources (RTL comments + the C
reference `src/quant/quant_simd.c`), in
`results/baseline-dataflow.md` section 1.

**Are they reciprocals?** Mathematically, exactly, in infinite-precision
real arithmetic. **Not necessarily bit-exact after independent IEEE-754
rounding** -- each division is its own separately-rounded operation, and
`rtl/q8_scale.sv`'s own header comment already flags this ("id is
computed as a direct division, NOT as 1.0f/d, which can round
differently in the last bit"). Section 4 below turns that qualitative
comment into a measured mismatch rate. Full reasoning:
`results/baseline-dataflow.md` sections 2-3.

**Can one be derived from the other exactly?** Not by simple
reciprocation (measured, section 4). Not by a power-of-two-style exact
bit trick either -- 127 is not a power of two, unlike Q4_0's `mx/-8.0`
(EXP-FPGA-DIV-001 Phase B1). Full reasoning:
`results/baseline-dataflow.md` section 3.

**Must they run in parallel?** Not architecturally -- `q8_scale.sv`'s
own header comment already discloses this as a deliberate, not
load-bearing, choice; `q4_scale.sv` (production since
EXP-FPGA-DIV-001's promotion) already demonstrates a non-parallel
divider architecture is buildable in this datapath. Full reasoning:
`results/baseline-dataflow.md` section 4.

**Edge cases** (all-zero block, NaN/Inf/subnormal `amax`, downstream
asymmetric-precision use of `d` vs `id`): fully derived in
`results/baseline-dataflow.md` sections 5-7 -- summary: `amax` is never
negative, never NaN, can be `+Inf`; all-zero blocks are handled by an
explicit RTL mux, not the divider's own `x/0` special case; `id` is
used at full F32 precision downstream (`q8_quantize_pack`'s per-lane
multiply), `d` is truncated to F16 before use, so a reconstruction
candidate must be judged against the *untruncated* F32 `d` to be fairly
compared.

## 2. Baseline latency and initiation interval

**MEASURED** (source-derived, cross-checked against
`docs/phase5-synthesizable-fpga.md` and EXP-FPGA-DIV-001's own
`baseline.md` section 3, whose numbers for `membrane_fp_divider`/
`q8_scale`/the whole datapath are unchanged since nothing in this
region of the RTL has been touched since that experiment):

| Unit | Latency (cycles) | II | Basis |
|---|---|---|---|
| `membrane_fp_divider` (standalone, `DELAY=1`) | 1 | 1 | Combinational division + 1 output register stage. |
| `q8_scale` (2 dividers, parallel) | 1 | 1 | `valid_out = d_valid && id_valid`, both `DELAY=1`, same `valid_in`. |
| Q8 encode chain (`q8_maxabs_reduce -> q8_scale -> q8_quantize_pack`) | 7 (`L_MAX`) | 1 | Unchanged since EXP-FPGA-DIV-001. |
| Whole `membrane_quant_stream_top` (every mode) | 7 (uniform) | 1 | Re-confirmed by this session's clean elaboration and fresh 520,000-transaction Verilator PASS (section 3). |

## 3. Elaboration and full-datapath cosimulation (MEASURED, this session)

`hierarchy -check -top membrane_quant_stream_top` over all 15 production
`.sv` files: **0 problems**. Re-ran the existing 520,000-transaction
Verilator cosimulation (`rtl/tb/tb_top_verilator.cpp`, exact command
from `docs/reproduction.md` section 1.4) against the unmodified RTL:

```
PASS: membrane_quant_stream_top Verilator cosim, 520000 transactions, 0 fails, 32.1s
```

Full stage breakdown, all 0 fails: reset-mid-stream flush, Q8 encode
(120,000 -- this experiment's own "focused Q8 encode" test), Q8 decode
(120,000 -- "focused Q8 decode"), Q4 encode (120,000), Q4 decode
(120,000), mixed-mode interleave (40,000). Confirms the divider pair's
behavior and its integration into `q8_scale`/the top-level pipeline are
unchanged, before any feasibility analysis is trusted.

## 4. Yosys synthesis (MEASURED, this session -- see `results/baseline-synthesis.txt` and `results/synthesis.csv` for full detail)

- **Standalone `membrane_fp_divider`**: generic 10,234 cells, ECP5
  73,629 cells (LUT4=37,998, CCU2C=10,173, PFUMX=15,848, L6MUX21=9,577,
  TRELLIS_FF=33) -- exactly reproduces EXP-FPGA-DIV-001's own number, a
  third independent confirmation.
- **`q8_scale` standalone -- NEW real measurement**: generic 21,800
  cells, **ECP5 123,742 cells** (LUT4=62,940, CCU2C=20,542,
  PFUMX=25,907, L6MUX21=14,288, TRELLIS_FF=65). EXP-FPGA-DIV-001 had to
  kill this exact synthesis run mid-`autoname` and only ever recorded a
  ~75-80K-cell *extrapolation*. **This corrects that estimate: the real
  number is significantly higher** (~1.68x one standalone divider, not
  the ~75-80K guess), though still meaningfully below a naive 2x-instance
  assumption (147,258) -- yosys's technology mapper shares some cost
  between the two parallel divider instances, the same qualitative
  effect EXP-FPGA-DIV-001's own Phase B1 documented for `q4_scale`'s
  two-instance baseline. Disclosed as a real correction, not glossed
  over.
- **Full `membrane_quant_stream_top` -- UNAVAILABLE**: `synth_ecp5`
  attempted, bounded at a 25-minute wall-clock timeout given this
  session's shared-machine memory constraints, killed by `timeout(1)`
  while still inside ABC's resource-sharing SAT analysis. No cell count
  was ever produced -- genuinely unavailable, not a partial/truncated
  number. This is consistent with (and larger in scope than)
  EXP-FPGA-DIV-001's own precedent of killing similar full-scope runs.

## 5. Differential feasibility study

Six candidate directions were identified. Candidates A/B/C are measured
directly (below); D/E/F are architectural questions this Phase A
harness cannot answer with a bit-value comparison and are analyzed on
paper only, per this experiment's own explicit "no new divider variant
written" scope.

### A. Both dividers unchanged (baseline)

The current, in-production behavior. Not a candidate to evaluate --
the reference everything else is measured against.

### B. Reciprocal reconstruction (`id` from `1/d`, or `d` from `1/id`)

**Measured** via `rtl/tb/tb_q8_scale_feasibility.cpp`, which drives the
real Verilated `membrane_fp_divider` twice per case: once to compute
the production `d`/`id` (exactly as `q8_scale.sv` does), once more to
compute `1.0/d` or `1.0/id` (the reciprocal-reconstruction candidate),
using the SAME real RTL for both -- no hand-written rounding model.
2,050,239 cases (2,000,000 uniform-random 31-bit `amax` magnitude
patterns + 50,000 real Q8-runtime `amax`-distribution-sample blocks + a
239-case edge-case/boundary set: zero, subnormal min/max, normal
min/max, `+Inf`/`-Inf`, quiet/signaling NaN, powers of two, a sparse
exponent x mantissa-boundary sweep, and named amax=127/amax=254
transition points). Full raw output:
`results/feasibility-differential-full.txt`.

| Reconstruction | Exact match | Mismatch | Max ULP (ordinary cases) |
|---|---|---|---|
| `1/d` vs. production `id` | 1,536,806 / 2,050,239 (74.96%) | 513,433 (25.04%) | 1 |
| `1/id` vs. production `d` | 1,472,765 / 2,050,239 (71.83%) | 577,474 (28.17%) | 1 |

**Every ordinary (non-categorical) mismatch differs by exactly 1 ULP**
(last-bit rounding, exactly matching `q8_scale.sv`'s own documented
concern) -- the tool's own reported "max_ulp" field is much larger
(~2.1 billion) only because it also includes the `amax=0` categorical
case: reconstructing from the production `id`/`d` (which the RTL
zero-masks to exactly `0.0`) via `1.0/0.0` produces `+Inf`, not `0.0`,
a correctness bug distinct from ordinary rounding, disclosed separately
in the mismatch category breakdown (`zero_pos`/`zero_neg`, 2 cases
each) rather than folded into a misleadingly huge ULP number.

**Conclusion: candidate B is REJECTED for direct substitution.** ~1 in
4 real `amax` values would produce a bit-different `id` (or `d`) if
reconstructed via reciprocation instead of direct division -- for a
quantization scale, a wrong last bit changes the dequantized value of
every one of the 32 elements in the block by a small but real amount,
which is exactly the class of silent numerical drift this project's own
bit-exactness discipline (`docs/phase4-ggml-quant-parity.md`) exists to
catch. This measurement makes `q8_scale.sv`'s own header-comment
warning concrete and quantified rather than just qualitative.

### C. Constant-reciprocal multiply for `amax/127`

**Measured**, same tool, same case set, driving the real Verilated
`membrane_fp_multiplier` with `amax * round_to_float(1/127)` and
comparing against the production `d` (this is the correct comparison
target -- multiplying by `1/127`'s nearest-float approximation computes
a candidate for `amax/127` itself, not for `127/amax`; see the source
file's own header comment for the derivation, corrected during this
session after an initial mistaken `vs. id` comparison was caught and
fixed before these numbers were produced).

| Candidate | Exact match | Mismatch | Max ULP (ordinary cases) |
|---|---|---|---|
| `amax * (1/127)` vs. production `d` | 1,957,196 / 2,050,239 (95.46%) | 93,043 (4.54%) | 1 |

**Notably better than candidate B** (95.46% vs. ~72-75%), and every
mismatch is again exactly 1 ULP -- no categorical failures (unlike `id`,
`d` has no zero-special-case to get wrong; `amax=0` times any finite
constant is exactly `0.0`, matching production behavior with no masking
needed). Still **not bit-exact**, confirming EXP-FPGA-DIV-001's own
`baseline.md` section 8 item 2 prediction ("not automatically
bit-exact... needs its own correctness-verification pass") -- now
quantified rather than assumed. A ~4.5% silent mismatch rate is too
high to substitute directly for the general-purpose divider without
further work (e.g. a correction step, which would itself add area/
latency and needs its own verification -- exactly the "table maker's
dilemma" `membrane_fp_divider.sv`'s own header already named as the
reason this project chose exact integer division over an approximate
divider in the first place).

### D. Shared/time-multiplexed single divider

**Architectural, not evaluated by bit-value differential** (nothing to
compare -- a shared divider computing the SAME two operations
sequentially would, by construction, produce bit-identical results to
today's parallel instances; the actual open questions are area
(1 divider + scheduling logic vs. 2 dividers) and latency (2 cycles
instead of 1, `q4_scale.sv`'s own chained-not-parallel precedent already
demonstrates the mechanical pattern in production). Not implemented or
synthesized this phase -- Phase B work.

### E. Dual exact radix-4 dividers

**Architectural, not evaluated by bit-value differential** for the same
reason as D (a radix-4 divider is, by EXP-FPGA-DIV-001 Phase B4's own
4,456,685-case differential result, already proven bit-exact against
`membrane_fp_divider`, so substituting two of them for `q8_scale`'s two
`membrane_fp_divider` instances would not introduce new bit-value risk
-- the open question is purely area/latency, not correctness). Real
prior-art numbers exist to reason from without new synthesis: Phase B4's
own standalone radix-4 divider measured 1,509 ECP5 cells (vs. this
experiment's own freshly-confirmed 73,629 for the wide combinational
divider it would replace, a **-97.9%** per-instance reduction) at a real
latency cost (~15 cycles mean vs. 1). Two radix-4 instances, run in
parallel (preserving `q8_scale`'s current parallel structure) would
plausibly land near ~3,000-4,000 ECP5 cells for the pair (extrapolated
from the single-instance number plus q4_scale-integration-level glue
overhead observed in EXP-FPGA-DIV-001 Phase B4's own `results/b4-synthesis.csv`,
**ESTIMATED, not measured for this specific two-instance-parallel
configuration**) -- a plausible ~30x area reduction at the `q8_scale`
level, at a real, disclosed latency cost this phase does not quantify
for the parallel case specifically (Phase B4's own number is for one
instance chained after a power-of-two shortcut, a different scheduling
shape than two radix-4 instances run in parallel would have).

### F. One divider + algebraic transform

**No exact algebraic candidate was identified.** 127 is not a power of
two (ruling out Q4_0 Phase B1's own exact-shift trick), and no other
compile-time-exact identity relates `amax/127` and `127/amax` in
IEEE-754 arithmetic beyond the reciprocal relationship already measured
and rejected as candidate B. This is a genuine null result for this
candidate, not an oversight -- disclosed rather than silently dropped.

## 6. Biggest technical risk in this baseline

Unchanged from EXP-FPGA-DIV-001's own `baseline.md` section 7, now
carrying a real, measured `q8_scale`-level number instead of an
extrapolation: `q8_scale`'s two `membrane_fp_divider` instances'
combinational critical path (not just their now-measured 123,742-cell
ECP5 footprint) is the largest open, **UNAVAILABLE**-Fmax risk in this
datapath -- no P&R tool exists in this environment to quantify it. This
experiment adds a second, now-measured dimension to that risk: `q8_scale`
alone is **larger, on a real Yosys ECP5 measurement, than the previous
~75-80K estimate implied** -- correcting the record, not just
reconfirming it.

## 7. Phase A decision

**NEXT_DUAL_RADIX4.**

Reasoning, ranked by what this phase actually measured (not assumed):

- Candidate B (reciprocal reconstruction) is **empirically rejected**
  for direct substitution -- a ~25-28% bit-mismatch rate, confirmed
  exactly 1 ULP per ordinary mismatch plus a real zero-case correctness
  bug if not separately handled, is too high a silent-drift risk for a
  quantization scale.
- Candidate C (constant-reciprocal multiply) is **measurably better but
  still not exact** (~4.5% mismatch, also exactly 1 ULP) -- promising
  enough to keep on the list for a *future* dedicated Phase B (paired
  with a correctness-verification pass, per EXP-FPGA-DIV-001's own
  precedent for exactly this kind of approximate-but-cheap candidate),
  but not exact enough to promote directly, and this phase's own
  explicit scope forbids designing that correction pass now.
- Candidate D (shared single divider) trades area for scheduling
  complexity and adds a real latency cost (2 cycles vs. 1) without
  the large, already-proven-exact area win candidate E offers -- lower
  priority, matching EXP-FPGA-DIV-001's own `baseline.md` section 8's
  ranking of the analogous choice for Q4_0.
- Candidate E (dual exact radix-4 dividers) is the strongest candidate
  by evidence already on record in this project, not new speculation:
  EXP-FPGA-DIV-001 Phase B4 already differentially proved
  `membrane_fp_divider_radix4` bit-exact against `membrane_fp_divider`
  at 4,456,685 cases, 0 mismatches, and already measured its standalone
  ECP5 cost at 1,509 cells (a **-97.9%** per-instance reduction vs. this
  experiment's own freshly-confirmed 73,629-cell baseline). Applying two
  such instances in `q8_scale`'s existing parallel structure would
  plausibly preserve `q8_scale`'s current 1-cycle-parallel latency
  shape far more closely than candidate D's sequential-share
  architecture would, while carrying **zero new bit-exactness risk**
  (radix-4's exactness is already proven, not something this Phase B
  would need to re-verify from scratch) -- the main open question for a
  future Phase B is the REAL two-instance-parallel ECP5 cost (this
  phase only has an ESTIMATED extrapolation, section 5 candidate E) and
  the real latency this parallel configuration would produce, neither
  of which was measured this phase, per its own no-new-RTL scope.

This is a Phase-A-internal, analysis-only decision. It does **not**
authorize writing, synthesizing, or promoting any candidate RTL --
that is exactly what a future, separately-authorized Phase B would do,
matching EXP-FPGA-DIV-001's own precedent precisely (`baseline.md`
section 8's candidate list there was proposal-only; its later Phase
B1-B4 sub-phases each required their own separate authorization to
proceed).

## 8. Phase B1 follow-up (candidate E, implemented and measured)

Everything above (sections 1-7) is this phase's own original,
analysis-only record and is unchanged. Candidate E (dual exact radix-4
dividers) was later implemented and measured in a separate,
separately-authorized phase: see [phase-b1.md](./phase-b1.md) and
`results/b1-comparison.md`. This section 5's own ESTIMATED
~3,000-4,000-ECP5-cell extrapolation for the two-instance-parallel
configuration is now superseded by a real integration measurement:
**2,775 ECP5 cells** (below the extrapolated range), a **-97.76%**
reduction vs. this baseline's own freshly-confirmed 123,742-cell
`q8_scale` number, at a real, measured initiation-interval cost of 16
cycles (vs. this baseline's own II=1) and 0 d/id mismatches across
4,052,224 differential cases. Phase B1's own decision:
`PROMOTE_CANDIDATE` (experiment-branch-only).
