# Reproduction — EXP-MIXED-Q8-Q5-FEASIBILITY-11B

## What's known

- **Base commit**: `d6e6189d0417858bfcf3d51c766eaa4be5fe46e1`
  (`kadireren7/membrane`, "feat: add adaptive Q8/Q5 KV policy", PR #20).
- **Prototype patch**: `../patches/phase11b-mixed-q8-q5-feasibility.patch`
  applies cleanly on top of that exact commit (adds the standalone
  `tools/membrane-kv-mixed-layer/` tree, its `CMakeLists.txt` wiring,
  and this phase's `scripts/verify-results.py` extension — no product
  files under `tools/membrane-run/` are touched).
- **llama.cpp pin**: `c0bc8591e8815c63cb01dd3f051a8b0df02501c9`.
- **membrane pin**: `c51b24695dfa56e0740f930f5ece7c79a077e16c`.
- **Models used**: SmolLM2-135M-Instruct-f16 only.
- **Mechanism**: reuses the Phase 4.1 `kv_type_override` callback
  unchanged — no llama.cpp source patch beyond what's already
  tracked/shipped was applied or required for this phase.
- **Hardware sensitivity**: throughput numbers were measured on the
  real GTX 1650 (Vulkan) and the same host's CPU — absolute tok/s on
  other hardware will differ, but the qualitative finding (mixed/Q8/Q5
  throughput indistinguishable at this model's scale) is expected to be
  more portable.

## What's NOT preserved / unknown

- The exact CLI invocations for each of the ratio/position/sensitivity
  ablation sweeps are not separately scripted beyond what each result
  field records (model, context, layer split).
- The 5-prompt subset's exact prompt text lives in the source branch's
  broader prompt-set infrastructure, not duplicated into this
  experiment's own `results/canonical/`; the result file records
  per-metric aggregates, not the raw prompt strings.
- Async/scheduling behavior under real concurrent load was not tested
  this phase.

## Expected outputs if reproduced

The mechanical feasibility result (mixed KV works, `type_map` matches
the requested split, generation completes correctly) is a structural
property of the `kv_type_override` mechanism and should reproduce
exactly. The sensitivity-based policy's advantage over naive layouts on
this specific 5-prompt sample is real-hardware/real-model-measurement
sensitive; expect the qualitative ranking (sensitivity-based best on
`delta_nll` and `top1_preservation`, competitive on `logit_rel_l2`) to
reproduce, not exact percentages. Throughput numbers are the most
noise-sensitive; expect "statistically indistinguishable across
all-Q8/all-Q5/mixed" to reproduce as a qualitative pattern, not exact
tok/s.

## Reproducibility classification

**PARTIALLY_REPRODUCIBLE** — base commit, prototype patch, and both
commit pins are preserved and sufficient to rebuild and re-run this
phase's probe; the model is named (not bundled); exact per-sweep CLI
invocations are not separately scripted, and the quality/throughput
numbers are real-hardware-measurement-sensitive by nature (disclosed
above and in the source artifact's own `limitations` field).
