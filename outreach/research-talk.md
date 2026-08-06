# Research talk outline (10–12 slides)

Source content only — no PPTX/slide deck file is produced here. Each
entry below is one slide's worth of content; a presenter should adapt
wording/visuals but keep the evidence labeling ("simulated," "assumed,"
"cosimulated," etc.) intact, since that labeling is the point of this
project's disclosure discipline.

## Slide 1 — Title

MEMBRANE: A Mixed-Precision, Exact-Retrieval Architecture for LLM
KV-Cache Memory. Kadir Eren Altıntaş. Research prototype — not a
product, no real CXL/FPGA hardware used yet.

## Slide 2 — Motivation

KV-cache memory grows linearly with context length × concurrency; GPU
memory, not compute, is often the real ceiling. Three cost sources:
capacity, full-attention traffic, PCIe offload latency. (Same framing as
`paper/main.md` §1.)

## Slide 3 — Failed approaches (lead with this, not last)

Blind lossless compression on real KV data: 1.000x (no gain — every
block falls back to RAW storage). Real KV tensors are near-maximum-
entropy at the byte level. This negative result is what redirected the
whole project toward quantization instead of generic compression.
*(Presenter note: leading with a negative result, not ending with one,
signals disclosure-first framing from slide 3 — deliberate choice.)*

## Slide 4 — Architecture overview

The four-diagram system from `docs/architecture.md`: LLM runtime → real
trace capture → policy engine → hot/warm/cold KV tiers → exact sparse
retrieval → simulated CXL/near-memory appliance → cosimulated FPGA
datapath.

## Slide 5 — Mixed precision

FP16→Q8→Q4 tiering, runtime-calibrated promotion/eviction. Bit-exact vs.
ggml's own reference quantizer: 100,000+ random blocks + edge cases
(NaN/Inf/denormal/all-zero/constant), 0 mismatches.

## Slide 6 — FPGA datapath

Fully synthesizable, purely-integer fixed-point Q8/Q4 pipeline.
Cosimulated in Verilator against the CPU reference: 520,000
transactions, 0 mismatches. Elaborates under yosys 0.33. **Not** placed,
routed, or run on silicon — real FPGA validation is the explicit ask of
this talk (tie forward to slide 11).

## Slide 7 — CXL / near-memory model

Discrete-event simulator, calibrated from real captured attention
traces. All link latency/bandwidth figures: cited, assumed,
industry-typical ranges — informed by the CXL Consortium's spec and
standard PCIe-generation bandwidth, not a literal quote from either. No
physical CXL device used anywhere in this project.

## Slide 8 — Exact sparse retrieval

Predictor + prefetch + a compulsory-miss fetch that is never
approximated — contrast with eviction-based (H2O, Scissorhands,
StreamingLLM) and selection-based (Quest, PNM-CXL) prior art, which
permanently exclude some content. Tradeoff: more bytes moved than a pure
eviction policy at the same cache size (`docs/results-summary.md`).

## Slide 9 — Unified evaluation (128K × 512)

462/462 discrete-event scenarios, two models, zero extrapolated rows.
187x–405x KV-traffic reduction vs. full-scan baseline. Retrieval
overhead hidden under compute floor on a scenario-dependent fraction of
steps. Quant-pipeline count dominates simulated hardware sensitivity
(25.7x p99 swing, 8→1 pipelines) — more than CXL link generation does.

## Slide 10 — Limitations (own them directly)

Small model scale (135M/360M, not 7B+). No real CXL hardware. No real
GPU serving-stack integration. No FPGA place-and-route or board result.
10ms p99 never met — bounded by model compute floor, not retrieval
quality. Full list: `paper/main.md` §7.

## Slide 11 — Requested collaboration

FPGA board + toolchain access (Vivado/Quartus) for real
place-and-route, and ideally board bring-up. CXL Type-3 device or
emulation platform access, if available. Not asking for funding or
exclusivity — see `outreach/lab-package/collaboration-scope.md`.

## Slide 12 — Next experiment

`docs/phase8-hardware-validation-plan.md` Level A: real
place-and-route for `membrane_quant_stream_top`, resolving the one
identified major risk (the FP32 divider's un-pipelined ~65-bit
combinational critical path) if timing doesn't close on the first
attempt. Success or failure, the result gets reported — that's the
whole point.

---

*Presenter note: if time allows a Q&A/discussion slide, add one after
slide 12 rather than compressing content elsewhere — the limitations
and collaboration-ask slides (10-11) should never be rushed to make
room for it.*
