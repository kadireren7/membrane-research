# Reproduction — EXP-KV-RUNTIME-RELOCATE-12D

## What's needed

1. A checkout of `third_party/llama.cpp` with the `kv_type_override`
   patch, Phase 12B's `kv_dev_override` patch, and Phase 12C's tool
   tree already in place, then apply
   `patches/llama.cpp-membrane-kv-runtime-relocate.patch` (reproduced
   inside `patches/phase12d-kv-runtime-relocate.patch`).
2. Build `tools/membrane-kv-runtime-relocate/` — `build-vulkan` for
   the real-GPU relocate probe (including the crash reproducer),
   `build-free-asan`/`build-llama-asan` for the llama-free/CPU-only
   sanitizer matrix (see `results/canonical/sanitizers.json`).
3. **The crash itself is expected to reproduce**: running
   `relocate_probe` with `retain_old_backing_for_research=false` and
   relocating the same layer a second time is expected to crash the
   next decode call — this is the phase's central, disclosed finding,
   not a build error.
4. Model weights: SmolLM2-135M-Instruct and SmolLM2-360M-Instruct
   (GGUF, Q8_0/Q5_1) — not included in this repository.
5. Hardware: GTX 1650 (4096 MiB VRAM) — the only backend this crash was
   observed on; not tested on CPU-only (no second backend to relocate
   to/from on this host's CPU-only build).

## What is NOT preserved

- Model weight files.
- A debugger-verified root-cause trace distinguishing the two observed
  crash signatures (SIGSEGV vs `GGML_ASSERT`) as the identical fault —
  only circumstantial consistency (always the decode immediately
  following a second same-layer relocation with release enabled) is
  recorded.
- The original Section 7–30 benchmark matrix (context matrix, dedicated
  pressure test, cycle stability, latency breakdown, state save/load,
  defrag/shift interaction) — deliberately not collected this closing
  checkpoint once the verdict was conclusive without it.

## Classification

**PARTIALLY_REPRODUCIBLE** — the patch, source, and the crash
reproducer are complete; the crash itself is Vulkan-observed only, and
whether it is backend-generic was not tested. Model weights and exact
driver versions are not preserved.
