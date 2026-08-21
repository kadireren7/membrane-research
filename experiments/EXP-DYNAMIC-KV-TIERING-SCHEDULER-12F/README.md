# EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F — Deterministic dynamic KV tiering scheduler

Phase 12F of the MEMBRANE Phase 12B–12G KV residency research chain.
Builds a scheduler on top of the now-safe relocate+retire primitives
from [EXP-KV-RUNTIME-RELOCATE-12D](../EXP-KV-RUNTIME-RELOCATE-12D/README.md) /
[EXP-KV-BUFFER-RETIREMENT-12E](../EXP-KV-BUFFER-RETIREMENT-12E/README.md).

**Previous:** [EXP-KV-BUFFER-RETIREMENT-12E](../EXP-KV-BUFFER-RETIREMENT-12E/README.md)
**Next:** [EXP-KV-PLACEMENT-BOTTLENECK-12G](../EXP-KV-PLACEMENT-BOTTLENECK-12G/README.md)

## Question

Can dynamic scheduling exploit the now-safe relocate+retire mechanism
for real performance benefit?

## What this phase did

`tools/membrane-kv-tiering-scheduler/scheduler_core.c` is a llama-free,
deliberately **non-adaptive** policy core: every decision is a pure
function of `(config, decode_idx, current per-layer state)` — no
attention scores, no learned hotness, no history beyond an explicit
predeclared configuration. It never talks to llama.cpp directly; a
real driver (`tiering_probe.cpp`) calls the already-established Phase
12B–12E primitives (`llama_kv_cache_relocate_layer`,
`llama_invalidate_graph_cache`) to apply whatever the core decides. No
new policy logic was added inside llama.cpp itself this phase.

## Result

- **Correctness**: dynamic-schedule output is byte-identical to
  static full-GPU, static full-CPU, and every static mixed split
  tested, at the exact same sequence positions
  (`results/canonical/correctness.json`).
- **Mechanism**: every requested transition in the first end-to-end
  schedule occurred, was correctly applied via
  `relocate_layer`+`invalidate_graph_cache`, and decode continued
  without error (`results/canonical/first_schedule.json`).
- **Determinism**: two independent runs against identical state
  produce an identical decision trace
  (`results/canonical/determinism.json`).
- **Stability**: 56 back-to-back relocations (>50 required) show no
  VRAM growth (`results/canonical/stability.json`).
- **Fail-closed**: an intentionally impossible budget correctly skips
  the promotion rather than attempting it
  (`results/canonical/budget_guard.json`).
- **Generality**: the same schedule mechanism was verified at Q5_1
  precision, on SmolLM2-360M, at ctx=16384, and under a memory-pressure
  regime (`results/canonical/q5.json`, `second_model.json`,
  `long_context.json`, `pressure_test.json`).
- **But — no performance advantage at this scale**: the static hot-set
  sweep underlying this decision (0/8/15/22/30 of 30 GPU-KV layers)
  shows throughput varying only **~21.9–24.3 tok/s** — KV placement is
  not the bottleneck at this model/context scale
  (`results/canonical/hotset_sweep.json`, `pareto.json`), so the
  scheduler's correct, stable, deterministic transitions add
  measurable per-transition cost
  (`results/canonical/transition_latency.json`, `frequency_sweep.json`)
  without a compensating throughput win. This directly echoes the
  project's own prior Phase 6.2 finding of no bytes/token win at
  tested scale.
- **Disclosed deviations from the original spec**: the pressure-regime
  test used ctx=65536 instead of the suggested ~200000, because the
  shared host had only ~214 MiB free RAM and 4.9/9.6 GiB swap in use
  from unrelated concurrent processes at test time — `journalctl`
  showed no OOM before or after the scaled-down runs
  (`results/canonical/pressure_test.json`). Async prefetch was analyzed
  but not attempted — no async variant of `relocate_layer` exists in
  the established primitive chain
  (`results/canonical/prefetch_feasibility.json`).

## Verdict

Literal source field (`results/canonical/summary.json`, `decision_gate`),
**quoted in full, not abbreviated**:

> **`WORKS_BUT_NO_PERF_ADVANTAGE`**

This is the exact, complete literal string from source — one of five
predeclared allowed values (`DYNAMIC_KV_TIERING_VIABLE`,
`WORKS_BUT_NO_PERF_ADVANTAGE`, `TOO_SLOW`, `BACKEND_LIMITED`,
`INCONCLUSIVE`), not a paraphrase or an abbreviation of a longer
string.

**This is a valuable negative result, not a claim that dynamic tiering
is universally useless.** It is scoped explicitly to the regime
actually tested: SmolLM2-135M/360M, GTX 1650, contexts up to 65536.
The phase's own `next_step_recommendation` is explicit: do not proceed
to an adaptive/learned hotness policy; the deterministic scheduler
mechanism itself is validated and safe to keep as a research primitive,
but the next useful step is finding a model/context/workload regime
where KV placement actually IS the bottleneck before investing further
in policy sophistication.

## Productization

**No.** Verified: `git grep` for `scheduler_core` /
`MEMBRANE_TIERING_` against product `main` returns no hits. The
scheduler's own explicit non-goals were respected throughout: no
ML/adaptive/learned-hotness policy, no token/block paging, no CUDA
work, no public product CLI, no llama.cpp source changes this phase.

## Contents

- `results/canonical/` — 46 artifacts: state machine, budget guard,
  correctness, determinism, the first end-to-end schedule, the hot-set
  throughput sweep and Pareto analysis (the data underlying the
  no-perf-advantage verdict), frequency/promotion-group sweeps,
  transition latency, stability, failure cases, Q5/second-model/
  long-context/pressure-regime generality checks, prefetch feasibility,
  and raw per-run captures (including raw decision traces).
- `patches/phase12f-dynamic-kv-tiering-scheduler.patch` — incremental
  `git diff`, **Phase 12E HEAD (`73ae8ba`) → this phase's HEAD
  (`79588b6`)**: the new `tools/membrane-kv-tiering-scheduler/` tool
  tree, its `CMakeLists.txt` wiring, and this phase's
  `scripts/verify-results.py` extension. Excludes
  `results/phase12/dynamic-tiering-scheduler/` (preserved separately
  above).
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/dynamic-kv-tiering-scheduler`
- Source branch HEAD: `79588b607d06763eaf13da4357f0b6c679310592`
- Base commit: Phase 12E HEAD, `73ae8ba12b9ce477d33ca407b9923f5d826e20b1`
- Date: 2026-08-20
- Chain position: 5th of 6 in the Phase 12B–12G chain
- Productized: **No** — see Productization above
- Full machine-readable record: `MANIFEST.json`
