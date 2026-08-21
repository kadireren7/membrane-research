# EXP-LAYER-SENSITIVITY-GENERALIZATION-11C — Does the sensitivity ranking generalize? (index)

**Previous:** [EXP-MIXED-Q8-Q5-FEASIBILITY-11B](../EXP-MIXED-Q8-Q5-FEASIBILITY-11B/README.md)
**Next:** [EXP-MODEL-SPECIFIC-MIXED-POLICY-11D](../EXP-MODEL-SPECIFIC-MIXED-POLICY-11D/README.md)

This directory preserves the research record for MEMBRANE Phase 11C.
Phase 11B's sensitivity-based layer-selection policy beat naive
layouts, but on a small sample (5 prompts, one model, one context
size). This phase is the generalization check that sample demanded.

## Question

Does Phase 11B's layer-sensitivity ranking generalize: across prompts
and task categories, across SmolLM2-135M vs 360M, across context sizes
(2048 vs 8192), and — the practically important question — does a
ranking built only from training prompts still beat naive layouts on
prompts it never saw?

## What this phase actually demonstrated

This phase's own `summary.json` answers five explicit research
questions (`research_questions_answered`), quoted precisely because the
honest answer is genuinely mixed across them, not a single verdict:

| # | Question | Answer (literal, from source) |
|---|---|---|
| 1 | Prompt-to-prompt stability | **PARTIAL** — full-ranking Spearman rho only 0.36 (135M) / 0.47 (360M), but a small set of layers (135M: 19, 27; 360M: 30, 23) are overwhelmingly and consistently the most sensitive in nearly every prompt. |
| 2 | Category stability | **YES, strongly** — category-vs-global rho 0.55–0.87 (135M), 0.61–0.93 (360M); the same top layers dominate every one of 11 categories. |
| 3 | Model stability | **PARTIAL** — exact-layer-index overlap is modest (`top10_overlap_after_depth_mapping=0.40`), but both models concentrate sensitivity in the mid-to-late depth region and each has one standout near-final layer (135M layer 27 at depth 0.93; 360M layer 30 at depth 0.97). |
| 4 | Context stability (2048 vs 8192) | **YES, fairly strongly** — rho=0.71, top-10 overlap 80%, top-2 layers identical and same order — with the disclosed caveat that both settings consumed similar actual token counts, so this tests KV-capacity effects, not long-context-usage effects specifically. |
| 5 | Policy usefulness (held-out) | **YES, clearly on rel_l2** — a ranking built from only 21 training prompts beat every naive layout (early/late/alternating/random) on `logit_rel_l2` at every intermediate Q8-budget tested (4/8/12/15/20/24 layers) on 10 held-out prompts never used to build it. Mixed on `top1_preservation` (won/tied 5 of 6 budgets) and `delta_nll` (won at higher budgets; random sometimes better at low budgets) — a real, disclosed mixed result, not smoothed over. |

- **Stable sensitive layers**: 135M's top-5 by support (out of 31
  prompts) are layers 19 (27/31), 27 (26/31), 21 (13/31), 22 (13/31),
  12 (12/31); 360M's are 30 (31/31), 23 (23/31), 18 (14/31), 13 (13/31),
  19 (12/31) (`results/canonical/summary.json:stable_sensitive_layers`).
- **Real memory-pressure check**: at the real GTX 1650 ctx=180000/360M
  boundary, all 4 layer-selection strategies for an 8-of-32 Q8 budget
  cost identical memory and all achieve full residency — sensitivity
  selection is a zero-memory-cost choice at this real hardware
  boundary (quality-at-boundary itself was not measured, by design, to
  avoid real OOM risk).

## Result

Beyond the top 2–3 dominant layers per model, the remaining ~27–29
layers show much lower and more prompt-dependent support
(`results/canonical/category_rankings.json:support_by_layer`) —
consistent with the modest full-ranking prompt-to-prompt correlation
even though the top few layers are highly stable. The practically
important result is #5: the held-out policy evaluation
(`results/canonical/holdout_policy.json`), which is the first evidence
in this chain that a *pre-computed, train-only* ranking — not just an
in-sample one — usefully beats naive baselines on unseen prompts.

## Verdict

This phase's own result artifacts do not record a single
`decision_gate`-style literal constant the way its sibling phases do
— its actual output is the five-question structured answer set quoted
above, plus a `decision_gate_inputs` object
(`results/canonical/summary.json`) summarizing each stability axis
qualitatively (`"cross_prompt_stability": "weak-to-modest for the full
ranking, strong for the top 2-3 layers"`, etc.). Reporting the real
fields directly here rather than inventing a single-token verdict that
does not exist in source.

**Net reading**: the full ranking's stability is genuinely partial —
disclosed, not glossed over — but the practically relevant claim (a
policy trained on some prompts generalizes to held-out prompts, and the
handful of truly dominant layers are robust across prompts/categories/
context) is well supported. This was judged sufficient to proceed to
building an actual per-model policy pipeline in Phase 11D, rather than
to stop the chain here.

## Forward link: what this motivated

The held-out-policy win and the strong category/context stability
motivated Phase 11D (`experiment/model-specific-mixed-policy`, source
HEAD `bb0b69a290c8de609bdce5494bb9b616972f39b9`): build a real
per-model calibrated policy pipeline (fingerprint → profile → policy)
and compare it against the product's actual shipped whole-cache
adaptive policy — not just naive layouts.

## Productization

**Not productized.** No code from
`tools/membrane-kv-mixed-layer/sensitivity_analysis.{c,h}` exists on
`main` (verified via `git grep`). This phase produced evidence, not
shippable product code.

## Contents

- `results/canonical/` — 18 artifacts: `summary.json` (top-level
  findings), `manifest.json`, `prompts.json` (31-prompt set, category
  grouping, train/holdout split), `rankings.json` (the large
  consolidated per-prompt/global ranking and rank-stability file, ~830
  KB — see Provenance for why this is kept whole rather than trimmed),
  `category_rankings.json`, `cross_model.json`, `context_stability.json`,
  `holdout_policy.json`, `pareto.json`, `pressure_boundary.json`,
  `performance.json`, and 7 `raw_*.jsonl` per-run/per-config record
  files backing the above (Stage A/B/C ablation sweeps, holdout-policy
  raw records, pressure-boundary raw records, CPU/Vulkan performance
  raw records).
- `patches/phase11c-layer-sensitivity-generalization.patch` — this
  phase's incremental delta against Phase 11B's HEAD: the new
  `sensitivity_analysis.c/.h` and `test_sensitivity_analysis.c` under
  `tools/membrane-kv-mixed-layer/`, its `CMakeLists.txt` wiring, and
  this phase's `scripts/verify-results.py` extension.
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/layer-sensitivity-generalization`
- Source branch HEAD: `eaeb25fed8c12bf924f5f42a3d78cd151cd130ca`
- Base commit (branch point): `8fc37408685ac8dac2d1e59edcfab5961136177b`
  (Phase 11B HEAD)
- Date: 2026-08-16
- Chain position: second phase of the linear Phase 11B–11E chain.
- Size note: `results/canonical/rankings.json` (~830 KB) is this
  phase's single largest artifact — it is a genuine consolidated
  computed-ranking file (per-prompt and global rel_l2/delta_nll/top1
  rankings plus Spearman/Kendall rank-stability, for all three sweeps),
  not a raw log dump, and is the primary evidence behind research
  questions 1 and 3 above; kept whole rather than trimmed, per
  `manifest.json`'s own description of it as "one consolidated file,
  not three separate ones."
- Productized: **No** — see Productization section above.
- Full machine-readable record: `MANIFEST.json`
