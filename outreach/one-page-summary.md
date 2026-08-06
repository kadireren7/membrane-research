# MEMBRANE — One-Page Summary

**Kadir Eren Altıntaş** · github.com/kadireren7/membrane

## Problem

LLM KV-cache memory grows linearly with context length and concurrent
requests; GPU memory capacity, not compute, is usually the real ceiling.

## Contribution

A per-block memory decision engine combining (1) mixed-precision KV
tiering verified bit-exact against ggml's reference quantizer, and (2)
**exact, non-approximate** sparse retrieval — a predictor prefetches,
but every needed block is exactly fetched on a miss, never dropped.
Simulated at 128K context × 512 concurrency; the same quantization math
is cosimulated in a synthesizable FPGA datapath.

## Evidence

- 462/462 discrete-event scenarios complete (two models), 187x–405x
  simulated KV-traffic reduction vs. a full-scan baseline.
- 520,000-transaction, zero-mismatch FPGA/CPU RTL cosimulation.
- 100,000+-block bit-exact quantization parity vs. ggml.
- Five disclosed negative results, including: PCIe FPGA offload is a
  net loss at live-decode batch sizes; naive KV eviction breaks recall
  tasks; a 10ms p99 target is never met (compute-floor-bound, not
  retrieval-bound).

## Missing proof

- No real FPGA board, no place-and-route, no timing-closure result.
- No real PCIe DMA measurement.
- No real CXL hardware — all CXL figures are cited, assumed ranges.
- Model scale (135M/360M) is far below production LLM sizes.

## Requested access

Time-boxed or remote access to an FPGA board + synthesis toolchain
(Vivado/Quartus), and, if available, a CXL Type-3 device or emulation
platform. No proprietary data or exclusivity requested.

## Expected output

A real synthesis/place-and-route result (pass or fail, reported either
way) for the existing, vendor-IP-free RTL; if board access follows, real
bit-exactness and throughput/latency numbers to replace this project's
current simulation-only claims — see
`docs/phase8-hardware-validation-plan.md` for the full 3-level plan and
pass/fail criteria.

---
*Research prototype. No product, no production claim, no real CXL
acceleration claimed anywhere in this project — see
`outreach/hardware-claim-gates.md` for exactly what can and cannot be
said at each verification stage.*
