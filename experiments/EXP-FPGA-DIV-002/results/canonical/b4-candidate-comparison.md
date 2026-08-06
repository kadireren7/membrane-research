# Phase B4 candidate comparison (SIMULATED -- Verilator cosim, not real FPGA timing)

All cycles/transaction figures below are classified MEASURED_BY_TOOL: real Verilator
cycle counts from tb_top_verilator_q8_b4_variant.cpp, cosimulated against the golden C
reference. No real Fmax, timing closure, or power figure is implied. B3-split is this
phase's own selected architectural base (task item 6) -- "vs B3-split" columns below
are the relevant comparison, not "vs B2."

## Overall cycles/transaction (full correctness run, all modes+stages combined)

| variant | overall cycles/txn | transactions | fails |
|---|---|---|---|
| baseline (membrane_quant_stream_top (baseline)) | 5.471 | 8382500 | 0 |
| b1 (Phase B1 (full serialization)) | 11.264 | 8382500 | 0 |
| b2 (Phase B2 (scheduler-improved)) | 9.674 | 8382500 | 0 |
| b3split (Phase B3 (split queues, this phase's own baseline)) | 9.133 | 8382500 | 0 |
| r1 (Phase B4 R1 (per-class single completion slots)) | 9.823 | 8382500 | 0 |
| r2 (Phase B4 R2 (two-entry completion queue)) | 9.39 | 8382500 | 0 |
| r3 (Phase B4 R3 (direct-retire bypass)) | 8.721 | 8382500 | 0 |

## Density-sweep profiles: cycles/transaction (lower is better)

| profile | baseline | b1 | b2 | b3split | r1 | r2 | r3 |
|---|---|---|---|---|---|---|---|
| 10pct_Q8ENC_90pct_other | 7.468 | 9.653 | 6.917 | 6.484 | 8.086 | 7.249 | 6.173 |
| 20pct_Q8ENC_80pct_other | 6.897 | 11.212 | 7.518 | 6.196 | 7.794 | 6.967 | 5.901 |
| 25pct_Q8ENC_75pct_other | 6.496 | 11.861 | 7.916 | 6.264 | 7.859 | 7.049 | 5.972 |
| 40pct_Q8ENC_60pct_other | 5.451 | 13.906 | 9.871 | 6.918 | 8.495 | 7.740 | 6.596 |
| 60pct_Q8ENC_40pct_other | 3.984 | 16.368 | 13.336 | 8.007 | 9.563 | 8.961 | 7.645 |

## Adversarial patterns: cycles/transaction (lower is better)

### adversarial_HOL_pattern

| variant | cycles/txn | vs B3-split |
|---|---|---|
| baseline (membrane_quant_stream_top (baseline)) | 2.001 | +44.7% |
| b1 (Phase B1 (full serialization)) | 5.451 | -50.7% |
| b2 (Phase B2 (scheduler-improved)) | 4.285 | -18.4% |
| b3split (Phase B3 (split queues, this phase's own baseline)) | 3.618 | (reference) |
| r1 (Phase B4 R1 (per-class single completion slots)) | 6.667 | -84.3% |
| r2 (Phase B4 R2 (two-entry completion queue)) | 3.751 | -3.7% |
| r3 (Phase B4 R3 (direct-retire bypass)) | 3.468 | +4.1% |

## Pure-stream profiles: per-mode mean latency (cycles)

### 100pct_Q8_DEC (Q8_DEC)

| variant | mean latency | vs B3-split |
|---|---|---|
| baseline (membrane_quant_stream_top (baseline)) | 51.380 | -42.0% |
| b1 (Phase B1 (full serialization)) | 51.610 | -42.7% |
| b2 (Phase B2 (scheduler-improved)) | 52.754 | -45.8% |
| b3split (Phase B3 (split queues, this phase's own baseline)) | 36.175 | (reference) |
| r1 (Phase B4 R1 (per-class single completion slots)) | 35.754 | +1.2% |
| r2 (Phase B4 R2 (two-entry completion queue)) | 37.246 | -3.0% |
| r3 (Phase B4 R3 (direct-retire bypass)) | 35.260 | +2.5% |

### 100pct_Q4_ENC (Q4_ENC)

| variant | mean latency | vs B3-split |
|---|---|---|
| baseline (membrane_quant_stream_top (baseline)) | 300.023 | -79.3% |
| b1 (Phase B1 (full serialization)) | 300.022 | -79.3% |
| b2 (Phase B2 (scheduler-improved)) | 317.020 | -89.4% |
| b3split (Phase B3 (split queues, this phase's own baseline)) | 167.367 | (reference) |
| r1 (Phase B4 R1 (per-class single completion slots)) | 167.375 | -0.0% |
| r2 (Phase B4 R2 (two-entry completion queue)) | 167.369 | -0.0% |
| r3 (Phase B4 R3 (direct-retire bypass)) | 158.373 | +5.4% |

### 100pct_Q4_DEC (Q4_DEC)

| variant | mean latency | vs B3-split |
|---|---|---|
| baseline (membrane_quant_stream_top (baseline)) | 51.465 | -38.4% |
| b1 (Phase B1 (full serialization)) | 51.156 | -37.6% |
| b2 (Phase B2 (scheduler-improved)) | 50.548 | -35.9% |
| b3split (Phase B3 (split queues, this phase's own baseline)) | 37.189 | (reference) |
| r1 (Phase B4 R1 (per-class single completion slots)) | 36.991 | +0.5% |
| r2 (Phase B4 R2 (two-entry completion queue)) | 36.850 | +0.9% |
| r3 (Phase B4 R3 (direct-retire bypass)) | 35.889 | +3.5% |

## Collateral slowdown vs. baseline at 20%/25% Q8_0-encode density

(task item 9's own success-target metric; mean latency, MEASURED_BY_TOOL)

### 20pct_Q8ENC_80pct_other

| mode | baseline | b1 | b1 vs base | b2 | b2 vs base | b3split | b3split vs base | r1 | r1 vs base | r2 | r2 vs base | r3 | r3 vs base |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Q8_DEC | 116.732 | 184.963 | +58.45% | 130.826 | +12.07% | 128.081 | +9.72% | 140.199 | +20.10% | 131.066 | +12.28% | 121.776 | +4.32% |
| Q4_ENC | 128.696 | 197.608 | +53.55% | 139.749 | +8.59% | 129.941 | +0.97% | 141.042 | +9.59% | 132.889 | +3.26% | 123.365 | -4.14% |
| Q4_DEC | 116.519 | 185.095 | +58.85% | 130.792 | +12.25% | 127.985 | +9.84% | 140.188 | +20.31% | 130.981 | +12.41% | 121.777 | +4.51% |

### 25pct_Q8ENC_75pct_other

| mode | baseline | b1 | b1 vs base | b2 | b2 vs base | b3split | b3split vs base | r1 | r1 vs base | r2 | r2 vs base | r3 | r3 vs base |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Q8_DEC | 110.199 | 195.493 | +77.40% | 137.366 | +24.65% | 129.416 | +17.44% | 141.484 | +28.39% | 132.667 | +20.39% | 123.189 | +11.79% |
| Q4_ENC | 122.169 | 207.525 | +69.87% | 145.648 | +19.22% | 129.929 | +6.35% | 141.248 | +15.62% | 133.361 | +9.16% | 123.707 | +1.26% |
| Q4_DEC | 110.309 | 195.757 | +77.46% | 137.497 | +24.65% | 129.420 | +17.32% | 141.474 | +28.25% | 132.630 | +20.23% | 123.230 | +11.71% |

