# Limitations summary

| Limitation | Where it applies |
|---|---|
| Small model scale (135M/360M, not 7B+) | All quality/latency findings |
| No real CXL hardware | §3.6, all CXL-tagged results |
| No real GPU serving-stack integration | All latency/throughput findings |
| Trace extrapolation for 128K context | Unified sweep (§5 RQ5-RQ7) |
| CPU compute floor bounds all latency | Every p99 number in this paper |
| Simulator assumptions (analytical queueing) | `membrane-cxl-sim`, `membrane-kv-exact-sim` |
| No FPGA place-and-route/on-board result | §3.7, §5 RQ3 |
| Limited model/prompt diversity | §5 RQ2, negative result 3 |
| Q4 quality risk not exhaustively characterized | §3.2, §5 RQ2 |
| Attention-trace top-k capture resolution | §5 RQ5, capacity/recall findings |
