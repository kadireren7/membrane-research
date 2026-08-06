# EXP-FPGA-DIV-001 -- production integration plan

**Status: B1+B4 production integration was merged through PR #2.** The
plan below was written before that merge, describing the (then-open)
clean candidate branch `feature/q4-radix4-divider`; it is left unchanged
as the plan that was actually executed, not edited to read as
already-current.

Companion to `promotion-audit.md` (file-by-file classification). This
document says precisely what changes in production RTL if/when this is
promoted, and what does not. **This plan describes the clean candidate
branch `feature/q4-radix4-divider`; it does not itself authorize a merge
into `main`** -- see `experiment.md`'s "Promotion status" section, unchanged.

## 1. Production RTL files that change

| File | Change |
|---|---|
| `rtl/q4_scale.sv` | `u_div_d` (`d = mx/-8.0f`) switches from `membrane_fp_divider` to the new `membrane_fp_scale_neg_pow2` (B1). `u_div_id` (`id = 1/d`) switches from `membrane_fp_divider` to the new `membrane_fp_divider_radix4` (B4). Module gains a new `busy` output port. Internal structure changes from a fixed-depth `zero_pipe`/`d_f32_pipe` delay-match array to a single hold register (correct only under the single-in-flight discipline the new top level enforces -- see item 4). |
| `rtl/membrane_quant_stream_top.sv` | Q4_0 encode's issuance/retirement is pulled out of the shared fixed-latency `tag_pipe`/`L_MAX` mechanism into its own full-serialization gate (`q4enc_inflight`) + direct-retire path, because `q4_scale`'s new divider has variable (not fixed) latency. Q8_0 encode, Q8_0 decode, and Q4_0 decode chains are **byte-identical, unmoved** -- still fixed-latency, still retiring through `tag_pipe`. External module ports are unchanged (see item 6). |

No other file under `rtl/` (production or otherwise) changes. `rtl/membrane_fp_divider.sv` itself is not modified or removed -- it remains in the tree and in use (`q8_scale.sv`'s two instances, see item 8).

## 2. Experimental modules promoted to production location

| Experimental source | Production destination | Notes |
|---|---|---|
| `rtl/experimental/fp_div/fp32_scale_neg_pow2.sv` | `rtl/membrane_fp_scale_neg_pow2.sv` | Module renamed `fp32_scale_neg_pow2` -> `membrane_fp_scale_neg_pow2` (matches the `membrane_*` naming convention every other production RTL file already uses -- `membrane_fp_adder.sv`, `membrane_fp_divider.sv`, `membrane_fp_multiplier.sv`). Port list, parameters, and logic are otherwise copied verbatim; header comment trimmed of B1-phase-specific narrative, kept technically complete (the exact-division derivation, the three edge cases, why overflow is unreachable). |
| `rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv` | `rtl/membrane_fp_divider_radix4.sv` | Module renamed `fp32_div_iterative_radix4_exact` -> `membrane_fp_divider_radix4`. Port list, parameters (`MANT_ITER_WIDTH`, `GUARD_BITS`, `EARLY_OUT_SPECIAL`, `OUT_REG_DEPTH`), and FSM are copied verbatim. Header trimmed of B4-phase narrative, kept technically complete (why it's honestly "radix-4", the bit-exactness argument, the radix-4-vs-radix-2 combinational-path tradeoff disclosure). |
| `rtl/experimental/fp_div/q4_scale_b4.sv` | Folded into `rtl/q4_scale.sv` (not a separate file) | `q4_scale` is a production module already; the promoted design becomes its new body rather than a same-named-differently-located copy. Internal instance names (`u_div_d`, `u_div_id`) unchanged so downstream references/waveform tooling keep working. |
| `rtl/experimental/fp_div/membrane_quant_stream_top_b4.sv` | Folded into `rtl/membrane_quant_stream_top.sv` (not a separate file) | Same reasoning -- `membrane_quant_stream_top` is the one production top level; the B4 scheduling becomes its Q4_0-encode section in place. |

Neither `rtl/experimental/fp_div/fp32_div_iterative_exact.sv` (B2's radix-2 divider) nor `rtl/experimental/fp_div/membrane_completion_reorder.sv` (B3's reorder buffer) is promoted or moved anywhere -- both stay under `rtl/experimental/fp_div/` on `experiment/fp-divider-pipeline` only (see `promotion-audit.md`).

## 3. Naming

Production naming follows the existing `membrane_*` convention for
general-purpose reusable blocks (`membrane_fp_scale_neg_pow2`,
`membrane_fp_divider_radix4`) rather than carrying over the experimental
`fp32_*`/`_b4` names, which described the module's role in the experiment's
own phase structure, not its behavior. `rtl/q4_scale.sv` and
`rtl/membrane_quant_stream_top.sv` keep their existing production names --
they are being edited in place, not replaced.

## 4. Parameter defaults

| Parameter | Default | Source | Notes |
|---|---|---|---|
| `membrane_fp_scale_neg_pow2.SHIFT` | `3` | B1 | Reproduces `mx / -8.0f`, the only value differential-tested (2,204,128 cases) or used anywhere in this datapath. |
| `membrane_fp_scale_neg_pow2.DELAY` | `1` | unchanged from `membrane_fp_divider.DELAY` | Matches every other `DELAY`/`DIV_DELAY` value in this codebase (hardcoded `1` throughout, per `membrane_quant_stream_top.sv`'s own header disclosure). |
| `membrane_fp_divider_radix4.MANT_ITER_WIDTH` | `26` | B4 | The only value derived/verified for FP32's 24-bit significand; a simulation-only `initial` assertion enforces this is never silently changed. |
| `membrane_fp_divider_radix4.GUARD_BITS` | `2` | B4 | The only value matching `membrane_fp_divider.sv`'s own rounding tail; same enforced-by-assertion discipline. |
| `membrane_fp_divider_radix4.EARLY_OUT_SPECIAL` | `1` (on) | B4 | NaN/Inf/zero denominators skip the 13-cycle iteration; matches B2/B4's own default, differential-tested at this setting. |
| `membrane_fp_divider_radix4.OUT_REG_DEPTH` | `0` | B4 | No extra output-drain cycles; matches the integration's own `q4_scale`/top-level timing (nothing downstream needs extra registering). |
| `q4_scale.DIV_DELAY` | `1` | unchanged | Passed through to `membrane_fp_scale_neg_pow2`'s own `DELAY`; the radix-4 divider's own latency is independent of this parameter (it is not a `DELAY`-style fixed pipe). |

## 5. Compatibility

- **Bit-exactness**: both new modules are differential-tested bit-exact
  against the real `membrane_fp_divider` RTL (B1: 2,204,128/2,204,128; B4:
  4,456,685/4,456,685 against both `membrane_fp_divider` and B2's radix-2,
  which served as a second independent reference during development). The
  promoted design changes *how* `d`/`id` are computed, never *what value*
  they produce, for any input -- including every special-case/edge-case
  input the differential tests exercise (NaN payload/sign handling, +-Inf,
  +-0, subnormal-as-normal treatment, flush-to-zero underflow).
- **Q4_0 encode's own output format** (the 18-byte packed block) is
  unchanged -- `q4_pack.sv` (unmodified) still consumes the same `d_f16`/
  `id_f32` port types and produces the same bytes it always has.
- **`q4_scale`'s port list** gains one new output (`busy`) but does not
  remove or retype any existing port -- any code that only reads
  `valid_out`/`d_f16_out`/`id_f32_out` and drives `valid_in` (never
  reasserting it before `valid_out` for its own prior request, which the
  existing production top level already always did, since it never issues
  more than one thing at a time into an in-order module) needs no changes.
  Code that streams into `q4_scale` back-to-back with no gating (the OLD
  discipline, correct only when every divider is fixed-latency) breaks --
  this is why `rtl/tb/tb_q4_scale.sv` needs updating (see
  `promotion-audit.md`).

## 6. External interface

**`membrane_quant_stream_top`'s external port list is unchanged**: same
`clk`/`rst_n`/`in_valid`/`in_ready`/`in_mode`/`in_id`/`in_data`/`out_valid`/
`out_ready`/`out_mode`/`out_id`/`out_data`/`out_error`, same widths, same
parameters (`ID_WIDTH`, `IN_FIFO_DEPTH`, `OUT_FIFO_DEPTH`), same mode
encoding, same 512-bit bus layout per mode, same backpressure/no-loss
guarantee, same `out_error` semantics. Any external driver (DMA engine,
testbench, downstream tool) that only speaks this module's `in_*`/`out_*`
handshake is unaffected -- confirmed directly by `rtl/tb/tb_membrane_quant_stream_top.sv`
needing zero changes to keep passing (it drives/checks purely through that
handshake, see `promotion-audit.md`).

What DOES change, internally: **per-transaction latency for Q4_0 encode
specifically** is no longer part of the uniform `L_MAX=7` guarantee the
module's own header currently documents for "every mode" -- it becomes
variable (measured B4 general-path mean ~270 cycles at the full-datapath
level, vs. baseline's fixed 7). Ordering is still preserved (FIFO, by
construction of the full-serialization gate, exactly as
`membrane_quant_stream_top_b4.sv` proves via its own 1,110,000-transaction,
0-fail/drop/duplicate result) -- but a consumer that assumed EVERY mode
retires in exactly 7 cycles from issue (as opposed to merely "in the order
issued, eventually") would need to stop assuming that specifically for Q4_0
encode. No such assumption exists in the current external interface
contract or its own testbench.

## 7. Latency change

| Mode | Baseline (current `main`) | After promotion |
|---|---|---|
| Q8_0 encode | 7 cycles, fixed | 7 cycles, fixed (unchanged chain) |
| Q8_0 decode | 7 cycles, fixed | 7 cycles, fixed (unchanged chain) |
| Q4_0 decode | 7 cycles, fixed | 7 cycles, fixed (unchanged chain) |
| Q4_0 encode | 7 cycles, fixed (2-cycle `q4_scale` + 2-cycle `q4_pack`, padded to 7) | **Variable**, general-path measured mean ~270 cycles at the full-datapath level (B4 result, `results/b4-full-datapath.json`); early-out (NaN/Inf/zero `d`) short-circuits to a 2-cycle `membrane_fp_divider_radix4` latency plus `q4_pack`'s fixed tail |
| Overall cycles/transaction (mixed workload) | baseline measured 2.812 (`results/b4-full-datapath.json`'s own comparison table) | B4 measured 7.734 on the same adversarial workload -- **higher** than baseline's own mixed-workload figure, because baseline's uniform 7-cycle padding is cheap when the workload is Q8-heavy; the relevant comparison for deciding whether this is a net win is area (see item 5 of the audit and `promotion-comparison.md`), not this raw cycle count, since baseline's latency numbers reflect a wide combinational divider with **no real timing-closure data**, not a demonstrated-achievable clock rate |

This latency change is real and disclosed, not glossed over: promoting B4
trades a uniform, predictable per-mode latency for Q4_0 encode specifically
in exchange for a large disclosed-simulated ECP5 area reduction at that
integration point (baseline `q4_scale` ECP5 74,382 cells -> B4 2,836 cells,
-96.19%, from `results/b4-synthesis.csv`) and a large disclosed-simulated
cycles/transaction improvement relative to B2 (not relative to baseline,
whose uniform-latency design was never trying to minimize cycles/transaction
by using fewer cells). See `promotion-comparison.md` for the reproduced
numbers on the clean branch.

## 8. Handshake change

`q4_scale`'s `u_div_id` instance moves from `membrane_fp_divider`'s
fixed-`DELAY` valid-only handshake to `membrane_fp_divider_radix4`'s full
`in_valid`/`in_ready`/`out_valid`/`out_ready` handshake -- a real protocol
change at that one internal instantiation point, matching how B2/B4 always
built it, with `out_ready` tied high (nothing downstream ever backpressures
the divider) and a `` `ifndef SYNTHESIS `` assertion enforcing the caller
(`membrane_quant_stream_top`'s new `q4enc_inflight` gate) never violates
`in_ready`. `membrane_quant_stream_top`'s own EXTERNAL `in_valid`/`in_ready`/
`out_valid`/`out_ready` handshake protocol (item 6) is unchanged -- this is
an internal handshake addition, not a change to the module's own contract
with its caller.

## 9. Does the external interface change?

**No.** See item 6.

## 10. Is the Q8 path affected?

**No.** `rtl/q8_scale.sv`, `rtl/q8_maxabs_reduce.sv`, `rtl/q8_quantize_pack.sv`,
`rtl/q8_dequantize.sv` are not modified, not touched by this plan, and not
referenced by any file this plan changes. `membrane_quant_stream_top.sv`'s
Q8_0 encode and Q8_0 decode chains are explicitly preserved byte-identical
(item 1) -- confirmed by inspection of `membrane_quant_stream_top_b4.sv`
itself, whose own header states this and whose Q8 chain code is a verified
unmodified copy of the production file's. `q8_scale.sv`'s own two
`membrane_fp_divider` instances (one constant-divisor, one variable) remain
completely untouched, still the same wide combinational divider with the
same disclosed, unquantified real-Fmax risk `baseline.md` section 7 first
raised -- this plan does not reduce or change that risk (see item 11 and the
final report's "remaining Q8 divider risk" note).

## 11. Backward compatibility

- **Bit-exact backward compatible** at the datapath level: for every input
  this datapath can produce (including every special-case value), Q4_0
  encode's output bytes are identical before and after this promotion --
  this is the entire point of the differential testing, not an incidental
  property.
- **Not** backward compatible at the micro-architectural timing level for
  Q4_0 encode specifically (item 7) -- any external tooling that hardcodes
  "Q4_0 encode always retires exactly 7 cycles after issue" (as opposed to
  reading `out_valid`) would break. No such tooling exists in this
  repository today (verified: `rtl/tb/tb_membrane_quant_stream_top.sv` reads
  `out_valid`/`out_id` generically, per item 6).
- **`q4_scale`'s own port list is additive** (new `busy` output only, item
  5) -- any code driving/reading only the pre-existing ports keeps
  compiling and, for correctly-gated callers, keeps behaving identically.
- No change to `rtl/membrane_fp_divider.sv` itself, so any other consumer of
  that general-purpose module (i.e. `q8_scale.sv`) is entirely unaffected.
