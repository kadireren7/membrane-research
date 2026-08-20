# Reproduction — EXP-KV-RAM-VRAM-TIERING-12A

## What's known

- **Base commit**: `d6e6189d0417858bfcf3d51c766eaa4be5fe46e1`
  (`kadireren7/membrane`, "feat: add adaptive Q8/Q5 KV policy").
- **Prototype patch**: `../patches/phase12a-kv-ram-vram-tiering-prototype.patch`
  applies cleanly on top of that exact commit (adds the standalone
  `tools/membrane-kv-tiering/` tree, its `CMakeLists.txt` wiring, and
  this phase's `scripts/verify-results.py` extension — no product
  files under `tools/membrane-run/` or `tools/membrane-llama-runtime/`
  are touched).
- **llama.cpp pin**: `c0bc8591e8815c63cb01dd3f051a8b0df02501c9` (this
  branch's `third_party/llama.cpp` submodule pin) — this experiment
  used llama.cpp's stock, unmodified per-layer device resolution
  (`model.dev_layer(il)`, `llama-kv-cache.cpp:214-225`); no llama.cpp
  source patch was applied or required for this experiment.
- **Models used**: SmolLM2-135M-Instruct and SmolLM2-360M-Instruct
  (GGUF files not bundled here — obtain from their original source,
  e.g. HuggingFace `HuggingFaceTB/SmolLM2-135M-Instruct` /
  `HuggingFaceTB/SmolLM2-360M-Instruct`).
- **Hardware sensitivity**: the VRAM-reduction and pressure-test
  results (`results/canonical/vram_reduction.json`,
  `results/canonical/pressure_test.json`) were measured on an NVIDIA
  GTX 1650 (4096 MiB VRAM) via real `nvidia-smi` readings — absolute
  VRAM/context numbers on other GPUs will differ; the qualitative
  finding (combined weight+KV placement saves real VRAM; true KV-only
  placement is not achievable via this mechanism) is architecture-level
  and more portable.
- **Build**: `tools/membrane-kv-tiering/placement_policy.c` builds
  llama-free (`add_library(membrane_tiering_placement_policy ...)`,
  unconditional); the three GPU integration probes
  (`copy_microbench.cpp`, `decode_impact_probe.cpp`,
  `graph_node_probe.cpp`, `live_movement_probe.cpp`) require
  `MEMBRANE_ENABLE_LLAMA=ON` and a real model to run against.

## What's NOT preserved / unknown

- No separately committed run script for the GPU integration probes —
  they were invoked manually this session; the exact CLI invocations
  are not preserved beyond what each result artifact's own fields
  record (model, context, layer counts).
- The decode-impact scenarios reload the model fresh per scenario for
  clean isolation, so absolute cross-scenario timing comparisons carry
  some model-load-variance noise — this is disclosed in the source
  artifact itself (`results/canonical/summary.json`, `limitations`),
  not hidden here.
- Async/pinned-memory transfer performance was not measured this phase
  at all (see README's "Result" section) — there is nothing to
  reproduce for that path from this experiment's own evidence.

## Expected outputs if reproduced

Re-running the round-trip copy proof
(`results/canonical/roundtrip_correctness.json`) against the same
model/context should reproduce the same qualitative result
(byte-identical restoration, checksum match) — this is a structural
property of the copy mechanism, not a noisy measurement. The VRAM
numbers (`vram_reduction.json`, `second_model.json`, `pressure_test.json`)
are real hardware measurements and will vary with driver/VRAM
availability on a different host, though the qualitative
weight+KV-combined-savings pattern should reproduce. The copy-bandwidth
and decode-impact numbers (`copy_microbench.json`, `decode_impact.json`)
are the most host-sensitive — expect the qualitative pattern
(single-layer cheap, all-layers-every-8-tokens expensive and worse at
larger contexts) to reproduce, not the exact percentages.

## Reproducibility classification

**PARTIALLY_REPRODUCIBLE** — base commit, prototype patch, llama.cpp
pin, and models (by name) are all preserved and sufficient to rebuild
and re-run the experiment's probes; exact CLI invocations for the
manually-run GPU integration probes are not separately scripted, and
several numeric results are real-hardware-measurement-sensitive by
their own nature (disclosed above and in the source artifacts
themselves), so exact original numbers should not be expected to
reproduce byte-for-byte even on the same hardware.
