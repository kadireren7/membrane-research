# EXP-MODEL-SPECIFIC-MIXED-POLICY-11D — Mixed policy vs. the shipped adaptive policy (index)

**Previous:** [EXP-LAYER-SENSITIVITY-GENERALIZATION-11C](../EXP-LAYER-SENSITIVITY-GENERALIZATION-11C/README.md)
**Next:** [EXP-CONSTRAINED-MIXED-VS-Q5-11E](../EXP-CONSTRAINED-MIXED-VS-Q5-11E/README.md)

This directory preserves the research record for MEMBRANE Phase 11D.
Phase 11C showed a held-out ranking beats *naive* layouts. This phase
asks the question that actually matters for product decisions: does a
real per-model mixed policy beat the product's *already-shipped*
whole-cache adaptive Q8/Q5 policy (PR #20), not just naive baselines?

## Question

Does a real, calibrated, per-model mixed-layer policy (fingerprint →
sensitivity profile → mixed layout) outperform MEMBRANE's actual
shipped, unmodified whole-cache adaptive Q8/Q5 policy, measured
head-to-head via the real product CLI (`membrane-run --kv adaptive`),
across calibration-set sizes, held-out prompts, and both tested models?

## What this phase actually demonstrated

- **The offline pipeline itself is real and safe**: fingerprint →
  profile → policy → mixed KV, calibration-size analysis (pure
  reanalysis of Phase 11C's raw data, no new inference), held-out
  evaluation on SmolLM2-360M, and a memory-pressure check — all built,
  all real, all reproducible (`results/canonical/reproducibility.json`
  confirms byte-identical generated profiles across two independent
  runs for both models).
- **Measured head-to-head against the real product CLI, unmodified**:
  this phase invokes `build-cpu-rc1/tools/membrane-run/membrane-run
  --kv adaptive --compare-kv` exactly as any user would — it does not
  modify or extend the product CLI in any way
  (`results/canonical/whole_cache_adaptive_comparison.json`).
- **In the regime that matters most, adaptive wins outright**: at
  ctx=2048 on CPU with no memory pressure, adaptive resolved to
  whole-cache Q8 for every single prompt on both models. Its resolved
  quality (mean `logit_rel_l2` ≈0.010 for 135M, ≈0.008 for 360M)
  clearly beat the mixed profile's quality (≈0.047 for 135M, ≈0.040 for
  360M) — despite mixed using *fewer* bytes (85.3% of Q8's byte cost).
  A partial per-layer mix is not a free win over adaptive when adaptive
  can simply pick whole-cache Q8.
- **Mixed does beat whole-cache Q5, consistently**: for 20.8% more
  bytes than all-Q5, mixed KV recovered roughly 17% (135M) to 14%
  (360M) of the quality gap toward Q8 — matching the pattern Phase
  11C's held-out results already suggested. But quality does not
  degrade linearly with bytes: at 85.3% of Q8's bytes, mixed's
  `rel_l2` was 4.1×–4.9× *worse* than Q8's, not a proportionally close
  approximation.
- **The one scenario where mixed could still matter was not tested
  here**: specifically, when adaptive would be *forced* down to
  whole-cache Q5 by real memory pressure (a byte budget strictly
  between all-Q5 and all-Q8 that adaptive's binary choice cannot
  occupy). CPU/ctx=2048 never pressured adaptive off Q8 in this
  comparison — testing that specific boundary needed a real
  memory-pressure setup, left to Phase 11E.
- **The real memory-pressure margin measured here was tighter than the
  pure byte-budget policy math suggested** (`memory_pressure_360m.json`)
  — a disclosed caveat on how confidently this mechanism could be
  pushed toward its own theoretical limits.

## Result

The offline mixed-policy pipeline generalizes cleanly across
calibration sizes, held-out prompts, and both tested models — this is
not a fragile, backend-limited, or irreproducible result. But it loses
to the product's actual policy in the regime that matters in practice
(comfortable memory), and its one real advantage (beating whole-cache
Q5) is real but only relevant in a narrower memory-pressure case this
phase did not directly measure against adaptive.

## Verdict

Literal source field (`results/canonical/summary.json`,
`decision_gate.verdict`):

> **`MODEL_SPECIFIC_POLICY_WORKS_BUT_NO_ADAPTIVE_ADVANTAGE`**

Rationale (same file, `decision_gate.rationale`, quoted in full):

> "The full offline pipeline (fingerprint -> profile -> policy -> mixed
> KV) is real, safe, reproducible, and generalizes across calibration
> sizes, held-out prompts, and both tested models -- this is not a
> fragile or backend-limited result. But measured head-to-head against
> the ALREADY-SHIPPED, unmodified --kv adaptive policy, the
> profile-based mixed layout does not win in the regime that matters
> most in practice (comfortable memory, adaptive resolves to Q8):
> adaptive's whole-cache Q8 clearly beats mixed KV on quality at a
> similar byte cost. Mixed KV's one measured, genuine advantage --
> filling the quality gap between whole-cache Q5 and Q8 -- only becomes
> relevant to the product's actual behavior in the narrower case where
> adaptive would otherwise be forced down to whole-cache Q5 by memory
> pressure, which this phase did not reproduce (would require Task
> #103's kind of boundary conditions combined with an apples-to-apples
> adaptive-vs-mixed comparison AT that boundary, not just a
> memory-comfortable comparison). The real memory-pressure margin at
> the one boundary tested is far tighter than the pure byte-budget
> policy's math suggests, which is a real caveat on how confidently
> this mechanism could be pushed toward its own theoretical limits
> without additional non-KV overhead margin."

**More policy sophistication is not itself a product benefit**: this
phase built a materially more complex mechanism than the shipped
adaptive policy (per-model calibration, sensitivity profiling, mixed
layout construction) and, measured honestly against the real product
CLI, that sophistication did not translate into a product-relevant win
in the regime tested. The mechanism worked; it did not clear the bar.

## Forward link: what this motivated

The one open question this phase explicitly left — does mixed KV beat
whole-cache Q5 specifically, at the real boundary where adaptive would
otherwise be forced down to Q5 — set up Phase 11E's terminal,
apples-to-apples constrained comparison at exactly that boundary.

## Productization

**Not productized.** No code from
`tools/membrane-kv-mixed-layer/model_profile.{c,h}` or this phase's
calibration scripts exists on `main` (verified via `git grep`). This
phase's comparison partner — the whole-cache adaptive policy — is
already product (PR #20, `d6e6189d0417858bfcf3d51c766eaa4be5fe46e1`),
invoked here unmodified as a real black box, not duplicated.

## Contents

- `results/canonical/` — 22 artifacts: `summary.json` (decision gate),
  `manifest.json`, `whole_cache_adaptive_comparison.json` (the central
  head-to-head measurement), `budget_sweep.json`,
  `calibration_size_analysis.json` + `calibration_size_holdout_quality.json`
  + `calibration_sizes.json` (calibration-set-size stability),
  `cpu_vulkan_validation.json`, `cross_model_safety.json`,
  `holdout_policy_360m.json`, `memory_pressure_360m.json`,
  `performance.json`, `profile_135m_train21.json` +
  `profile_360m_train21.json` (the actual generated per-model
  profiles), `profile_generation_performance.json`,
  `regression_sanitizers.json`, `reproducibility.json` (determinism
  check), and 6 `raw_*.jsonl` per-run record files.
- `patches/phase11d-model-specific-mixed-policy.patch` — this phase's
  incremental delta against Phase 11C's HEAD: `model_profile.c/.h` and
  `test_model_profile.c` under `tools/membrane-kv-mixed-layer/`, 5 new
  `scripts/build_*`/`calibration_size_analysis.py` helper scripts, its
  `CMakeLists.txt` wiring, and this phase's `scripts/verify-results.py`
  extension.
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/model-specific-mixed-policy`
- Source branch HEAD: `bb0b69a290c8de609bdce5494bb9b616972f39b9`
- Base commit (branch point): `eaeb25fed8c12bf924f5f42a3d78cd151cd130ca`
  (Phase 11C HEAD)
- Date: 2026-08-16
- Chain position: third phase of the linear Phase 11B–11E chain.
- Productized: **No** — see Productization section above.
- Full machine-readable record: `MANIFEST.json`
