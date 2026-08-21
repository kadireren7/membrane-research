# EXP-KV-DEVICE-OVERRIDE-12B — Minimal KV device override patch

Phase 12B of the MEMBRANE Phase 12B–12G KV residency research chain
(12B → 12C → 12D → 12E → 12F → 12G, each a superset of its predecessor).
This phase built the exact fix Phase 12A's own findings identified as
missing: a minimal llama.cpp patch decoupling KV cache device placement
from model weight device placement.

**Previous:** [EXP-KV-RAM-VRAM-TIERING-12A](../EXP-KV-RAM-VRAM-TIERING-12A/README.md) (independent branch, precedes and motivated this chain, not an ancestor of it)
**Next:** [EXP-KV-PROMOTION-PREFETCH-12C](../EXP-KV-PROMOTION-PREFETCH-12C/README.md)

## Question

Can a llama.cpp KV cache layer be placed on a different backend device
than that layer's own model weights, independently and controllably?

## What this phase did

A minimal (68 insertions / 7 deletions, 6 files) additive llama.cpp
patch adds a `kv_dev_override` callback —
`ggml_backend_dev_t (*)(int32_t il, ggml_backend_dev_t default_dev, void *user_data)`
— queried once per layer at context-construction time, mirroring the
existing `kv_type_override` extension point's shape and threading
pattern exactly. Returning `NULL` (or leaving the field unset) falls
back to `default_dev`, verified byte-identical to unpatched behavior.

A llama-free pure-logic module (`tools/membrane-kv-device-override/split_policy.c`)
implements the deterministic split decision
(`layers [0, split) → CPU KV, [split, n_layer) → GPU KV`) used by the
real-GPU integration probe (`device_override_probe.cpp`), kept testable
without a model or GPU.

## Result

- **Default-behavior regression**: clean. Product CLI smoke test
  (native/q8/q5/adaptive) all exit 0, byte-identical
  `kv_allocated_bytes`/`estimated_model_bytes`/`gpu_layers_selected`
  vs. pre-patch baseline. An unset callback, an always-NULL callback,
  and an always-default-choosing callback proved exactly equivalent
  (`results/canonical/default_regression.json`).
- **Per-layer placement proof**: SmolLM2-135M, 30 layers, a requested
  0–14 CPU / 15–29 GPU split reproduced exactly, cross-validated three
  independent ways (llama.cpp's own weight-device log, its own
  KV-device log, and direct `cb_eval`/`view_src` tensor inspection) —
  zero mismatches. Weights never moved (31/31 and 33/33 weight-layer
  log lines stayed on the GPU device regardless of KV split)
  (`results/canonical/placement_maps.json`).
- **Real VRAM reduction, KV-attributable only**: 12–96 MiB measured via
  external `nvidia-smi` on SmolLM2-135M across ctx 2048–16384 (growing
  with context, as expected), 21 MiB on SmolLM2-360M — with weights
  independently confirmed unchanged, unlike Phase 12A's combined
  weight+KV `--gpu-layers N` numbers (`results/canonical/q8_memory.json`).
  Q5_1 works identically (67 MiB at ctx=16384/half-split), confirming
  precision and placement are orthogonal (`results/canonical/q5_memory.json`).
- **A genuinely new capability**: at ctx=200000 on the real GTX 1650,
  full-GPU-KV hard-fails to allocate compute buffers while a 16/16 KV
  split succeeds cleanly, weights unchanged — a configuration that was
  not runnable before this patch, achieved with zero weight movement
  (`results/canonical/pressure_test.json`).
- **Correctness**: token-identical generation across three placement
  ratios (0%, 50%, 83% CPU-KV) on the same prompt/model/context, greedy
  decoding, 8 tokens each — no logit-level (`rel_l2`/`delta_nll`)
  comparison was additionally run, disclosed as a scope limitation, not
  a claim of bit-exact numerical equality (`results/canonical/correctness.json`).
- **Graph/attention compatibility**: `ggml_backend_sched` automatically
  inserts cross-device copies as CPU-placed KV layers increase
  (`graph_splits` grows 2 → 32 → 52 while `graph_nodes` stays constant
  at 1296) — the same pre-existing scheduler machinery ordinary partial
  weight offload already uses, exercised here for a new reason
  (`results/canonical/graph_behavior.json`).
- **Performance**: on SmolLM2-135M/GTX1650/ctx=16384, throughput did
  NOT degrade as more KV moved to CPU (22.3 → 25.1 tok/s across
  100%→25% GPU-KV ratios while VRAM strictly decreased) — disclosed as
  scale/hardware-specific, not a general claim.

## Verdict

Literal source field (`results/canonical/summary.json`, `decision_gate`):

> **`KV_DEVICE_OVERRIDE_VIABLE`**

Rationale (same file, `decision_gate_rationale`): all five viability
criteria were met with real, measured evidence — a tiny patch mirroring
an already-accepted precedent; provably unchanged default behavior;
independently placeable weights and KV, cross-validated three ways; a
real, isolated, KV-attributable VRAM reduction plus a genuinely-not-
otherwise-runnable configuration made runnable; and preserved
correctness with comparable maintenance burden to the sibling
`kv_type_override` patch.

## Productization

**Yes — this is one of the two strongest direct research inputs to
product PR #21** (`kadireren7/membrane`, merged `8287e8c14f1b5af2e9e17fa95e85663e94697d85`,
"feat: add static KV residency planner (--kv-placement)"; see
[EXP-KV-PLACEMENT-BOTTLENECK-12G](../EXP-KV-PLACEMENT-BOTTLENECK-12G/README.md)
for the other).

- **The llama.cpp patch is byte-for-byte identical** between this
  branch's `patches/llama.cpp-membrane-kv-device-override.patch` and
  the same file on product `main` (verified directly, `diff` exit 0)
  — the `kv_dev_override` callback shipped unchanged.
- **The research-side C policy code is NOT the product's policy code.**
  This phase's `tools/membrane-kv-device-override/split_policy.c` is a
  minimal, fixed-split decider built only to make the mechanism
  testable. The product's `tools/membrane-run/kv_residency_policy.c`
  is a materially more sophisticated, independently-written VRAM-budget
  planner (per-layer fit against `device_free_bytes`, a 15% GPU reserve,
  a configurable margin) that consumes the same `kv_dev_override`
  capability this phase proved viable — it does not reuse this phase's
  policy logic or source.
- Do not claim byte-identical productization beyond the llama.cpp patch
  itself.

## Contents

- `results/canonical/` — 19 artifacts: patch scope/reproducibility,
  per-layer placement proof, default-behavior regression, Q8/Q5 VRAM
  accounting across a context matrix and a second model, correctness,
  the synchronous cross-device performance curve, the GTX-1650 pressure
  test, graph/scheduler behavior classification, maintenance
  assessment, and raw per-run JSON/JSONL captures.
- `patches/phase12b-kv-device-override.patch` — this phase's own
  incremental `git diff`-format patch, **historical base
  (`d6e6189`) → this phase's HEAD (`a1ebd9e`)**: the new
  `tools/membrane-kv-device-override/` tool tree, the llama.cpp-facing
  `patches/llama.cpp-membrane-kv-device-override.patch` file, its
  `CMakeLists.txt` wiring, and this phase's `scripts/verify-results.py`
  extension. Excludes `results/phase12/kv-device-override/` (preserved
  separately above, not duplicated in the patch).
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/kv-device-override`
- Source branch HEAD: `a1ebd9e285bd763cfb448c8d8202536802783bb9`
- Base commit (branch point): `d6e6189d0417858bfcf3d51c766eaa4be5fe46e1`
- Date: 2026-08-16
- Chain position: **first** of the linear Phase 12B–12G chain (12A precedes and motivated it, but is not its ancestor)
- Productized: **Yes** — llama.cpp patch only, see Productization above
- Full machine-readable record: `MANIFEST.json`
