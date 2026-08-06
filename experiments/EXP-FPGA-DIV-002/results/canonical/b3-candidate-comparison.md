# Phase B3 candidate comparison (SIMULATED -- Verilator cosim, not real FPGA timing)

All cycles/transaction figures below are classified MEASURED_BY_TOOL: real Verilator
cycle counts from tb_top_verilator_q8_b3_variant.cpp, cosimulated against the golden C
reference. No real Fmax, timing closure, or power figure is implied.

## Overall cycles/transaction (full correctness run, all modes+stages combined)

| variant | overall cycles/txn | transactions | fails |
|---|---|---|---|
| baseline (membrane_quant_stream_top (baseline)) | 5.477 | 8042500 | 0 |
| b1 (Phase B1 (full serialization)) | 11.407 | 8042500 | 0 |
| b2 (Phase B2 (scheduler-improved)) | 9.831 | 8042500 | 0 |
| b3l2 (Phase B3 (lookahead=2)) | 9.671 | 8042500 | 0 |
| b3l4 (Phase B3 (lookahead=4)) | 9.575 | 8042500 | 0 |
| b3split (Phase B3 (split queues)) | 9.302 | 8042500 | 0 |

## Density-sweep profiles: cycles/transaction (lower is better)

| profile | baseline | b1 | b2 | b3_l2 | b3_l4 | b3_split |
|---|---|---|---|---|---|---|
| 10pct_Q8ENC_90pct_other | 7.434 | 9.683 | 6.860 | 6.391 | 6.090 | 6.493 |
| 20pct_Q8ENC_80pct_other | 6.891 | 11.204 | 7.519 | 6.907 | 6.594 | 6.179 |
| 25pct_Q8ENC_75pct_other | 6.493 | 11.842 | 7.971 | 7.315 | 7.058 | 6.267 |
| 40pct_Q8ENC_60pct_other | 5.434 | 13.923 | 9.858 | 9.438 | 9.253 | 6.927 |
| 60pct_Q8ENC_40pct_other | 3.996 | 16.378 | 13.349 | 13.172 | 13.125 | 8.008 |

## Adversarial HOL pattern: cycles/transaction (lower is better)

| variant | cycles/txn | vs B2 |
|---|---|---|
| baseline (membrane_quant_stream_top (baseline)) | 1.995 | +53.4% |
| b1 (Phase B1 (full serialization)) | 5.451 | -27.2% |
| b2 (Phase B2 (scheduler-improved)) | 4.285 | (reference) |
| b3l2 (Phase B3 (lookahead=2)) | 4.118 | +3.9% |
| b3l4 (Phase B3 (lookahead=4)) | 3.785 | +11.7% |
| b3split (Phase B3 (split queues)) | 3.618 | +15.6% |

## Pure-stream profiles: per-mode mean latency (cycles)

### 100pct_Q8_DEC (Q8_DEC)

| variant | mean latency | vs B2 |
|---|---|---|
| baseline (membrane_quant_stream_top (baseline)) | 51.812 | +1.8% |
| b1 (Phase B1 (full serialization)) | 51.905 | +1.6% |
| b2 (Phase B2 (scheduler-improved)) | 52.768 | (reference) |
| b3l2 (Phase B3 (lookahead=2)) | 54.361 | -3.0% |
| b3l4 (Phase B3 (lookahead=4)) | 54.053 | -2.4% |
| b3split (Phase B3 (split queues)) | 36.809 | +30.2% |

### 100pct_Q4_ENC (Q4_ENC)

| variant | mean latency | vs B2 |
|---|---|---|
| baseline (membrane_quant_stream_top (baseline)) | 300.024 | +5.4% |
| b1 (Phase B1 (full serialization)) | 300.021 | +5.4% |
| b2 (Phase B2 (scheduler-improved)) | 317.026 | (reference) |
| b3l2 (Phase B3 (lookahead=2)) | 353.444 | -11.5% |
| b3l4 (Phase B3 (lookahead=4)) | 390.859 | -23.3% |
| b3split (Phase B3 (split queues)) | 167.367 | +47.2% |

### 100pct_Q4_DEC (Q4_DEC)

| variant | mean latency | vs B2 |
|---|---|---|
| baseline (membrane_quant_stream_top (baseline)) | 50.548 | +3.5% |
| b1 (Phase B1 (full serialization)) | 50.212 | +4.1% |
| b2 (Phase B2 (scheduler-improved)) | 52.370 | (reference) |
| b3l2 (Phase B3 (lookahead=2)) | 51.946 | +0.8% |
| b3l4 (Phase B3 (lookahead=4)) | 51.029 | +2.6% |
| b3split (Phase B3 (split queues)) | 36.744 | +29.8% |

## Collateral slowdown vs. baseline at 20%/25% Q8_0-encode density
(task item 9's own success-target metric; mean latency, MEASURED_BY_TOOL
from `results/b3-performance.csv`; positive % = slower than baseline)

### 20pct_Q8ENC_80pct_other

| mode | baseline mean | b2 | b2 vs base | b3l2 | b3l2 vs base | b3l4 | b3l4 vs base | b3split | b3split vs base |
|---|---|---|---|---|---|---|---|---|---|
| Q8_DEC | 116.464 | 130.899 | +12.39% | 139.446 | +19.73% | 159.876 | +37.28% | 127.623 | **+9.58%** |
| Q4_ENC | 128.759 | 139.879 | +8.64% | 149.095 | +15.79% | 169.593 | +31.71% | 129.524 | **+0.59%** |
| Q4_DEC | 116.377 | 130.781 | +12.38% | 139.670 | +20.02% | 159.539 | +37.09% | 127.765 | **+9.79%** |

### 25pct_Q8ENC_75pct_other

| mode | baseline mean | b2 | b2 vs base | b3l2 | b3l2 vs base | b3l4 | b3l4 vs base | b3split | b3split vs base |
|---|---|---|---|---|---|---|---|---|---|
| Q8_DEC | 110.315 | 138.262 | +25.33% | 147.423 | +33.64% | 170.075 | +54.17% | 129.420 | +17.32% |
| Q4_ENC | 122.337 | 146.612 | +19.84% | 156.107 | +27.60% | 178.817 | +46.17% | 129.978 | +6.25% |
| Q4_DEC | 110.127 | 138.113 | +25.41% | 147.614 | +34.04% | 170.201 | +54.55% | 129.559 | +17.65% |

**<=10% target (task item 9)**: MET by b3split at 20% density on all
three modes (bold above); NOT MET by b3split at 25% density on Q8_DEC/
Q4_DEC (Q4_ENC stays under at 6.25%); NOT MET by b2, b3l2, or b3l4 at
either density on any mode. b3l2/b3l4 collateral is worse than b2's own
at every density/mode combination measured here -- see phase-b3.md
section "Why lookahead makes collateral worse, not better" for the
real, measured root cause (shadow-retirement contention plus constant
per-issue lookahead/compaction overhead, not head-of-line blocking
itself, dominates once bypass is allowed).

