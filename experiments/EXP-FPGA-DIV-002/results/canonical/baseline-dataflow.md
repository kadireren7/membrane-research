# EXP-FPGA-DIV-002 — Q8_0 dataflow analysis

Phase A, step 2. Derived from reading current RTL source
(`rtl/q8_scale.sv`, `rtl/membrane_fp_divider.sv`,
`rtl/membrane_quant_stream_top.sv`, `rtl/q8_maxabs_reduce.sv`) and the
C reference it must match bit-exact (`src/quant/quant_simd.c`). No RTL
was changed to produce this document — pure reading + cross-checking
against two independent sources per claim, matching EXP-FPGA-DIV-001's
own methodology (`experiments/EXP-FPGA-DIV-001/experiment.md` §Method).

## 1. The two divider operations

`rtl/q8_scale.sv` instantiates `membrane_fp_divider` (`rtl/membrane_fp_divider.sv`)
twice, both `DELAY=1`, both driven by the same `amax_f32` (widened from
`amax_f16_in` via `membrane_fp_pkg::f16_to_f32_bits`):

| Instance | Operation | Constant operand | Role |
|---|---|---|---|
| `u_div_d` | `d_f32_raw = amax_f32 / 127.0` | `b_in = 32'h42FE0000` (127.0f) | Q8_0 block scale `d`, truncated to F16 for `d_f16_out` |
| `u_div_id` | `id_f32_raw = 127.0 / amax_f32` | `a_in = 32'h42FE0000` (127.0f) | Q8_0 reciprocal scale `id`, kept F32 (`id_f32_final`, zeroed when `amax==0`), fed to `q8_quantize_pack`'s per-lane multiply |

Both match the C reference exactly, confirmed by direct comparison
against `src/quant/quant_simd.c` (scalar, SSE4.1, and AVX2 paths — all
three compute the identical two-line form):

```c
d = amax / 127.0f;
id = (amax != 0.0f) ? 127.0f / amax : 0.0f;
```

(`src/quant/quant_simd.c` lines 153-154 scalar, 315-316 SSE4.1,
424-425 AVX2 — all three are byte-identical in this pair of lines,
confirmed by `grep`.) The file's own comment
(`src/quant/quant_simd.c` lines 8-10) is explicit that this matches
**ggml's own AVX2 CPU-backend path**, which "computes the reciprocal
directly from amax, NOT 1/d" — i.e. this is not this project's own
invented convention, it is inherited from the upstream reference
implementation this codebase is bit-exact-parity-tested against
(`docs/phase4-ggml-quant-parity.md`).

## 2. Are the two divisions reciprocals of each other?

**Mathematically, yes, exactly**: `d = amax/127` and `id = 127/amax`
satisfy `d * id = 1` in exact (infinite-precision) real arithmetic for
any nonzero `amax`, by definition (each is the other's algebraic
reciprocal, both derived from the same two operands).

**In IEEE-754 binary32 arithmetic, not necessarily bit-exact.** Each
division is independently rounded to the nearest representable `float`
(round-to-nearest-even, `membrane_fp_divider.sv`'s own rounding tail).
Rounding `amax/127` to the nearest float and rounding `127/amax` to the
nearest float are two *separate* rounding events on two different
exact quotients (`amax/127` and `127/amax` are reciprocals in exact
math, but the float space is not closed under reciprocation — the
correctly-rounded float nearest to `1/x` is not always the reciprocal
of the correctly-rounded float nearest to `x`). Whether they happen to
agree with `1/d == id` (bit-exact) for a *specific* `amax` is an
empirical question, not something derivable from the algebra alone —
this is exactly what §5 (`tb_q8_scale_feasibility.cpp`) measures
directly, at scale, rather than assumed.

`rtl/q8_scale.sv`'s own header comment (lines 4-8, unchanged, already
in the codebase before this experiment) already flags this precisely:

> note `id` is computed as a direct division, NOT as `1.0f/d`, which
> can round differently in the last bit (docs/phase4-ggml-quant-parity.md);
> this module replicates the direct-division form.

So the RTL's own design intent is already explicit: **`id` must NOT be
derived from `d`** (or vice versa) via a software/hardware reciprocal
reconstruction, because doing so is a *different arithmetic
operation* than what the bit-exact-parity contract requires (a direct,
independently-rounded division), and can disagree in the last bit. This
experiment's differential test (§5) turns that qualitative comment into
a quantified mismatch count/rate across a large, realistic input
sample.

## 3. Can one result be derived exactly from the other?

**Not by simple reciprocation (`1/d` or `1/id`)** — per §2, this is a
different rounding event from the direct division and is not
guaranteed to agree. Candidate B in the feasibility study (§4 of
`baseline.md`) tests this directly rather than assuming it.

**Not by a cheap bit-manipulation trick either** — unlike Q4_0's
`mx/-8.0f` (an exact power-of-two division, replaced in
EXP-FPGA-DIV-001 Phase B1 by a pure exponent-subtract/sign-flip with
*zero* rounding, because dividing by a power of two never needs
rounding), 127 is **not** a power of two, so `amax/127` is a genuinely
lossy division with no analogous exact bit-manipulation shortcut. This
is the same conclusion `baseline.md` §8 item 2 in EXP-FPGA-DIV-001
already reached ("this is *not* automatically bit-exact... needs its
own correctness-verification pass"), carried forward and empirically
tested here (candidate C).

## 4. Must the two dividers run in parallel?

**Not architecturally required** — `q8_scale.sv`'s own header comment
(lines 14-19) already discloses this was a deliberate choice, not a
hardware necessity: "a production design targeting minimum area would
likely share one divider across both operations at the cost of extra
scheduling logic, not attempted here." `q4_scale.sv` (in production
since EXP-FPGA-DIV-001's promotion) already demonstrates the
alternative pattern for a *different* pair of operations (a
power-of-two shortcut plus one iterative divider, chained not
parallel) — proof by existing precedent that a non-parallel, shared/
sequential divider architecture is buildable and integrable into this
datapath's issue/retire discipline (`q4enc_inflight` gating,
`membrane_quant_stream_top.sv`). Candidates D/E in the feasibility
study evaluate what a shared or dual-small-divider Q8 architecture
would cost, on paper only (Phase A does not implement either).

## 5. `all-zero` block behavior

When every element in a 32-element block is `0.0` (or `-0.0`), `amax`
reduces to exactly `0.0` (`q8_maxabs_reduce.sv`'s own invariant, §7
below). `q8_scale.sv` handles this with an **explicit mux**, not by
relying on the divider's own `x/0` special-case handling:

- `d = 0.0 / 127.0 = 0.0` (ordinary division, correctly rounds to
  exactly `0.0` — no special case needed, `membrane_fp_divider.sv`'s
  `b_is_zero`/`a_is_zero` paths are not even reached here since the
  *divisor* `127.0` is never zero for `u_div_d`).
- `id`: `u_div_id` computes `127.0 / 0.0`, which by IEEE-754 (and
  `membrane_fp_divider.sv`'s own `b_is_zero` branch, line 175-176)
  would produce `+Infinity` — but `q8_scale.sv` **overrides** this with
  an explicit `zero_pipe`-delayed mux (`id_f32_final = zero_pipe[...] ?
  32'h0 : id_f32_raw`, lines 50-63) that forces `id` to exactly `0.0`
  whenever `amax_is_zero`, matching the C reference's own ternary
  (`id = amax != 0.0f ? 127.0f/amax : 0.0f`) exactly rather than
  letting the divider's `+Inf` reach the output. The mux's own delay
  line is deliberately kept in lock-step with `DIV_DELAY` (comment,
  lines 50-54) so the zero-override always lands on the correct
  transaction even if `DIV_DELAY` were ever changed from its current
  hardcoded `1`.

## 6. NaN / Inf / subnormal behavior

**Not reachable in this datapath's real operation**, by construction,
independently confirmed from two sources:

- `amax` is *never* NaN and *never* negative (`q8_maxabs_reduce.sv`
  lines 1-18: the C reference's own `amax=0.0f; if (fabsf(x) > amax)
  amax = fabsf(x)` loop can never be won by a NaN comparison — IEEE-754
  comparison against NaN is always false — so `amax` starts at `0.0`
  and only ever gets reassigned to a genuine non-negative finite
  magnitude; the module's own NaN-substitutes-to-0.0 property makes
  this true in hardware too). `amax` *can* be `+Infinity` if any input
  element's magnitude is `+Inf` (a valid, comparable, non-NaN value
  that legitimately wins the max-reduction).
- `membrane_fp_divider.sv`'s own header (lines 39-42) states this
  explicitly as this datapath's documented invariant: "amax's
  invariant... guarantees it is always a genuine non-negative finite
  value or +Infinity, never NaN."
- Consequence for `u_div_d` (`amax_f32 / 127.0`): divisor is always the
  finite nonzero constant `127.0`, so this is never a `0/0` or
  `Inf/Inf` case; if `amax = +Inf`, result is `+Inf` (finite/Inf-style
  handling in `membrane_fp_divider.sv`'s `a_is_inf` branch, line
  169-170) — an F16-unrepresentable value, truncated by
  `f32_to_f16_bits`; downstream, `membrane_quant_stream_top.sv`'s own
  `q8_err_raw = f16_is_special(q8_d_f16)` (line 360) flags this as an
  error condition rather than silently producing a bad quantized
  block.
- Consequence for `u_div_id` (`127.0 / amax_f32`): if `amax = +Inf`,
  `127/Inf` correctly rounds to `+0.0` (a legitimate, non-special
  IEEE-754 result, `membrane_fp_divider.sv`'s `b_is_inf` branch, line
  171-172) — not an error case for the divider itself, though the
  overall block is still flagged via `d`'s own special-value check
  above.
- **Subnormal amax**: `membrane_fp_divider.sv`'s own header (lines
  39-48) explicitly discloses that subnormal *operand* support is
  "out of scope and not verified" for this divider in general (treats
  a subnormal's hidden bit as 1, which is incorrect for true
  subnormals) — but also states this datapath's real operands "are
  never subnormal" as an established invariant from the same
  amax-derivation reasoning above (F16's own subnormal range, when
  widened to F32 by `f16_to_f32_bits`, does not produce an F32-subnormal
  result — F16 subnormals widen to small-but-normal F32 values, since
  F16's exponent bias/range sits entirely inside F32's normal range).
  This experiment's differential testbench (§5 of `baseline.md`) still
  *exercises* subnormal `amax` inputs deliberately (smallest/largest
  subnormal, per the task's own instruction), both to stress-test the
  divider's general-purpose behavior beyond this datapath's real domain
  (the module is documented as "a legitimately reusable FP32 divider
  block" beyond just this call site) and to make sure any future
  reused-elsewhere assumption is checked, not to claim this datapath's
  own `amax` can ever actually be subnormal (it cannot, per the
  invariant above).

## 7. Downstream use of `d` and `id`

- `d_f16_out` (`d_f32_raw` narrowed to F16 via `f32_to_f16_bits`) is
  stored directly in the packed Q8_0 block header
  (`q8_quantize_pack.sv`'s `d_f16_in` port,
  `membrane_quant_stream_top.sv` line 352) — this is the value a
  consumer later reads back to *reconstruct* real magnitudes from
  quantized bytes (`q8_dequantize.sv`, the decode side).
- `id_f32_out` (`id_f32_final`, kept full F32 precision, not narrowed)
  feeds `q8_quantize_pack.sv`'s **per-lane multiply**
  (`membrane_quant_stream_top.sv` line 352, `id_f32_in`) — every one of
  the 32 elements in the block is multiplied by this single `id` value
  (via `membrane_fp_multiplier.sv`) to produce the quantized integer
  lane before rounding/clamping to `[-127, 127]`x (`quant_simd.c`'s own
  `q8_quantize_lane`-equivalent scalar clamp, lines 127-128). This is
  the operation `id`'s own extra precision (F32, not truncated to F16
  like `d`) exists to serve — losing precision here would shift every
  one of the 32 quantized values in the block, not just the one stored
  scale.
- **Asymmetric precision matters for any candidate that reconstructs
  one value from the other**: `d` is F16-truncated before use, but
  `id` is used at full F32. A candidate that computes `id` from the
  *already-F16-truncated* `d` (rather than from the untruncated
  `d_f32_raw`) would introduce an extra, avoidable rounding step not
  present in the current design — candidate B in the feasibility study
  is explicit about reconstructing from the untruncated F32 intermediate
  to give it the fairest possible chance at matching, not a strawman
  double-rounded version.
