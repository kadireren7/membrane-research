# EXP-MIXED-Q8-Q5-FEASIBILITY-11B — Per-layer mixed Q8/Q5 KV feasibility (index)

**Previous:** [EXP-KV-Q5-EVALUATION-10B](../EXP-KV-Q5-EVALUATION-10B/README.md) (Q5_1 shipped as `--kv q5`, PR #19) and the subsequent whole-cache adaptive Q8/Q5 policy (PR #20) — this phase asks whether going *finer* than whole-cache, to per-layer heterogeneity, is worth it.
**Next:** [EXP-LAYER-SENSITIVITY-GENERALIZATION-11C](../EXP-LAYER-SENSITIVITY-GENERALIZATION-11C/README.md)

This directory preserves the research record for MEMBRANE Phase 11B, the
first phase of the Phase 11B–11E mixed-precision KV chain. It is a
**decision-point** result: mixed per-layer Q8_0/Q5_1 KV cache storage
works mechanically, with a real quality edge over naive layouts at
equal memory in a small sample — but the sample was deliberately small,
which is exactly what this phase's own limitations section says and
what Phase 11C exists to check.

## Question

Is per-layer heterogeneous Q8_0/Q5_1 KV storage — as opposed to the
already-shipped whole-cache adaptive choice (PR #20, either all
layers Q8 or all layers Q5) — mechanically feasible using MEMBRANE's
existing infrastructure, and does a sensitivity-based (highest-error
layers get Q8) layer-selection policy beat naive equal-memory layouts?

## What this phase actually demonstrated

- **Mechanically feasible with zero upstream changes**: mixed per-layer
  KV reuses the Phase 4.1 `kv_type_override` callback unchanged — the
  same signature already accepted and shipped, just returning a
  different `ggml_type` per layer instead of one type for the whole
  cache. `upstream_modification_required: false`
  (`results/canonical/mixed-layer-q8-q5-feasibility.json`).
- **Real no-code probe evidence**, not simulated: a real log line
  confirming the override fired exactly once per context creation, the
  returned `type_map` matching the requested split exactly, and
  correct generation (`token_identity=true`, `top1_preservation=1.0`)
  on the smoke prompt — the model genuinely consumed both K/V dtypes in
  the same forward pass.
- **Sensitivity-based policy beat every naive layout at equal memory**
  (15 of 30 layers Q8, `sensitivity_based_policy_vs_naive`): best
  `delta_nll` (0.00127, next-best ~7× worse), best `top1_preservation`
  (0.975), competitive (2nd-best) `logit_rel_l2` — on a 5-prompt
  sample, disclosed as small, not a statistically large one.
- **No throughput advantage or penalty**: generation tok/s across
  all-Q8, all-Q5, and the sensitivity-based mixed config were
  statistically indistinguishable on both CPU and Vulkan at this small
  model's scale — differences sat within run-to-run noise
  (`results/canonical/mixed-layer-q8-q5-feasibility.json:performance`).
  This does **not** replicate Phase 4.1's F16/Q8/Q4 finding of
  measurably higher peak RSS for mixed configs; here mixed RSS deltas
  sat *between* the all-Q8 and all-Q5 deltas, not above both.

**Distinguishing feasibility from advantage** (do not conflate): the
mechanism *worked* — it is not a failed prototype. What remained
genuinely open after this phase was whether the positive quality
result generalizes past one model, one context size, and five prompts.

## Result

- Real, working mixed-layer KV probe tool
  (`tools/membrane-kv-mixed-layer/`, preserved via this phase's
  incremental patch), llama-free core policy logic plus a real
  llama.cpp-integrated probe.
- Storage math validated: `theoretical_mixed_bytes` computed from the
  exact per-layer `ggml_row_size()`-based formula, no approximation.
- Implementation complexity assessed as low: purely additive, 0
  existing files modified besides two build-registration lines, no new
  public API, no new Vulkan/CPU-specific branching
  (`implementation_complexity`).
- Limitations disclosed directly in source, not discovered later: a
  5-prompt subset (not the full 31-prompt derisk set), one model
  (SmolLM2-135M), one context size (2048), an offline-only research
  policy not validated for runtime adaptivity or generalization.

## Verdict

Literal source field
(`results/canonical/mixed-layer-q8-q5-feasibility.json`,
`feasibility_classification`):

> **`B_MIXED_LAYER_EXISTING_MEMBRANE_INFRA_EXISTS`**

This is a feasibility classification, not a product go/no-go — the
phase's own scope (see `no_code_probe` and `upstream_modification_reason`)
was explicitly to determine *whether the mechanism exists and works*,
not to make a shipping decision. It does, using infrastructure MEMBRANE
already had.

**Scope of this conclusion**: SmolLM2-135M, ctx=2048, a 5-prompt
sample. The sensitivity-based policy's advantage over naive layouts is
real in this sample but explicitly not yet cross-validated — that is
exactly Phase 11C's job.

## Forward link: what this motivated

This phase's own limitations section — small sample, single model,
single context — directly motivated Phase 11C
(`experiment/layer-sensitivity-generalization`, source HEAD
`eaeb25fed8c12bf924f5f42a3d78cd151cd130ca`): does the sensitivity
ranking hold up across more prompts, task categories, both tested
models, and a second context size?

## Productization

**Not productized.** No code from
`tools/membrane-kv-mixed-layer/mixed_layer_policy.{c,h}` or this
phase's probe exists on `main` (verified: `git grep` for this phase's
distinctive symbols against `main` returns no hits). Product `main`
carries only the whole-cache adaptive Q8/Q5 policy (PR #20) and
explicit `--kv q5` (PR #19) — both coarser than, and independent of,
this phase's per-layer mechanism.

## Contents

- `results/canonical/mixed-layer-q8-q5-feasibility.json` — the single
  consolidated result file: architecture findings, no-code probe
  evidence, storage math, ratio/layer-position/sensitivity-ablation
  experiments, the sensitivity-vs-naive comparison, a Pareto curve, CPU
  and Vulkan validation matrices, a memory-pressure result,
  performance, implementation complexity, tests, sanitizers, and
  disclosed limitations.
- `patches/phase11b-mixed-q8-q5-feasibility.patch` — this phase's real
  prototype code as a single `git diff`-format patch against this
  phase's real base commit (`d6e6189`): the new, standalone
  `tools/membrane-kv-mixed-layer/` tool tree (`mixed_layer_policy.c/.h`,
  `main.cpp`, `test_mixed_layer_policy.c`), its `CMakeLists.txt`
  wiring, and this phase's `scripts/verify-results.py` extension.
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/mixed-q8-q5-layer-feasibility`
- Source branch HEAD: `8fc37408685ac8dac2d1e59edcfab5961136177b`
- Base commit (branch point): `d6e6189d0417858bfcf3d51c766eaa4be5fe46e1`
  ("feat: add adaptive Q8/Q5 KV policy", PR #20) — the whole-cache
  adaptive policy this entire chain measures itself against.
- Date: 2026-08-16
- Chain position: first phase of the linear Phase 11B–11E chain
  (11B → 11C → 11D → 11E).
- Productized: **No** — see Productization section above.
- Full machine-readable record: `MANIFEST.json`
