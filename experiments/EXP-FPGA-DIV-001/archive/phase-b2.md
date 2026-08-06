# EXP-FPGA-DIV-001 Phase B2 -- exact iterative divider for Q4_0's `1/d`

Branch `experiment/fp-divider-pipeline`. Builds on `baseline.md`'s Phase A
characterization and `phase-b1.md`'s Phase B1 (which removed `q4_scale`'s
constant-divisor `mx/-8.0f` division from `membrane_fp_divider.sv`, replacing
it with an exact power-of-two shortcut). Phase B2 targets the ONE remaining
divider instance Phase B1 explicitly left untouched: `q4_scale`'s `u_div_id`
(`id = 1/d`), Q4_0's last genuinely **variable-divisor** division, still the
single-cycle, wide, un-pipelined combinational `membrane_fp_divider.sv` at the
start of this phase. `q8_scale`'s two divider instances remain completely
untouched, exactly as Phase B1 left them. No production RTL file was
modified. No real FPGA timing/Fmax claim is made anywhere in this document.

## 1. Reference behavior (task item 1)

Re-derived directly from `rtl/membrane_fp_divider.sv`'s own logic (its header
comment gives the full derivation; summarized here as the exact contract the
new module must reproduce bit-for-bit):

- **Supported FP32 subset**: every 32-bit pattern is accepted as input (no
  input is rejected), but the OUTPUT behavior for subnormal/underflow/NaN
  cases below is a disclosed simplification of true IEEE-754, not full
  compliance -- see below.
- **Sign handling**: `result_sign = sign_a ^ sign_b`, EXCEPT when `a` is NaN
  (its own sign is preserved unchanged, not XORed).
- **Zero handling**: `b==0, a!=0` -> signed Infinity (`result_sign` applies);
  `a==0, b!=0` -> signed zero; `a==0 && b==0` -> the fixed x86 "real
  indefinite" pattern `0xFFC00000` (sign hard-coded to 1, NOT derived from
  operand signs).
- **Normal/subnormal behavior**: the hidden bit is treated as 1 REGARDLESS of
  the exponent field -- i.e. a genuinely subnormal operand (exponent field
  `0`) is silently treated as if it were normal with an implicit leading 1.
  This is disclosed as out-of-scope/non-IEEE in `membrane_fp_divider.sv`'s
  own header, not something Phase B2 is newly introducing.
- **Underflow/overflow**: `exp_result <= 0` flushes to a signed zero
  (flush-to-zero, no gradual underflow/subnormal OUTPUT ever produced);
  `exp_result >= 255` flushes to signed Infinity.
- **NaN**: `a` NaN wins over `b` NaN (checked in that order); the winning
  NaN's original bits are OR'd with the quiet bit (`0x00400000`), sign
  preserved. `a_is_inf && b_is_inf` and `a_is_zero && b_is_zero` both produce
  the fixed indefinite pattern above, not a derived-sign NaN.
- **+Inf/-Inf**: `a` Inf (and `b` finite nonzero) -> signed Inf; `b` Inf (and
  `a` finite) -> signed zero.
- **Rounding convention**: round-to-nearest-even on the exact 26-bit
  fixed-point quotient (`quot64 = ((1<<25)*{1,mant_a}) / {1,mant_b}`), using
  a 2-bit guard/round field plus a sticky bit derived from the EXACT integer
  remainder (`rem64 != 0`) -- not an approximation.
- **Unsupported/non-IEEE corner cases**: subnormal OPERANDS (treated as
  normal, see above) and subnormal OUTPUTS (never produced, flush-to-zero
  instead) are the two disclosed non-IEEE simplifications. Everything else
  (NaN propagation, Inf arithmetic, zero handling, rounding) matches IEEE-754
  binary32 division exactly.

The new module targets THIS reference's actual bit behavior, not idealized
IEEE-754 -- exactly the same discipline `phase-b1.md` followed for its own
constant-divisor replacement.

## 2. Architecture: `rtl/experimental/fp_div/fp32_div_iterative_exact.sv`

A radix-2 restoring-division iterative divider. Full design rationale,
state-by-state behavior, and why restoring division reproduces
`membrane_fp_divider.sv`'s exact quotient/remainder (not an approximation) is
in that file's own header comment -- summarized here:

- **FSM**: `IDLE -> ITER -> ROUND -> [DRAIN] -> DONE -> IDLE`. `IDLE`
  latches operands and decodes every special case combinationally (cheap:
  equality/XOR checks, not a divide). `ITER` runs `MANT_ITER_WIDTH` (26)
  cycles of one-bit-per-cycle restoring division on the 24-bit significands.
  `ROUND` applies the SAME guard/round/sticky round-to-nearest-even tail
  structure as `membrane_fp_divider.sv` (copied verbatim, adapted to read
  from the iteratively-computed `quot_reg`/`rem_reg` instead of a
  combinational `/`/`%`). `DRAIN` is `OUT_REG_DEPTH` extra hold cycles
  (0 by default). `DONE` holds `out_valid`/`quotient` until `out_ready`.
- **Handshake**: `in_valid`/`in_ready` (input accepted only when both are
  asserted, i.e. only from `IDLE`), `out_valid`/`out_ready` (result held
  stable, never dropped, until consumed), `busy` (`state != IDLE`). Reset is
  asynchronous and unconditional -- it forces `IDLE` and safely discards any
  half-finished iteration (no external state is written mid-computation, so
  there is nothing unsafe to discard).
- **Single in-flight**: `in_ready` is only ever asserted from `IDLE` -- this
  is the deliberate "area-first, first design point" this phase's task spec
  asks for, not a pipelined (II=1) divider. Initiation interval equals
  measured latency by construction.
- **Parameters**: `MANT_ITER_WIDTH` (26, only verified value -- a property
  of FP32's 24-bit significand, not a free knob), `GUARD_BITS` (2, only
  verified value -- matches `membrane_fp_divider.sv`'s specific rounding
  tail), `EARLY_OUT_SPECIAL` (genuinely optional, default 1: skips the
  26-cycle iteration for NaN/Inf/zero operands, which never need mantissa
  division in the reference either), `OUT_REG_DEPTH` (genuinely optional,
  default 0: extra output-holding delay cycles, same convention as
  `membrane_fp_divider.sv`'s own `DELAY`).
- **Why restoring division is exact, not approximate**: the module computes
  the exact same integer quotient/remainder `membrane_fp_divider.sv`'s
  Verilog `/`/`%` operators compute, one quotient bit per cycle via
  shift-compare-subtract, instead of in one combinational step. This is a
  mathematical identity (floor division has one correct answer, computed two
  different ways), not an approximation needing a correction pass -- and it
  is checked empirically for 2.45M+ cases below, not just argued.
- Fully synthesizable: no `real`/`shortreal`/DPI anywhere in this file.
  FSM state is encoded via `localparam` (not `typedef enum`), matching this
  project's established yosys-0.33-compatibility house style (see
  `docs/phase5-synthesizable-fpga.md` and `membrane_quant_stream_top.sv`'s
  own `MODE_*` constants).

## 3. Component-level differential test (task item 4)

`rtl/tb/tb_fp32_div_iterative_exact.cpp`, real Verilator RTL-vs-RTL
cosimulation (not RTL-vs-idealized-math) against the unmodified
`rtl/membrane_fp_divider.sv`, for the exact `1.0f / d` operation
(numerator pinned at the F32 constant `0x3F800000`, matching
`rtl/q4_scale.sv`'s own `u_div_id` instantiation), plus a smaller
general-purpose (both operands random) supplementary set since this module
is deliberately kept general.

**Case breakdown** (`--full` scale, see
`results/b2-differential.json` for the raw machine-readable report):

| Category | Count |
|---|---|
| Denominator exponent/mantissa boundary sweep (all 256 exponents x 2 signs x 8 curated mantissas, numerator=1.0f) | 4,096 |
| Specials x specials cross product (both operands vary -- the only way to exercise `a_is_nan`/`a_is_inf`/`a_is_zero`, since numerator is pinned to 1.0f everywhere else) | 576 |
| Powers of two (numerator=1.0f) | 12 |
| Uniform random denominator, numerator=1.0f (the real Q4_0 operation), random `out_ready` backpressure on ~25-33% of cases | 2,200,000 |
| Uniform random BOTH operands (general-purpose correctness beyond the pinned-numerator real usage) | 200,000 |
| Q4_0 runtime `d`-distribution sample (real `mx/-8.0f` values computed from synthetic F16 blocks via the project's own `membrane_f16_to_f32`, not idealized random floats) | 50,000 |
| Reset-mid-computation recovery follow-up case | 1 |
| Back-to-back throughput measurement (general path, no backpressure) | 2,000 |
| **Total** | **2,456,685** |

**Result: 2,456,685 / 2,456,685 exact matches, 0 mismatches.** Also checked:
accepted-input count == output count == total cases (2,456,685, no
drops/duplicates), 0 reset-recovery failures, 0 timeouts/deadlocks (10,000
cycle-per-case bound, never hit).

**Latency (MEASURED)**:

| | min | mean | max |
|---|---|---|---|
| All cases (incl. randomized backpressure stalls) | 3 | 29.134 | 48 |
| No backpressure only | 3 | 28.886 (n=1,850,018) | 29 |

`min=3` is the `EARLY_OUT_SPECIAL` path (NaN/Inf/zero operands skip the
26-cycle iteration entirely); the no-backpressure general path is a constant
29 cycles (min==max==29 among general-path-only back-to-back transactions,
see below) -- backpressure-affected cases extend measured latency purely
from held-low `out_ready`, not extra computation.

**Initiation interval (MEASURED)**: 29.000 cycles/transaction, from 2,000
back-to-back general-path transactions with no backpressure (58,000 total
cycles / 2,000 transactions, exact integer ratio -- confirming the general
path's latency is deterministic, not merely averaging out to 29). Matches
the single-in-flight design point: II == latency.

**Decision on this test alone**: exact parity achieved (0/2,456,685
mismatches) -- per this experiment's own gating rule, Q4_0 integration is
authorized.

## 4. Q4_0 integration (task item 5)

New files, zero changes to any production file (same discipline as Phase
B1):

- `rtl/experimental/fp_div/q4_scale_b2.sv` -- `u_div_d` unchanged from Phase
  B1 (`fp32_scale_neg_pow2`, SHIFT=3); `u_div_id` replaced by
  `fp32_div_iterative_exact`. Because the divider's latency is no longer a
  compile-time constant, the fixed-depth `zero_pipe`/`d_f32_pipe` delay-
  matching arrays `q4_scale.sv`/`q4_scale_b1.sv` use are replaced by a single
  HOLD register for `d_f32_raw`/`d_is_zero` -- correct because only one
  transaction is ever in flight through this module (see that file's own
  header for the full reasoning, and an `` `ifndef SYNTHESIS `` assertion
  that checks the single-in-flight discipline is actually honored at
  runtime).
- `rtl/experimental/fp_div/membrane_quant_stream_top_b2.sv` -- see section 6.

Q4_0 scale's latency is no longer fixed at 2 cycles (Phase A/B1's number) --
it now inherits the iterative divider's own variable latency (3-29+ cycles,
measured above) plus `u_div_d`'s fixed 1 cycle. `q8_scale.sv` and both of its
divider instances are completely untouched -- verified both structurally (no
line of `rtl/q8_scale.sv` or `rtl/membrane_fp_divider.sv` changed, confirmed
by `git diff` against `main`) and empirically (Q8 encode/decode transactions
in the full-datapath test below are bit-exact and unaffected).

## 5. Full datapath implications (task item 6) -- the honest part

`membrane_quant_stream_top.sv`'s entire retirement scheme rests on one
invariant its own header states explicitly: *"EVERY mode takes the exact
same fixed number of cycles, L_MAX, from issue to result"* -- which is what
lets a single shared `tag_pipe` shift register guarantee in-order retirement
with no reorder buffer. Phase B2's iterative Q4_0 divider breaks that
invariant ON PURPOSE (Q4_0 encode's latency is no longer fixed, and is now
usually larger than `L_MAX=7`). `membrane_quant_stream_top_b2.sv` does not
pretend otherwise -- it re-architects the retirement path around this fact:

- Q8_0 encode, Q8_0 decode, and Q4_0 decode are **byte-identical** to the
  production file's own logic for those three chains -- same fixed
  `L_MAX=7` `tag_pipe` retirement, completely untouched. This is what makes
  "Q8 is unaffected" a structural fact checkable by diff, not just an
  empirical claim.
- Q4_0 encode is fully **serialized**: a Q4_0 encode transaction is only
  issued when `in_flight==0` (no fixed-latency-mode transaction outstanding)
  and no other Q4_0 encode transaction is already in flight
  (`q4enc_inflight`). Once issued, issuance of ANY transaction (any mode) is
  blocked until it fully retires. Its result retires directly the instant
  `q4_pack_valid` pulses (not routed through `tag_pipe`), which by
  construction can never collide with `tag_pipe`'s own `retire_fire` on the
  same cycle (nothing was allowed to issue into `tag_pipe` during the
  window a Q4_0 encode transaction is in flight) -- asserted directly in
  the RTL (`` `ifndef SYNTHESIS ``), not just argued in this document.

This is the simplest correct way to keep global in-order retirement without
building an actual reorder buffer -- at a real, measured cost:

**Full-datapath test (task item 6)**: `rtl/experimental/fp_div/tb_top_verilator_variant.cpp`
(the SAME C++ source used for Phase B1, now compiled a third way via
`-DMEMBRANE_B2_VARIANT`, plus a new dedicated "reset while Q4_0 encode
divider busy" stage and per-mode latency instrumentation added this phase --
see `results/b2-full-datapath.json` for the full machine-readable report):

| | Baseline | B1 | B2 |
|---|---|---|---|
| Transactions | 520,000 | 520,000 | 520,000 |
| Fails | 0 | 0 | 0 |
| Dropped / duplicated | 0 / 0 | 0 / 0 | 0 / 0 |
| Deadlock/timeout | none | none | none |
| Reset-mid-stream flush | pass | pass | pass |
| **Reset while Q4_0 encode divider busy (new this phase)** | pass | pass | pass |
| Overall cycles/transaction | 3.006 | 3.006 | **9.589** |
| Q8_ENC mean latency (cycles) | 12.009 | 12.009 | 22.842 |
| Q8_DEC mean latency (cycles) | 11.973 | 11.973 | 22.814 |
| Q4_ENC mean latency (cycles) | 11.998 | 11.998 | **473.225** |
| Q4_DEC mean latency (cycles) | 11.955 | 11.955 | 22.803 |

**The honest finding**: Q4_0 encode's OWN latency rising ~39x (12 -> 473
cycles) is expected and by design (a 26-cycle iterative divider plus full
serialization). What is NOT free is that Q8_ENC/Q8_DEC/Q4_DEC's mean latency
also rose (~12 -> ~22.8 cycles, ~1.9x) even though their own RTL is
byte-identical to the production file -- purely from occasionally queuing
behind an in-flight, fully-serializing Q4_0 encode transaction. This
"collateral" throughput cost on modes that were never touched is the real
architectural price of this phase's simplest-correct serialization choice,
and is the main input to this phase's CONTINUE decision (section 9) --
not the iterative divider's own latency, which is expected and disclosed.

Also checked and confirmed clean: mode switching (interleaved in the mixed
40,000-transaction stage, all 4 modes randomly selected), reset while the
divider is genuinely mid-iteration (new dedicated test, not just a
reset-during-Q8-encode test), and the `in_flight`/`q4enc_inflight`
credit-accounting assertions (including the "`tag_pipe` retire and Q4 encode
direct retire never collide" assertion) -- all pass with 0 failures across
520,000 transactions.

## 6. Synthesis matrix (task item 7)

Same Yosys 0.33 (git sha1 2584903a060), same scripts/mapping convention as
Phase A/B1. Full table in `results/synthesis.csv`; headline numbers:

| Scope | Variant | Generic cells | ECP5 cells | FF (TRELLIS_FF) |
|---|---|---|---|---|
| standalone | baseline (`membrane_fp_divider`) | 10,234 | 73,629 | 33 |
| standalone | B1 (`fp32_scale_neg_pow2`) | 223 | 126 | 33 |
| standalone | **B2 (`fp32_div_iterative_exact`)** | **1,223** | **1,471** | **180** |
| `q4_scale` integration | baseline | 21,666 | 74,382 | 98 |
| `q4_scale` integration | B1 | 11,658 | 72,727 | 98 |
| `q4_scale` integration | **B2 (`q4_scale_b2`)** | **2,646** | **2,268** | **238** |

**Deltas vs. baseline**:

- Standalone unit: ECP5 cells -98.0% (73,629 -> 1,471); generic -88.0%
  (10,234 -> 1,223); FF +445% (33 -> 180, expected -- an iterative design
  trades combinational width for pipeline/FSM registers).
- `q4_scale` integration point (the number that actually predicts real FPGA
  resource usage, per Phase B1's own finding about ABC's cross-instance
  sharing): ECP5 cells **-96.9%** (74,382 -> 2,268); generic -87.8%
  (21,666 -> 2,646); FF +142.9% (98 -> 238).

This is a dramatically larger area win than Phase B1's own -2.2%
`q4_scale`-level result. Phase B1's finding (ABC was already sharing most of
the two dividers' cost, so removing one barely moved the needle) does NOT
apply here, because Phase B2 removes the actual shared cost itself -- the
wide combinational divide -- rather than one of two similar instances of it.
0 `membrane_fp_divider` instances remain anywhere in `q4_scale_b2.sv` (both
call sites are now divider-free: one exact power-of-two shortcut, one
iterative exact divider).

**Whole top-level synthesis**: attempted for baseline/B1/B2 this session,
but the yosys run timed out (>5 minutes) under this session's 5.6 GiB RAM
ceiling combined with concurrent desktop memory pressure (disclosed in
`experiment.md`'s Environment section) -- not a synthesizability failure,
just not completed here, the same precedent Phase A/B1 already set (neither
of those phases synthesized the whole top-level either, only standalone +
`q4_scale`). Marked `UNAVAILABLE`/`not_attempted` in `results/synthesis.csv`,
not silently omitted. The whole design's real BEHAVIOR at that level (as
opposed to its synthesized cell count) IS fully exercised, by the 520,000-
transaction Verilator cosimulation in section 5.

## 7. Timing (task item 8)

No vendor place-and-route tool exists in this environment (same disclosure
as every prior phase). **Fmax = UNAVAILABLE. Timing closure = UNVERIFIED**
for baseline, B1, AND B2.

What IS structurally true, and checkable by inspection of the synthesized
netlist statistics above: the single-cycle, wide (24-bit x 24-bit -> 26-bit)
combinational `/` operator inside `membrane_fp_divider.sv` -- the specific
structure Phase A's `baseline.md` section 7 flagged as "the single biggest
disclosed risk" -- has been structurally REMOVED from BOTH of Q4_0's divider
call sites (Phase B1 removed it from the constant-divisor site, Phase B2
removes it from the variable-divisor site). What replaces it in Phase B2 is
a small, shallow combinational restoring-division step (one compare and one
conditional 25-bit subtract per cycle, registered every cycle) plus a
similarly small combinational rounding tail -- both far narrower than the
original 24x24-bit combinational divide.

The correct statement, matching this project's own established convention,
is: **the single-cycle wide combinational divide was structurally removed
from every Q4_0 divider call site; actual timing closure remains unverified
without vendor place-and-route.** Not "timing is fixed."

`q8_scale.sv`'s two divider instances (`amax/127.0` and `127/amax`, both
still the general `membrane_fp_divider.sv`) are completely untouched by this
phase -- the same structural timing risk Phase A/B1 disclosed for them still
applies, unchanged.

## 8. Area-throughput comparison

See `results/b2-comparison.md` for the full baseline/B1/B2 table (bit-exactness,
divider count, cell counts, FF growth, latency, II, transactions/cycle, Q4
block throughput, full-datapath impact, complexity, remaining risk) and the
100/200 MHz model-estimate ops/s figures (explicitly labeled ESTIMATED, not
measured silicon).

## 9. Decision

**CONTINUE.**

Meets every PROMOTE_CANDIDATE correctness/reproducibility bar: exact parity
(2,456,685/2,456,685 differential cases, 0 mismatches), full datapath clean
(520,000/520,000 transactions, 0 fails/drops/duplicates, reset-while-busy
verified, ordering/credit assertions clean), the wide combinational
variable-divisor divide structurally removed, and a genuinely large,
reproducible ECP5-mapped area win at the `q4_scale` integration point
(-96.9%, dramatically larger than Phase B1's own -2.2%).

Not promoted to `main` yet because the throughput cost is real and extends
beyond what a "Q4_0 got slower" framing alone would suggest: this phase's
simplest-correct serialization choice (block ALL issuance, any mode, while
one Q4_0 encode transaction is in flight) measurably slows down Q8_0
encode/decode and Q4_0 decode too (~1.9x mean latency), even though their
own RTL is byte-identical to production. That collateral cost is a queueing/
scheduling weakness in THIS integration, not an inherent property of having
an iterative divider -- a follow-on design that lets the three fixed-latency
chains continue issuing/retiring through their own `tag_pipe` while a Q4_0
encode transaction is separately in flight (they do not actually share any
compute resource with it) would very plausibly eliminate most of this
collateral cost without needing a faster divider at all. That is exactly the
"queueing improvement needed" case the task's own CONTINUE criterion
describes, not a correctness or synthesis failure, and not a REJECT-class
outcome (nothing mismatched, deadlocked, misordered, or failed to
synthesize).

## 10. Reproduction

`scripts/run-exp-fp-divider-001.sh --phase b2 --quick` (fast smoke: a small
differential run, small integration run, synthesis elaboration only) or
`--phase b2 --full` (the exact numbers in this document: 2.45M+ differential
cases, full 520,000-transaction datapath test for baseline/B1/B2, complete
generic+ECP5 synthesis matrix at the standalone and `q4_scale` levels).
`--resume` skips rebuilding already-built binaries; `--output-dir` redirects
build/output artifacts.
