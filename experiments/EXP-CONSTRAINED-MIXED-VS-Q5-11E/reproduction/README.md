# Reproduction — EXP-CONSTRAINED-MIXED-VS-Q5-11E

## What's known

- **Base commit**: `bb0b69a290c8de609bdce5494bb9b616972f39b9` (Phase
  11D HEAD, `kadireren7/membrane`).
- **Prototype patch**: `../patches/phase11e-constrained-mixed-vs-q5.patch`
  applies cleanly on top of that exact commit (adds
  `constrained_policy_sim.c/.h`, `sequential_compare.cpp`, and
  `test_constrained_policy_sim.c` under `tools/membrane-kv-mixed-layer/`,
  its `CMakeLists.txt` wiring, and this phase's `scripts/verify-results.py`
  extension).
- **llama.cpp pin**: `c0bc8591e8815c63cb01dd3f051a8b0df02501c9`.
- **Model used**: SmolLM2-360M-Instruct-f16 only (the primary
  constrained point requires this model's specific ctx=180000
  VRAM-boundary behavior on the real GTX 1650).
- **Profile reused, not regenerated**: this phase validates (does not
  regenerate) Phase 11D's `profile_360m_train21.json` — reproducing
  this phase requires that exact profile file, preserved in
  `EXP-MODEL-SPECIFIC-MIXED-POLICY-11D/results/canonical/`.
- **Quality methodology preserved directly**:
  `results/canonical/quality_methodology.json`, `quality_raw.jsonl`,
  and `quality_raw_f16ref.jsonl` contain the true F16 ground-truth
  generation methodology and raw per-prompt records, not just
  aggregates.
- **Hardware sensitivity**: real GTX 1650 (Vulkan), `nvidia-smi` VRAM
  readings for the central 276 MiB measurement.

## What's NOT preserved / unknown

- Quality was measured on 5 pre-registered prompts, not the full
  10-prompt holdout set, due to real host RAM constraints on the
  5.6 GiB dev machine (disclosed in source, in
  `quality_methodology.json`, before any run) — this is a real,
  disclosed scope limit of the source experiment itself, not a
  reproduction gap introduced by this migration.
- Points A and C (`boundary_scan.json`) were not independently
  re-measured for quality/naive-layout comparison in this phase — only
  Point B (the primary constrained point) got the full quality sweep;
  Point A's comfortable-regime conclusion leans on Phase 11D's own
  prior finding.
- The full sanitizer matrix was not re-run in full for this phase given
  real time/host-memory constraints — see
  `results/canonical/regression_sanitizers.json` for exactly what was
  run and why.

## Expected outputs if reproduced

The VRAM delta measurement (276 MiB measured vs. 274.66 MiB
theoretical) is expected to reproduce closely, since it is driven by
`ggml_row_size()`-based byte math cross-validated against a real
`nvidia-smi` reading, not noise. The quality result (mixed loses to
whole-cache Q5, 1 of 5 held-out prompts) is described in source as
deterministic and bit-identical on repeat — expect this to reproduce
exactly on the same hardware/model/profile, not just qualitatively.

## Reproducibility classification

**PARTIALLY_REPRODUCIBLE** — base commit, prototype patch, llama.cpp
pin, the reused Phase 11D profile, and raw per-prompt quality records
are all preserved; the 5-prompt (not 10-prompt) sample size and the
single-model/single-boundary scope are real, disclosed limitations of
the source experiment itself; exact VRAM numbers remain
hardware-sensitive even though the qualitative result is described as
deterministic on the same hardware.
