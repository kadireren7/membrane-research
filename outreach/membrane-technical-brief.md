# MEMBRANE — Technical Brief

**Author:** Kadir Eren Altıntaş · **Repository:** https://github.com/kadireren7/membrane
· **Paper:** `paper/main.md` / `paper/main.tex` · **Demo:** `scripts/demo.sh` (~25–50s depending on cache state, no model download)

This is a request for hardware access and research collaboration, not a
product pitch. Every claim below is labeled by evidence class
(real measurement, RTL simulation, synthesis check, discrete-event
simulation, or explicit assumption) so a reviewer can tell at a glance
what is and isn't proven yet.

## 1. Problem

LLM inference servers are usually compute-rich but memory-poor: KV-cache
size grows linearly with context length and concurrent request count,
and GPU memory capacity — not FLOPs — caps achievable scale before
compute does. Two classic mitigations have real, measured costs: naive
lossy KV eviction can silently break retrieval-heavy tasks, and moving
KV data off-GPU over PCIe has a per-transfer latency cost that is easy
to underestimate at small batch sizes.

## 2. MEMBRANE's approach

MEMBRANE explores a per-block memory *decision engine* combining two
levers: (a) mixed-precision tiering (FP16→Q8→Q4, verified bit-exact
against ggml's own reference quantizer) and (b) **exact, non-approximate
sparse retrieval** — a predictor decides what to prefetch, but every
genuinely-needed block is exactly fetched on a miss, never dropped. Both
levers are simulated end-to-end (discrete-event, calibrated from real
captured attention traces) before being committed to RTL, and the RTL is
cosimulated against the same CPU reference math it's meant to
accelerate.

## 3. Current verification level (evidence classes, not conflated)

| Evidence class | What's actually verified |
|---|---|
| **Real CPU/inference measurement** | Bit-exact Q8_0/Q4_0 quantize/dequantize vs. ggml's own reference (100,000+ random blocks + edge cases); real captured KV/attention traces from actual llama.cpp inference on SmolLM2-135M/-360M; real quality validation runs. |
| **RTL simulation (cosimulation)** | `membrane_quant_stream_top` (the full Q8/Q4 encode/decode datapath) cosimulated in Verilator against the CPU reference: 520,000 transactions, 0 mismatches. |
| **Synthesis check (yosys, no P&R)** | The full RTL hierarchy elaborates cleanly under yosys 0.33. Real, measured `synth_ecp5` technology-mapped cell counts exist for the individual arithmetic modules (e.g. the FP32 divider: ~73,600 LUT-class cells) — **not** an Fmax or timing-closure result. |
| **Discrete-event simulation** | A unified 128K-context × 512-concurrency sweep (462/462 scenarios, both models) modeling a near-memory/CXL appliance's queueing/contention, calibrated from real traces. |
| **Assumed CXL values** | All CXL link latency/bandwidth figures are explicit, cited, industry-typical assumptions — informed by the CXL Consortium's published specification and standard PCIe-generation bandwidth figures, not measurements or a literal quote from either — no real CXL device was used anywhere in this project. |
| **Unverified hardware claims** | No physical FPGA board, no place-and-route, no real PCIe DMA, no real CXL hardware. This is exactly the gap this outreach is about. |

## 4. Key results (all sourced; see `benchmarks/MANIFEST.json` and `paper/claim-audit.md`)

- 462/462 unified-sweep scenarios complete, zero extrapolated rows.
- 187x–405x KV-traffic reduction vs. a full-scan-CXL baseline at a
  representative point (discrete-event simulation).
- 520,000-transaction, zero-mismatch FPGA/CPU cosimulation.
- 100,000+-block bit-exact CPU/ggml quantization parity.
- Quant-pipeline count dominates simulated CXL link generation as a
  hardware sensitivity (8→1 pipelines: 25.7x p99 latency increase).

## 5. Negative results (kept visible, not hidden)

- Blind lossless compression fails on real KV-cache data (1.000x; RAW
  fallback engaged on every block).
- PCIe-round-trip FPGA quantization offload is a **net loss** at
  live-decode batch sizes — a real per-block CPU quantize cost
  (20–180ns) is smaller than any realistic real PCIe round trip
  (1–2µs+), a composed but disclosed, not-yet-measured-on-real-hardware
  conclusion.
- Naive approximate KV eviction breaks recall-shaped prompts (as low as
  0.04 top-1 match rate on two tested policies).
- A 10ms p99 latency target is not met in any of the 462 real scenarios
  — bounded by each model's own decode compute floor, not retrieval
  quality.
- Micro-batching shows no measurable benefit at calibrated real demand
  levels.

## 6. Why real hardware is needed now

Every hardware-adjacent number above is either a cosimulation, a
synthesis-level cell count, or an explicit assumption. Three specific
open questions cannot be answered without physical hardware:

1. **Does the RTL actually close timing and produce correct results on
   silicon?** The FP32 divider (the dominant resource cost, ~73,600
   LUT-class cells, an un-pipelined ~65-bit combinational divide) is a
   real, disclosed timing-closure risk that only a real P&R run can
   resolve (see `docs/phase8-hardware-validation-plan.md` Level A).
2. **Is the PCIe-offload negative result (§5) actually correct at real
   transport latencies?** This project's own emulation charges near-zero
   transport cost; only a real board with real DMA can measure this
   (Level B).
3. **Does the near-memory/CXL simulation's queueing model resemble a
   real CXL device's behavior at all?** This can only be checked against
   a real CXL Type-3 memory device or vendor emulation platform
   (Level C).

## 7. Collaboration request

Access to (in order of value, not all required at once):
1. An FPGA board with a real synthesis toolchain (Vivado for
   Xilinx/AMD Alveo-class, or Quartus for an Altera/Intel equivalent) —
   even remote/shared access sufficient for place-and-route + a
   loopback DMA test.
2. A CXL Type-3 memory device or an accessible CXL emulation/prototyping
   platform.
3. Engineering time from someone with real board-bring-up experience
   (PCIe DMA framework selection, AXI integration) — this project's RTL
   is deliberately kept vendor-IP-free and interface-only
   (`hardware/README.md`) specifically so it can be adapted without
   requiring us to redistribute vendor IP.

## 8. What we'd need from a lab

- Toolchain access (Vivado/Quartus license + a target board), even
  time-boxed or remote.
- A point of contact for board-specific integration questions (DMA
  framework choice, AXI clocking-domain constraints).
- No data-sharing obligation beyond what's already public in this
  repository — all traces/prompts used are already committed and
  non-sensitive.

## 9. Proposed 2–4 week validation plan

- **Week 1**: synthesis + place-and-route for `membrane_quant_stream_top`
  on the target board's real toolchain; resolve or re-pipeline the FP32
  divider if timing does not close (`docs/phase8-hardware-validation-plan.md`
  Level A).
- **Week 2**: bitstream bring-up, loopback DMA, known-vector test,
  100K-random-block parity test on real silicon (Level B, phase 1).
- **Week 3**: sustained throughput/latency measurement, queue-depth and
  batch-size scaling, CPU-vs-FPGA comparison, power/thermal measurement
  (Level B, phase 2).
- **Week 4 (if a CXL platform is available)**: memory-tier integration
  smoke test against a real or emulated CXL Type-3 device (Level C,
  scoped down to whatever the available platform actually supports).

Full detail, prerequisites, and pass/fail criteria for every step:
`docs/phase8-hardware-validation-plan.md` and
`hardware/experiment-protocol.md`.
