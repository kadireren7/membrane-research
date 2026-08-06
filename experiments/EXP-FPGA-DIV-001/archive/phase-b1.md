# EXP-FPGA-DIV-001 Phase B1 -- exact power-of-two scaling for Q4's `mx / -8.0f`

Branch `experiment/fp-divider-pipeline`. Builds on `baseline.md`'s
Phase A characterization and candidate direction 1 ("Power-of-two
constant-divisor shortcut for `q4_scale`'s `u_div_d`"). Scope is
exactly that one call site -- see "Rules" in the task this phase
implements: the other 3 `membrane_fp_divider` instances (`q4_scale`'s
`u_div_id`, both of `q8_scale`'s instances) are untouched, no general
pipelined/iterative divider was written, and no real FPGA timing/Fmax
claim is made anywhere in this document.

## What changed

Three new files, zero changes to any production file:

- `rtl/experimental/fp_div/fp32_scale_neg_pow2.sv` -- a new,
  `/`-operator-free module that computes `a * -(2^-SHIFT)` (SHIFT=3
  reproduces `a / -8.0f`) via exponent subtraction and a sign flip,
  not integer division.
- `rtl/experimental/fp_div/q4_scale_b1.sv` -- a copy of
  `rtl/q4_scale.sv` with only `u_div_d` swapped from
  `membrane_fp_divider` to `fp32_scale_neg_pow2`; `u_div_id` (`1/d`,
  a variable-divisor division) is byte-identical to the production
  file.
- `rtl/experimental/fp_div/membrane_quant_stream_top_b1.sv` -- a copy
  of `rtl/membrane_quant_stream_top.sv` with only the `u_q4_scale`
  instantiation swapped from `q4_scale` to `q4_scale_b1` -- diff is
  exactly 2 lines (module name, one instantiation), verified with a
  plain `diff` against the production file before any further edits
  (see this file's own header comment for the same claim, checked
  again here).

`rtl/q4_scale.sv`, `rtl/membrane_quant_stream_top.sv`, and every other
production RTL file are unmodified by this experiment -- `git diff`
against `main` touches only files under `rtl/experimental/fp_div/`,
`rtl/tb/tb_fp32_scale_neg_pow2.cpp`, `scripts/`, and `experiments/`.

## Why exact bit-for-bit match was expected (not assumed)

Re-derived from `rtl/membrane_fp_divider.sv`'s own arithmetic before
writing a single line of the new module (full derivation in that
module's own header comment, summarized here):

For divisor `b = -(2^SHIFT)` exactly, the divider's significand
division `quot64 = ((1<<25)*{1,mant_a}) / {1,mant_b}` has `mant_b = 0`,
so the divisor is exactly `2^23` and the dividend is exactly
`{1,mant_a} << 25` -- an EXACT division, remainder always 0, for every
possible `mant_a`. That means the rounding path (guard/round/sticky,
round-to-nearest-even) is never actually exercised for this specific
divisor: the surviving mantissa bits are always exactly `mant_a`,
unchanged, and only the exponent moves (by exactly `SHIFT`) with the
sign flipped. This is a mathematical property of dividing by an exact
power of two through this specific integer-division construction, not
a guess -- and it is exactly what `rtl/q4_scale.sv`'s own header
comment already said about this operation ("mathematically an exact
power-of-two division... no rounding is ever needed").

## Which edge cases defeat a bare "subtract SHIFT from the exponent" rule

A naive `exp_out = exp_a - SHIFT, sign_out = ~sign_a` is wrong in three
disclosed ways, all handled explicitly in
`fp32_scale_neg_pow2.sv` before the linear formula ever runs:

1. **NaN sign is NOT flipped.** `membrane_fp_divider.sv`'s own
   `a_is_nan` branch returns `a_in | 32'h00400000` -- i.e. `a`'s
   original sign is preserved, only the quiet bit is forced. A general
   `sign_a ^ sign_b` rule would flip it, which would NOT match the
   reference. This is the one case where the "divide by a negative
   number flips the sign" intuition is simply false for this RTL.
2. **Inf/NaN's exponent field (`8'hFF`) must be intercepted before the
   linear formula, not run through it.** `0xFF - 3` is a bogus finite
   exponent (`0xFC`) -- letting the formula run on an Inf/NaN operand
   would silently produce a wrong finite-looking result instead of the
   correct Inf/NaN.
3. **Underflow must flush to a signed zero, not go negative or wrap.**
   Every FP32 result this datapath ever produces uses biased-exponent 0
   to mean "zero" -- there is no gradual-underflow/subnormal *output*
   path anywhere in `membrane_fp_divider.sv`. Any `a` whose raw
   exponent field is `<= SHIFT` (this includes every genuinely
   subnormal `a`, whose exponent field is always 0, as well as normal
   `a` too small to survive the shift) must flush to zero, sign
   flipped, matching the reference's own non-IEEE simplification
   exactly -- not "improved" to a mathematically more correct
   subnormal result, which would NOT match.

(Overflow, `exp_result >= 255`, is provably unreachable for this exact
operation for any `SHIFT >= 0` -- the largest finite `a` has exponent
field 254, so `exp_result <= 254 - SHIFT`, always under 255. Not a
"missed" edge case, disclosed as structurally impossible instead of
silently omitted.)

## Parity result

**Component-level differential test** (`rtl/tb/tb_fp32_scale_neg_pow2.cpp`,
new module vs. the real `membrane_fp_divider` RTL with `b_in` held at
the exact F32 constant -8.0, run in this experiment, full `--full`
scale):

- 4,096 exhaustive exponent/sign/curated-mantissa boundary cases
- 32 named IEEE-754 special values (+-0, smallest/largest subnormal,
  smallest/largest normal, +-Inf, quiet/signaling/max/min-payload NaN,
  exponent=3-vs-4 transition values)
- 2,200,000 uniformly random 32-bit patterns
- **total: 2,204,128 cases, 2,204,128 exact matches, 0 mismatches**

**Full-datapath parity test** (`rtl/experimental/fp_div/tb_top_verilator_variant.cpp`,
same C++ source compiled twice, once per variant -- see "Reproduction"):
520,000 transactions each (100,000+ per mode x4 + 40,000 mixed-mode,
randomized backpressure both directions, reset-mid-stream flush,
credit-accounting/ordering assertions built into the RTL itself), run
separately against baseline `membrane_quant_stream_top` and
`membrane_quant_stream_top_b1`:

| Variant | Transactions | Fails | Dropped | Duplicated | Deadlock/timeout |
|---|---|---|---|---|---|
| baseline | 520,000 | 0 | 0 | 0 | none |
| B1 | 520,000 | 0 | 0 | 0 | none |

Both variants exercise Q8 encode/decode (byte-identical RTL in both
tops -- unaffected by construction, and confirmed unaffected by this
same run, not a separate test), Q4 encode/decode, and mixed-mode
ordering identically. See `results/b1-comparison.md` for the full
per-stage breakdown.

## Cell reduction (see `results/synthesis.csv`, `results/b1-comparison.md` for full detail)

Two granularities, and they tell **different** stories -- **do not
conflate them**:

- **Standalone unit** (the divider/replacement in isolation): generic
  10,234 -> 223 cells (-97.8%); ECP5-mapped 73,629 -> 126 cells
  (-99.8%).
- **`q4_scale` (the actual integration point, both dividers/replacement
  present)**: generic 21,666 -> 11,658 cells (-46.2%); **ECP5-mapped
  74,382 -> 72,727 cells, only -2.2%.**

The ECP5-mapped integration-level number is the one that predicts real
FPGA resource usage, and it is small. Why: ABC's technology mapping was
already finding substantial cross-instance sharing between `q4_scale`'s
two (structurally identical) general-divider instances in the
baseline -- baseline `q4_scale`'s ECP5 total (74,382) is barely larger
than a SINGLE standalone divider's (73,629), not close to the ~2x a
naive per-instance extrapolation (this project's own Phase A doc
included) would predict. Removing one of the two dividers therefore
removes only the small marginal cost ABC wasn't already sharing away,
not "a whole divider's worth" of real FPGA resources. The generic
(pre-technology-mapping) number looks much better (-46.2%) precisely
because it is measured *before* that sharing optimization runs.

## Remaining three divider instances (unchanged by this phase)

- `q4_scale`'s `u_div_id` (`1/d`, variable divisor) -- still the
  general `membrane_fp_divider`, and per the finding above, still the
  dominant cost of `q4_scale` even after this change.
- `q8_scale`'s `u_div_d` (`amax/127.0`, constant but not a power of
  two) and `u_div_id` (`127/amax`, variable divisor) -- both untouched,
  `rtl/q8_scale.sv` itself unmodified.

## No real hardware

Every number in this document and in `results/synthesis.csv` is a
yosys generic or `synth_ecp5` cell count, or a Verilator/differential
simulation result. **No place-and-route tool is available in this
environment. Fmax = UNAVAILABLE. Timing closure = UNVERIFIED**, for
both the baseline and the B1 variant, on ECP5 or any other target.
Removing one divider instance out of four in the full datapath does
not, by itself, resolve the combinational-critical-path risk
`baseline.md` section 7 already disclosed -- `q4_scale`'s remaining
`u_div_id` (a genuinely variable-divisor division) is still the same
single, un-pipelined, wide combinational `membrane_fp_divider`, and
`q8_scale`'s two instances are entirely untouched. This phase does not
claim to have made timing closure more or less likely on real
hardware; it has no way to measure that.

## Decision

**CONTINUE.** Exact parity is achieved (differential: 2,204,128/2,204,128;
full datapath: 520,000/520,000 for both variants, 0 dropped/duplicated
transactions, no deadlock), synthesis is clean (0 elaboration/CHECK
problems in all 4 synthesis runs), and there IS a measurable resource
reduction -- but the metric that actually predicts real FPGA resource
usage (ECP5-mapped cells at the `q4_scale` integration point) shows
only a 2.2% reduction, not a decisive win, because yosys's technology
mapper was already sharing most of the cost between `q4_scale`'s two
divider instances. This is exact, safe, and a real (if modest) win,
worth keeping on this branch and building on -- not a large enough
resource result on its own to promote to `main`, and not a failure
(nothing here regressed, mismatched, or failed to synthesize). See
`results/b1-comparison.md` for the full numeric comparison this
decision is based on.

## Reproduction

`scripts/run-exp-fp-divider-001.sh --phase b1 --quick` (fast smoke:
small differential run, small integration check, synthesis elaboration
only) or `--phase b1 --full` (the exact numbers in this document: 2.2M+
differential cases, full 520,000-transaction datapath test per variant,
complete synthesis matrix).

## Phase B2 forward pointer

Everything above this section is unchanged, left as originally written
per this project's own disclosed-not-rewritten convention. The one
divider instance this phase explicitly left untouched --
`q4_scale`'s `u_div_id` (`id = 1/d`, a variable-divisor operation) -- is
exactly what Phase B2 targets next, with a synthesizable multi-cycle
iterative divider instead of another combinational shortcut (`1/d` has
no constant-divisor structure to exploit the way `mx/-8.0f` did here).
See `phase-b2.md` and `experiment.md`'s own "Phase B2 addendum" section
for the full result -- not repeated here.
