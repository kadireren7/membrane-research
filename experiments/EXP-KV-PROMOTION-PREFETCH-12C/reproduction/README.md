# Reproduction — EXP-KV-PROMOTION-PREFETCH-12C

## What's needed

1. A checkout of `third_party/llama.cpp` with the pre-existing
   `kv_type_override` patch plus Phase 12B's
   `patches/llama.cpp-membrane-kv-device-override.patch` already
   applied (see [EXP-KV-DEVICE-OVERRIDE-12B/reproduction](../EXP-KV-DEVICE-OVERRIDE-12B/reproduction/README.md)) —
   this phase adds no further llama.cpp source changes of its own.
2. Apply `patches/phase12c-kv-promotion-prefetch.patch` on top (the
   new `tools/membrane-kv-promotion-prefetch/` tree, `CMakeLists.txt`
   wiring, `scripts/verify-results.py` extension).
3. Build matrix exercised this phase: `build-vulkan` (Release, real GPU
   probe runs), `build-free-asan` (llama-free, Debug+ASan+UBSan),
   `build-cpu-rc1` (`GGML_VULKAN=OFF`, Release), `build-llama-asan`
   (`GGML_VULKAN=OFF`, Debug+ASan+UBSan). No Vulkan+ASan/UBSan
   combination was attempted — disclosed resource-risk limitation on
   the 5.6 GiB RAM research host (see `results/canonical/summary.json`'s
   `sanitizer_limitation` field).
4. Model weights: SmolLM2-135M-Instruct and SmolLM2-360M-Instruct
   (GGUF, Q8_0/Q5_1) — not included in this repository.
5. Hardware: GTX 1650 (4096 MiB VRAM) for every real-copy/VRAM/latency
   number.

## What is NOT preserved

- Model weight files.
- Exact host driver/runtime versions beyond `summary.json`'s prose.
- A CI harness — manual execution on a single shared research host.
- The one untested failure scenario (interrupted copy mid-transfer)
  has no fault-injection mechanism in this prototype at all — it was
  never exercised, not merely unrecorded.

## Classification

**PARTIALLY_REPRODUCIBLE** — patch and source are complete and exact;
the poison-test methodology is fully specified and would be expected
to reproduce the same qualitative result (static rebind) on any
llama.cpp version where `llama_kv_cache::layers` remains private and
write-once. Exact latency/stall numbers are GTX-1650-specific.
