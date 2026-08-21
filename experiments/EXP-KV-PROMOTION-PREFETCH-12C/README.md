# EXP-KV-PROMOTION-PREFETCH-12C — Controlled KV promotion/prefetch prototype

Phase 12C of the MEMBRANE Phase 12B–12G KV residency research chain.
Builds directly on [EXP-KV-DEVICE-OVERRIDE-12B](../EXP-KV-DEVICE-OVERRIDE-12B/README.md)'s
`kv_dev_override` patch (verified present and unchanged this phase:
verifier 75/75, `main` and the v0.3.0-rc1 release both confirmed
untouched).

**Previous:** [EXP-KV-DEVICE-OVERRIDE-12B](../EXP-KV-DEVICE-OVERRIDE-12B/README.md)
**Next:** [EXP-KV-RUNTIME-RELOCATE-12D](../EXP-KV-RUNTIME-RELOCATE-12D/README.md)

## Question

Can a live KV tensor be copied between CPU and GPU backing after
context construction, does the copy stay correct, and — critically —
does the live decode graph actually consume the relocated copy, or
only the placement decided at construction time?

**This phase does not claim to achieve dynamic tiering.** It is
scoped strictly to answering whether copying alone is sufficient.

## What this phase did

`tools/membrane-kv-promotion-prefetch/promotion_probe.cpp` performs a
real single-tensor `ggml_backend_tensor_get`/`set` copy of a layer's
K or V cache between CPU and GPU backing (reusing the mechanism Phase
12A proved safe), then runs a **poison test**: after the copy, the
original tensor's bytes are corrupted and generation is checked — if
output is unaffected, the live graph is proven to still be reading the
original, not the copy. `tools/membrane-kv-promotion-prefetch/ownership_state.c`
is a llama-free state machine modeling the *idealized* tiering system
this prototype does not yet achieve, kept separate and testable from
what the real integration can currently deliver.

## Result

- **Copy mechanically works, is correct, and is fast**: byte
  round-trip verified in every trial (single-layer promotion/demotion,
  an 8-step multi-layer schedule, both models, both KV precisions, the
  real ctx=200000 pressure boundary); sub-millisecond to low-millisecond
  latency for 1–8 real KV-layer-sized groups; <2% wall-clock slowdown
  even for 32 repeated sync promotions across 8 layers during 32-token
  generation (`results/canonical/single_layer_promotion.json`,
  `copy_microbench.json`, `sync_stall.json`).
- **The rebind is static — proven, not inferred**: the poison test
  **falsifies live consumption in every single trial** across both
  models, both contexts tested, and both KV precisions
  (`results/canonical/single_layer_promotion.json`,
  `single_layer_demotion.json`). Every promotion/demotion this phase
  performs is a real, correct copy the decode graph never reads —
  classification `COPY_WORKS_BUT_PLACEMENT_IS_STATIC`.
- **Exact source-level cause found**: reading `llama-kv-cache.h`/`.cpp`
  directly (not inferring from comments) shows the graph already
  re-resolves `layers[ikv].k/.v` fresh on every `llama_decode()` call
  (so no new graph-rebuild primitive is needed to *consume* a rebound
  tensor) — but `llama_kv_cache::layers` is `private`, written once at
  construction (`layers.push_back(...)`), and never written to again
  anywhere in the file. The missing piece is narrower than a rebuild:
  simply mutating that pointer between decode calls would be
  sufficient. **The exact missing primitive was specified but
  deliberately not written this phase**: a
  `relocate_layer(il, new_buft)`-style method
  (`results/canonical/ownership_model.json`).
- **No VRAM savings claimed**: every promotion is a genuine duplicate
  for its lifetime; scratch release correctly returns external
  `nvidia-smi` VRAM to baseline, but this is release of the *probe's*
  scratch tensor, not of `llama_kv_cache`'s own authoritative tensor
  (`results/canonical/vram_accounting.json`).
- **Generality**: 50/50 repeated alloc/copy/free cycles stable, no
  RSS/VRAM creep; 6/7 fault scenarios fail closed (1 untested,
  disclosed, not claimed-passing); reproduced on SmolLM2-360M
  (`results/canonical/cycle_stability.json`, `failure_cases.json`).
- **Two real product-adjacent bugs found and fixed this phase**:
  (1) `cmd_info`/`cmd_move` read tensor pointers before ever decoding,
  always reporting `MISSING`; (2) GPU-device acquisition sites had no
  NULL check and `SIGABRT`ed on a CPU-only build instead of failing
  closed — both fixed and verified (`summary.json:bugs_found_and_fixed_this_phase`).

## Verdict

Literal source field (`results/canonical/summary.json`, `decision_gate`):

> **`LIVE_KV_COPY_WORKS_BUT_REBIND_STATIC`**

("Live rebind classification: B — `COPY_WORKS_BUT_PLACEMENT_IS_STATIC`")

## Productization

**No.** No code from this branch exists on product main. This phase's
value is entirely research-lineage: it proved the copy primitive safe
and precisely located (by source trace) the exact missing rebind
primitive that [EXP-KV-RUNTIME-RELOCATE-12D](../EXP-KV-RUNTIME-RELOCATE-12D/README.md)
went on to build. Do not attribute Phase 12D/12E's graph-invalidation
solution to this phase — this phase only identified the gap, it did
not solve it.

## Contents

- `results/canonical/` — 34 artifacts: ownership model and live-rebind
  classification, single-layer promotion/demotion with poison tests,
  multi-layer schedule, copy microbenchmark, sync-stall cost, async
  prefetch feasibility, VRAM accounting, cycle stability, failure
  cases, the GTX-1650 pressure test, headroom rule audit, Q5
  orthogonality, second-model reproduction, and raw per-run captures.
- `patches/phase12c-kv-promotion-prefetch.patch` — incremental
  `git diff`, **Phase 12B HEAD (`a1ebd9e`) → this phase's HEAD
  (`73090ef`)**: the new `tools/membrane-kv-promotion-prefetch/` tool
  tree, its `CMakeLists.txt` wiring, and this phase's
  `scripts/verify-results.py` extension. Excludes
  `results/phase12/promotion-prefetch/` (preserved separately above).
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/kv-promotion-prefetch`
- Source branch HEAD: `73090efff08fa37627c1a5bf4e0be8480c700576`
- Base commit: Phase 12B HEAD, `a1ebd9e285bd763cfb448c8d8202536802783bb9`
- Date: 2026-08-17
- Chain position: 2nd of 6 in the Phase 12B–12G chain
- Productized: **No** — see Productization above
- Full machine-readable record: `MANIFEST.json`
