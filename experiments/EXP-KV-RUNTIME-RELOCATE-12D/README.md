# EXP-KV-RUNTIME-RELOCATE-12D — Runtime KV relocate/rebind primitive (works, but leaks)

Phase 12D of the MEMBRANE Phase 12B–12G KV residency research chain.
Builds the exact primitive [EXP-KV-PROMOTION-PREFETCH-12C](../EXP-KV-PROMOTION-PREFETCH-12C/README.md)
specified but did not write.

**Previous:** [EXP-KV-PROMOTION-PREFETCH-12C](../EXP-KV-PROMOTION-PREFETCH-12C/README.md)
**Next:** [EXP-KV-BUFFER-RETIREMENT-12E](../EXP-KV-BUFFER-RETIREMENT-12E/README.md)

## Question

Can runtime KV backing actually be rebound/relocated — i.e. does the
LIVE decode graph, on the very next call, read the newly-relocated
data rather than the original?

## What this phase did

A small (173 insertion lines, 0 deletions, 4 files), additive llama.cpp
patch adds `bool llama_kv_cache::relocate_layer(int32_t il, ggml_backend_buffer_type_t new_buft, bool retain_old_backing_for_research = false)`
plus a thin C-API wrapper. Every relocation allocates a brand-new,
dedicated `(ggml_context, ggml_backend_buffer_t)` pair — never a slice
of the shared construction-time bulk buffer other layers may still
occupy — copies real bytes, and replaces `layers[il].k`/`.v`.

Correctness is established by a **ground-truth poison test**: after
relocation, decode is checked to confirm it reads the NEW backing, not
a stale one — the same falsifiable methodology Phase 12C used to
disprove live consumption, now used to prove it.

## Result — this is a real negative/partial result, disclosed in full

- **Runtime rebind is genuinely real**: poison testing proves decode
  consumes the newly-rebound backing, in both directions (CPU→GPU,
  GPU→CPU), repeatedly for the same layer, across both models and both
  KV precisions (`results/canonical/single_cpu_gpu.json`,
  `single_gpu_cpu.json`, `poison_test.json`,
  `repeated_same_layer_retained.json`). Weight placement is unaffected
  by KV relocation (`results/canonical/weights_unchanged.json`).
- **Old backing cannot yet be safely freed.** With
  `retain_old_backing_for_research=false` (release enabled, the real
  default), a SECOND relocation of the SAME layer reproducibly crashes
  the NEXT `llama_decode()` call. **Two distinct crash signatures
  observed**: SIGSEGV (this session's reproducer, reproducible across
  repeated trials and a 4-decode settling-window variant) and
  `GGML_ASSERT(buffer != nullptr)` at `ggml-vulkan.cpp:7815`
  `ggml_vk_tensor_subbuffer` (an earlier session, different call
  pattern). A third, structurally similar call pattern (`roundtrip`)
  did NOT crash across 4 repeated runs — disclosed honestly as
  **non-uniform, call-pattern-dependent**, not as evidence that
  pattern is safe (`results/canonical/release_failure.json`).
- **This finding is preserved exactly as evidenced, not softened.**
  The phase's own `explicit_non_actions_confirmed` field records
  `"verdict_not_softened_toward_viable": true`.
- **What remains unknown, disclosed honestly**: the exact internal
  owner of the stale reference was not conclusively confirmed within
  this phase's scope (candidates considered: `ggml_backend_sched`'s
  `prev_*_backend_ids` arrays, Vulkan buffer-pool reuse — neither
  confirmed); whether the crash is Vulkan-specific was not tested (no
  second backend available on this host's CPU-only build); whether
  some other release strategy (deferred/batched release) could be safe
  was not exhaustively searched.
- **Given this closing checkpoint's scope**, the original Section 7–30
  benchmark matrix (context matrix, dedicated pressure-regime test,
  cycle stability, latency breakdown, state save/load, defrag/shift
  interaction) was deliberately skipped once release was shown unsafe
  — none of it could change the verdict.

## Verdict

Literal source field (`results/canonical/summary.json`, `decision_gate`):

> **`RUNTIME_KV_RELOCATE_WORKS_BUT_LEAKS_BACKING`**

Meaning (same file, `decision_gate_meaning`): runtime rebind itself is
functional and proven by ground-truth poison testing; old backing
cannot currently be safely released — retaining old buffers avoids the
crash but defeats true dynamic-memory reclamation. Not yet suitable for
real tiering.

## Productization

**No.** Verified: `git grep` for `relocate_layer` /
`llama_kv_cache_relocate_layer` against product `main` returns no
hits. This mechanism, crash included, remains entirely research-only.
It was never a productization candidate on its own — it directly
motivated [EXP-KV-BUFFER-RETIREMENT-12E](../EXP-KV-BUFFER-RETIREMENT-12E/README.md)'s
fix.

## Contents

- `results/canonical/` — 32 artifacts: architecture/root-cause
  investigation notes, single-direction relocation results, repeated-
  same-layer behavior, the release-failure crash evidence (including
  raw stderr/partial-stdout captures), failure atomicity, Q5
  compatibility, second-model reproduction, different-layer control,
  sanitizer matrix, maintenance assessment, and raw per-run captures.
- `patches/phase12d-kv-runtime-relocate.patch` — incremental
  `git diff`, **Phase 12C HEAD (`73090ef`) → this phase's HEAD
  (`9c99eea`)**: the new `tools/membrane-kv-runtime-relocate/` tool
  tree, the llama.cpp-facing
  `patches/llama.cpp-membrane-kv-runtime-relocate.patch`, its
  `CMakeLists.txt` wiring, and this phase's `scripts/verify-results.py`
  extension. Excludes `results/phase12/runtime-relocate/` (preserved
  separately above).
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/kv-runtime-relocate`
- Source branch HEAD: `9c99eead15492b9aed1e46bb9775c78728848e35`
- Base commit: Phase 12C HEAD, `73090efff08fa37627c1a5bf4e0be8480c700576`
- Date: 2026-08-18
- Chain position: 3rd of 6 in the Phase 12B–12G chain
- Productized: **No** — remains research-only, crash disclosed
- Full machine-readable record: `MANIFEST.json`
