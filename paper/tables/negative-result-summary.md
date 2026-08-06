# Negative-result summary

Hand-curated (these are qualitative findings, not a single CSV
column) but each row's number is drawn from a cited source -- see
`paper/claim-audit.md` for the exact wording/source/limitation per
row.

| # | Finding | Key number | Source |
|---|---|---|---|
| 1 | Blind lossless compression fails on real KV data | 1.000x (RAW fallback engaged) | `docs/phase2-kv-analysis.md` |
| 2 | PCIe-round-trip FPGA offload is a net loss at live-decode batch sizes | real PCIe RTT (1-2us) vs. 20-180ns real CPU quantize cost | `docs/phase5-pcie-hardware-loop.md` §9-10 |
| 3 | Naive approximate KV eviction breaks recall-shaped tasks | as low as 0.04 top-1 match rate | `docs/phase6-attention-working-set.md` §7 |
| 4 | 10ms p99 bound not met by any real scenario, either model | 0/225 real rows per model | `benchmarks/cxl-sim/unified-sweep.csv` |
| 5 | Micro-batching shows no measurable benefit at calibrated demand | null result (no measurable delta) | `docs/phase6-cxl-near-memory.md` §8 |
