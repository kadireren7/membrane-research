# Reproduction — EXP-KV-PLACEMENT-BOTTLENECK-12G

## What's needed

1. A checkout of `third_party/llama.cpp` with all of Phases 12B/12D/12E's
   llama.cpp patches applied — this phase adds **no further llama.cpp
   source changes** (confirmed: `manifest.json:no_llama_cpp_source_changes: true`).
2. Apply `patches/phase12g-kv-placement-bottleneck.patch` (the new
   `tools/membrane-kv-bottleneck-analysis/` module, the new
   `bottleneck` mode on `tiering_probe.cpp`, `scripts/phase12g_*.py`
   sweep-automation scripts, `CMakeLists.txt` wiring,
   `scripts/verify-results.py` extension).
3. Build `build-vulkan` (Release) for the real sweep; `test_bottleneck_analysis`
   (llama-free) runs on any build.
4. Model weights: SmolLM2-135M-Instruct, SmolLM2-360M-Instruct, and
   qwen2.5-1.5b-instruct (fp16, quantized to Q8_0/Q5_1 at load) — not
   included in this repository.
5. Hardware: GTX 1650 (4096 MiB VRAM), single host, real-time host
   noise gating applied per-batch (`results/canonical/benchmark_validity.json`).
   Wall-clock for the full sweep as run: ~1202 seconds.

## What is NOT preserved

- Model weight files.
- The exact concurrent host load present during each of the 16
  benchmark batches (only the before/after `mem_available`/`swap_used`/
  `load1` snapshots used for validity gating are preserved, in
  `benchmark_validity.json`).
- A dedicated max-context/VRAM-headroom experiment at the true GTX-1650
  pressure boundary within this phase's own sweep — the capacity claim
  in `README.md` explicitly draws on Phases 12B/12C/12E's separate
  pressure-test evidence rather than re-deriving it here.

## Classification

**PARTIALLY_REPRODUCIBLE** — patch, source, and the full sweep
automation are complete and exact; the specific tok/s and VRAM figures
are GTX-1650-specific and depend on the exact quantized model files
(not preserved). The qualitative finding (no tested regime clears the
predeclared 10% bottleneck threshold; CPU-KV at parity or faster) would
be expected to reproduce on comparable hardware at comparable model/
context scales.
