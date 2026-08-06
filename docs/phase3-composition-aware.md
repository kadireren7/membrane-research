# Phase 3.4 — Composition-Aware Mixed-Precision KV Policy Optimizer

Phase 3.3 built a precision map by unioning **independent** per-layer
scores and found the union did not compose linearly: all 19 "Q4-safe"
layers passed their own 0.995-cosine bar in isolation, but quantizing
them simultaneously produced an aggregate cosine of 0.987 -- below the
very bar each one cleared alone. This phase fixes that by never trusting
an isolated score: a greedy optimizer evaluates every candidate **live,
against the policy as it currently stands**, so what gets measured is
always the real composed effect. Every number below is a live
measurement; nothing is extrapolated from Phase 3.3's cached results.

## Headline

**At an identical memory reduction (2.682x -- both policies happened to
land on exactly 38 of 60 K/V slots quantized), the composition-aware
policy beats Phase 3.3's independent-score policy on logit cosine
similarity on all 6 of 6 prompts**, mean cosine 0.9973 vs 0.9930. The
greedy search accepted 38 candidates and rejected 22 over 60 live
evaluations (289s), then bounded backtracking re-checked the last 4
accepted choices and kept all of them (no single bad interaction was
found among those four). The search directly reproduces, from live
composition data rather than a hypothesis, Phase 3.3's finding that K is
more sensitive than V: **28 of 30 V-slots were accepted vs only 10 of 30
K-slots**, and most K rejections were the strict K-specific cosine bar,
not the shared thresholds.

## Method: what "composition-aware" means here, concretely

**Starting policy** (item 1): all 60 slots (30 layers × {K, V}) at Q8.
No FP16 anywhere -- Phase 3.2 already established uniform Q8 is safe, so
the search space here is only "which Q8 slots can move to Q4," not
whether Q8 itself is safe.

**Greedy search** (item 2): a fixed priority queue -- every V-slot
(ascending layer) before any K-slot (item 5's "V tried first"), K-slots
carrying a *stricter* cosine bar (0.9975 vs 0.995) reflecting the
"higher penalty for K" instruction. Each round pops the next slot in the
queue, builds a trial policy (that one slot at Q4, every previously
accepted slot exactly as accepted, everything else still at Q8), and
evaluates the trial **live across the full valid prompt set** (see
baseline filtering below) -- never against a cached single-slot score.
If the trial clears every threshold, it is accepted immediately and the
next round's trials are built on top of it (composition-aware:
round *k+1* always measures the actual effect of *k* prior acceptances,
not an assumption that they combine additively). This is a
first-improvement greedy (accept the first candidate that passes, in
priority order) rather than a best-of-batch greedy that would compare
several candidates before choosing: the task explicitly rules out full
brute force and asks for a parametrized search budget, and evaluating
exactly one candidate per round bounds total live evaluations to the
number of slots considered (≤60), which comparing batches would not. The
memory/quality ratio is still computed and logged for every candidate
(accepted or not) for the report, even though the accept decision itself
is decided by threshold-clearing in priority order, not by the ratio.

**Backtracking** (item 3): after greedy converges, the last
min(4, accepted-count) acceptances are re-tested **individually**, most
recent first, each against the policy as it stands *at that point in the
backtrack pass* (so an earlier reversion can change what the next one is
tested against -- also composition-aware, not independent). A reversion
is kept if either (a) the slot was demonstrably the specific cause of a
threshold failure in the full policy, or (b) it holds a small share of
the total memory gain (<10%) and reverting it improves cosine by ≥0.001.
This is bounded to exactly 4 extra evaluations, not the 2⁴=16 subsets a
brute-force check of "the last 4" would require.

**Quality thresholds** (item 4, defaults used): aggregate logit cosine
≥0.995 (V) / ≥0.9975 (K, stricter), top-1 ≥98%, top-5 ≥99%, and every
recall test the all-Q8 reference answered correctly on the valid prompt
set must still be answered correctly (never a prompt the FP16 baseline
itself could not solve -- item 6's rule, applied here as "must not
regress *from all-Q8*," which is item 4's exact wording).

**Baseline filtering** (item 6): before any optimization, every
candidate prompt's FP16 baseline is captured fresh and checked against
its expected answer (prompts without a recall check are always valid).
Only prompts the baseline actually solves enter the search's valid
evaluation set; the rest are still run through the final comparison
table for completeness, but clearly labeled "excluded, informational
only" and never allowed to influence an accept/reject decision.

## Experiment setup

Same model, submodule commit, and host as Phases 3.2/3.3:
`SmolLM2-135M-Instruct-f16.gguf` (30 layers, head_dim 64), llama.cpp
`c0bc8591e8815c63cb01dd3f051a8b0df02501c9`, AMD Ryzen 5 5600H /
Linux 6.18 / gcc 13.3.0, CPU-only, 4 threads, greedy sampling, 1024-token
context, 32 generated tokens per experiment, search budget 60 (every
slot considered exactly once in the worst case).

```bash
cmake -S . -B build-llama -G Ninja -DCMAKE_BUILD_TYPE=Release -DMEMBRANE_ENABLE_LLAMA=ON
cmake --build build-llama --target membrane-kv-sensitivity
./build-llama/tools/membrane-kv-sensitivity/membrane-kv-sensitivity \
    --model models/SmolLM2-135M-Instruct-f16.gguf \
    --mode optimize --search-budget 60 \
    --prompt benchmarks/kv/prompts/recall.txt 7429 \
    --prompt benchmarks/kv/prompts/natural.txt - \
    --prompt benchmarks/kv/prompts/code.txt - \
    --prompt benchmarks/kv/prompts/repeat.txt - \
    --prompt benchmarks/kv/prompts/distractor.txt 6183 \
    --prompt benchmarks/kv/prompts/secrets.txt 815 \
    --n-tokens 1024 --gen-tokens 32 \
    --out benchmarks/results/phase3-composition-aware/optimizer.jsonl
```

## Item 6 — baseline filtering result

| prompt | result |
|---|---|
| recall.txt | VALID |
| natural.txt | VALID |
| code.txt | VALID |
| repeat.txt | VALID |
| distractor.txt | **EXCLUDED** -- FP16 baseline itself answers "1953" (a decoy), not "6183" |
| secrets.txt | **EXCLUDED** -- FP16 baseline itself answers "402" (the wrong person's key), not "815" |

Confirms Phase 3.3's finding independently, from a fresh measurement:
4/6 prompts form the valid evaluation set. The all-Q8 reference on that
set: cosine 0.999967, top-1 99.22%, top-5 100% -- the recall-preservation
requirement is anchored to this, not to FP16.

## Item 2/5 — the greedy search, in full

64 live evaluations total (60 greedy + 4 backtracking), 289.4s search
time. Cosine shown is the **aggregate over the 4 valid prompts** after
that slot is added to the policy built so far -- i.e. it visibly and
monotonically drifts down as more slots compound, which is the direct,
measured evidence of non-linear composition this phase set out to find
and control for.

**V-slots** (all 30 tried, tightest-first is layer order since the queue
is ascending): 28 accepted, 2 rejected (layers 16 and 25, both on
top-1 dropping to 97.66% < 98%, cosine was still fine at that point --
composed cosine ranged 0.999952 → 0.998755 across the run as more
V-slots stacked up).

**K-slots** (all 30 tried after V): only **10 accepted** (layers 0, 2,
3, 4, 6, 8, 11, 14, 20, 25), **20 rejected**. The dominant rejection
reason (14 of 20) was the strict 0.9975 K bar being cleared by V-only
composition headroom but not by K -- e.g. layer 12 K: cosine 0.997397,
comfortably above the shared 0.995 bar but below K's own 0.9975,
rejected specifically for that reason. This is item 5's "higher penalty
for K" catching real cases the shared threshold alone would have passed.

## Item 3 — backtracking result

The last 4 accepted slots (all K, in acceptance order: layer 25, 20, 14,
11) were re-tested individually. **All 4 were kept at Q4** -- no single
bad interaction was found. Notably, reverting layer 20 K actually made
the aggregate cosine *worse* (0.997442 without it vs 0.997551 with it),
a direct, measured example of a non-additive interaction running in the
opposite direction from what independent scoring would predict: this
slot's presence is not purely a cost to be justified by its own memory
gain, its removal also disturbs the composition of what remains.

## Item 5 — final layer × K/V precision map

| | Q4-eligible | Q8-only |
|---|---|---|
| **V** (30 slots) | 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,17,18,19,20,21,22,23,24,26,27,28,29 (28) | 16, 25 |
| **K** (30 slots) | 0,2,3,4,6,8,11,14,20,25 (10) | 1,5,7,9,10,12,13,15,16,17,18,19,21,22,23,24,26,27,28,29 (20) |

38/60 slots Q4, 22/60 Q8 -- coincidentally the same slot *count* as
Phase 3.3's 19-whole-layer union (19×2=38), which is exactly what makes
the item-7 comparison below a fair, matched-memory-budget test of
*which* 38 slots to pick, not how many.

## Item 7 — five-way comparison (all 6 prompts)

`all_q8`/`all_q4` are native ggml types (real measured KV bytes);
`phase33_policy`/`phase34_policy` are spliced (analytically projected KV
bytes, per the methodology established in Phase 3.3). FP16 baseline is
omitted from the table (trivially 1.000x/100%/1.0) except as the recall
reference already covered above.

| prompt | config | top-1 | top-5 | cosine | KL | recall | KV |
|---|---|---|---|---|---|---|---|
| recall | all-Q8 | 100% | 100% | 0.99975 | 0.0004 | OK | 1.70x |
| recall | all-Q4 | 96.9% | 100% | 0.97745 | 0.0178 | **FAIL** | 3.21x |
| recall | Phase 3.3 policy | 100% | 100% | 0.98722 | 0.0291 | OK | 2.68x |
| recall | **Phase 3.4 policy** | 96.9% | 100% | **0.99466** | 0.0051 | OK | 2.68x |
| natural | all-Q8 | 96.9% | 100% | 0.99997 | 0.0002 | OK | 1.41x |
| natural | all-Q4 | 87.5% | 100% | 0.99086 | 0.0613 | OK | 2.66x |
| natural | Phase 3.3 policy | 96.9% | 100% | 0.99367 | 0.0407 | OK | 2.68x |
| natural | **Phase 3.4 policy** | 100% | 100% | **0.99862** | 0.0129 | OK | 2.68x |
| code | all-Q8 | 100% | 100% | 0.99994 | 0.0002 | OK | 1.43x |
| code | all-Q4 | 90.6% | 100% | 0.98865 | 0.0294 | OK | 2.69x |
| code | Phase 3.3 policy | 93.8% | 100% | 0.99502 | 0.0163 | OK | 2.68x |
| code | **Phase 3.4 policy** | 100% | 100% | **0.99843** | 0.0062 | OK | 2.68x |
| repeat | all-Q8 | 100% | 100% | 0.99998 | 0.0000 | OK | 1.75x |
| repeat | all-Q4 | 100% | 100% | 0.98793 | 0.0015 | OK | 3.30x |
| repeat | Phase 3.3 policy | 96.9% | 100% | 0.99252 | 0.0048 | OK | 2.68x |
| repeat | **Phase 3.4 policy** | 96.9% | 100% | **0.99849** | 0.0020 | OK | 2.68x |
| distractor* | all-Q8 | 100% | 100% | 0.99979 | 0.0002 | FAIL* | 1.68x |
| distractor* | all-Q4 | 90.6% | 100% | 0.96837 | 0.0322 | FAIL* | 3.16x |
| distractor* | Phase 3.3 policy | 93.8% | 100% | 0.99383 | 0.0102 | OK* | 2.68x |
| distractor* | Phase 3.4 policy | 100% | 100% | **0.99695** | 0.0124 | FAIL* | 2.68x |
| secrets* | all-Q8 | 96.9% | 100% | 0.99968 | 0.0002 | FAIL* | 1.64x |
| secrets* | all-Q4 | 84.4% | 100% | 0.97343 | 0.0205 | OK* | 3.08x |
| secrets* | Phase 3.3 policy | 100% | 100% | 0.99603 | 0.0095 | FAIL* | 2.68x |
| secrets* | Phase 3.4 policy | 100% | 100% | **0.99672** | 0.0331 | FAIL* | 2.68x |

`*` -- distractor/secrets are the excluded, baseline-invalid prompts;
their recall column reflects the base model's own capability (see Phase
3.3), not a quantization effect, exactly as item 6 requires them to be
reported.

**Phase 3.4's cosine is higher than Phase 3.3's on all 6 prompts**
(mean 0.9973 vs 0.9930), at the identical 2.68x memory reduction, and
recall stays correct on every *valid* prompt for both policies (the two
excluded prompts' recall status is not a safety signal either way).
`all-Q4` remains the only configuration to break a valid recall test
outright (`recall.txt`), reconfirming Phase 3.2/3.3's finding on this
run's own fresh measurements.

One honest nuance: `recall.txt`'s own top-1 under Phase 3.4 (96.9%) sits
below the 98% bar used during search -- because that bar is applied to
the **mean across the 4 valid prompts**, not to each prompt individually;
the aggregate cleared 98%+ throughout the search while this one
constituent prompt did not. The policy is safe by the criterion it was
optimized against (the aggregate, plus every prompt's own recall check),
but a per-prompt top-1 floor was not separately enforced. A stricter
variant enforcing the bar per-prompt, not just in aggregate, is a natural
follow-up.

## Item 8 — timing and overhead

| metric | value |
|---|---|
| greedy search wall time | 289.4s (60 evaluations) |
| backtracking wall time | included above (4 evaluations) |
| per-layer sweep (Phase 3.3 comparison policy) | ~90s (unchanged from Phase 3.3) |
| final 5-way comparison (6 prompts × 4 configs) | remainder of the run |
| **total policy decision overhead (this whole run)** | **430.5s (7.2 min)** |
| live evaluations used | 64 / 60 search budget (the 4 extra are backtracking) |
| search budget parameter | `--search-budget 60` (parametrized; a smaller budget stops the queue early and reports exactly how many of the 60 slots were considered) |
| peak RSS (whole process) | 466 MB |

This is a one-time, offline cost per model; it does not repeat once a
policy is fixed.

## Verification (items 10/11)

- The standard MEMBRANE test suite (unchanged by this phase -- no new
  code went into `membrane_core`) remains green: Release, ASan+UBSan,
  and TSan all 14/14.
- The full optimizer flow -- baseline filtering, greedy search,
  backtracking, per-layer/age sweeps, and the final comparison table --
  was built and run under ASan+UBSan against the real model with
  **zero sanitizer diagnostics**.
- The self-test inherited from Phase 3.3 (perturbing nothing reproduces
  the true baseline exactly) still passes; the optimizer's `all_q8`
  reference row and Phase 3.3's independent-policy row match their
  Phase 3.2/3.3 counterparts within the expected run-to-run determinism
  already established (near-identical to 5-6 significant figures).

The raw output (`optimizer.jsonl`, gitignored) lives in
`benchmarks/results/phase3-composition-aware/`; every number above
regenerates with the command in "Experiment setup."

## What this phase changes about the recommendation

Phase 3.3 recommended `adaptive FP16/Q8` (no Q4 at all) as the safest
option and flagged its own `adaptive FP16/Q8/Q4` as carrying a thinner
margin than its construction implied. This phase's composition-aware
policy reaches the **same 2.68x memory reduction Phase 3.3's more
aggressive policy did, but with materially higher cosine similarity on
every single prompt tested (mean 0.9973 vs 0.9930), and no observed
compounding surprises after bounded backtracking**. For any deployment
that wants more than Q8's ~1.4-1.9x and is willing to pay the one-time
~7-minute profiling cost on a comparably sized model, the
composition-aware policy is the better-supported choice between the two
Q4-inclusive options measured across this project; it does not change
the standing conclusion that uniform `all-Q4` is unsafe.
