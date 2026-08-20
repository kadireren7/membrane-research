# EXP-KV-Q5-EVALUATION-10B — Q5_0/Q5_1 KV cache evaluation (index)

This directory preserves the research record for MEMBRANE Phase 10B: a
direct extension of `EXP-KV-Q4-STORAGE-10A`'s methodology to Q5_0 and
Q5_1, explicitly searching for a format "whose quality stays much closer
to Q8 than Q4" while still meaningfully saving memory over Q8. This
experiment's Q5_1 result **was productized**: `--kv q5` in `main`
(`kadireren7/membrane` PR #19).

## Question

Do Q5_0 or Q5_1 KV storage close the quality gap Q4_0 left open
(`EXP-KV-Q4-STORAGE-10A`), while still saving meaningful memory over Q8?

## Result

- **Memory**: real win over Q8 for both variants — Q5_0 uses 34.375% of
  native (64.7% of Q8's footprint), Q5_1 uses 37.5% of native (70.6% of
  Q8's footprint) (`results/canonical/quality_bar_verdict_per_format.json`).
- **Performance**: no meaningful cost for either variant on either
  backend — every measured delta vs Q8 was -3.6% to +2.0%, within or
  close to run-to-run noise (`results/canonical/q5-kv-evaluation.json`,
  `performance_repetitions` section).
- **Stability**: 0 crashes, 0 unexpected exits across 90+ runs on CPU
  and Vulkan, across every tested context/model
  (`results/canonical/q5-kv-evaluation.json`, `decision_gate.reasoning`).
- **Quality**: both variants clear Q4_0's bar by a wide margin on
  rel-L2 and NLL on both tested models. Q5_1 is consistently the
  stronger variant on every quality axis (top-1: 94.5%/98.65% vs Q4's
  92.5%/96.5% on 135M/360M; rel-L2 the best of any Q5 variant on both
  models) at a modest (~9%) memory cost over Q5_0
  (`results/canonical/quality_bar_verdict_per_format.json`, `cross_format_comparison`).
  Q5_0's own quality result is mixed — clearly better than Q4 on the
  larger model, but its worst-case top-1 result on the smallest model
  does not clear the bar as cleanly.
- **Trade-off scope (disclosed, not hidden)**: Q5 does not close the
  gap to Q4's raw context-capacity/GPU-residency advantage — Q4 still
  wins there. The real trade-off is three-way: Q4 for maximum context
  capacity, Q5 for a memory/quality middle ground, Q8/native for best
  quality (`results/canonical/gpu_memory_pressure5_360m.jsonl` and its summary
  in `results/canonical/q5-kv-evaluation.json`).

## Verdict

Literal source field (`results/canonical/q5-kv-evaluation.json`,
`decision_gate.verdict` / `decision_gate.preferred_format`):

> **`Q5_PRODUCT_CANDIDATE`**, preferred format **`Q5_1`**

Per-format verdicts (`results/canonical/quality_bar_verdict_per_format.json`,
quoted verbatim):

> Q5_0: "Promising on the larger model, but the smallest-model
> worst-case top1 result does not clear the bar cleanly -- mixed."
>
> Q5_1: "Consistently the stronger of the two Q5 variants across every
> prompt-level metric (top1, rel-L2, worst-case) on both models, at a
> modest memory cost over q5_0."

**Scope of this conclusion**: tested on SmolLM2-135M and SmolLM2-360M,
CPU and Vulkan backends. This is not a claim that Q5_1 is universally
superior to Q5_0 or Q8 in every deployment — it is the result of this
phase's specific quality bar (clearly better than Q4 on top-1/rel-L2/NLL,
meaningfully lower memory than Q8, no severe cross-prompt instability)
on the models actually tested.

## Productization

This result **was carried into product**:

- Product PR: [kadireren7/membrane#19](https://github.com/kadireren7/membrane/pull/19)
  — "feat: add Q5 KV runtime mode"
- Merge commit: `d101fc80d535b09adecd90345312d50774bd821c`
- Merged: 2026-08-14T14:33:03Z
- Product feature: `--kv q5` (implements Q5_1, matching this
  experiment's preferred-format finding)

Note: the product PR is a clean re-implementation for the product CLI,
not a cherry-pick of this branch's own prototype commit — the
productization link above is a research→product *finding* link, not a
code-identity claim. The prototype code in `patches/` here is this
experiment's own exploratory implementation, preserved for
reproducibility, not the shipped product code (see `main`'s own
`tools/membrane-run/` for the current product implementation).

## Contents

- `results/canonical/` — 15 raw/measured artifacts (Q5 vs Q4 quality-bar
  comparison, cross-format comparison, memory-pressure ladder including
  Q5 rows, multi-prompt aggregate across both models, raw per-run JSONL
  captures for CPU/Vulkan/second-model matrices).
- `patches/phase10b-q5-kv-evaluation-prototype.patch` — the
  **incremental** prototype code change on top of
  `EXP-KV-Q4-STORAGE-10A`'s own patch (this experiment's branch descends
  directly from the Q4 branch) — extends the same product tool files to
  add Q5_0/Q5_1 support alongside the existing Q4 support, plus this
  phase's own `scripts/verify-results.py` extension. Deliberately does
  **not** re-include Q4's own patch content — see
  `EXP-KV-Q4-STORAGE-10A/patches/` for that; apply both in sequence to
  reconstruct the full branch state.
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/q5-kv-evaluation`
- Source branch HEAD: `abfe448105ed3a6101496263beba9d9707d39258`
- Base commit (direct parent): `4aa6757fc8a0e68d338b9815684277b68a423a50`
  (`experiment/q4-kv-storage`'s own HEAD — this branch is a direct,
  linear descendant, not an independent branch)
- Date: 2026-08-14
- Productized: **Yes** — see Productization section above.
- Full machine-readable record: `MANIFEST.json`
