# Claim audit

Every headline/major claim used in `paper/main.md` and `paper/main.tex`,
audited individually. `paper/scripts/verify-paper.py` re-derives what it
can automatically (see its checks); this document is the human-readable
record of the same audit, including the claims that can't be fully
automated (wording/overclaim checks).

Columns: **claim id**, **exact wording** (as used in the paper),
**claim type** (REAL/SIMULATED/EXTRAPOLATED/ORACLE/ASSUMED, per this
project's standing labeling discipline), **source artifact**,
**manifest entry**, **experiment configuration**, **limitations**,
**allowed wording**, **prohibited overclaim wording**.

---

### C-01: Unified scale (128K context × 512 concurrency)

- **Exact wording**: "a unified 128K-context × 512-concurrency
  discrete-event stress sweep"
- **Claim type**: SIMULATED
- **Source artifact**: `benchmarks/cxl-sim/unified-sweep.csv`
  (`context_tokens`, `concurrency` columns, constant at 131072/512 for
  every non-analytical row)
- **Manifest entry**: `benchmarks/cxl-sim/unified-sweep.csv` (SIMULATED, complete)
- **Experiment configuration**: `membrane-kv-exact-sim`, `UNIFIED_TARGET_STEPS`/`UNIFIED_CONCURRENCY` constants, both SmolLM2 checkpoints, out-of-core streaming backend (Phase 6.5)
- **Limitations**: discrete-event simulation calibrated from a real trace, not a real 512-way concurrent GPU serving run; single-worker/limited-worker execution constrained by a 5.6 GiB development machine
- **Allowed wording**: "simulated at 128K context and 512 concurrency", "discrete-event stress sweep"
- **Prohibited overclaim wording**: "real 512-way concurrent serving", "measured on a production server", "hardware-validated at this scale"

### C-02: 462/462 scenario completion

- **Exact wording**: "462 of 462 scenarios complete (231 SmolLM2-135M + 231 SmolLM2-360M), zero extrapolated rows"
- **Claim type**: SIMULATED (scenario completeness is a real fact about the simulator run, not a performance measurement)
- **Source artifact**: `benchmarks/cxl-sim/unified-sweep.csv`
- **Manifest entry**: `benchmarks/cxl-sim/unified-sweep.csv`
- **Experiment configuration**: verified via `scripts/verify-results.py` check "unified-sweep.csv: 462 total rows, 231/231 per model" and `membrane-kv-exact-sim-verify`
- **Limitations**: "complete" means every scenario in the defined sweep matrix ran to completion, not that the matrix covers every possible host/device/precision/comparison combination that could exist
- **Allowed wording**: "462/462 scenarios completed", "full matrix, no extrapolated rows"
- **Prohibited overclaim wording**: "exhaustive hardware coverage", "complete search of the design space"

### C-03: 187×–405× KV traffic reduction vs. full-scan-CXL (both models, representative point)

- **Exact wording**: "187x-405x reduction in mean bytes/token vs. a full-scan-CXL baseline at the representative 8GiB-host/2TiB-device point (99.6x-215.3x vs. a precision-matched compressed baseline)"
- **Claim type**: SIMULATED (135M: 187.2x-321.1x; 360M: 244.9x-404.7x — see C-04)
- **Source artifact**: `benchmarks/cxl-sim/unified-sweep.csv`
- **Manifest entry**: `benchmarks/cxl-sim/unified-sweep.csv`
- **Experiment configuration**: `host_cache_total_bytes=8589934592`, `device_total_bytes=2199023255552`, `precision=all-q8`, all 5 non-analytical comparisons + 2 analytical baselines, both models
- **Limitations**: a single representative point, not the full range across all 5 host-cache × 3 device × 3 precision combinations (see C-04 for the full spread); "full-scan-cxl" itself is an analytical, not simulated, baseline
- **Allowed wording**: "at the representative 8GiB/2TiB point", always naming which baseline (full-scan vs. compressed)
- **Prohibited overclaim wording**: stating a single number without naming the baseline it's measured against; implying this range holds at every cache/device size (it does not — see capacity accounting in `docs/phase6-unified-stress.md` §3)

### C-04: SmolLM2-360M-specific reduction figures

- **Exact wording**: "for SmolLM2-360M specifically: 244.9x-404.7x vs. full-scan-CXL, 130.2x-215.3x vs. compressed-full-scan-CXL, at the same representative point"
- **Claim type**: SIMULATED
- **Source artifact**: `benchmarks/cxl-sim/unified-sweep.csv`, filtered `model=SmolLM2-360M`
- **Manifest entry**: `benchmarks/cxl-sim/unified-sweep.csv`
- **Experiment configuration**: same as C-03, model-filtered
- **Limitations**: never to be presented merged with SmolLM2-135M's numbers (187.2x-321.1x / 99.6x-171x) as if they were one model's result or an averaged figure
- **Allowed wording**: always naming "SmolLM2-360M" explicitly when using these specific numbers
- **Prohibited overclaim wording**: presenting a 135M number as if it were 360M's or vice versa; an unlabeled combined "our best result is 405x" without naming which model

### C-05: 520,000-transaction RTL cosimulation

- **Exact wording**: "520,000 transactions cosimulated in Verilator against the real CPU quantizer reference, 0 mismatches"
- **Claim type**: REAL (an actual program execution on this machine; the RTL itself is not on physical silicon — see C-05's limitations)
- **Source artifact**: `rtl/tb/tb_top_verilator.cpp` (constants `N_PER_MODE=120000`, `N_MIX=40000`, `4*120000+40000=520000`); run log reproduced by `scripts/demo.sh` and `docs/reproduction.md` Level 1.4
- **Manifest entry**: none — this is a source-code-verified constant plus a live run log, not a checked-in CSV artifact; verified instead by `scripts/verify-results.py`'s direct inspection of `tb_top_verilator.cpp`
- **Experiment configuration**: `membrane_quant_stream_top.sv` full pipeline, all four Q8/Q4 encode/decode modes plus mixed-mode interleave, randomized valid/ready backpressure, golden vectors from `src/quant/quant_simd.c`
- **Limitations**: cosimulation, not silicon; confirms functional/bit-exact correctness, not real Fmax/power/area
- **Allowed wording**: "520,000-transaction Verilator cosimulation", "cosimulated against the CPU reference"
- **Prohibited overclaim wording**: "FPGA-validated", "hardware-proven", "run on an FPGA", "real accelerator throughput"

### C-06: Quantization parity vector count (100,000+)

- **Exact wording**: "100,000+ randomized blocks plus every documented edge case, 0 mismatches vs. ggml's own Q8_0/Q4_0 reference"
- **Claim type**: REAL (an actual program execution linking the real ggml CPU backend)
- **Source artifact**: `tests/unit/test_ggml_quant_parity.c`
- **Manifest entry**: none — source-code-verified constant + CI/local run log (same reasoning as C-05)
- **Experiment configuration**: `-DMEMBRANE_ENABLE_LLAMA=ON` build, `membrane_ggml_quant` linked directly against `ggml`, edge cases: all-zero, constant, extrema, NaN/Inf, denormal
- **Limitations**: parity is checked against ggml's CPU backend specifically (not GPU/CUDA backends, which this project has not built or tested)
- **Allowed wording**: "bit-exact vs. ggml's CPU Q8_0/Q4_0 reference"
- **Prohibited overclaim wording**: "hardware-independent", "verified on GPU", "matches every ggml backend"

### C-07: FPGA pipeline-count sensitivity (25.7x)

- **Exact wording**: "dropping quant-pipeline count from 8 to 1 inflates p99 latency by 25.7x (15.67ms -> 402.1ms)"
- **Claim type**: SIMULATED
- **Source artifact**: `benchmarks/cxl-sim/unified-sweep-hardware-sensitivity.csv`
- **Manifest entry**: `benchmarks/cxl-sim/unified-sweep-hardware-sensitivity.csv`
- **Experiment configuration**: SmolLM2-135M, `exact-predictor-prefetch`, all-Q8, 256MiB host / 1TiB device, `pipelines-1` vs. `pipelines-8 (default)` rows
- **Limitations**: 135M-only by original design (this matrix isolates hardware sensitivity, not per-model behavior); pipeline count is a simulated quant-engine parameter, not a measurement from real quant-engine silicon
- **Allowed wording**: "simulated pipeline-count sensitivity", always naming SmolLM2-135M
- **Prohibited overclaim wording**: extending this ratio to SmolLM2-360M without re-deriving it; implying a real FPGA quant engine was benchmarked at 1 vs. 8 pipelines

### C-08: 10ms p99 bound failure

- **Exact wording**: "0 of 225 real (non-analytical) rows meet a 10ms p99 target, for either model"
- **Claim type**: SIMULATED
- **Source artifact**: `benchmarks/cxl-sim/unified-sweep.csv`
- **Manifest entry**: `benchmarks/cxl-sim/unified-sweep.csv`
- **Experiment configuration**: full 462-row matrix, `p99_latency_ns` column vs. 10,000,000ns bound, both models, all real rows
- **Limitations**: the 10ms figure is this project's own chosen illustrative target, not an externally mandated SLA; failure is dominated by the model's own compute floor (see C-09), not retrieval quality
- **Allowed wording**: "does not meet a 10ms p99 target in any real scenario, for either model"
- **Prohibited overclaim wording**: implying the bound was close to being met, or that a different retrieval policy would have met it (root cause is compute floor, disclosed in C-09)

### C-09: CPU/model compute floor as the dominant latency term

- **Exact wording**: "the model's own real decode compute floor (15.67ms/token for SmolLM2-135M, ~41.0ms/token for SmolLM2-360M) already exceeds the 10ms bound before any KV-retrieval latency is added"
- **Claim type**: REAL (`SMOLLM2_135M_TOK_PER_SEC`/`SMOLLM2_360M_TOK_PER_SEC` are measured real decode speeds) combined with SIMULATED (`model_compute_floor_ns` as used in the sweep)
- **Source artifact**: `benchmarks/cxl-sim/unified-sweep.csv` (`model_compute_floor_ns`, `hidden_under_compute_fraction` columns)
- **Manifest entry**: `benchmarks/cxl-sim/unified-sweep.csv`
- **Experiment configuration**: compute floor is a per-model constant derived from real measured tokens/sec; `hidden_under_compute_fraction` is real per-row simulated data, 124 SmolLM2-360M rows only (pre-Phase-6.5 135M rows predate this column and are `n/a`, not zero)
- **Limitations**: `hidden_under_compute_fraction` is `n/a` (not populated) for all 231 SmolLM2-135M rows and 107 pre-6.5 SmolLM2-360M rows — must not be described as "measured for both models across all rows"
- **Allowed wording**: "for the 124 SmolLM2-360M rows where this metric is populated"
- **Prohibited overclaim wording**: describing `hidden_under_compute_fraction` as available for SmolLM2-135M or for all 360M rows

### C-10: Approximate KV eviction recall failure

- **Exact wording**: "naive approximate eviction policies show real, measured recall failures on recall-shaped prompts, as low as 0.04 top-1 match rate"
- **Claim type**: REAL (real captured attention traces + real prompt categories, though the eviction policies themselves are simulated replay, not a live serving system)
- **Source artifact**: `docs/phase6-attention-working-set.md` §7 (no separate CSV artifact tracked in the manifest for this specific figure — sourced to the doc's own reported table, which cites `benchmarks/cxl-sim/workingset-sweep.csv`-family data)
- **Manifest entry**: `benchmarks/cxl-sim/workingset-sweep.csv` / `benchmarks/cxl-sim/workingset-context-sweep.csv` (both SIMULATED, complete)
- **Experiment configuration**: `sliding-window+sink` and a recency-biased predictive policy, 5 real prompt categories (recall/distractor/code/natural/longcontext, `benchmarks/kv/prompts/*.txt`)
- **Limitations**: two specific approximate policies tested, not an exhaustive survey of all possible eviction heuristics; "0.04" is the worst observed point, not a typical value
- **Allowed wording**: "a real, measured recall failure mode for the two tested approximate eviction policies"
- **Prohibited overclaim wording**: "all eviction policies fail this way", "eviction is always unsafe"

### C-11: Micro-batching null result

- **Exact wording**: "micro-batching plus coalescing shows no measurable throughput or latency benefit at this project's calibrated real demand levels"
- **Claim type**: SIMULATED
- **Source artifact**: `docs/phase6-cxl-near-memory.md` §8, `docs/phase6-exact-sparse-retrieval.md` §8 (comparison-table rows, not a single dedicated CSV column)
- **Manifest entry**: `benchmarks/cxl-sim/sweep-report.csv` (Phase 6.1's own sweep, SIMULATED, complete)
- **Experiment configuration**: micro-batching + coalescing enabled vs. disabled, at demand levels calibrated from real captured attention traces (not an artificially overloaded synthetic demand)
- **Limitations**: null result specific to the calibrated demand levels actually tested; does not rule out a benefit at a different (untested) demand level or hit-rate regime
- **Allowed wording**: "no measurable benefit at the demand levels this project actually calibrated and tested"
- **Prohibited overclaim wording**: "micro-batching never helps", "proven ineffective in general"

---

## Prohibited overclaim phrase list (project-wide, not just the claims above)

Checked automatically by `paper/scripts/verify-paper.py`; each requires a
specific allowlisted context (e.g., explicitly negating the claim, as in
"is not production-ready") to appear at all:

`first`, `novel`, `unprecedented`, `production`, `production-ready`,
`real CXL acceleration`, `hardware-proven`, `state-of-the-art` (without a
named, cited comparison), `best-in-class`, `outperforms all`.
