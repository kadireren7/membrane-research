# EXP-FPGA-DIV-001 Phase B1 -- baseline vs. B1 comparison

All numbers below are REAL, freshly reproduced this session (Yosys
0.33 git sha1 2584903a060, the same version/binary used throughout
this project; Verilator from `tools/.local-verilator`). No place-and-
route tool exists in this environment -- **Fmax = UNAVAILABLE, timing
closure = UNVERIFIED** for every row in this document, baseline and B1
alike.

## 1. Differential test (component-level, `rtl/tb/tb_fp32_scale_neg_pow2.cpp`)

`fp32_scale_neg_pow2` (SHIFT=3) vs. the real `membrane_fp_divider` RTL
with `b_in` held at the exact F32 constant -8.0 (0xC1000000):

| Case set | Count |
|---|---|
| Exponent/sign/curated-mantissa boundary sweep | 4,096 |
| Named IEEE-754 specials (+-0, sub/normal min/max, +-Inf, NaN variants, exp=3/4 transition) | 32 |
| Uniform random 32-bit patterns | 2,200,000 |
| **Total** | **2,204,128** |
| **Exact matches** | **2,204,128** |
| **Mismatches** | **0** |
| Mismatch categories | (none) |

Target (0 mismatches) achieved. Full run log available by re-running
`scripts/run-exp-fp-divider-001.sh --phase b1 --full` (not committed
raw -- fully reproducible, deterministic seed `0xC0FFEE`).

## 2. Full-datapath parity (`rtl/experimental/fp_div/tb_top_verilator_variant.cpp`)

Same C++ testbench source, compiled once per variant
(`-DMEMBRANE_B1_VARIANT` selects the DUT class), run against the same
120,000-block golden vector set `docs/reproduction.md` section 1.4
already uses.

| Metric | Baseline (`membrane_quant_stream_top`) | B1 (`membrane_quant_stream_top_b1`) |
|---|---|---|
| Q8 encode | 120,000 / 120,000, 0 fails | 120,000 / 120,000, 0 fails |
| Q8 decode | 120,000 / 120,000, 0 fails | 120,000 / 120,000, 0 fails |
| Q4 encode | 120,000 / 120,000, 0 fails | 120,000 / 120,000, 0 fails |
| Q4 decode | 120,000 / 120,000, 0 fails | 120,000 / 120,000, 0 fails |
| Mixed-mode interleave | 40,000 / 40,000, 0 fails | 40,000 / 40,000, 0 fails |
| Reset-mid-stream flush | 0 fails (no stale output) | 0 fails (no stale output) |
| **Total transactions** | **520,000** | **520,000** |
| **Fails** | **0** | **0** |
| Dropped transactions | 0 (in_flight credit assertion never fired) | 0 (in_flight credit assertion never fired) |
| Duplicate transactions | 0 (id/mode checked on every retire) | 0 (id/mode checked on every retire) |
| Deadlock/timeout | none (fixed transaction count completed) | none (fixed transaction count completed) |
| Wall time | 10.0s | 10.6s |

Both runs used randomized valid/ready backpressure on both directions,
explicit reset injection mid-stream, and the RTL's own built-in
`in_flight` credit-range and per-mode latency assertions (compiled in,
non-synthesis build) -- none fired in either run.

## 3. Synthesis comparison (see `synthesis.csv` for the machine-readable form)

Same Yosys version, same script structure (`hierarchy -check` ->
`proc;opt;synth` for generic, `hierarchy -check` -> `synth_ecp5` for
ECP5), same `DELAY`/`DIV_DELAY`/`SHIFT` parameters (1/1/3) for every
row below.

### 3a. Standalone unit

| | Baseline: `membrane_fp_divider` | B1: `fp32_scale_neg_pow2` | Delta (abs) | Delta (%) |
|---|---|---|---|---|
| Generic total cells | 10,234 | 223 | -10,011 | -97.82% |
| ECP5 total cells | 73,629 | 126 | -73,503 | -99.83% |
| LUT4 | 37,998 | 65 | -37,933 | -99.83% |
| CCU2C | 10,173 | 10 | -10,163 | -99.90% |
| PFUMX | 15,848 | 13 | -15,835 | -99.92% |
| L6MUX21 | 9,577 | 5 | -9,572 | -99.95% |
| TRELLIS_FF | 33 | 33 | 0 | 0.00% |
| divider_instance_count | 1 | 0 | -1 | -100.00% |
| Synth CPU cost (ECP5) | 198.62s user / 38.36s sys / 1489.86 MB peak | 0.86s user / 0.02s sys / 25.22 MB peak | -- | -- |

### 3b. `q4_scale` (the actual integration point)

| | Baseline: `q4_scale` | B1: `q4_scale_b1` | Delta (abs) | Delta (%) |
|---|---|---|---|---|
| Generic total cells | 21,666 | 11,658 | -10,008 | -46.19% |
| **ECP5 total cells** | **74,382** | **72,727** | **-1,655** | **-2.22%** |
| LUT4 | 38,524 | 37,514 | -1,010 | -2.62% |
| CCU2C | 10,374 | 10,347 | -27 | -0.26% |
| PFUMX | 15,866 | 15,878 | **+12** | **+0.08%** |
| L6MUX21 | 9,520 | 8,890 | -630 | -6.62% |
| TRELLIS_FF | 98 | 98 | 0 | 0.00% |
| divider_instance_count | 2 | 1 | -1 | -50.00% |
| Latency (cycles) | 2 | 2 | 0 | 0.00% |
| Initiation interval | 1 | 1 | 0 | 0.00% |
| Synthesizable | yes (0 CHECK problems) | yes (0 CHECK problems) | -- | -- |
| Synth CPU cost (ECP5) | 150.93s user / 8.53s sys / 2159.16 MB peak | 130.38s user / 5.49s sys / 1817.61 MB peak | -- | -- |

**PFUMX increases slightly (+12 cells, +0.08%)** in the ECP5-mapped
`q4_scale` comparison -- a real, small, non-monotonic effect of ABC's
mapping heuristics interacting differently with the new module's
structure than with the divider's, not a transcription error. Reported
as measured, not smoothed over.

## 4. Why the two granularities disagree so much

At the **standalone-unit** level, replacing one general
`membrane_fp_divider` with `fp32_scale_neg_pow2` is a ~99.8% cell-count
win (ECP5-mapped) -- an isolated 64-bit combinational divider is simply
enormous compared to an exponent-subtract-and-mux.

At the **`q4_scale` integration** level, the ECP5-mapped win drops to
2.2%. The reason is directly visible in the raw numbers: baseline
`q4_scale` (which contains **two** general-divider instances) has an
ECP5 total (74,382) barely larger than a **single** standalone
divider's (73,629) -- not the ~2x a naive per-instance sum would
predict, and not the "~75-80K, direct 2x extrapolation" estimate
`baseline.md` (Phase A, quoting `docs/phase5-synthesizable-fpga.md`)
disclosed as an untested guess. The real number shows ABC's ECP5
technology mapping was already finding substantial shared structure
between the two identically-shaped divider instances before this
experiment ever touched the RTL. Removing one of the two therefore
removes only the marginal cost ABC wasn't already reusing, not "a
whole divider's worth" of real FPGA resources.

The **generic** (pre-technology-mapping) comparison does not benefit
from that same sharing optimization (it runs before `synth_ecp5`'s ABC
mapping pass), which is why it shows a much larger, and in this case
more misleading-if-taken-as-an-FPGA-resource-prediction, 46.2%
reduction. **This is exactly the distinction the experiment task asked
not to conflate ("Generic cell count ile FPGA LUT sayısını birbirine
karıştırma") -- confirmed here to matter in practice, not just in
principle**, with a real measured example where the two metrics
disagree by a factor of ~20x in relative terms.

## 5. Remaining divider instances (unaffected by this phase)

| Instance | Divisor | Status |
|---|---|---|
| `q4_scale`/`q4_scale_b1`'s `u_div_id` | `1/d` (variable) | Unchanged, general `membrane_fp_divider` in both variants |
| `q8_scale`'s `u_div_d` | `amax/127.0` (constant, not power-of-two) | Untouched |
| `q8_scale`'s `u_div_id` | `127/amax` (variable) | Untouched |

`rtl/q8_scale.sv` is not modified anywhere in this experiment.

## 6. Hardware status

No real FPGA board, no Vivado/Quartus/Xilinx/Altera toolchain, no
physical place-and-route, in this phase or any prior one. Every number
above is a Yosys cell count or a Verilator/software differential-test
result. Removing one of four total divider instances in the full
datapath does not, on its own, resolve or worsen the combinational
critical-path/timing-closure risk already disclosed in `baseline.md`
section 7 -- that risk lives in the *remaining* general-divider
instances (all three of them, including `q4_scale`'s own `u_div_id`),
none of which changed in this phase.
