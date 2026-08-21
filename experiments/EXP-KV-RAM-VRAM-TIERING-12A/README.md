# EXP-KV-RAM-VRAM-TIERING-12A — RAM↔VRAM KV tiering feasibility (index)

This directory preserves the research record for MEMBRANE Phase 12A: the
project's first investigation into tiering llama.cpp's KV cache between
system RAM and GPU VRAM using only the mechanisms available at the time
(no llama.cpp source changes attempted or made this phase). This is a
**decision-point** result, not a failed project — it identified exactly
what was and wasn't possible with existing public mechanisms, and its
own `next_step_recommendation` directly named the fix that Phase 12B
([`EXP-KV-DEVICE-OVERRIDE-12B`](../EXP-KV-DEVICE-OVERRIDE-12B/README.md))
went on to build — the first phase of the Phase 12B–12G KV residency
chain (see [`experiments/README.md`](../README.md)'s "Phase 12
progression" table for the full chain through productization).

**Next:** [EXP-KV-DEVICE-OVERRIDE-12B](../EXP-KV-DEVICE-OVERRIDE-12B/README.md) (not an ancestor branch — a motivated, independent follow-on)

## Question

Can MEMBRANE independently place or move llama.cpp's KV cache between
host RAM and GPU VRAM — separately from where the model's weights live —
using the public API and mechanisms that already existed at this point
in the project, with no llama.cpp source changes?

## What this phase actually demonstrated (and what it did not)

To keep this record precise and not blur into later phases' work,
this experiment is described strictly in terms of the five capabilities
below — **only A and (a coarser form of) B were demonstrated here**.
C, D, and E did not exist yet in any form and are not claimed by this
experiment:

| Capability | This phase (12A) |
|---|---|
| A. Moving/copying KV data | **Yes, proven** — a real, live, single-tensor GPU↔host byte copy via 100% public API (`cb_eval` + `ggml_tensor::view_src` + `ggml_backend_tensor_get`/`set`), round-trip byte-identical, decode continued correctly afterward (`results/canonical/roundtrip_correctness.json`). New working infrastructure MEMBRANE did not have before this phase. |
| B. Controlling initial KV backing device, independent of weights | **No, not achieved** — proven *impossible* with existing public API. The only working placement control at this point is the existing `--gpu-layers N` mechanism, and it places a layer's KV on the **same** device as that layer's weights, because both are resolved through the identical `model.dev_layer(il)` lookup (`llama-kv-cache.cpp:214-225` calling the same per-layer device assignment `llama-model.cpp:1307` uses for weight offload — verified directly against real `gpu_policy` output this phase; see `results/canonical/architecture.json`). This coupling is exactly what motivated Phase 12B. |
| C. Changing model weight placement | Not this phase's target; weight placement is the pre-existing `--gpu-layers` behavior, unchanged. |
| D. Runtime KV relocation (moving an already-constructed context's KV without rebuilding it) | **Not attempted this phase.** The Phase 12A copy proof (capability A) is a standalone/live-tensor-level copy demonstration, not a reusable runtime relocation primitive — that capability was built later, in Phase 12D, on top of Phase 12B's device-override patch (which this phase's own findings motivated but did not build). |
| E. Dynamic scheduling (automatic promotion/demotion policy) | **Not attempted this phase.** Section 16 of this phase's own summary explicitly lists candidate hot/cold signals as a "research-only listing... none implemented this phase" — this phase was scoped to manual/static placement analysis only. Dynamic scheduling was built much later, in Phase 12F, on the full Phase 12B–12E chain. |

## Result

- **Real positive findings**: a working, public-API-only single-tensor
  live copy mechanism (new capability); genuine external VRAM reduction
  confirmed via real `nvidia-smi` readings (not a shadow/simulated
  number) for the existing combined weight+KV `--gpu-layers N`
  mechanism, at every tested context on both models
  (`results/canonical/vram_reduction.json`, `second_model.json`); a
  real, measured ~10–13% usable-context extension on the actual
  constrained hardware (GTX 1650) using that already-shipped mechanism,
  where whole-cache adaptive and whole-cache explicit Q5 already fail
  (`results/canonical/pressure_test.json`).
- **The blocking finding**: roughly half of the measured VRAM saving
  from `--gpu-layers N` is weight VRAM, not KV VRAM specifically
  (`results/canonical/memory_accounting.json`) — true KV-only placement,
  independent of weights, was not achieved, because of the
  `model.dev_layer(il)` coupling described above.
- **Copy cost, measured, not assumed**: 0.55–0.70 ms per 786 KB
  single-layer copy (1.1–1.4 GB/s effective, well below PCIe theoretical
  because non-pinned host memory forces the synchronous transfer path);
  single-layer movement is cheap in decode-throughput terms (under 3.5%
  stall even at ctx=16384), but an all-layers/every-8-tokens worst-case
  scenario gets materially worse at larger contexts (47% stall,
  ~halved throughput at ctx=16384) (`results/canonical/copy_microbench.json`,
  `decode_impact.json`).
- **Async/pinned-memory transfer**: real async transfer infrastructure
  exists in the pinned Vulkan backend, but requires pinned host memory
  to activate; this phase's probes did not use pinned memory, so its
  bandwidth numbers are synchronous-path numbers only, not a fair test
  of async's real potential (`results/canonical/prefetch_feasibility.json`)
  — disclosed as a limitation, not glossed over.

## Verdict

Literal source field (`results/canonical/summary.json`, `decision_gate`):

> **`TIERING_REQUIRES_UPSTREAM_CHANGE`**

Rationale (same file, `decision_gate_rationale`, quoted in full):

> "The phase's actual target capability -- authoritative KV cache
> placement across GPU VRAM and system RAM, independent of weight
> placement, with controlled movement -- requires a llama.cpp change
> (decoupling llama_kv_cache's per-layer buffer-type resolution from
> model.dev_layer(il), which currently also governs weight placement).
> This was identified precisely (file, function, smallest fix,
> upstreamability, maintenance risk -- architecture.json) and NOT
> written, per Section 2's explicit STOP instruction. This verdict is
> not a rejection of tiering as a direction: this phase also produced
> substantial, real, positive evidence [...] that should inform
> whatever comes next, but that existing mechanism moves weights and
> KV together, which is a different, coarser capability than what this
> phase set out to prove feasible."

**Why existing placement behavior was insufficient**: the only
placement control available at this point (`--gpu-layers N`) is
structurally a weight-placement mechanism that happens to carry KV
along with it, not an independent KV-placement mechanism — verified
directly against the pinned llama.cpp source (`llama-kv-cache.cpp:214-225`,
`llama-model.cpp:1307`), not inferred.

**Scope of this conclusion**: tested against the specific pinned
llama.cpp commit this phase used (see `MANIFEST.json`), on SmolLM2-135M
and SmolLM2-360M. This is a real, source-verified architectural finding
about that llama.cpp version's coupling, not a permanent claim about
llama.cpp in general — a later llama.cpp version, or a patch, could
change this (which is exactly what Phase 12B went on to do).

## Forward link: what this motivated

This phase's own `next_step_recommendation` (`results/canonical/summary.json`)
states, verbatim:

> "Design and, in a future phase, propose the smallest llama.cpp patch
> (the kv_dev_override callback in llama-kv-cache.cpp, mirroring
> kv_type_override's existing shape and precedent) that decouples KV
> cache buffer-type resolution from weight buffer-type resolution --
> this is the single blocking change standing between this phase's
> proven building blocks (safe live single-tensor copy, real VRAM
> accounting) and true KV-only RAM<->VRAM tiering. Do NOT execute it in
> this phase."

The subsequent research branch, `experiment/kv-device-override`
(source HEAD `a1ebd9e285bd763cfb448c8d8202536802783bb9`, Phase 12B —
[`EXP-KV-DEVICE-OVERRIDE-12B`](../EXP-KV-DEVICE-OVERRIDE-12B/README.md)),
built exactly this: a minimal `kv_dev_override` callback patch to
llama.cpp, closing the gap this phase identified. Phase 12B and the
Phase 12B–12G research chain it started ultimately informed the static
KV residency product feature — see `productization` note below for why
that is a research-lineage link, not a direct claim about this phase.

## Productization

**This phase itself was not productized.** No code from this branch
(`tools/membrane-kv-tiering/`, its CMakeLists.txt wiring, or its
`scripts/verify-results.py` extension) exists on `main` or was reused
by any later branch — verified directly (`git grep` for this branch's
distinctive symbols/paths against `main` and `experiment/kv-device-override`
returned no hits).

The later static KV residency product feature (`--kv-placement`,
`kadireren7/membrane` PR #21) is a **different experiment's** direct
result (Phase 12G's capacity-only finding) — it is not this phase's
productization, only a distant downstream consequence via the research
lineage this phase started. `productized: false` in `MANIFEST.json`
reflects this phase specifically.

## Contents

- `results/canonical/` — 18 raw/measured artifacts: architecture
  study and the weight/KV-coupling correction, copy round-trip proof,
  copy-cost microbenchmark, placement-unit/policy design, exact
  GPU/RAM byte accounting, VRAM-reduction tests on two models, the
  GTX-1650 pressure test, decode-throughput impact across four
  scenarios, async/prefetch feasibility, failure-mode testing, and raw
  per-run JSONL captures.
- `patches/phase12a-kv-ram-vram-tiering-prototype.patch` — the actual
  prototype code as a single `git diff`-format patch against this
  phase's real base commit (`d6e6189`): the new, standalone
  `tools/membrane-kv-tiering/` tool tree (llama-free placement-policy
  arithmetic plus three real-GPU integration probes), its `CMakeLists.txt`
  wiring, and this phase's `scripts/verify-results.py` extension. One
  patch is sufficient here (unlike the Phase 10 migration's two
  chained patches) because this branch is a single commit against a
  single base, not a linear chain of phases.
- `reproduction/README.md` — what's needed to re-run this, and what
  isn't preserved.

## Provenance

- Source repository: `kadireren7/membrane`
- Source branch: `experiment/kv-ram-vram-tiering`
- Source branch HEAD: `c0029afad68c2aed7b7232ae8cd2e727cde6585c`
- Base commit (branch point): `d6e6189d0417858bfcf3d51c766eaa4be5fe46e1`
  ("feat: add adaptive Q8/Q5 KV policy") — this is the exact commit
  that was `main`'s tip for most of this research session, and remains
  an ancestor of current `main`.
- Date: 2026-08-15
- Chain position: **independent** — this branch is not an ancestor of
  the Phase 12B–12G chain (`experiment/kv-device-override` through
  `experiment/kv-placement-bottleneck-discovery`); it precedes and
  motivated that chain but shares no commit history with it.
- Productized: **No** — see Productization section above.
- Full machine-readable record: `MANIFEST.json`
