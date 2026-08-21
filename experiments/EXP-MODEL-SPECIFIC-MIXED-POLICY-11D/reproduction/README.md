# Reproduction — EXP-MODEL-SPECIFIC-MIXED-POLICY-11D

## What's known

- **Base commit**: `eaeb25fed8c12bf924f5f42a3d78cd151cd130ca` (Phase
  11C HEAD, `kadireren7/membrane`).
- **Prototype patch**: `../patches/phase11d-model-specific-mixed-policy.patch`
  applies cleanly on top of that exact commit (adds `model_profile.c/.h`
  and `test_model_profile.c` under `tools/membrane-kv-mixed-layer/`, 5
  new `scripts/build_*`/`calibration_size_analysis.py` helper scripts,
  its `CMakeLists.txt` wiring, and this phase's `scripts/verify-results.py`
  extension).
- **llama.cpp pin**: `c0bc8591e8815c63cb01dd3f051a8b0df02501c9`.
- **Models used**: SmolLM2-135M-Instruct-f16 and SmolLM2-360M-Instruct-f16.
- **The adaptive-policy comparison uses the real, unmodified product
  CLI**: `build-cpu-rc1/tools/membrane-run/membrane-run --kv adaptive
  --compare-kv`, called exactly as any user would — this phase's own
  `whole_cache_adaptive_comparison.json` explicitly notes it does not
  modify or extend the product CLI, so reproducing it requires building
  the real product (`kadireren7/membrane`) at a commit that includes PR
  #20, not just this experiment's own patch.
- **Determinism verified in source**: profile generation is
  byte-identical across two independent runs for both models
  (`results/canonical/reproducibility.json`).
- **Generated profiles are preserved directly**: `profile_135m_train21.json`
  and `profile_360m_train21.json` are the actual generated per-model
  profiles, not just a description of them.

## What's NOT preserved / unknown

- The calibration-size analysis is a pure reanalysis of Phase 11C's
  raw ablation data — reproducing it requires Phase 11C's raw JSONL
  files (preserved in that experiment's own `results/canonical/`), not
  a fresh run.
- Exact CLI flags for the memory-pressure boundary test
  (`memory_pressure_360m.json`) beyond what the result file itself
  records are not separately scripted.

## Expected outputs if reproduced

The head-to-head result (adaptive beats mixed in comfortable memory,
mixed beats whole-cache Q5 consistently) is expected to reproduce
qualitatively, since it held across both tested models with a
consistent pattern. The determinism property (byte-identical profiles
across repeated runs) is a structural property of the deterministic
sort-based ranking construction and should reproduce exactly, not just
qualitatively.

## Reproducibility classification

**PARTIALLY_REPRODUCIBLE** — base commit, prototype patch, llama.cpp
pin, generated profiles, and both models (by name) are preserved;
reproducing the adaptive-policy comparison specifically requires
building the real product at a commit including PR #20 (not just this
experiment's own patch), and real-hardware measurement sensitivity
applies to the quantitative quality/performance numbers.
