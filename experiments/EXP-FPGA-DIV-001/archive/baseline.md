# EXP-FPGA-DIV-001 -- baseline: `membrane_fp_divider` architecture

Phase A only: characterize the existing divider as-is. No RTL behavior
changed, no new divider variant written. Every number below is labeled
**MEASURED**, **SIMULATED**, **ESTIMATED**, or **UNAVAILABLE** --
matching this project's REAL/SIMULATED/EXTRAPOLATED/ORACLE/ASSUMED
disclosure convention, adapted to hardware-synthesis terms.

## 1. Divider RTL location

`rtl/membrane_fp_divider.sv` -- a single module, `membrane_fp_divider`,
parameterized by `DELAY` (default 1). Bit-exact IEEE-754 binary32
divider built on a single, wide, combinational Verilog `/`/`%` on
64-bit unsigned integers (24-bit x 24-bit significand division widened
to a 65-bit dividend / ~40-bit divisor), followed by `DELAY` output
register stages via a separate `valid_delay_line.sv` instance. See that
file's own header comment for the full bit-exactness derivation (not
repeated here).

## 2. Which modules use it (MEASURED, via `grep`/hierarchy elaboration)

| Consumer | Instances | Divisor role |
|---|---|---|
| `rtl/q4_scale.sv` | `u_div_d`: `mx_f32 / -8.0` (constant divisor, exact power-of-two) | computes Q4_0 block scale `d` |
| `rtl/q4_scale.sv` | `u_div_id`: `1.0 / d_f32_raw` (variable divisor, chained after `u_div_d`) | computes Q4_0 reciprocal scale `id` |
| `rtl/q8_scale.sv` | `u_div_d`: `amax_f32 / 127.0` (constant divisor, not a power of two) | computes Q8_0 block scale `d` |
| `rtl/q8_scale.sv` | `u_div_id`: `127.0 / amax_f32` (variable divisor, parallel with `u_div_d`) | computes Q8_0 reciprocal scale `id` |

Both `q4_scale` and `q8_scale` are instantiated exactly once each inside
`rtl/membrane_quant_stream_top.sv` (`u_q4_scale` at line 348, `u_q8_scale`
at line 258), each with `DIV_DELAY(1)` -- so the full top-level design
contains **4 total `membrane_fp_divider` instances**, all `DELAY=1`.
`DIV_DELAY`/`DELAY` is hardcoded to `1` everywhere in this repository;
no other configuration has ever been instantiated or tested.

No other module in `rtl/` instantiates `membrane_fp_divider` (confirmed
by `grep -rn membrane_fp_divider rtl/ --include='*.sv'`, excluding
`rtl/tb/`).

## 3. Baseline latency and initiation interval

**MEASURED** (derived directly from RTL source: the `DELAY` parameter
value actually used everywhere, `q4_scale`/`q8_scale`'s own internal
wiring, and `docs/phase5-synthesizable-fpga.md` section 3's already-
published, source-derived pipeline accounting -- re-derived and
cross-checked against the current source this session, not re-run
through a waveform simulation to re-measure it; the whole datapath's
520,000-transaction Verilator cosim re-run in this experiment, section
5 below, exercises this timing implicitly by construction but was not
used to hand-measure per-cycle latency numerically).

| Unit | Latency (cycles, issue to retire) | Initiation interval | Basis |
|---|---|---|---|
| `membrane_fp_divider` (standalone, `DELAY=1`) | 1 | 1 | Purely combinational division + 1 output register stage (`valid_delay_line`, `DEPTH=1`); no internal recurrence, so a new operation can issue every cycle. |
| `q4_scale` (2 dividers, chained: `u_div_id` consumes `u_div_d`'s output) | 2 | 1 | `rtl/q4_scale.sv`'s own comment + `membrane_quant_stream_top.sv`'s matching 2-cycle `x_in` delay line (line 354). Chaining adds latency, not a stall -- still fully pipelined. |
| `q8_scale` (2 dividers, parallel: both depend only on `amax_f32`) | 1 | 1 | `valid_out = d_valid && id_valid`, both driven by `DELAY=1` dividers with the same `valid_in`. |
| Q8 encode chain (`q8_maxabs_reduce -> q8_scale -> q8_quantize_pack`) | 7 (= `L_MAX`) | 1 | `docs/phase5-synthesizable-fpga.md` section 3. |
| Q4 encode chain (`q4_scan -> q4_scale -> q4_pack`) | 4, padded to `L_MAX=7` | 1 | `docs/phase5-synthesizable-fpga.md` section 3. |
| Whole `membrane_quant_stream_top` (every mode) | 7 (uniform, by padding) | 1 | Same source; re-confirmed by this session's clean `hierarchy -check -top membrane_quant_stream_top` re-elaboration (section 4) and the fresh 520,000-transaction Verilator PASS (section 5) -- a latency/II regression would have shown up as cosim mismatches or throughput loss, and did not. |

Sustained throughput at II=1 is therefore 1 block/cycle (32 elements/
cycle) for the whole design, independent of the 7-cycle fixed latency,
as long as the output FIFO has credit (unchanged from
`docs/phase5-synthesizable-fpga.md`).

## 4. Elaboration (MEASURED, this session)

`hierarchy -check -top membrane_quant_stream_top` over the full RTL set
(all 15 non-testbench `.sv` files) re-run this session: **0 problems**,
same clean-elaboration result `docs/phase5-synthesizable-fpga.md`
section 5 already established. `hierarchy -check -top
membrane_fp_divider` (standalone, with only `valid_delay_line.sv` as a
dependency): also 0 problems, both as part of the generic-synth and the
`synth_ecp5` runs in `results/baseline-synthesis.txt`.

## 5. Yosys synthesis (MEASURED, this session -- see `results/baseline-synthesis.txt` for full detail)

- **Generic (technology-independent)**: 10,234 total cells (dominated
  by `$_XOR_` 1,808, `$_ANDNOT_` 3,882, `$_MUX_` 1,404, `$_OR_` 1,167).
- **ECP5-mapped**: 73,629 total cells (LUT4 37,998 / CCU2C 10,173 /
  PFUMX 15,848 / L6MUX21 9,577 / TRELLIS_FF 33 / 0 hard multipliers) --
  this exactly reproduces the number already on record in
  `docs/phase5-synthesizable-fpga.md` section 7, confirmed deterministic
  by an independent re-run in this experiment.
- `q4_scale`/`q8_scale`/the full top were **not** separately
  re-synthesized to completion this session (each contains 2 divider
  copies; a full top-level or per-scale-module `synth_ecp5` run is a
  multi-hundred-thousand-cell job that previously had to be killed
  mid-`autoname` even for a single module -- see
  `docs/phase5-synthesizable-fpga.md` section 7). The existing ~2x
  extrapolation for `q4_scale`/`q8_scale` (~75-80K LUT-class cells
  each) stands, **unchanged and not re-verified**, and a naive
  extrapolation to all 4 divider instances in the full design would put
  the divider's own share of the whole datapath's cell budget around
  250-300K LUT-class cells -- **ESTIMATED**, not measured, disclosed as
  such, not attempted as a real run in this Phase A pass.

## 6. Verilator full-pipeline cosimulation (SIMULATED, this session)

Re-ran the existing 520,000-transaction cosimulation
(`rtl/tb/tb_top_verilator.cpp`) against the unmodified RTL, following
the exact command in `docs/reproduction.md` section 1.4:

```
PASS: membrane_quant_stream_top Verilator cosim, 520000 transactions, 0 fails, 10.0s
```

Confirms the divider's behavior (and its integration into `q4_scale`/
`q8_scale`/the top-level pipeline) is unchanged and still bit-exact,
before any Phase B work is considered. Full stage breakdown: reset-
mid-stream flush, Q8 encode (120,000), Q8 decode (120,000), Q4 encode
(120,000), Q4 decode (120,000), mixed-mode interleave (40,000) -- all
0 fails, matching `docs/phase5-synthesizable-fpga.md` section 4's
original result exactly.

## 7. Biggest technical risk in this baseline

**The divider's combinational critical path, not just its cell count,
is the largest open risk** -- disclosed already in
`docs/phase5-synthesizable-fpga.md` section 7 and re-confirmed
structurally by this session's synthesis:

- `num64 / den64` is a single, un-pipelined, ~65-bit-by-~40-bit
  combinational Verilog `/` operator. `DELAY` only adds *output*
  register stages after this combinational block finishes -- it does
  not break the division itself into multiple cycles.
- **UNAVAILABLE**: no P&R tool exists in this environment, so there is
  no real Fmax/timing-closure number for this path, on ECP5 or any
  other target, in this document or anywhere else in this project.
- The disclosed, informed expectation (unchanged by this experiment,
  not newly measured) is that a combinational path this wide is very
  unlikely to close timing at any realistic FPGA clock frequency in a
  single cycle.
- Secondary, MEASURED consequence: this same combinational width is
  exactly what makes yosys's own `techmap`/`autoname` passes the
  dominant cost of synthesizing this module at all (48-61% of wall
  time in both runs this session) -- iterating on divider variants
  under this toolchain will be slow for the same structural reason,
  which matters for how Phase B experiments should be scoped.

This is the single largest piece of engineering work `docs/phase5-
synthesizable-fpga.md` already flagged as incomplete, and remains
exactly that after this baseline re-characterization -- not resolved,
not newly discovered, re-confirmed.

## 8. Candidate directions for Phase B (proposed only -- nothing here was implemented this session)

Ranked by how directly they're grounded in what's already disclosed in
this codebase's own comments, not by assumed impact:

1. **Power-of-two constant-divisor shortcut for `q4_scale`'s `u_div_d`
   (`mx_f32 / -8.0`).** `rtl/q4_scale.sv`'s own header comment already
   states this division is "mathematically an exact power-of-two
   division (no rounding is ever needed)" and is only routed through
   the general divider for uniformity. Replacing just this one instance
   with an exponent-subtract/sign-flip (no integer division hardware at
   all) is exact by construction -- essentially zero correctness risk,
   likely the cheapest, safest Phase B starting point, and removes one
   of the 4 divider instances entirely.
   **Implemented and evaluated in Phase B1** (`phase-b1.md`,
   `results/b1-comparison.md`, same branch): exact parity confirmed
   (2,204,128/2,204,128 differential cases, 520,000/520,000 full-
   datapath transactions, both 0 mismatches), but the real
   ECP5-mapped resource win at the `q4_scale` integration point turned
   out small (-2.2%) because yosys's technology mapper was already
   sharing most of the cost between `q4_scale`'s two divider instances
   in the baseline -- a real, disclosed, non-obvious finding, not the
   large win a naive component-level comparison (-99.8%) would suggest.
   Decision: CONTINUE, not yet promoted to `main`.
2. **Constant-reciprocal-multiply for `q8_scale`'s `u_div_d`
   (`amax_f32 / 127.0`).** 127 is a fixed, non-power-of-two constant
   known at compile time; a precomputed-reciprocal multiply can replace
   the general divider here, but (unlike direction 1) this is *not*
   automatically bit-exact -- it needs its own correctness-verification
   pass against the existing reference before being trusted, the same
   "table maker's dilemma" concern `membrane_fp_divider.sv`'s header
   already raised for the general approximate-divider approach.
3. **Genuinely pipelined (multi-cycle restoring, non-restoring, or SRT)
   divider for the two variable-divisor instances** (`q4_scale`'s
   `u_div_id` = `1/d`, `q8_scale`'s `u_div_id` = `127/amax`) -- the
   directions above only remove the two *constant*-divisor instances;
   these two genuinely need a variable-divisor divider. This is the
   change that would actually address section 7's critical-path/
   timing-closure risk (not just cell count), and is the largest,
   highest-verification-cost option of the three.
4. **Time-multiplex `q8_scale`'s two currently-parallel dividers into
   one shared instance** -- `rtl/q8_scale.sv`'s own header comment
   already names this as a not-attempted area/scheduling-complexity
   tradeoff. Lower priority than 1-3: it only affects `q8_scale`, and
   trades area for scheduling logic and (likely) latency, rather than
   removing structural risk.

None of these were written or synthesized this session; this list is
input to a future, separate Phase B experiment record, not a claim
about their eventual results.
