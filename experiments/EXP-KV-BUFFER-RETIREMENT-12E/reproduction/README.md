# Reproduction — EXP-KV-BUFFER-RETIREMENT-12E

## What's needed

1. A checkout of `third_party/llama.cpp` with `kv_type_override`,
   Phase 12B's `kv_dev_override`, and Phase 12D's `relocate_layer`
   patches already applied, then apply
   `patches/llama.cpp-membrane-kv-buffer-retirement.patch` (reproduced
   inside `patches/phase12e-kv-buffer-retirement.patch`), which adds
   `llama_invalidate_graph_cache()`.
2. Apply the accompanying change to
   `tools/membrane-kv-runtime-relocate/relocate_probe.cpp` (also inside
   `patches/phase12e-kv-buffer-retirement.patch`) that exercises the
   invalidate-then-retire path.
3. Build `build-vulkan` (Release) for the real-GPU cycle/multi-layer/
   pressure tests; the llama-free/CPU sanitizer matrix from Phase 12D
   applies unchanged here.
4. Model weights: SmolLM2-135M-Instruct and SmolLM2-360M-Instruct
   (GGUF, Q8_0/Q5_1) — not included in this repository.
5. Hardware: GTX 1650 (4096 MiB VRAM) for the pressure-boundary
   reclamation proof (ctx=200000, 360M). CPU-only control is a
   source-level argument (`results/canonical/cpu_control.json`), not
   independently live-tested — no second backend was available on this
   host's CPU-only build to relocate to/from.

## What is NOT preserved

- Model weight files.
- A debugger-verified confirmation that the two Phase 12D crash
  signatures (SIGSEGV vs `GGML_ASSERT`) were the identical underlying
  fault — only that this phase's fix (`llama_invalidate_graph_cache`)
  eliminates both in every trial run this phase.
- Live confirmation on any non-Vulkan backend.

## Classification

**PARTIALLY_REPRODUCIBLE** — patch, source, and the full retirement
test suite are complete and exact; the reclamation MiB figures are
GTX-1650-specific; CPU-backend generality rests on a source-level
argument only.
