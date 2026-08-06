# MEMBRANE: A Mixed-Precision, Exact-Retrieval Architecture for LLM KV-Cache Memory

**Author:** Kadir Eren Altıntaş

**Status:** Complete, claim-audited manuscript. Every headline number
below is sourced (see `paper/claim-audit.md`) and machine-checked where
possible (`paper/scripts/verify-paper.py`, `scripts/verify-results.py`).
Related Work cites 14 independently-verified primary sources (see
`paper/related-work-matrix.md`, `paper/references.bib`); no citation was
added without confirming its title/author list/venue against its own
arXiv or ACM page. `paper/main.pdf` builds successfully as a real
GitHub Actions artifact on every push (workflow: `Paper Build`; the PDF
itself is not committed to this repository — see
`paper/scripts/README.md`). This is a research-prototype manuscript,
not a claim of a production system or of real CXL/FPGA hardware — see
§7 (Limitations and Threats to Validity).

---

## Abstract

LLM inference servers are typically compute-rich but memory-poor:
KV-cache size grows linearly with context length and concurrent request
count, and full-attention traffic and classic PCIe-based offload latency
both become binding constraints before compute does. We present
MEMBRANE, a research prototype exploring whether a per-block memory
decision engine — combining precision tiering and exact (non-approximate)
retrieval — can reduce KV-cache memory traffic without degrading
retrieval quality, and where such a design does not help. MEMBRANE
combines a runtime-calibrated mixed-precision KV-cache tier (FP16/Q8/Q4,
verified bit-exact against ggml's reference quantizer), an exact sparse
KV retrieval engine (predictor, prefetch, and a compulsory-miss fetch
that is never approximated), a discrete-event near-memory/CXL appliance
simulator calibrated from real captured attention traces, and a
synthesizable FPGA quantization datapath cosimulated against the same
CPU reference math. Across a unified 128K-context x 512-concurrency
discrete-event stress sweep (462/462 scenarios, two open SmolLM2
checkpoints), we measure 187x-405x KV-traffic reduction against a
full-scan-CXL baseline and a 520,000-transaction, zero-mismatch
Verilator cosimulation of the FPGA datapath against its CPU reference.
We also report, without suppressing them, five null/negative findings:
blind lossless compression fails on real KV-cache data; PCIe-round-trip
FPGA quantization offload is a net loss at live-decode batch sizes;
naive approximate KV eviction breaks recall-shaped prompts; a 10ms p99
latency target is not met in any of the 462 real scenarios, bounded by
each model's own decode compute floor rather than retrieval quality; and
micro-batching shows no measurable benefit at calibrated real demand
levels. MEMBRANE remains a simulation-heavy research prototype: no
physical CXL or FPGA hardware was used, and every hardware-adjacent
number is either a cosimulation, a synthesis-level check, or an explicit,
cited assumption.

## 1. Introduction

Large language model inference servers are, in practice, often
memory-bound rather than compute-bound at serving time. Three cost
sources compound as context length and concurrent request count grow:

1. **KV-cache capacity.** The KV cache for a single sequence grows
   linearly with context length, and the aggregate footprint across
   concurrently-served sequences multiplies that by request count —
   GPU memory capacity, not FLOPs, is what actually runs out first at
   long context and high concurrency
   [@kwon2023pagedattention; @sheng2023flexgen].
2. **Full-attention traffic cost.** Naively re-reading the entire KV
   history for every decode step scales the memory traffic moved per
   token linearly with context length; this is the cost that sparse and
   selective-attention methods
   [@zhang2023h2o; @xiao2024streamingllm; @tang2024quest] each try to
   reduce by different means.
3. **Classic PCIe offload latency.** Moving KV data off-GPU (to CPU RAM,
   disk, or a device across PCIe) trades capacity for a real per-transfer
   latency cost that offloading systems must amortize carefully
   [@sheng2023flexgen; @lee2024infinigen] — and, as this paper's own
   hardware-in-the-loop measurement shows (§6, RQ4), that cost can make
   small-batch FPGA-based offload a net loss rather than a win.
4. **The resulting need for an explicit memory-tier / near-memory
   design.** Recent architecture work has begun proposing CXL-attached
   near-memory processing specifically to address this
   [@xie2025trace; @kim2025pnmcxl] — MEMBRANE's own near-memory/CXL
   simulator (§3.6) is built in the same spirit, evaluated at 128K
   context and 512 concurrency, but entirely in simulation.

MEMBRANE's research question is narrower and more falsifiable than
"build a faster KV-cache": *for a real KV access pattern captured from
actual inference (not synthesized), does a decision engine that tiers
precision and retrieves exactly — rather than approximately — measurably
reduce memory traffic without degrading retrieval quality, and at what
latency cost? And where does this design not help?* §5 (Evaluation) and
§6 (Negative Results) answer both halves of that question honestly, with
every headline number traceable to a committed artifact
(`benchmarks/MANIFEST.json`).

### Contributions

This paper makes five contributions, each stated at the scope it can
actually support (see `paper/related-work-matrix.md` for how each
compares to the closest prior work found in this project's literature
search):

1. **A runtime-calibrated, hot/warm/cold KV-cache tiering policy over
   FP16/Q8/Q4 precision**, with the quantize/dequantize path verified
   bit-exact against ggml's own reference implementation across
   100,000+ randomized blocks plus every disclosed edge case (§3.2, §5
   RQ2).
2. **A synthesizable FPGA quantization datapath sharing the identical
   bit-exact arithmetic as the CPU reference**, cosimulated in Verilator
   across 520,000 transactions with zero mismatches — to our knowledge,
   among the surveyed FPGA-quantization and CXL near-memory literature
   (`paper/related-work-matrix.md`), none report this specific
   CPU/RTL bit-exact-parity verification methodology for KV
   quantization at this transaction count (§3.7, §5 RQ3).
3. **A disclosed, quantified negative result on PCIe-round-trip FPGA
   quantization-offload economics at live-decode (small) batch sizes**,
   derived from a real hardware-in-the-loop timing model — not stated
   this way in the surveyed near-memory/FPGA sources (§6, RQ4).
4. **An exact (non-approximate) sparse KV retrieval architecture for a
   simulated CXL/near-memory tier**, combining a predictor/prefetch
   mechanism with a compulsory-miss fallback that never permanently
   excludes attended content — distinguished from eviction-based
   [@zhang2023h2o; @liu2023scissorhands; @xiao2024streamingllm] and
   selection-based [@tang2024quest; @kim2025pnmcxl] prior art (§3.6,
   §3.8, §5 RQ5).
5. **A real, out-of-core discrete-event simulation and
   artifact-verification infrastructure** — checkpoint/resume, a
   SHA-256-tracked artifact manifest, and automated headline-claim
   auditing (`scripts/verify-results.py`, `paper/scripts/verify-paper.py`)
   — that completed a unified 128K-context x 512-concurrency stress
   sweep (462/462 scenarios) on a memory-constrained (5.6 GiB RAM)
   development machine, with every headline number in this paper
   automatically checked against its source artifact (§4, §5 RQ6-RQ7).

We use "first"/"novel"/"unprecedented" nowhere in this list without the
explicit, hedged "to our knowledge, among the surveyed literature"
qualifier in contribution 2 — the literature search behind
`paper/related-work-matrix.md` was thorough but not exhaustive, and no
absolute priority claim is made.

## 2. Background

MEMBRANE's own empirical starting point (Phase 2 of this project) is a
negative one: byte-level lossless compression does not work on real
KV-cache tensors. Measured Shannon entropy of real, captured F16 KV data
from actual llama.cpp inference is 7.3-7.4 bits/byte, independent of
prompt content, block size, or K-vs-V — byte-level RLE achieves exactly
1.000x (every block falls back to raw storage) and would expand the data
to 0.502x without that fallback (`docs/phase2-kv-analysis.md`). This
result, not an a priori design choice, is what redirected this project
toward quantization rather than generic compression, and it previews
this paper's broader empirical stance: every design decision below is
justified by a measurement in this repository, not by assumption.

## 3. System design

MEMBRANE is a from-scratch C11/C++17 implementation, described here as
one coherent system rather than a phase-by-phase development log — see
`docs/architecture.md` for the four accompanying diagrams (end-to-end
system, KV lifecycle, FPGA datapath, exact retrieval path) this section
follows.

### 3.1 Block store and lossless baseline

The foundation is a budget-aware block store (`include/membrane/store.h`)
with pluggable codecs and a persistent file-backed cold tier
(`include/membrane/backend.h`). Its lossless codec path (RAW/RLE) is the
empirical baseline that motivated §2's negative result — real KV data
does not compress losslessly, so the store's adaptive RAW fallback
(never expanding incompressible data) is what actually matters at this
layer, not the RLE codec itself.

### 3.2 Mixed-precision (Q8/Q4) tiering

FP16 (hot) -> Q8 (warm) -> Q4 (cold) tiering, promoted/evicted under a
fixed byte budget. The quantize/dequantize transformation itself is
verified bit-exact against ggml's own Q8_0/Q4_0 reference implementation
— not an approximation of it — across 100,000+ randomized blocks plus
every disclosed edge case (NaN/Inf/denormal/all-zero/constant), zero
mismatches (`tests/unit/test_ggml_quant_parity.c`; see
`docs/phase4-ggml-quant-parity.md`; claim C-06 in `paper/claim-audit.md`).

### 3.3 Runtime-calibrated policy

Precision-tier promotion/eviction thresholds are calibrated against real
runtime behavior (Phase 4's runtime-calibration and variance work), not
fixed a priori — `docs/phase4-runtime-calibration.md` and
`docs/phase4-runtime-variance.md` document a real decode-shape bug found
and fixed during this calibration process, and quantify that the choice
of quantize function (not measurement noise) dominates the gap between
offline-predicted and runtime-observed policy behavior.

### 3.4 Hot/warm/cold residency

The three precision tiers above double as three residency tiers: hot
(FP16, always resident), warm (Q8, resident but compressed), cold (Q4,
resident but maximally compressed) — with eviction beyond cold handled
by the exact-retrieval path below, not by silently dropping data.

### 3.5 Exact sparse retrieval

A predictor estimates which KV blocks a future attention step will need
and issues a prefetch; every block attention actually needs is exactly
fetched on a compulsory miss if not already resident — retrieval
correctness is never approximated, only the prefetch *decision* is
predicted (`tools/membrane-kv-workingset-sim`,
`tools/membrane-kv-exact-sim`). This is the design property that most
directly distinguishes MEMBRANE from eviction-based prior art (§8).

### 3.6 CXL / near-memory appliance simulation

A discrete-event simulator (`tools/membrane-cxl-sim`) modeling a
near-memory/CXL appliance: link, device, and quant-engine queues with
real simulated contention, calibrated once from a real captured trace
and replayed at scale (`docs/phase6-cxl-near-memory.md`). No physical
CXL hardware exists in this project; link latency/bandwidth figures are
assumed, cited, industry-typical ranges drawn from the CXL Consortium's
own published specification [@cxlconsortium2023spec] (§7).

### 3.7 FPGA datapath

A fully synthesizable, purely-integer fixed-point Q8/Q4 encode/decode
pipeline (`rtl/membrane_quant_stream_top.sv` and supporting modules) —
no `real`/`shortreal`/DPI constructs anywhere in the production datapath,
confirmed to elaborate cleanly under yosys 0.33. Cosimulated in
Verilator against the same CPU reference math
(`src/quant/quant_simd.c`) for 520,000 transactions across all four
encode/decode modes plus a mixed-mode interleave, zero mismatches
(`rtl/tb/tb_top_verilator.cpp`; see `docs/phase5-synthesizable-fpga.md`;
claim C-05). This has not been placed, routed, or run on physical
silicon (§7).

### 3.8 Out-of-core, memory-bounded simulation infrastructure

The unified 128K x 512-concurrency sweep's largest runs exceed this
5.6 GiB development machine's RAM if held naively in memory. A chunked,
checksummed out-of-core trace format, a bounded LRU chunk cache, and a
real `/proc`-based memory guard let the full sweep complete without
raising the RAM ceiling (`docs/phase6-out-of-core-simulator.md`) — an
engineering constraint turned into a disclosed, reproducible design
choice, not hidden as an implementation detail.

## 4. Methodology

Every experiment in this paper belongs to exactly one of the following
classes, named explicitly in every results table (§5, §9) so that a
reader never has to infer what kind of evidence a number represents:

| Class | Meaning | Example in this paper |
|---|---|---|
| **Real inference** | Actual forward-pass execution of a real model (llama.cpp, SmolLM2-135M/-360M) | Quality validation runs (`membrane-kv-quality`) |
| **Real trace capture** | Real KV/attention tensors captured from a real inference run | `.kvtrace`/`.attntrace` files, `membrane-kv-capture` |
| **CPU benchmark** | An actual program execution measuring CPU-side behavior | `test_ggml_quant_parity` (100,000+ blocks, C-06) |
| **RTL simulation (cosimulation)** | An actual Verilator run of real RTL against a real CPU reference | `tb_top_verilator.cpp` (520,000 transactions, C-05) |
| **Synthesis (not silicon)** | Static elaboration/synthesizability check (yosys), no execution | RTL elaborates under yosys 0.33 (§3.7) |
| **Discrete-event simulation** | Calibrate-once-replay-many simulation of queueing/contention, seeded from a real trace | `membrane-cxl-sim`, `membrane-kv-exact-sim` (462-scenario sweep) |
| **Extrapolated long-context trace** | A real trace synthetically extended to a longer context length | `extend_synthetic()`/`.attntrace3`, used for the 128K-context sweep |
| **Oracle analysis** | Fed ground truth directly; an upper bound, not a claim a real predictor could know the future | `oracle` comparison rows in `unified-sweep.csv` |
| **Assumed hardware profile** | An explicit, cited estimate; no real hardware measured | CXL link latency/bandwidth ranges (§3.6) |

## 5. Evaluation

Each research question below states its experiment, result,
interpretation, and limitation explicitly, with its experiment class
(§4) named. Full sourcing: `paper/claim-audit.md`; raw data:
`benchmarks/MANIFEST.json`.

### RQ1: How much capacity does lossless vs. lossy KV representation actually recover?

- **Experiment** (CPU benchmark + discrete-event simulation): real
  entropy/compression measurement on captured KV tensors (Phase 2),
  followed by capacity accounting (`cap_effective_capacity_ratio`) across
  3 device sizes x 2 models in the unified sweep.
- **Result**: lossless byte-level compression recovers **0x** additional
  capacity on real KV data (1.000x, RAW fallback engaged on every block).
  Q8/Q4 lossy tiering, by contrast, is what makes
  `cap_effective_capacity_ratio` exceed 1.0 at the 2TiB device point for
  SmolLM2-135M — but SmolLM2-360M's fp16 tier *never* reaches ratio 1.0
  even at the largest tested device (0.7998 at 2TiB), a real,
  model-dependent capacity result (`docs/phase6-unified-stress.md` §3).
- **Interpretation**: lossless compression is not a viable lever for KV
  capacity on this data; lossy precision tiering is, but its benefit is
  precision- and model-size-dependent, not a fixed multiplier.
- **Limitation**: capacity accounting is simulated/analytical
  (`cap_*` fields), not measured against a real device's actual capacity
  behavior.

### RQ2: How well does runtime-calibrated mixed precision preserve quality?

- **Experiment** (real inference + CPU benchmark): real
  `membrane-kv-quality` runs against actual llama.cpp inference on both
  models, plus the bit-exact CPU/ggml parity check (C-06).
- **Result**: quantize/dequantize itself is bit-exact (0 mismatches,
  100,000+ blocks); end-to-end generation quality under the calibrated
  runtime policy is measured directly against real inference, not
  estimated (`benchmarks/cxl-sim/quality-reverify/*.jsonl`).
- **Interpretation**: precision transitions do not themselves introduce
  quantization error beyond what Q8_0/Q4_0 already implies — any quality
  effect traces to the policy's tier-placement decisions, not to a
  faulty quantizer.
- **Limitation**: quality validation is at 135M/360M model scale, far
  below production LLM sizes (§7).

### RQ3: Does the bit-exact FPGA datapath reproduce the CPU reference math?

- **Experiment** (RTL simulation/cosimulation): `tb_top_verilator.cpp`
  drives `membrane_quant_stream_top` against `quant_simd.c`'s golden
  vectors, all four Q8/Q4 encode/decode modes plus a mixed-mode
  interleave, randomized valid/ready backpressure, explicit
  reset-mid-stream flush tests.
- **Result**: **520,000 transactions, 0 mismatches** (C-05).
- **Interpretation**: the RTL arithmetic is bit-exact with its CPU
  reference under the tested transaction space, including edge cases —
  a cosimulation result, not a silicon result.
- **Limitation**: no physical FPGA was used; no Fmax/power/area numbers
  exist (§7).

### RQ4: Why does PCIe quantization offload fail to help small autoregressive batches?

- **Experiment** (hardware-in-the-loop emulation, composed/analytical):
  a real per-block CPU quantize/dequantize timing measurement (20-180ns)
  composed with a disclosed, cited estimate of real PCIe doorbell/DMA/
  completion-interrupt round-trip cost (routinely low-single-digit
  microseconds), against the emulation's own near-zero (~210ns)
  in-emulation transport cost.
- **Result**: a real PCIe round trip of even 1-2 microseconds per call
  would make per-block or small-batch FPGA offload a **net loss** versus
  keeping quantization on CPU, unless batched far more aggressively than
  live autoregressive decoding naturally allows
  (`docs/phase5-pcie-hardware-loop.md` §9-10).
- **Interpretation**: the bottleneck is transport granularity, not
  quantization compute — batching strategy, not a faster PCIe link, is
  the lever that would change this conclusion.
- **Limitation**: this is a composed, disclosed, not-independently-
  measured-on-real-hardware conclusion, explicitly labeled as such.

### RQ5: How much does near-memory exact sparse retrieval reduce full-scan traffic?

- **Experiment** (discrete-event simulation): the unified sweep's 5
  real (non-analytical) comparisons vs. 2 analytical full-scan
  baselines, both models, all host-cache x device-size x precision
  points.
- **Result**: **187x-405x** reduction vs. full-scan-CXL at the
  representative 8GiB-host/2TiB-device point (99.6x-215.3x vs. a
  precision-matched compressed baseline); a separate, smaller
  4K-context capacity-bound scenario shows 985.7x-1,281.1x
  (`docs/phase6-exact-sparse-retrieval.md` §12) — never conflated with
  the 128K-context figure (C-03, C-04).
- **Interpretation**: exact retrieval with a real predictor achieves
  reduction close to the oracle upper bound (oracle bytes/token within
  ~0.003% of the real predictor's, per-model) — the predictor is not
  leaving much on the table relative to what perfect foreknowledge would
  achieve.
- **Limitation**: simulated, calibrated from real traces but not
  measured on a real GPU serving system with real concurrent requests.

### RQ6: What is capacity and p99 behavior at the full 128K x 512 workload?

- **Experiment** (discrete-event simulation, out-of-core backend):
  full 462/462-scenario sweep, both models, all host-cache/device/
  precision/comparison combinations.
- **Result**: 462/462 scenarios complete (C-02); capacity behavior is
  real and model-dependent (RQ1); p99 latency **never** meets a 10ms
  illustrative bound in any of the 462 real scenarios, for either model
  (C-08).
- **Interpretation**: at this scale, the system's own completion and
  capacity behavior is fully characterized; the p99 result is explained
  by RQ7, not by a retrieval-quality shortfall.
- **Limitation**: single-worker/limited-worker execution constrained by
  this development machine's RAM (§7); not evaluated on a
  larger-memory production machine.

### RQ7: Is the real bottleneck the link, the quant pipeline, the cache, or the model's own compute?

- **Experiment** (discrete-event simulation, hardware-sensitivity
  sub-sweep): 10 hardware profile points (CXL latency/bandwidth
  variants, 1/2/4/8 quant pipelines), SmolLM2-135M, one representative
  scenario point.
- **Result**: dropping quant-pipeline count from 8 to 1 inflates p99 by
  **25.7x** (15.67ms -> 402.1ms, C-07); CXL-3.0-class vs.
  CXL-latency-low/medium/high bandwidth/latency variants show **no
  measurable difference** at the same point. Independently, the model's
  own real decode compute floor (15.67ms/token for 135M, ~41.0ms/token
  for 360M) alone already exceeds the 10ms bound before any KV-retrieval
  latency is added (C-09).
- **Interpretation**: **the quant-pipeline count and the model's own
  compute floor dominate; CXL link generation, at this scenario point,
  does not.** This is a real, actionable finding for anyone considering
  building this system in hardware: over-provision quant engines before
  chasing a newer CXL generation.
- **Limitation**: the hardware-sensitivity sub-sweep is SmolLM2-135M-only
  by original design (C-07); not re-derived for SmolLM2-360M.

## 6. Negative results

Kept as a first-class, visible section — these are as scientifically
load-bearing as §5's positive findings, not an appendix or a footnote.

1. **Blind lossless compression fails on real KV-cache data** (§2, RQ1):
   byte-level RLE achieves exactly 1.000x on real captured F16 KV
   tensors (every block falls back to RAW); without that fallback it
   would expand the data to 0.502x. Real KV data is near-maximum-entropy
   at the byte level (7.3-7.4 bits/byte), independent of prompt content.
2. **PCIe-round-trip FPGA offload is a net loss at live-decode batch
   sizes** (RQ4): a real PCIe round trip of even 1-2 microseconds would
   make per-block/small-batch offload lose to CPU-only quantization,
   given real per-block CPU cost of only 20-180ns.
3. **Naive approximate KV eviction breaks recall-shaped tasks**: two
   tested approximate eviction policies show real, measured recall
   failures on recall-shaped prompts, as low as 0.04 top-1 match rate,
   while performing acceptably on natural/code prompts (C-10).
4. **A 10ms p99 latency target is not met by any of the 462 real
   scenarios, for either model** (RQ6): bounded by each model's own real
   decode compute floor, not by retrieval quality (C-08, C-09).
5. **Micro-batching shows no measurable benefit** at this project's
   calibrated, real attention-derived demand levels (C-11) — a genuine
   null result from a mechanism that was actually built and run, not an
   unimplemented feature.

## 7. Limitations and threats to validity

- **Small model scale.** Evaluation uses SmolLM2-135M-Instruct and
  SmolLM2-360M-Instruct — far below production LLM sizes (7B+, used by
  most cited prior work, `paper/related-work-matrix.md`). Quality and
  latency findings may not transfer unchanged to larger models.
- **No real CXL hardware.** Every CXL link latency/bandwidth figure is
  an assumed, cited, industry-typical range (§3.6); no physical CXL
  device was used anywhere in this project.
- **No real GPU serving-stack integration.** MEMBRANE does not plug into
  vLLM, TensorRT-LLM, or any production serving engine; its exact
  retrieval and mixed-precision runtime are evaluated standalone and in
  simulation, not as a component of a real multi-tenant GPU server.
- **Trace extrapolation for long context.** The 128K-context sweep
  synthetically extends real captured traces (`extend_synthetic()`) —
  this is a disclosed, deterministic extension of real data, not a fresh
  128K-context real capture.
- **CPU compute floor bounds all latency results.** Every p99 latency
  number in this paper is lower-bounded by each model's own real decode
  speed on this project's CPU inference setup; a faster (e.g. GPU-based)
  serving stack would shift this floor, changing the balance between
  compute-bound and retrieval-bound latency.
- **Simulator assumptions.** The discrete-event simulators
  (`membrane-cxl-sim`, `membrane-kv-exact-sim`) model queueing/
  contention analytically; they are calibrated from real traces but do
  not model every real-hardware effect (e.g. real DRAM refresh timing,
  real PCIe protocol overhead beyond the disclosed estimate in RQ4).
- **No FPGA place-and-route or on-board result.** The FPGA datapath is
  cosimulated (Verilator) and confirmed synthesizable (yosys) at the RTL
  level only — no Fmax, power, area, or on-board timing result exists.
- **Limited model and prompt diversity.** Two model checkpoints, a small
  authored set of prompt categories (`benchmarks/kv/prompts/`) — not a
  broad benchmark suite (e.g. LongBench-scale prompt diversity).
- **Q4 quality risk.** Q4 is this project's most aggressive precision
  tier; §5 RQ2's bit-exactness claim covers the quantizer itself, not a
  guarantee that every possible Q4 deployment configuration preserves
  downstream task quality equally (see `docs/phase3-kv-q8-quality.md`
  family of results for the quality envelope actually measured).
- **Attention-trace top-k capture resolution.** Captured attention
  traces store a fixed top-k per step (not the full attention
  distribution) — a resolution choice made for file-size tractability
  (`docs/phase6-exact-sparse-retrieval.md` §2), which could hide
  lower-ranked attention mass in principle.

## 8. Related work

See `paper/related-work-matrix.md` for the full comparison table across
14 sources spanning KV-cache quantization, mixed-precision KV cache, KV
eviction, sparse attention, attention sinks, heavy-hitter selection,
paged KV-cache management, KV-cache offloading, CXL/near-memory
processing, and FPGA quantization. Summarized by category:

- **KV-cache eviction / heavy-hitter selection**: H2O
  [@zhang2023h2o] and Scissorhands [@liu2023scissorhands] both evict
  low-importance tokens under a submodular or persistence-of-importance
  heuristic, reporting real throughput gains (up to 29x) on real GPUs at
  6.7B-30B model scale — MEMBRANE's exact-retrieval design responds
  directly to this category by never permanently discarding attended
  content, at the cost of not yet being validated at that model scale or
  on real hardware.
- **Attention sinks / sparse attention**: StreamingLLM
  [@xiao2024streamingllm] and Quest [@tang2024quest] both reduce
  attention computation via fixed or query-aware sparsity on real GPUs;
  MEMBRANE's predictor plays a similar role but exists to decide
  memory-tier residency, not to reduce attention FLOPs directly.
- **Paged KV-cache management**: PagedAttention/vLLM
  [@kwon2023pagedattention] is the strongest real-hardware, real-
  concurrency result surveyed — exact (no data loss), production-scale,
  real serving throughput gains. MEMBRANE shares the "never lose data"
  property but has not been validated at anything close to this scale or
  realism.
- **KV-cache offloading / memory tiering**: FlexGen
  [@sheng2023flexgen] and InfiniGen [@lee2024infinigen] both tier KV/
  weight data across real GPU/CPU/disk hardware, with InfiniGen's
  speculative-rehearsal prefetch the closest real-hardware analogue to
  MEMBRANE's predictor+prefetch+compulsory-fetch design.
- **KV-cache quantization / mixed precision**: KIVI
  [@liu2024kivi] and KVQuant [@hooper2024kvquant] both achieve
  sub-4-bit KV quantization on real GPUs; MEMBRANE's Q8/Q4 tiers are
  less aggressive but are the only ones in this comparison independently
  verified bit-exact against ggml's own reference implementation
  across 100,000+ cases.
- **Retrieval-aware / long-context attention**: Landmark Attention
  [@mohtashami2023landmark] retrieves relevant blocks via a trained
  landmark token, extending real fine-tuned context to ~32K; MEMBRANE's
  retrieval requires no model fine-tuning but has only been evaluated in
  simulation.
- **CXL near-memory architectures**: TRACE [@xie2025trace] and PNM-CXL
  [@kim2025pnmcxl] both propose CXL-side compression/selection
  mechanisms for LLM serving, published in the same period as this
  project's own Phase 6 work; this paper does not independently verify
  whether their reported results come from real silicon, an FPGA
  prototype, or simulation, and flags that uncertainty explicitly rather
  than assuming either answer.
- **FPGA quantization accelerators**: F-BFQ [@haris2025fbfq] presents a
  real-FPGA (per its own abstract) block floating-point quantization
  accelerator; MEMBRANE's datapath is cosimulated and synthesizability-
  checked but not yet run on a physical board — a genuine capability gap
  relative to F-BFQ.
- **CXL specification**: this project's simulated link latency/
  bandwidth figures are explicit, cited, industry-typical assumptions —
  informed by the CXL Consortium's published specification
  [@cxlconsortium2023spec] and standard PCIe-generation bandwidth
  figures, not a literal quote from either, and not from an
  implementation of the specification.

## 9. Ethics and artifact disclosure

- **Model licenses.** SmolLM2-135M-Instruct and SmolLM2-360M-Instruct are
  real, public checkpoints from HuggingFaceTB; this project does not
  redistribute their weights (gitignored, see `docs/licensing.md`) and
  makes no license claim about them beyond pointing to their own
  upstream terms.
- **Trace privacy.** Captured KV/attention traces are derived from
  short, non-sensitive prompts authored specifically for this project
  (`benchmarks/kv/prompts/*.txt`) — no private, personal, or
  third-party-copyrighted text was used to generate any committed trace.
- **Benchmark generation.** All benchmark artifacts cited in this paper
  were generated by this project's own tools against its own captured
  traces or synthetic extensions thereof (see `benchmarks/MANIFEST.json`
  for the generating command of every cited artifact) — none are
  copied or adapted from a third-party benchmark suite.
- **Energy measurements.** This paper reports no energy/power numbers.
  No energy measurement, estimated or real, appears anywhere in this
  project's artifacts.
- **AI tool contribution policy (placeholder).** Most of this project's
  code, documentation, and this manuscript's text were drafted by an AI
  coding agent (Claude Code) under Kadir Eren Altıntaş's direction —
  he set the architecture, experimental design, validation criteria,
  and technical decisions, and reviewed every commit before it went in
  (see `outreach/ai-assistance-disclosure.md` for the full breakdown).
  `[placeholder: final AI-contribution disclosure wording to be
  confirmed against the target venue's specific policy at submission
  time — policies differ across venues and this project has not yet
  selected one, see `paper/submission-options.md`]`.
- **Authorship.** Kadir Eren Altıntaş is the sole human author of this
  work — the project's creator, lead, and sole owner/maintainer,
  responsible for its direction and every claim in it. No co-authors
  are listed. This is a claim about direction and ownership, not a
  claim that every line of text/code was typed by hand without AI
  assistance — see `outreach/ai-assistance-disclosure.md`.

## 10. Conclusion

MEMBRANE demonstrates that a per-block precision-plus-exact-retrieval
decision engine, calibrated from real inference traces, measures real KV
traffic reductions at a meaningful combined scale (128K context, 512
concurrency) without approximating retrieval quality — while also
demonstrating, with equal visibility, five places this specific design
does not help (§6). Read together with `paper/related-work-matrix.md`,
the honest position is: MEMBRANE contributes a disclosed, bit-exact-
verified, exact-retrieval alternative to the eviction/selection-based
mainstream of KV-cache memory management, evaluated rigorously in
simulation and at CPU-benchmark scale, but not yet at the model scale,
real-hardware realism, or production-serving integration that its
closest prior art already has.

---

## Appendix A: Reproducibility

- **Commit**: this manuscript corresponds to the state of the repository
  as of Phase 7.2; the exact commit is recorded in
  `docs/phase7-academic-paper.md` and in `paper/scripts/verify-paper.py`'s
  output.
- **Build flags**: `-DCMAKE_BUILD_TYPE=Release`, `-DMEMBRANE_ENABLE_LLAMA=ON`
  (for the ggml-parity build only), `-DMEMBRANE_ENABLE_SANITIZERS=ON` /
  `-DMEMBRANE_ENABLE_TSAN=ON` for verification builds. See
  `docs/reproduction.md`.
- **Model versions**: SmolLM2-135M-Instruct, SmolLM2-360M-Instruct
  (F16 GGUF conversions via `third_party/llama.cpp/convert_hf_to_gguf.py`).
- **Trace formats**: `.kvtrace` (raw KV capture), `.attntrace`/
  `.attntrace2` (attention capture, compact v2 encoding),
  `.attntrace3` (chunked, out-of-core, Phase 6.5) — see
  `include/membrane/attntrace3.h`.
- **Hardware/software environment**: this project's own development
  machine, 5.6 GiB RAM (a real, disclosed constraint shaping the
  out-of-core design, §3.8); gcc/clang, CMake >= 3.16, Verilator (for
  RTL cosimulation), yosys 0.33 (for synthesizability checking).
- **Full commands**: `docs/reproduction.md` (3 levels: quick
  verification, model-backed verification, full research reproduction),
  `scripts/demo.sh` (~25–50s quick check, depending on cache state), `paper/build.sh` (this
  manuscript's own build).
- **Expected runtime/RAM/disk**: Level 1 ~2-5 min; Level 2 ~30-90 min;
  Level 3 multi-hour, ~1-2 GiB transient disk, memory-budgeted (default
  768 MiB) — see `docs/reproduction.md` for exact figures per level.
- **Checkpoint/resume**: the full unified sweep supports interrupt/
  resume via a JSONL checkpoint with trace/config-hash staleness
  detection (`docs/phase6-out-of-core-simulator.md` §3-4).
- **Artifact hashes**: every cited artifact's SHA-256 is tracked in
  `benchmarks/MANIFEST.json`, checked by `scripts/verify-results.py` and
  `paper/scripts/verify-paper.py`.
- **Known platform quirks**: ThreadSanitizer + ASLR interaction on this
  kernel requires `setarch $(uname -m) -R` around the TSan test run
  (not a data race — see `docs/phase6-unified-stress.md` §13); Icarus
  Verilog 12.0 hangs on a specific Q8/Q4-decode-combined simulation
  (worked around by using Verilator instead, see
  `docs/phase5-synthesizable-fpga.md`).

Full detail beyond this appendix: `docs/reproduction.md`,
`docs/phase7-academic-paper.md`.

---

References: `paper/references.bib` (14 verified entries).
Figures: `paper/figures/README.md` and `paper/scripts/` (regenerated
from committed CSV artifacts, never hand-authored).
Tables: `paper/tables/` (regenerated by
`paper/scripts/generate-tables.py`, see `paper/scripts/README.md`).
