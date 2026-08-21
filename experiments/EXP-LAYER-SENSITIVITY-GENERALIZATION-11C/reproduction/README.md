# Reproduction — EXP-LAYER-SENSITIVITY-GENERALIZATION-11C

## What's known

- **Base commit**: `8fc37408685ac8dac2d1e59edcfab5961136177b` (Phase
  11B HEAD, `kadireren7/membrane`).
- **Prototype patch**: `../patches/phase11c-layer-sensitivity-generalization.patch`
  applies cleanly on top of that exact commit (adds
  `sensitivity_analysis.c/.h` and `test_sensitivity_analysis.c` under
  `tools/membrane-kv-mixed-layer/`, its `CMakeLists.txt` wiring, and
  this phase's `scripts/verify-results.py` extension).
- **llama.cpp pin**: `c0bc8591e8815c63cb01dd3f051a8b0df02501c9`.
- **membrane pin**: `1c0cb4fbcc08bed9556ee6298e49791f9b5c8f9f`.
- **Models used**: SmolLM2-135M-Instruct-f16 and SmolLM2-360M-Instruct-f16.
- **Prompt set**: 31 prompts across 11 task categories, with a
  documented train/holdout split (21 train / 10 holdout) — the split
  rule and category grouping are preserved in
  `results/canonical/prompts.json`; the prompt text itself is included
  there.
- **Hardware sensitivity**: real GTX 1650 (Vulkan) and the same host's
  CPU for the performance-stability sweep; the memory-pressure boundary
  test used the real ctx=180000/SmolLM2-360M GPU boundary.

## What's NOT preserved / unknown

- The three ablation sweeps (Stage A: 135M/ctx2048, Stage B:
  360M/ctx2048, Stage C: 135M/ctx8192) were run as single-layer
  ablations across all layers per prompt — the exact per-invocation CLI
  is not separately scripted beyond what `manifest.json` documents per
  raw file.
- Cross-context stability (research question 4) is explicitly disclosed
  as testing KV-capacity effects, not long-context-usage effects
  specifically, since both context settings consumed similar actual
  token counts on these prompts — this is a known scope limit of the
  measurement itself, not a reproduction gap.

## Expected outputs if reproduced

The stable-sensitive-layer finding (a small set of layers dominating
sensitivity across nearly every prompt) is expected to reproduce
qualitatively on the same models/prompts, since it held across 31
prompts and 11 categories, not a handful. The held-out policy result
(training-only ranking beats naive layouts on `logit_rel_l2` at every
budget) is the most load-bearing result for downstream phases and
should reproduce as a qualitative ranking; exact `rel_l2`/`delta_nll`
values are real-measurement-sensitive.

## Reproducibility classification

**PARTIALLY_REPRODUCIBLE** — base commit, prototype patch, both commit
pins, and the full prompt set (with its train/holdout split) are all
preserved; exact per-sweep CLI invocations are not separately scripted
beyond `manifest.json`'s per-file documentation, and real-hardware
measurement sensitivity applies to all quantitative results as noted
above.
