# EXP-KV-Q4-STORAGE-10A — Experimental Q4_0 KV cache storage (index)

This directory preserves the research record for MEMBRANE Phase 10A: an
experimental Q4_0 KV cache storage mode, evaluated end-to-end (real ggml
allocation, real decode, real quality/performance/memory measurement) but
**not productized**. No product KV precision mode in `main` implements
Q4_0 — `--kv q8`/`--kv q5` exist instead (see `EXP-KV-Q5-EVALUATION-10B`).

## Question

Can KV cache tensors be stored genuinely as Q4_0 (not a shadow/simulated
copy — the real ggml block format, real allocation, real decode) on both
CPU and Vulkan, and if so, is the resulting quality good enough to ship?

## Result

- **Memory**: real, exact win. Q4_0's real 4.5-bit-per-element block
  format (2-byte fp16 scale + 16 bytes of packed 4-bit quants per
  32-element block) puts real observed KV allocation at 28.125% of
  native (F16) — an exact match to the block-format-derived expectation
  at every tested context size (512–16384) and on both tested models
  (`results/canonical/storage_theoretical_vs_observed.json`).
- **Performance**: no meaningful cost. The measured Vulkan delta vs Q8
  (-1.09%) was within both modes' own run-to-run noise
  (`results/canonical/performance_repetition_135m_vulkan.json`).
- **GPU residency**: a real, meaningful capacity win — Q4 KV lets more
  model layers stay GPU-resident at a fixed context, and supports a
  substantially larger full-GPU-residency context range than Q8 or
  native (`results/canonical/gpu_memory_pressure_360m.json`).
- **Quality**: a real, reproducible cost, and it is the reason this
  mode was not shipped. On both tested models, Q4_0's logit rel-L2 and
  delta-NLL are more than an order of magnitude worse than Q8's
  (12.8×–13.4× and 49.6×–58.6× respectively), and top-1 token
  preservation drops materially (88.7%–92.6% vs 98.8% for Q8)
  (`results/canonical/quality_ladder.json`, section 11).

## Verdict

Literal source field (`results/canonical/q4-kv-validation.json`,
`decision_gate.verdict`):

> **`Q4_MEMORY_WIN_QUALITY_TOO_HIGH_COST`**

Full reasoning (same field, `decision_gate.reasoning`, quoted in full):

> "Storage/backend/performance results are all strongly positive: Q4_0
> KV works authoritatively (no shadow cache) end-to-end on both CPU and
> Vulkan via the pure public API, byte accounting matches theory
> exactly at every measured point, [...]" — see the full field in
> `results/canonical/q4-kv-validation.json` for the complete text; not
> re-typed here to avoid drift from the source.

**Scope of this conclusion**: tested on SmolLM2-135M and SmolLM2-360M,
CPU and Vulkan backends, contexts 512–16384. This is a real, reproducible
finding *in the tested regime* — it is not a claim that Q4 quantization
is universally unusable for every model/task, only that it failed this
phase's quality bar on the models and prompts actually tested.

## What happened instead

Phase 10A's own charter explicitly forbade productizing Q4 before its
own quality-bar decision was in (`results/canonical/section15_bench_cli_decision.json`
records the CLI-surface decision: *"NOT implemented in Phase 10A"*, for
exactly this reason). The project moved directly to evaluating Q5_0/Q5_1
as a middle ground in Phase 10B — see `EXP-KV-Q5-EVALUATION-10B`, which
resulted in `--kv q5` shipping in product (PR #19).

## Contents

- `results/canonical/` — 10 raw/measured artifacts (quality ladder, memory-pressure
  ladder, storage-theory-vs-observed comparison, CLI-surface decision
  record, and raw per-run JSONL captures for CPU/Vulkan/second-model
  matrices).
- `patches/phase10a-q4-kv-storage-prototype.patch` — the actual prototype
  code (not new files — incremental changes to existing product tool
  files: `tools/membrane-run/{compat_check,main,product_cli}.{c,h,cpp}`,
  `tools/membrane-llama-runtime/{decode_loop.cpp,kv_store_telemetry.h}`,
  plus the Phase-10A-specific `scripts/verify-results.py` verifier
  extension) that implemented Q4_0 KV mode support for this experiment,
  as a single `git diff`-format patch against its real base commit.
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/q4-kv-storage`
- Source branch HEAD: `4aa6757fc8a0e68d338b9815684277b68a423a50`
- Base commit (branch point): `a6d5377c852cc1eeb88d224b25726f74c515a405`
  ("chore: prepare MEMBRANE v0.3.0-rc1")
- Date: 2026-08-14
- Productized: **No**. Superseded by the Phase 10B Q5 evaluation
  (`EXP-KV-Q5-EVALUATION-10B`), which was productized instead.
- Full machine-readable record: `MANIFEST.json`
