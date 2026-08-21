# EXP-KV-PLACEMENT-BOTTLENECK-12G — KV placement bottleneck regime discovery

Phase 12G of the MEMBRANE Phase 12B–12G KV residency research chain —
the final phase, and the direct research bridge to product PR #21.

**Previous:** [EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F](../EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F/README.md)
**Next:** Productization — [PR #21](https://github.com/kadireren7/membrane/pull/21), `feat: add static KV residency planner (--kv-placement)`, merged `8287e8c14f1b5af2e9e17fa95e85663e94697d85`

## Question

Where is the actual bottleneck/value in KV placement — throughput
(as Phase 12F's scheduler hoped to exploit) or capacity — and is there
ANY tested regime where KV placement is a real performance bottleneck?

## What this phase did

A systematic sweep across models (SmolLM2-135M, SmolLM2-360M,
qwen2.5-1.5B-instruct), contexts (2048–32768, plus a dedicated
larger-context/larger-model matrix), Q8_0 and a Q5_1 subset, and
CPU/GPU KV split ratios from 0% to 100% GPU-resident — 16 benchmark
batches, 50 Phase-A sweep points, 2 Phase-B confirmed points (7 repeats
each), 9 large-model points, 2 Q5 points. A predeclared bottleneck-
regime rule (fixed **before** seeing results,
`results/canonical/break_even.json`) required ≥10% slowdown, with
noise/cv/host-validity gating, persisting at a second nearby point,
before any MEANINGFUL/STRONG classification could stand. No llama.cpp
source changes were made this phase — only a new `bottleneck` mode on
the existing `tiering_probe.cpp` driver and a new pure, llama-free
`tools/membrane-kv-bottleneck-analysis/` helper module.

## Result

- **Zero valid ≥10% CPU-KV performance penalty anywhere tested.**
  Every one of the 16 batches classified `NO_BOTTLENECK`
  (`results/canonical/bottleneck_map.json`). The single worst-case
  point across the ENTIRE sweep — 1.5B, ctx=8192, 50% GPU-KV — was
  actually a **0.51% SPEEDUP** relative to 100%-GPU-KV, not a slowdown
  (`results/canonical/summary.json:strongest_slowdown_observed`).
- **CPU-KV at parity or faster than GPU-KV at every valid tested
  point.** E.g. at 135M/ctx=2048, tok/s strictly *increases* as more KV
  moves to CPU: 23.6 (100% GPU-KV) → 24.4 → 25.2 → 25.8 → 26.6 (0%
  GPU-KV) (`results/canonical/bottleneck_map.json`). Pareto analysis
  confirms the 0%-GPU-KV point as Pareto-optimal (lowest VRAM, highest
  tok/s) in that group (`results/canonical/pareto.json`).
  `benchmark_validity.json` shows only 1 of 16 batches flagged noisy,
  and it does not back any bottleneck claim.
- **VRAM/capacity effect confirmed as real** (lower GPU-KV percentage
  correlates with lower VRAM used at every model/context combination
  tested, e.g. 385 → 361 MiB, 135M/ctx=2048, 100%→0% GPU-KV;
  `results/canonical/bottleneck_map.json`, `larger_model.json`) — this
  phase's own sweep does not include a dedicated max-context/VRAM-
  headroom experiment at the true GTX-1650 pressure boundary the way
  Phases 12B/12C/12E did; the capacity claim rests on this phase's
  consistent VRAM-vs-split correlation plus the prior phases' direct
  pressure-boundary evidence (ctx=200000 configurations only runnable
  with CPU-KV present) and PR #21's own separately-measured Vulkan-OOM
  boundary (see Productization below) — not re-derived from scratch
  this phase.
- **A performance-driven dynamic policy was explicitly stopped based on
  this data**: `next_step_recommendation` — "Stop performance-driven
  dynamic policy work and focus on capacity/residency productization."

## Verdict

Literal source field (`results/canonical/summary.json`, `decision_gate`):

> **`KV_PLACEMENT_CAPACITY_ONLY_ON_TESTED_REGIME`**

One of five predeclared allowed values (`KV_PLACEMENT_BOTTLENECK_CONFIRMED`,
`KV_PLACEMENT_CAPACITY_ONLY_ON_TESTED_REGIME`,
`KV_PLACEMENT_BOTTLENECK_MODEL_DEPENDENT`,
`KV_PLACEMENT_BOTTLENECK_BACKEND_LIMITED`,
`KV_PLACEMENT_BOTTLENECK_INCONCLUSIVE`), `bottleneck_regime_confirmed: false`.

**Scope of this conclusion**: tested regime only — SmolLM2-135M/360M
and qwen2.5-1.5B, contexts up to 32768 (larger-model matrix) or 65536
(inherited from Phase 12F), on a single GTX 1650. This is not a
universal claim that KV placement can never be a throughput bottleneck
at other scales.

## Productization

**Yes — together with Phase 12B, one of the two strongest direct
research inputs to product PR #21** (`kadireren7/membrane`, merged
`8287e8c14f1b5af2e9e17fa95e85663e94697d85`, "feat: add static KV
residency planner (--kv-placement)").

- PR #21's own description cites this phase's literal verdict string
  (`KV_PLACEMENT_CAPACITY_ONLY_ON_TESTED_REGIME`) directly and states
  its central conclusion in the same terms this phase found: CPU-
  resident KV was at parity or faster than GPU-resident KV across every
  valid tested point, so the demonstrated product value is VRAM
  capacity, not throughput.
- PR #21's own central measured result (qwen2.5-1.5B, ctx=28500, a real
  reproduced Vulkan out-of-device-memory error with default placement,
  avoided by `auto`/`cpu`, with a measured failure boundary between
  ctx=26500 and 26800 — a ~6.4% context uplift **at that one
  configuration**, explicitly disclosed by the PR as narrow, not
  general) is the **product team's own separate measurement**, made
  consistent with this phase's findings but not copied from this
  phase's sweep data.
- **No code** from this phase's `tools/membrane-kv-bottleneck-analysis/`
  exists on product `main` (verified via `git grep`) — only the
  *finding* was productized (capacity is the value, stop optimizing for
  speed), not the analysis tooling itself.
- The product decision explicitly does **not** ship any of the dynamic
  mechanisms Phases 12C–12F built (live relocation, safe retirement,
  deterministic scheduling) — PR #21's planner is **static, pre-
  context-only**, using only Phase 12B's minimal `kv_dev_override`
  device-placement capability.

## Contents

- `results/canonical/` — 26 artifacts: the full bottleneck map across
  the model/context/split matrix, Pareto analysis, context-scaling and
  model-scaling breakdowns, the larger-model (1.5B) matrix, Q5
  comparison, break-even rule (predeclared), benchmark validity/host-
  noise gating, reproducibility check, weight-vs-compute control,
  graph-split and utilization/transfer-activity diagnostics, dynamic-
  reentry check, and raw per-run captures.
- `patches/phase12g-kv-placement-bottleneck.patch` — incremental
  `git diff`, **Phase 12F HEAD (`79588b6`) → this phase's HEAD
  (`e91923a`)**: the new `tools/membrane-kv-bottleneck-analysis/`
  module, the new `bottleneck` mode added to
  `tools/membrane-kv-tiering-scheduler/tiering_probe.cpp`, the sweep-
  automation scripts (`scripts/phase12g_*.py`), `CMakeLists.txt`
  wiring, and this phase's `scripts/verify-results.py` extension.
  Excludes `results/phase12/kv-placement-bottleneck/` (preserved
  separately above).
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/kv-placement-bottleneck-discovery`
- Source branch HEAD: `e91923a2e4ea8fb28d4d27bea91d604ffb6bda27`
- Base commit: Phase 12F HEAD, `79588b607d06763eaf13da4357f0b6c679310592`
- Date: 2026-08-21
- Chain position: 6th (last) of 6 in the Phase 12B–12G chain
- Productized: **Yes** — finding only, not the analysis code; see
  Productization above
- Full machine-readable record: `MANIFEST.json`

## Product vs. research boundary (chain-wide)

Verified directly against product `main` (`8287e8c14f1b5af2e9e17fa95e85663e94697d85`):

**Product `main` contains**: static, pre-context-only KV placement
(`tools/membrane-run/kv_residency_policy.c`), the `kv_dev_override`
llama.cpp capability from Phase 12B (byte-identical patch), the
`--kv-placement default|gpu|cpu|auto` CLI flag. **No runtime KV
movement of any kind.**

**Research-only** (verified absent from `main` via direct `git grep`):
Phase 12C's live-copy prototype, Phase 12D's `relocate_layer` (and its
disclosed backing-retention crash), Phase 12E's
`llama_invalidate_graph_cache` retirement fix, Phase 12F's deterministic
scheduler. All four remain safe, validated, but unshipped research
primitives.
