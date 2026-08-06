# EXP-FPGA-DIV-001 Phase B2 -- baseline / B1 / B2 area-throughput comparison

All numbers below are labeled MEASURED (real Verilator cosimulation or real
Yosys synthesis output, this session), SIMULATED (derived from a MEASURED
number by simple arithmetic, e.g. ops/s from a measured cycle count),
ESTIMATED (a stated-assumption projection, e.g. "at 100/200 MHz"), or
UNAVAILABLE (not obtainable in this environment). No number here is measured
silicon or vendor place-and-route output.

## 1. Bit-exactness

| | Baseline | B1 | B2 |
|---|---|---|---|
| Component differential cases | -- (is the reference) | 2,204,128 | **2,456,685** |
| Component mismatches | -- | 0 | **0** |
| Full-datapath transactions | 520,000 | 520,000 | 520,000 |
| Full-datapath fails | 0 | 0 | **0** |

All MEASURED. B2's differential test additionally includes a full specials
x specials cross product (576 cases) and a real Q4_0 runtime `d`-distribution
sample (50,000 cases derived from synthetic F16 blocks via
`membrane_f16_to_f32`), neither of which B1's own differential test needed
(B1's divisor was a fixed constant, not a runtime-varying value).

## 2. Divider count and area (MEASURED, Yosys 0.33, same scripts/mapping as Phase A/B1)

| | Baseline | B1 | B2 |
|---|---|---|---|
| `membrane_fp_divider` instances in `q4_scale` | 2 | 1 | **0** |
| Standalone generic cells | 10,234 | 223 | **1,223** |
| Standalone ECP5 cells | 73,629 | 126 | **1,471** |
| Standalone ECP5 FF | 33 | 33 | **180** |
| `q4_scale`-level generic cells | 21,666 | 11,658 | **2,646** |
| `q4_scale`-level ECP5 cells | 74,382 | 72,727 | **2,268** |
| `q4_scale`-level ECP5 FF | 98 | 98 | **238** |

**`q4_scale`-level ECP5 cells, vs. baseline**: B1 -2.2%, **B2 -96.9%**. B1's
own small result was explained in `phase-b1.md` by ABC's technology mapper
already sharing most of the two divider instances' cost -- B2's result is
much larger because it removes the ACTUAL shared wide-combinational-divide
cost itself (both remaining call sites are now divider-free), not merely one
of two similar instances of it.

## 3. Latency / initiation interval / throughput (MEASURED)

| | Baseline | B1 | B2 |
|---|---|---|---|
| Standalone divider latency (cycles) | 1 | 1 | 3 (special) - 29 (general) |
| Standalone divider II (cycles) | 1 | 1 | **29** |
| Standalone divider max in-flight | pipelined (II=1) | pipelined (II=1) | **1 (explicit)** |
| `q4_scale` latency (cycles) | 2 | 2 | 3 - ~32 (measured range) |
| Transactions/cycle (standalone, no backpressure) | 1.0 | 1.0 | **0.0345** |

B2's II/latency numbers come directly from
`results/b2-differential.json`'s throughput-measurement stage (2,000
back-to-back general-path transactions, 58,000 cycles, exact 29.0
cycles/transaction, no backpressure).

## 4. Full-datapath impact (MEASURED, `results/b2-full-datapath.json`)

| | Baseline | B1 | B2 |
|---|---|---|---|
| Overall cycles/transaction | 3.006 | 3.006 | **9.589** |
| Q8_ENC mean latency (cycles) | 12.009 | 12.009 | 22.842 |
| Q8_DEC mean latency (cycles) | 11.973 | 11.973 | 22.814 |
| Q4_ENC mean latency (cycles) | 11.998 | 11.998 | **473.225** |
| Q4_DEC mean latency (cycles) | 11.955 | 11.955 | 22.803 |
| Q4_ENC max latency (cycles) | 44 | 44 | 556 |

Q4_0 encode's own throughput at this test's mode mix (25% Q4 encode +
40,000 mixed-mode transactions): baseline processes a Q4_0 encode roughly
every 12 cycles when running alone; B2 fully serializes the ENTIRE datapath
around each Q4_0 encode transaction, so its effective throughput is bounded
by (encode's own ~473-cycle latency) transactions/second at the target
clock -- see section 5 for concrete ops/s at assumed clock rates. This is
also why non-Q4-encode modes slow down too (~1.9x mean latency) -- see
`phase-b2.md` section 5 for the full explanation (a queueing/scheduling
cost, not an inherent property of the divider itself).

## 5. Implementation complexity and remaining risk

| | Baseline | B1 | B2 |
|---|---|---|---|
| New files | -- | 3 | 3 (`fp32_div_iterative_exact.sv`, `q4_scale_b2.sv`, `membrane_quant_stream_top_b2.sv`) + 1 shared testbench update |
| Production files touched | -- | 0 | **0** |
| FSM / sequential control added | none | none | 3-bit FSM per divider instance, plus top-level `q4enc_inflight` serialization logic |
| Remaining `membrane_fp_divider` risk (Q4 path) | 2 instances, full risk | 1 instance (`u_div_id`) | **0 instances** |
| Remaining `membrane_fp_divider` risk (Q8 path) | 2 instances, untouched | 2 instances, untouched | 2 instances, untouched (out of this phase's scope) |
| Structural timing risk (wide combinational divide) | present at all 4 sites | present at 3 of 4 sites | **present at 2 of 4 sites (both in `q8_scale`)** |
| New architectural risk introduced | -- | none (drop-in, same latency/II) | Q4_0 encode fully serializes against every other mode -- a real, measured, disclosed throughput cost (not a correctness risk: 520,000/520,000 transactions pass) |

## 6. Model-estimate throughput at assumed clock rates (ESTIMATED -- not measured silicon)

Purely `cycles -> seconds` arithmetic at two assumed clock rates, using the
MEASURED cycle counts above. **Not** a claim about achievable Fmax on real
hardware -- see `phase-b2.md` section 7 for why that number is UNAVAILABLE
in this environment.

| Assumed clock | Q4_0 encode ops/s (B2, isolated, 473-cycle mean latency) | Q4_0 encode ops/s (baseline, isolated, 12-cycle mean latency) | Standalone divider ops/s (B2, 29-cycle II) | Standalone divider ops/s (baseline, 1-cycle II) |
|---|---|---|---|---|
| 100 MHz | ~211,000 blocks/s | ~8,333,000 blocks/s | ~3,448,000 ops/s | 100,000,000 ops/s |
| 200 MHz | ~423,000 blocks/s | ~16,667,000 blocks/s | ~6,897,000 ops/s | 200,000,000 ops/s |

These are cycles-to-seconds projections at an ASSUMED clock, not measured
silicon and not a timing-closure claim (B2's structurally smaller
combinational path may in practice tolerate a HIGHER real clock than
baseline's wide divide would -- unverified either way without vendor
place-and-route, per section 7 of `phase-b2.md`).

## 7. Bottom line

B2 achieves a dramatically larger real (Yosys-measured) area reduction than
B1 (-96.9% vs. -2.2% at the actual `q4_scale` integration point) and fully
removes the wide combinational divide from Q4_0's last remaining variable-
divisor call site, with 0 mismatches across 2.45M+ differential cases and
520,000/520,000 clean full-datapath transactions. The cost is a real,
measured, ~39x latency increase for Q4_0 encode itself plus a smaller but
real ~1.9x collateral latency increase for the three OTHER modes, from this
phase's simplest-correct full-serialization scheduling choice -- see
`phase-b2.md` section 9 for why this is decided CONTINUE, not
PROMOTE_CANDIDATE, pending a queueing/scheduling improvement that does not
require a faster divider.
