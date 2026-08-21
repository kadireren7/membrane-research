# Experiments

| Experiment | Question | Status | Decision | Maintained/promoted? | README |
|---|---|---|---|---|---|
| EXP-FPGA-DIV-001 | Can the Q4_0 datapath's general-purpose FP32 divider be replaced by something with meaningfully fewer synthesized cells, bit-exact? | Complete (4 phases) | `PROMOTE_CANDIDATE` → merged | **Yes** — [kadireren7/membrane#2](https://github.com/kadireren7/membrane/pull/2) | [README.md](EXP-FPGA-DIV-001/README.md) |
| EXP-FPGA-DIV-002 | Can the same idea (exact radix-4 division) work for Q8_0's *dual*-divider case, and can the resulting scheduler's collateral cost be bounded? | Complete (5 phases) | `RESEARCH_COMPLETE_NO_PROMOTION` | No — experimental only, nothing merged | [README.md](EXP-FPGA-DIV-002/README.md) |
| EXP-KV-Q4-STORAGE-10A | Does experimental Q4_0 KV cache storage clear a quality bar sufficient for product use? | Complete (1 phase) | `Q4_MEMORY_WIN_QUALITY_TOO_HIGH_COST` | No — real quality cost too high; superseded by EXP-KV-Q5-EVALUATION-10B | [README.md](EXP-KV-Q4-STORAGE-10A/README.md) |
| EXP-KV-Q5-EVALUATION-10B | Do Q5_0/Q5_1 KV storage close the quality gap Q4_0 left open, while still saving meaningful memory over Q8? | Complete (1 phase) | `Q5_PRODUCT_CANDIDATE` (Q5_1 preferred) | **Yes** — [kadireren7/membrane#19](https://github.com/kadireren7/membrane/pull/19) (`--kv q5`) | [README.md](EXP-KV-Q5-EVALUATION-10B/README.md) |
| EXP-MIXED-Q8-Q5-FEASIBILITY-11B | Is per-layer heterogeneous Q8_0/Q5_1 KV storage feasible, and does sensitivity-based layer selection beat naive layouts? | Complete (1 phase) | `B_MIXED_LAYER_EXISTING_MEMBRANE_INFRA_EXISTS` | No — feasible, no throughput edge; motivated 11C's generalization check | [README.md](EXP-MIXED-Q8-Q5-FEASIBILITY-11B/README.md) |
| EXP-LAYER-SENSITIVITY-GENERALIZATION-11C | Does the sensitivity ranking generalize across prompts, categories, models, and context sizes? | Complete (1 phase) | Mixed, per-question (no single literal constant in source) | No — research-only; held-out policy win motivated 11D | [README.md](EXP-LAYER-SENSITIVITY-GENERALIZATION-11C/README.md) |
| EXP-MODEL-SPECIFIC-MIXED-POLICY-11D | Does a real per-model mixed policy beat the product's shipped whole-cache adaptive Q8/Q5 policy? | Complete (1 phase) | `MODEL_SPECIFIC_POLICY_WORKS_BUT_NO_ADAPTIVE_ADVANTAGE` | No — research-only; loses to adaptive in comfortable memory, motivated 11E | [README.md](EXP-MODEL-SPECIFIC-MIXED-POLICY-11D/README.md) |
| EXP-CONSTRAINED-MIXED-VS-Q5-11E | At the real boundary where adaptive is forced to Q5, does mixed KV beat whole-cache Q5 on quality? | Complete (1 phase) | `CONSTRAINED_MIXED_WORKS_BUT_NO_QUALITY_WIN` | No — terminal negative result; justified not shipping mixed-KV product complexity | [README.md](EXP-CONSTRAINED-MIXED-VS-Q5-11E/README.md) |
| EXP-KV-RAM-VRAM-TIERING-12A | Can KV cache be placed/moved between RAM and VRAM independently of model weight placement, using only pre-existing public mechanisms? | Complete (1 phase) | `TIERING_REQUIRES_UPSTREAM_CHANGE` | No — motivated Phase 12B's device-override patch | [README.md](EXP-KV-RAM-VRAM-TIERING-12A/README.md) |
| EXP-KV-DEVICE-OVERRIDE-12B | Can KV cache placement be independently overridden from weight placement via a minimal llama.cpp patch? | Complete (1 phase) | `KV_DEVICE_OVERRIDE_VIABLE` | **Yes** (llama.cpp patch only) — [kadireren7/membrane#21](https://github.com/kadireren7/membrane/pull/21) (`--kv-placement`) | [README.md](EXP-KV-DEVICE-OVERRIDE-12B/README.md) |
| EXP-KV-PROMOTION-PREFETCH-12C | Can live KV copy occur safely, and does the live decode graph actually consume the relocated copy? | Complete (1 phase) | `LIVE_KV_COPY_WORKS_BUT_REBIND_STATIC` | No — research-only; motivated Phase 12D's relocate primitive | [README.md](EXP-KV-PROMOTION-PREFETCH-12C/README.md) |
| EXP-KV-RUNTIME-RELOCATE-12D | Can runtime KV backing actually be rebound/relocated, and is it safe to free the old backing? | Complete (1 phase) | `RUNTIME_KV_RELOCATE_WORKS_BUT_LEAKS_BACKING` | No — research-only; real release-crash disclosed, motivated Phase 12E's fix | [README.md](EXP-KV-RUNTIME-RELOCATE-12D/README.md) |
| EXP-KV-BUFFER-RETIREMENT-12E | Can old KV backing be retired (freed) safely after relocation? | Complete (1 phase) | `KV_BUFFER_RETIREMENT_VIABLE` | No — research-only; root cause fixed, dynamic movement still not shipped | [README.md](EXP-KV-BUFFER-RETIREMENT-12E/README.md) |
| EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F | Can dynamic scheduling exploit the safe relocate+retire mechanism for real performance benefit? | Complete (1 phase) | `WORKS_BUT_NO_PERF_ADVANTAGE` | No — research-only; scheduler works but no measured throughput win at tested scale | [README.md](EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F/README.md) |
| EXP-KV-PLACEMENT-BOTTLENECK-12G | Where is KV placement's actual value — throughput or capacity? | Complete (1 phase) | `KV_PLACEMENT_CAPACITY_ONLY_ON_TESTED_REGIME` | **Yes** (finding only) — [kadireren7/membrane#21](https://github.com/kadireren7/membrane/pull/21) (`--kv-placement`) | [README.md](EXP-KV-PLACEMENT-BOTTLENECK-12G/README.md) |

## Phase 11 progression: mixed Q8/Q5 KV precision (11B → 11E)

One linear research chain (11B → 11C → 11D → 11E, each a superset of
its predecessor), ending in a negative result that justified *not*
shipping additional product complexity. Full detail in each phase's
own README; this table is intentionally short.

| Phase | Question | Verdict | Product consequence |
|---|---|---|---|
| 11B | Is per-layer mixed Q8/Q5 KV feasible, and does sensitivity-based selection beat naive layouts? | `B_MIXED_LAYER_EXISTING_MEMBRANE_INFRA_EXISTS` | Feasible, no throughput edge; motivated 11C |
| 11C | Does the sensitivity ranking generalize across prompts/categories/models/context? | Mixed per-question (no single literal constant; see experiment README) | Held-out policy beats naive baselines; motivated 11D |
| 11D | Does a real per-model mixed policy beat the shipped whole-cache adaptive policy? | `MODEL_SPECIFIC_POLICY_WORKS_BUT_NO_ADAPTIVE_ADVANTAGE` | Loses to adaptive in comfortable memory; motivated 11E |
| 11E | At the boundary where adaptive is forced to Q5, does mixed KV beat whole-cache Q5? | `CONSTRAINED_MIXED_WORKS_BUT_NO_QUALITY_WIN` | Nothing shipped; justified keeping the adaptive policy (PR #20) unchanged |

**Product decision**: keep the shipped whole-cache adaptive Q8/Q5
policy ([PR #20](https://github.com/kadireren7/membrane/pull/20)) and
explicit `--kv q5` ([PR #19](https://github.com/kadireren7/membrane/pull/19))
exactly as they are. Do **not** build a Q8→mixed→Q5 hierarchy — per-layer
mixed precision, sensitivity-driven layer policy, model-specific mixed
policy, and constrained mixed policy all remain validated, safe,
research-only mechanisms; product `main` was verified (`git grep`) to
contain none of them.

## Phase 12 progression: KV residency (12A → 12G)

One linear research chain (12B → 12C → 12D → 12E → 12F → 12G, each a
superset of its predecessor; 12A precedes and motivated the chain but
is not its ancestor), ending in a single product feature. Full detail
in each phase's own README; this table is intentionally short.

| Phase | Question | Verdict | Product consequence |
|---|---|---|---|
| 12A | Can KV be placed/moved RAM↔VRAM independent of weights, with existing public API only? | `TIERING_REQUIRES_UPSTREAM_CHANGE` | Motivated 12B; nothing shipped |
| 12B | Can KV device placement be independently overridden via a minimal patch? | `KV_DEVICE_OVERRIDE_VIABLE` | llama.cpp patch shipped byte-identical in PR #21 |
| 12C | Does live KV copy work, and does decode consume the copy? | `LIVE_KV_COPY_WORKS_BUT_REBIND_STATIC` | Nothing shipped; motivated 12D |
| 12D | Can KV backing actually be rebound at runtime? | `RUNTIME_KV_RELOCATE_WORKS_BUT_LEAKS_BACKING` | Nothing shipped; real leak/crash disclosed, motivated 12E |
| 12E | Can old backing be retired safely after relocation? | `KV_BUFFER_RETIREMENT_VIABLE` | Nothing shipped; root cause + fix stay research-only |
| 12F | Does dynamic scheduling built on 12B–12E win on performance? | `WORKS_BUT_NO_PERF_ADVANTAGE` | Nothing shipped; scheduler stays research-only |
| 12G | Where is the real value — speed or capacity? | `KV_PLACEMENT_CAPACITY_ONLY_ON_TESTED_REGIME` | Finding shipped as PR #21's static `--kv-placement` planner |

**Product decision**: ship only static, pre-context KV residency
placement (`--kv-placement default\|gpu\|cpu\|auto`) using Phase 12B's
minimal device-override mechanism. Do **not** ship live relocation,
graph invalidation/retirement, or the dynamic scheduler — all three
remain validated, safe, research-only primitives (12C–12F), and product
`main` was verified (`git grep`) to contain none of them.

## Negative / stopped directions (repo-wide index)

Concise pointers only — full evidence lives in each experiment's own
`results/canonical/`. See also `paper/tables/negative-result-summary.md`
for the paper's own curated negative-result table (Phases 2/5/6),
which this index does not duplicate.

| Finding | Verdict | Experiment |
|---|---|---|
| Q4_0 KV cache: real memory win, quality cost too high for product use | `Q4_MEMORY_WIN_QUALITY_TOO_HIGH_COST` | [EXP-KV-Q4-STORAGE-10A](EXP-KV-Q4-STORAGE-10A/README.md) |
| Model-specific mixed KV policy works but loses to the shipped adaptive policy in comfortable memory | `MODEL_SPECIFIC_POLICY_WORKS_BUT_NO_ADAPTIVE_ADVANTAGE` | [EXP-MODEL-SPECIFIC-MIXED-POLICY-11D](EXP-MODEL-SPECIFIC-MIXED-POLICY-11D/README.md) |
| At the real boundary where adaptive is forced to Q5, mixed KV is measurably worse quality than whole-cache Q5 — terminal result for the Phase 11 chain | `CONSTRAINED_MIXED_WORKS_BUT_NO_QUALITY_WIN` | [EXP-CONSTRAINED-MIXED-VS-Q5-11E](EXP-CONSTRAINED-MIXED-VS-Q5-11E/README.md) |
| Existing public KV placement API is insufficient — moves weights and KV together, not independently | `TIERING_REQUIRES_UPSTREAM_CHANGE` | [EXP-KV-RAM-VRAM-TIERING-12A](EXP-KV-RAM-VRAM-TIERING-12A/README.md) |
| Live KV copy works, but the live decode graph keeps reading the original — rebind is static | `LIVE_KV_COPY_WORKS_BUT_REBIND_STATIC` | [EXP-KV-PROMOTION-PREFETCH-12C](EXP-KV-PROMOTION-PREFETCH-12C/README.md) |
| Runtime KV relocate works, but a second relocation with release enabled reproducibly crashes the next decode | `RUNTIME_KV_RELOCATE_WORKS_BUT_LEAKS_BACKING` | [EXP-KV-RUNTIME-RELOCATE-12D](EXP-KV-RUNTIME-RELOCATE-12D/README.md) |
| Deterministic dynamic KV scheduler works correctly, but no throughput advantage at tested scale | `WORKS_BUT_NO_PERF_ADVANTAGE` | [EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F](EXP-DYNAMIC-KV-TIERING-SCHEDULER-12F/README.md) |
| Stop optimizing KV placement for speed — capacity is the only value observed in the tested regime | `KV_PLACEMENT_CAPACITY_ONLY_ON_TESTED_REGIME` | [EXP-KV-PLACEMENT-BOTTLENECK-12G](EXP-KV-PLACEMENT-BOTTLENECK-12G/README.md) |

See `ROADMAP.md` at this repository's root for what happens next
(including why `EXP-FPGA-DIV-002` is closed rather than continued as a
`B5`) and each experiment's own README for full phase-by-phase detail.
