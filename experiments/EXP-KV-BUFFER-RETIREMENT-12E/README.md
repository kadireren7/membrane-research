# EXP-KV-BUFFER-RETIREMENT-12E — Safe KV buffer retirement

Phase 12E of the MEMBRANE Phase 12B–12G KV residency research chain.
Finds the exact root cause of, and fixes,
[EXP-KV-RUNTIME-RELOCATE-12D](../EXP-KV-RUNTIME-RELOCATE-12D/README.md)'s
release-crash.

**Previous:** [EXP-KV-RUNTIME-RELOCATE-12D](../EXP-KV-RUNTIME-RELOCATE-12D/README.md)
**Next:** [EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F](../EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F/README.md)

## Question

Can a relocated KV layer's old backing be retired (freed) safely, and
if so, under what exact rule?

## Root cause (source-traced, not guessed)

The exact failing tensor was identified: `cache_v_l4 (view)`, the
`cpy_v()` write-path node for layer 4's V cache, sourced from a graph
that was **not rebuilt** for the crashing decode — its `tensor->buffer`
pointed at an already-freed `ggml_backend_buffer` struct
(`results/canonical/failing_tensor_trace.json`).

Two candidates were investigated and **ruled out directly**, not by
assumption:

- `ggml_backend_sched`'s own state — correct and irrelevant; on a
  graph-*reuse* decision, `ggml_backend_sched_split_graph` is never
  even called for that decode (`results/canonical/scheduler_lifetime.json`).
- Vulkan's `ggml_tensor::extra` — the Vulkan backend does not use it
  for tensor/buffer identity at all (only one unrelated debug-print
  reference in the entire ~18000-line `ggml-vulkan.cpp`); buffer
  identity is resolved fresh from `tensor->buffer->context` on every
  use (`results/canonical/vulkan_extra_lifetime.json`).

**The real owner**: `llama_context::gf_res_prev` — a cached
`llm_graph_result` from the LAST graph build, kept alive and
**re-executed unchanged** whenever `res->can_reuse(gparams)` returns
`true` (`llama-context.cpp:1336`), which happens on essentially every
same-shape autoregressive decode step. This reuse path skips
`cpy_v`/`get_v` (so it never picks up a relocated layer's new
placement) and skips `ggml_backend_sched_reset` (so nothing forces
recomputation either) (`results/canonical/view_lifetime.json`).

## What this phase did

A small (145 insertions / 6 deletions, 5 files), additive llama.cpp
patch adds `llama_invalidate_graph_cache(ctx)` — call it once, any time
after one or more `relocate_layer()` calls that changed KV backend
placement, and before the next `llama_decode()`. No delay, no
generation counting, no additional reference counting.

## Result

- **Immediate retirement is safe** — no delay/generation-queue
  mechanism required, collapsing the original A–H timing experiment
  space to one clean answer (`results/canonical/retirement_timing.json`).
- **50/50 cycles** (layer 4, starts CPU) and **20/20 cycles** (layer
  20, starts GPU) — real free every cycle, no crash, no assertion, no
  VRAM/RSS creep, correct decode output every cycle
  (`results/canonical/cycle_stability.json`).
- **Real, external, measured memory reclamation**: 6/9/15/28 MiB
  reclaimed for 1/2/4/8-layer groups at ctx=8192/135M; **132 MiB
  reclaimed at the real ctx=200000/360M GTX-1650 pressure boundary**
  — first true dynamic-memory-reclamation proof at that scale (3107 →
  3237 → 3105 MiB: static split → promote → decode → demote → retire →
  VRAM genuinely returns) (`results/canonical/memory_reclamation.json`,
  `pressure_test.json`).
- **Multi-layer groups** (1/2/4/8 layers) promote+retire cleanly with a
  single `invalidate_graph_cache` call per batch
  (`results/canonical/multi_layer.json`).
- **Both directions confirmed safe with the fix** (CPU-start and
  GPU-start layers) (`results/canonical/backend_direction.json`).
- **7 failure-atomicity cases** (4 carried from Phase 12D plus 3
  retirement-specific), all fail closed, no half-rebound state, no
  double-free (`results/canonical/failure_atomicity.json`).
- **Disclosed sharp edge**: the research-only deferred-retirement path
  (`defer_retirement_for_research`/`retire_layer_backing`) cannot
  actually retire a layer's FIRST relocation (its old backing is the
  shared bulk buffer) — the validated real fix does not use this path,
  so this limitation does not affect the adopted solution.

## Verdict

Literal source field (`results/canonical/summary.json`, `decision_gate`):

> **`KV_BUFFER_RETIREMENT_VIABLE`**

Meaning (same file, `decision_gate_meaning`): the exact/sufficient
lifetime boundary was identified; old backing can be safely freed,
immediately, no delay needed; repeated relocation+release is stable
across 50+ cycles and multi-layer groups; real memory reclamation is
proven, including at the real hardware pressure boundary. The fix is
small and reuses an existing, precedented internal llama.cpp mechanism.

## Productization

**No.** Verified: `git grep` for `llama_invalidate_graph_cache` /
`retire_layer_backing` against product `main` returns no hits. This
remains research-only. This phase completed the technical proof that
dynamic KV movement CAN be made memory-safe end-to-end — but
[EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F](../EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F/README.md)
and [EXP-KV-PLACEMENT-BOTTLENECK-12G](../EXP-KV-PLACEMENT-BOTTLENECK-12G/README.md)'s
findings are what determined it was not worth shipping (no measured
performance win at tested scale) — this phase's mechanism is not
present anywhere in product `main`.

## Contents

- `results/canonical/` — 26 artifacts: the exact failing-tensor trace,
  view-lifetime and scheduler-lifetime source investigation (the root
  cause), retirement strategy/timing, memory reclamation, cycle
  stability (both directions), multi-layer and pressure-regime
  results, CPU-control argument, failure atomicity, maintenance
  assessment, and raw per-run captures.
- `patches/phase12e-kv-buffer-retirement.patch` — incremental
  `git diff`, **Phase 12D HEAD (`9c99eea`) → this phase's HEAD
  (`73ae8ba`)**: the new
  `patches/llama.cpp-membrane-kv-buffer-retirement.patch`, this
  phase's `results/canonical`-adjacent `verify-results.py` extension,
  and the modification to `tools/membrane-kv-runtime-relocate/relocate_probe.cpp`
  needed to exercise the new invalidate-then-retire path. Excludes
  `results/phase12/buffer-retirement/` (preserved separately above).
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/kv-buffer-retirement`
- Source branch HEAD: `73ae8ba12b9ce477d33ca407b9923f5d826e20b1`
- Base commit: Phase 12D HEAD, `9c99eead15492b9aed1e46bb9775c78728848e35`
- Date: 2026-08-19
- Chain position: 4th of 6 in the Phase 12B–12G chain
- Productized: **No** — see Productization above
- Full machine-readable record: `MANIFEST.json`
