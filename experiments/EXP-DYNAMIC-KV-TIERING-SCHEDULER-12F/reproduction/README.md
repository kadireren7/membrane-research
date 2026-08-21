# Reproduction — EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F

## What's needed

1. A checkout of `third_party/llama.cpp` with all of Phases 12B/12D/12E's
   llama.cpp patches applied (`kv_dev_override`, `relocate_layer`,
   `llama_invalidate_graph_cache`) — this phase adds **no further
   llama.cpp source changes**; `scheduler_core.c` is pure, llama-free
   policy logic, and `tiering_probe.cpp` only calls existing public
   `llama.h` primitives.
2. Apply `patches/phase12f-dynamic-kv-tiering-scheduler.patch` (the new
   `tools/membrane-kv-tiering-scheduler/` tree, `CMakeLists.txt`
   wiring, `scripts/verify-results.py` extension).
3. Build `build-vulkan` (Release) for the real scheduler-driven decode
   runs; `test_scheduler_core` (llama-free) runs on any build.
4. Model weights: SmolLM2-135M-Instruct and SmolLM2-360M-Instruct
   (GGUF, Q8_0/Q5_1) — not included in this repository.
5. Hardware: GTX 1650 (4096 MiB VRAM). The pressure-regime test used
   ctx=65536 (not the originally suggested ~200000) due to real,
   disclosed host RAM/swap pressure from unrelated concurrent processes
   at test time (`results/canonical/pressure_test.json`) — this is a
   host-state-dependent deviation, not necessarily reproducible at the
   same context on a different host state.

## What is NOT preserved

- Model weight files.
- The exact concurrent host load present during the pressure-regime
  test.
- Any async-prefetch variant of `relocate_layer` — analyzed as
  infeasible with the established primitive chain, never implemented.

## Classification

**PARTIALLY_REPRODUCIBLE** — patch, source, and the full scheduler
test/benchmark suite are complete and exact. The central throughput
finding (no measured advantage) is scale/hardware-specific to
SmolLM2-135M/360M on a GTX 1650; the mechanism's correctness/
determinism/stability findings are expected to reproduce on comparable
hardware.
