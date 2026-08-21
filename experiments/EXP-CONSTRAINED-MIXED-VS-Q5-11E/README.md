# EXP-CONSTRAINED-MIXED-VS-Q5-11E — The terminal comparison (index)

**Previous:** [EXP-MODEL-SPECIFIC-MIXED-POLICY-11D](../EXP-MODEL-SPECIFIC-MIXED-POLICY-11D/README.md)
**Next:** Product decision — retain the shipped whole-cache adaptive Q8/Q5 policy ([PR #20](https://github.com/kadireren7/membrane/pull/20), `d6e6189d0417858bfcf3d51c766eaa4be5fe46e1`) unchanged; no mixed-KV product feature was built. See "Productization consequence" below.

This directory preserves the research record for MEMBRANE Phase 11E —
the terminal decision point of the Phase 11B–11E mixed-precision KV
chain. It answers the one question Phase 11D left open, at the exact
boundary where the answer would matter to the product.

## Question

At a real GPU memory-pressure point where whole-cache Q8 cannot provide
safe residency but whole-cache Q5 can (H1), does a profile-selected
mixed Q8/Q5 layout retain some Q8 layers at the same practical
residency as whole-cache Q5, with measurably *better* quality against a
true F16 ground-truth reference?

## What this phase actually demonstrated

**The primary constrained point (Point B)**: SmolLM2-360M, ctx=180000,
real GTX 1650, Vulkan:

- `product_explicit_q8` rejects (`exit_code=5`, the real product
  `gpu_policy` check)
- `product_adaptive` selects Q5 (`Q5_REQUIRED_FOR_FULL_RESIDENCY`)
- `product_explicit_q5` succeeds: 32/32 GPU residency, ~68 MiB of the
  product's own conservative headroom
- Precondition for H1 satisfied: this is exactly the regime where
  adaptive is *forced* to Q5, the case Phase 11D could not directly
  measure.

**Mixed fits at the same practical residency as Q5**: VRAM delta
measured (nvidia-smi) at 276 MiB, matching the theoretical KV-byte
delta (274.66 MiB) to within 1.3 MiB.

**But mixed loses on quality, measured against a true F16 reference**
(5 pre-registered holdout prompts, `gen_tokens=32`): whole-cache Q5's
mean `logit_rel_l2` was 0.066795; the best mixed profile (top-8 Q8
layers) scored 0.074887 — *worse*. Mixed beat Q5 on only 1 of 5 held-out
prompts. Sensitivity-based layer placement did beat every naive layout
at the same byte budget — the placement mechanism itself still works —
but even the best placement was worse than simply staying whole-cache
Q5.

**No performance advantage either**: `performance.material_difference:
false`.

**A second, independent safety finding — a false-safe VRAM boundary**:
the tighter empirical VRAM model built in this phase predicts mixed
peak usage to within 0.05%, which sounds like an improvement over the
product's conservative formula (which overpredicts by ~451 MiB at
whole-Q5, safe direction) — but that tighter model produces a
**confirmed false-safe result right at the real top-8/top-9 layer
boundary**. This is disclosed as a real, independent reason mixed KV
is not safe to productize as-is, even setting the quality result aside.

**Control points confirm the picture is consistent, not cherry-picked**:
at Point A (ctx=100000, comfortable memory), adaptive selects Q8 as
expected — mixed is not positioned as superior there either, consistent
with Phase 11D. At Point C (ctx=186000, an extreme point), even
whole-cache Q5 fails closed — mixed cannot be safer than Q5 there
either, since mixed always costs ≥ Q5 bytes for any nonzero Q8 count.

**A simulated mechanical hierarchy would have made things worse, not
better**: `future_policy_simulation` shows a naive `Q8 → mixed → Q5 →
fail` hierarchy would fire the mixed branch at Point B — and this
phase's own quality data shows that firing would make quality *worse*
than simply falling through to Q5. A "does it fit" check alone is not a
sufficient gate for a hypothetical future policy.

**Fail-closed safety re-verified live** (not just inherited from Phase
11D): an out-of-range layer index (99) and a negative index (-1) both
produced a clean `exit_code=2` in this phase's own live test.

## Result

The comparison was clean and internally cross-validated: the VRAM delta
matches KV-byte math almost exactly, and an all-Q5 self-check behaves
as a perfect sanity control. This is not a fragile, noisy, or
backend-limited negative result — it is a deliberately narrow, clean
measurement at the one boundary where mixed KV's case was strongest,
and it still lost.

## Verdict

Literal source field (`results/canonical/summary.json`,
`decision_gate`):

> **`CONSTRAINED_MIXED_WORKS_BUT_NO_QUALITY_WIN`**

Rationale (same file, `decision_gate_rationale`, quoted in full):

> "Mixed fits at the same practical residency as Q5 at the primary
> constrained point, and profile-based (sensitivity) layer placement
> beats every naive equal-budget layout. But against a true F16
> reference, the best mixed configuration is measurably WORSE quality
> than simply staying whole-cache Q5 (beats Q5 on only 1 of 5 held-out
> prompts), uses more VRAM, and offers no performance advantage. This
> does not meet the CONSTRAINED_MIXED_ADVANTAGE_CONFIRMED bar (requires
> mixed quality measurably BETTER than Q5), and is not
> fragile/backend-limited/inconclusive -- the measurement was clean,
> reproducible (deterministic, bit-identical on repeat), and internally
> cross-validated (VRAM delta matches KV-byte math; F_all_q5 self-check
> is a perfect sanity control)."

**Scoped precisely — do not overgeneralize**: this says per-layer
mixed Q8/Q5 KV remained technically viable throughout this chain, but
in *this tested constrained comparison* it did not provide a sufficient
quality advantage over whole-cache Q5 to justify product complexity.
It does not say mixed precision can never help — only that, at the one
real boundary tested (SmolLM2-360M, ctx=180000, this profile), it did
not.

Next-step recommendation (same file, verbatim):

> "Stop mixed-KV productization for the constrained-regime case and
> retain the product's current whole-cache adaptive policy unchanged.
> Do not pursue a Q8->mixed->Q5 hierarchy on the strength of this
> phase's data -- the mixed branch would make quality worse, not
> better, at the one point measured."

## Productization consequence

**Per-layer mixed Q8/Q5 KV was never productized**, at any phase in
this chain. The product decision this phase's negative result directly
supports: keep the whole-cache adaptive Q8/Q5 policy
([PR #20](https://github.com/kadireren7/membrane/pull/20),
`d6e6189d0417858bfcf3d51c766eaa4be5fe46e1`) exactly as shipped, on top
of explicit Q5_1 ([PR #19](https://github.com/kadireren7/membrane/pull/19),
`d101fc80d535b09adecd90345312d50774bd821c`) — do not build a
Q8→mixed→Q5 hierarchy on top of it. Verified: `git grep` for every
symbol from `tools/membrane-kv-mixed-layer/` against product `main`
returns no hits.

**This negative result is a first-class outcome, not an omission**:
the chain tested the more complicated idea, it worked technically at
every phase, it was compared honestly against the shipped policy and
against whole-cache Q5 at the exact boundary where it had the best
case, it did not justify itself, and — because of that — it was not
shipped. That is stronger evidence for the current product's
simplicity than never having tried.

## Product vs. research boundary

**Product `main` contains**: explicit Q8, explicit Q5_1 (`--kv q5`,
PR #19), and whole-cache adaptive Q8/Q5 selection (PR #20). **Research
only** (this entire Phase 11B–11E chain): per-layer mixed Q8/Q5,
sensitivity-driven layer policy, model-specific mixed policy, and
constrained mixed-vs-Q5 policy. Per-layer mixed precision does **not**
exist in product `main` at any phase of this chain — verified directly
against current `main` (`8287e8c14f1b5af2e9e17fa95e85663e94697d85`).

## Contents

- `results/canonical/` — 17 artifacts: `summary.json` (decision gate),
  `manifest.json`, `boundary_scan.json` (Points A/B/C), `hardware_state.json`,
  `quality_methodology.json` + `quality_summary.json` +
  `quality_raw.jsonl` + `quality_raw_f16ref.jsonl` (the central quality
  comparison), `layout_comparison.json` (sensitivity vs. naive at
  equal budget), `q8_count_sweep.json`, `overhead_calibration.json` +
  `vram.json` (the VRAM measurement and the false-safe finding),
  `adaptive_head_to_head.json`, `future_policy_simulation.json`,
  `performance.json`, `regression_sanitizers.json`, and
  `raw_q8_budget_search_180k.jsonl`.
- `patches/phase11e-constrained-mixed-vs-q5.patch` — this phase's
  incremental delta against Phase 11D's HEAD: `constrained_policy_sim.c/.h`,
  `sequential_compare.cpp`, and `test_constrained_policy_sim.c` under
  `tools/membrane-kv-mixed-layer/`, its `CMakeLists.txt` wiring, and
  this phase's `scripts/verify-results.py` extension.
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/constrained-mixed-vs-q5`
- Source branch HEAD: `96b93b1de8acd35cce50055b91ce2ae8d6246039`
- Base commit (branch point): `bb0b69a290c8de609bdce5494bb9b616972f39b9`
  (Phase 11D HEAD)
- Date: 2026-08-16
- Chain position: fourth and terminal phase of the linear Phase
  11B–11E chain — no Phase 11F was started.
- Productized: **No** (finding informs a "do not build" product
  decision, not a shipped feature) — see Productization consequence
  above.
- Full machine-readable record: `MANIFEST.json`
