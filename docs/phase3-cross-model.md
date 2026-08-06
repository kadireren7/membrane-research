# Phase 3.6 — Cross-Model Generalization Benchmark

## Purpose

Phases 3.1–3.5 built and validated a risk-aware, per-prompt-hard-constrained
greedy KV-cache quantization optimizer on a single model, `SmolLM2-135M-Instruct`
(30 layers). That is not, by itself, evidence the optimizer generalizes: a
policy (or even the *methodology*) tuned against one small model's specific
sensitivity pattern could easily be an artifact of that model's size or
training, not a real property of KV-cache quantization. This phase re-runs
the exact same tool, thresholds, and methodology independently on two more
models spanning roughly a 10x parameter range, and adds a transfer-policy
experiment to test whether a policy learned on one model is *safe* (not
just "similar") when denormalized onto another.

**What this phase does NOT do:** it does not claim the optimizer works on
every architecture, does not claim identical per-layer decisions transfer
across models (the transfer-policy experiment specifically tests this and
reports failures honestly if they occur), and does not loosen any quality
threshold to make a larger model's search cheaper — the only sanctioned
scope-reduction tool is a smaller `--search-budget` (item 8), which is
disclosed per model below.

## 1. Model set

Three models, F16 weights (no weight quantization — the KV-cache type is
the only varying precision factor), none committed to the repo
(`models/` and `*.gguf` are gitignored). All measured through the same
llama.cpp submodule commit as every prior phase since Phase 2.1:
`c0bc8591e8815c63cb01dd3f051a8b0df02501c9`.

| | SmolLM2-135M-Instruct | SmolLM2-360M-Instruct | Qwen2.5-1.5B-Instruct |
|---|---|---|---|
| Source | `bartowski/SmolLM2-135M-Instruct-GGUF` | `bartowski/SmolLM2-360M-Instruct-GGUF` | `Qwen/Qwen2.5-1.5B-Instruct-GGUF` (official) |
| File | `SmolLM2-135M-Instruct-f16.gguf` | `SmolLM2-360M-Instruct-f16.gguf` | `qwen2.5-1.5b-instruct-fp16.gguf` |
| License | Apache 2.0 | Apache 2.0 | Apache 2.0 |
| SHA-256 | `f535f83ec568d040f88ddc04a199fa6da90923bbb41d4dcaed02caa924d6ef57` | `7d23be4097d67c5c43c3df62ebc19609a25f483c7d0b77d3989e1d94e36ccab6` | `fc89e330deb3fd8fa560f1c0f35a1e2b8da96d59e13445559ed190307a6f5649` |
| File size | 270,885,952 bytes | 725,553,792 bytes | 3,560,416,288 bytes |
| Architecture | Llama-family | Llama-family | Qwen2 |
| Layers | 30 | 32 | 28 |
| Hidden size | 576 | 960 | 1536 |
| Attention heads | 9 | 15 | 12 |
| KV heads (GQA) | 3 | 5 | 2 |
| head_dim | 64 (576/9) | 64 (960/15) | 128 (1536/12) |
| Trained context length | 8192 | 8192 | 8192 |
| KV slots searched (layers x {K,V}) | 60 | 64 | 56 |

All three head dimensions are multiples of 32 (compatible with ggml's
`Q8_0`/`Q4_0` block-quantized types, block size 32 — this was the exact
compatibility check that ruled out `stories15M`'s head_dim=48 back in
Phase 3.2). llama.cpp commit is unchanged from every prior phase, so any
behavioral difference across models in this report reflects the model,
not a moving inference-engine target.

Tool: `tools/membrane-kv-sensitivity/main.cpp`, run in `--mode risk` with
`--skip-legacy-comparison` (this phase's item 3 asks for
FP16/Q8/Q4/conservative/balanced/aggressive only, not the Phase 3.3/3.4
per-layer/per-age sweep policies from earlier phases — skipping that sweep
avoids ~60-70 extra single-slot experiments per model that this phase does
not need).

## 2. Evaluation set

A model-agnostic 8-prompt package under `benchmarks/kv/prompts/`, covering
the 7 required categories plus one bonus general-generation prompt carried
over from Phase 3.5 for continuity:

| File | Category | Expected answer |
|---|---|---|
| `recall.txt` | exact fact recall | `7429` |
| `distractor.txt` | distractor-heavy recall (3 decoy numbers) | `6183` |
| `secrets.txt` | multi-fact recall (2 people, 2 secrets, only one asked) | `815` |
| `code.txt` | code completion | none (no exact-match gate) |
| `natural.txt` | natural language reasoning/continuation | none |
| `repeat.txt` | repeated context (60x identical sentence) | none |
| `longcontext.txt` | long-context retrieval (~400-word passage, fact ~60% through) | `5927` |
| `short.txt` | general/short generation (bonus, from Phase 3.5) | none |

**Per-model FP16 baseline gate (item 2):** for every model, each prompt's
FP16 baseline is captured fresh and checked against the expected answer
first. Only prompts the model's own FP16 baseline answers correctly enter
the "valid" set that the optimizer's hard gate is computed over —
baseline-invalid prompts are still run and reported in the final
comparison table, explicitly labeled `EXCLUDED, informational only`, but
never gate an accept/reject decision. This is not a hypothetical
safeguard: it fires differently per model (see per-model results below),
which is itself part of what this phase measures.

## 3. Policies

Per model, independently: `all-FP16` (implicit reference), `all-Q8`
(native, uniform `GGML_TYPE_Q8_0`), `all-Q4` (native, uniform
`GGML_TYPE_Q4_0`), and the three Phase 3.5 risk-aware Pareto tiers
(`conservative`/`balanced`/`aggressive`), each **independently searched
from scratch** on that model's own valid prompt set and its own baseline —
no policy or per-layer decision is copied from one model to another as a
starting point. (The transfer-policy experiment, item 5 below, is the
*only* place a policy crosses models, and it is validated against the
target model's own constraints before being called safe.)

## 4. A real measurement bug found and fixed mid-phase

While reviewing the first 135M run's results, the "all-Q8 (native)"
memory-reduction ratio came out **below 1x for `short.txt`** (0.174x —
implying Q8 quantization *increased* KV memory), which is physically
impossible for a fixed cell count. Root cause: `run_kv_combo()` measured
the native Q8/Q4 KV byte size *after* generating `gen_tokens` (128) more
tokens, while `base.f16_state_bytes` (the ratio's numerator) was measured
*before* generation, on the prompt alone. For prompts much shorter than
`gen_tokens` (like the one-sentence `short.txt`), the extra 128 cells from
generation dominate the native measurement's cell count, artificially
shrinking the apparent ratio — the exact same class of numerator/
denominator mismatch the project caught once before for the projected/
spliced metric (Phase 3.4).

Fix: `run_kv_combo()` now measures native KV bytes immediately after the
prompt decode, before any generation — the same cell-count basis as
`base.f16_state_bytes`. Verified after the fix: `all-Q8` native ratio is a
consistent ~1.88x across every prompt on 135M (previously ranged 0.17x–
1.46x depending on prompt length). The pareto search's own accept/reject
decisions were **not** affected by this bug (they use `run_experiment`,
which never called the buggy function), so the existing checkpoint could
be resumed rather than re-searched from scratch. All reported native-type
memory ratios below are post-fix.

A second, smaller gap fixed in the same pass: the risk-aware comparison
table previously measured `all-Q4`'s native *memory/speed* but never its
*quality* (cosine/top1/top5/recall) — added, since item 6's "clearly
better quality than all-Q4" success criterion is unverifiable without it.

A third gap, in the checkpoint/resume mechanism itself (found while smoke-
testing item 8 before any real model run): resuming from a checkpoint
originally reset the live-evaluation counter to 0, so a search interrupted
after using its full `--search-budget` and then resumed would silently
search *another* full budget's worth of new candidates on top of the
already-decided ones — quietly exceeding the disclosed budget on every
resume. Fixed so `search_budget` bounds the total live evaluations across
the whole logical search, interrupt/resume boundaries included; verified
by interrupting a real search mid-tier and confirming the resumed run's
final policy and timing were byte-identical to an uninterrupted control
run. The checkpoint format was also extended to persist each decision's
cosine/top1/top5/reason (not just accept/reject), so a search resumed
more than once doesn't lose layer-position/rejection-reason data for
slots decided in an earlier invocation.

## 5. Results — SmolLM2-135M

**Baseline filtering:** 5/8 prompts valid (`recall`, `code`, `natural`,
`repeat`, `short`). `distractor.txt` and `secrets.txt` were excluded here
too, consistent with Phase 3.3/3.5's reconfirmed finding that this model's
FP16 baseline doesn't reliably pick the right number out of a
distractor-heavy passage. **New finding this phase:** `longcontext.txt` is
also excluded — 135M's FP16 baseline does not reliably retrieve a fact
placed ~60% through a ~400-word passage at this context length. This is
disclosed, not hidden: `longcontext.txt` still runs and is reported in the
final table, labeled `EXCLUDED, informational only`, and never gates a
decision.

**Search:** `--search-budget 60` (all 60 slots — 30 layers x {K,V} —
exhaustive, not a reduced subset), `--robustness-budget 8`, `gen_tokens
128`, `n_tokens 1024`. Total wall clock for the first from-scratch run:
4344.2s (72.4 min); a later re-run (adding the all-Q4 quality
measurement, after the bug fixes above) completed in 1090.2s by resuming
from checkpoint — all 180 search decisions fast-forwarded with 0 new live
evaluations, confirming the resume mechanism reproduces the original
search exactly.

| Config | min per-prompt cosine | min top1 | min top5 | mean KV reduction | 0 hard-gate violations |
|---|---|---|---|---|---|
| all-Q8 (native) | 0.999880 | 99.22% | 100.00% | 1.880x | yes |
| all-Q4 (native) | **0.967050** | 89.84% | 99.22% | 3.545x | **no** |
| conservative | 0.998578 | 99.22% | 100.00% | 2.254x | yes |
| balanced | 0.998643 | 99.22% | 100.00% | 2.341x | yes |
| aggressive | 0.998387 | 98.44% | 100.00% | 2.623x | yes |

(Metrics computed over the 5 valid prompts only; `all-Q4`'s min cosine and
hard-gate failure come from the same mechanism Phase 3.3 first exposed:
naive uniform Q4 is not safe, even though its aggregate numbers alone
would look fine.)

**Item 6 success criteria — all five met on this model:**
≥2.0x reduction (2.254x–2.623x, all three tiers) — zero valid-prompt
hard-gate violations — min per-prompt cosine ≥0.995 (0.9984–0.9986) —
clearly better quality than all-Q4 (0.9984+ vs 0.9671, and Q4 actually
breaks recall on a valid prompt) — higher reduction than all-Q8 (2.254x+
vs 1.880x).

**Layer-position distribution (accepted Q4 slots, 30 layers, 0-indexed):**

| Tier | K accepted | V accepted |
|---|---|---|
| conservative | none (0/30) | 21/30: `0,1,3,4,5,6,7,9,10,11,13,17,18,20,21,22,23,25,26,27,29` |
| balanced | 4/30: `8,9,26,29` | 21/30 (same as conservative) |
| aggressive | 9/30: `0,1,5,6,8,10,20,25,28` | 27/30: `0-12,15-28` (mostly all, gaps at 13,14) |

**K vs V sensitivity:** K is markedly more sensitive than V at every
margin on this model — 0 K slots survive the conservative margin at all
(the extra `K_STRICT_COSINE=0.9975` bar plus conservative's own margin is
essentially unreachable), and even at aggressive (zero margin), only 9/30
K slots are accepted vs 27/30 V slots. No obvious early/mid/late-layer
clustering in the accepted K set (0,1,5,6,8,10,20,25,28 spans the full
depth); V is nearly uniformly acceptable except two mid-network gaps
(layers 13–14).

**Rejection reasons:** at `conservative`, every rejection is a cosine
-margin failure (39/39) — the tight cosine bar, not top1/top5, is the
binding constraint at this margin. At `balanced` and `aggressive`, top1
failures dominate (24/35 and 22/24 respectively) — `short.txt`'s general-
class 98.5%/98.0% top1 bar (one wrong token in 128 costs 0.78 points) is
the single most common rejection cause across both tiers.

**Robustness (item 5 of Phase 3.5, reused):** balanced policy re-searched
under original/reversed/shuffled prompt orderings at reduced budget 8 —
0 mismatched decisions across all 3 orderings (16 total candidate x
ordering comparisons). Order-independence holds.

**Real hardware performance (item 7, native types only — see the
scope-limitation note under §6):**

| Config | TTFT | tok/s | KV bytes | peak RSS |
|---|---|---|---|---|
| all-FP16 | 248.0ms | 63.1 | 9,843,948 | 781 MB |
| all-Q8 | 316.8ms | 66.2 | 5,232,348 | 781 MB |
| all-Q4 | 337.2ms | 64.2 | 2,772,828 | 781 MB |

TTFT is slightly *higher* for the quantized native types here (extra
quantize/dequantize work per decode step outweighs any memory-bandwidth
win at this small a model and this short a prompt) — tok/s differences
are within noise. Peak RSS is dominated by the model weights and compute
buffers at this size, not the KV cache, so it does not vary by config.

## 6. Results — SmolLM2-360M

**Baseline filtering:** **8/8 prompts valid** — unlike 135M, this model's
FP16 baseline correctly answers `distractor.txt`, `secrets.txt`, and
`longcontext.txt` too. This is the first direct evidence in this phase
that baseline task capability, not just quantization tolerance, changes
with scale: a 2.7x larger model solves recall tasks the smaller one
cannot, independent of anything the KV-cache optimizer does.

**Search:** `--search-budget 20` (of 64 total slots — a real, disclosed
reduction from the exhaustive search used on 135M, per item 8; see the
scope-limitation note below), `--robustness-budget 4`, same `gen_tokens
128`/`n_tokens 1024` as 135M. Total wall clock: 6109.8s (101.8 min).

| Config | min per-prompt cosine | min top1 | min top5 | mean KV reduction | 0 hard-gate violations |
|---|---|---|---|---|---|
| all-Q8 (native) | 0.999918 | 98.44% | 100.00% | 1.881x | yes |
| all-Q4 (native) | **0.986200** | 85.16% | 99.22% | 3.550x | **no** |
| conservative | 0.999924 | 99.22% | 100.00% | 1.939x | yes |
| balanced | 0.999924 | 99.22% | 100.00% | 1.939x | yes |
| aggressive | 0.999694 | 98.44% | 100.00% | **2.098x** | yes |

**Item 6 success criteria:** `conservative`/`balanced` fall short of the
2.0x reduction bar (1.939x) — a direct, disclosed consequence of the
reduced 20-candidate search budget, not a quality problem (their min
cosine, 0.999924, is the best of any tier on this model). `aggressive`
clears all five criteria: 2.098x reduction, zero hard-gate violations,
min cosine 0.999694 ≥ 0.995, clearly better quality than all-Q4 (0.9997
vs 0.9862, and Q4 breaks recall on this model too), and higher reduction
than all-Q8 (2.098x vs 1.881x). **360M is the second model to satisfy
item 6's success bar (the user's requirement was "at least two models" —
135M and 360M together already meet it).**

**Layer-position distribution:**

| Tier | K accepted | V accepted |
|---|---|---|
| conservative | 0/32 (untested) | 4/20 tested: `3,6,10,12` |
| balanced | 0/32 (untested) | 4/20 tested (same) |
| aggressive | 0/32 (untested) | 14/20 tested: `0,2,3,4,5,7,8,9,10,11,12,15,16,18` |

**Scope limitation, disclosed (more severe than it first looks):** with
`search_budget=20`, every tier's live search evaluated **exactly V-slots
for layers 0–19, and nothing else** — confirmed directly from the
decision log (all `pareto_decision` records for all three tiers have
`kv="V"` and `layer` in `[0,19]`). Layers 20–31's V-sensitivity is
**untested**, and **zero K-slots were evaluated at any layer, in any
tier**. The priority queue design (V-slots ascending layer 0..31, then
K-slots ascending layer 0..31) means a budget below 32 can never reach a
K-slot at all, and a budget of exactly 20 stops partway through V. **The
"0 K accepted" and "only early/mid V layers accepted" results for 360M
are therefore not sensitivity findings** the way 135M's exhaustive-search
results are — they are an artifact of where the reduced budget happened
to stop. This is exactly the kind of gap item 4 asked to be reported
honestly rather than glossed over: for 360M, this run answers "is a
partial-depth Q4-V-only policy safe" but does **not** answer "is K more
sensitive than V" or "is there a layer-position pattern" — those would
need a budget ≥33 (32 V-slots plus at least one K-slot) to even begin
answering, and a budget of 64 (exhaustive, matching 135M's methodology)
to answer with the same confidence as the 135M result.

**Robustness:** STABLE, 0 mismatched decisions across 3 orderings (4
candidates x 2 comparisons).

**Transfer-policy check (item 5):** the 135M-learned normalized balanced
policy (4 K-fractions, 21 V-fractions), denormalized onto 360M's 32
layers, is **UNSAFE**: `recall.txt` drops to 97.66% top1, below the
99.50% recall-critical bar (99.50% = 98.50% class floor + the balanced
tier's 0.5-point margin used on 135M). Per the explicit requirement, this
transfer policy is **not accepted** — 360M's own independently-searched
balanced policy (1.939x, min cosine 0.999924) is used instead. This is a
genuine negative result: a policy that generalizes perfectly well *in
which layers get quantized, proportionally* does not automatically
generalize in *whether that quantization stays safe* — the two questions
are different, and only the second one is what "safe to deploy" actually
requires.

**Real hardware performance:**

| Config | TTFT | tok/s | KV bytes | peak RSS |
|---|---|---|---|---|
| all-FP16 | 640.8ms | 25.2 | 17,495,836 | 1242 MB |
| all-Q8 | 822.7ms | 24.9 | 9,297,436 | 1242 MB |
| all-Q4 | 769.6ms | 25.9 | 4,924,956 | 1242 MB |

Same pattern as 135M: quantized native types don't show a real decode-
speed win at this model size (differences are within run-to-run noise),
memory is the only real, measured benefit of native quantization. TTFT
and tok/s are both roughly 2.5x worse than 135M's, consistent with the
~2.7x hidden-size-driven compute increase.

## 7. Qwen2.5-1.5B — attempted, incomplete (disclosed, not hidden)

The model itself is fully verified and compatible: downloaded, SHA-256
confirmed against the official repo's published hash
(`fc89e330deb3fd8fa560f1c0f35a1e2b8da96d59e13445559ed190307a6f5649`,
3,560,416,288 bytes), architecture checked (head_dim=128, divisible by 32,
compatible with `Q8_0`/`Q4_0`), and it loads and runs correctly under the
same llama.cpp commit as the other two models (confirmed via the tool's
own baseline-capture and self-test passes, which did complete).

**What did not complete:** even with the most conservative parameters
used anywhere in this phase (`--search-budget 8`, `--robustness-budget
2`, i.e. an intentionally small candidate search), **baseline filtering
alone — capturing and classifying all 8 prompts, before the search even
starts — did not finish within 71 minutes** of wall-clock time on this
host (4 of 8 prompts' baselines had been captured when the run was
stopped). Extrapolating from the 135M -> 360M scaling observed above
(roughly 2.7x more wall time for a ~2.7x-2.9x hidden-size increase),
1.5B's ~4.2x parameter count and 2x head_dim over 360M put a full
baseline-filter-plus-search-plus-comparison run at several hours minimum
on this host — outside what was practical to complete in this session.

**This is reported as an honest scope limitation, not suppressed.** No
Qwen2.5-1.5B quality, memory-reduction, or performance numbers are
reported anywhere in this document, the summary CSV, or the JSONL,
because none were measured. The attempt's partial log is kept at
`benchmarks/results/phase3-cross-model/qwen15b-incomplete-attempt.log`
for anyone who wants to verify this account. Re-running Qwen2.5-1.5B to
completion (on faster hardware, or with a longer time budget and the
existing `--checkpoint`/`--resume` mechanism to span multiple sessions)
is the natural next step this phase did not reach — the tooling built for
it (pre-screening, resumable checkpointing, transfer-policy evaluation)
is already in place and was validated on the two models that did
complete.

The user's own success criterion for this phase was "at least two
models" meeting the item 6 bar; 135M and 360M do (§5, §6), so this gap
does not block the phase's stated goal, but it does mean the "does the
optimizer generalize across roughly a 10x parameter range" question is
only answered up to the 2.7x range actually measured (135M -> 360M), not
the full ~11x range (135M -> 1.5B) originally intended.

## 8. Cross-model generalization analysis (item 4)

Answering the four specific questions this phase asked, honestly bounded
by what was actually measured — the 135M/360M budget difference (60,
exhaustive, vs 20, partial) is a real confound in several of these and is
called out wherever it applies rather than glossed over.

**Is K more sensitive than V in every model?** Only answerable for 135M,
where the exhaustive search gives a clean result: yes, sharply so — 0/30
K-slots survive even the conservative margin, vs 21/30 V-slots, and even
at zero margin (aggressive) only 9/30 K-slots pass vs 27/30 V-slots. For
360M, the reduced budget never evaluated a single K-slot (§6), so this
question is **not answerable** from this run — "every model" cannot be
claimed from n=1. The Qwen attempt did not reach the search phase at all
(§7). **Verdict: unconfirmed beyond a single data point.**

**Is there a common early/mid/late-layer pattern?** On 135M (full-depth
data), no: the accepted K layers (`0,1,5,6,8,10,20,25,28` at aggressive)
span the entire depth without visible early/mid/late clustering, and V is
acceptable almost everywhere except two mid-network layers (13-14). On
360M, only layers 0-19 of 32 were ever tested, so a depth-wide pattern
can't be assessed at all — within that tested range, accepted V layers
(`3,6,10,12` at conservative/balanced; adds `0,2,4,5,7-9,11,15,16,18` at
aggressive) don't show an obvious early-vs-late split either, but this is
a partial-depth sample, not a real answer. **Verdict: no pattern found in
the one model where it could be tested; the second model's data cannot
address the question.**

**Does the safe-Q4 ratio change as models grow?** The *native* memory
ratios (all-Q8 ~1.88x, all-Q4 ~3.55x) are essentially identical across
135M and 360M — expected, since these are pure functions of the ggml
`Q8_0`/`Q4_0` block format (block size 32) applied to a KV row, not of
model architecture. For the *risk-aware* policies, 135M's aggressive tier
reached 2.623x vs 360M's 2.098x — a real difference, but **this
comparison is confounded by the search budget being exhaustive (60) on
135M and partial (20) on 360M**: 360M simply had fewer candidates
considered before the budget ran out, independent of whether more of its
layers would have been safe to quantize. This phase cannot cleanly
separate a "larger models tolerate less Q4" effect from a "smaller search
budget finds less" effect with the data collected — an equal-budget
(ideally exhaustive-on-both) re-run would be needed to answer this
properly.

**Do the same thresholds work across all models?** Yes, in the sense that
matters most: the same class thresholds, margins, and `gen_tokens=128`
granularity (all established on 135M in Phase 3.5) produced a real,
non-empty, all-constraints-satisfied policy on 360M too, with no
threshold needing to be loosened or `gen_tokens` needing to be raised to
make anything achievable. Notably, 360M's *achieved* quality at every
tier (min cosine 0.9997-0.9999) is higher than 135M's (min cosine
0.9984-0.9986) — suggestive that the larger model may be more numerically
tolerant of KV quantization, not less, but this is confounded by the same
budget difference as the previous question (360M's search accepted fewer,
more conservatively-placed candidates simply because it considered
fewer) and should not be read as a confirmed trend.

**Overall:** the one comparison this phase can make cleanly across both
completed models — does *some* independently-searched, per-prompt-hard-
constrained policy exist that beats both memory-reduction and quality
bars simultaneously — is yes on both 135M and 360M (§9). The deeper
structural questions (K-vs-V pattern, layer-position pattern, ratio-vs-
scale trend) need an equal, sufficiently large search budget on every
model to answer without a budget confound, which this phase's time
constraints did not allow for the two larger models.

## 9. Success criteria (item 6) — final status

The user's bar: in at least two models, simultaneously (a) ≥2.0x KV
memory reduction, (b) zero valid-prompt hard-gate violations, (c) minimum
per-prompt cosine ≥0.995, (d) clearly better quality than all-Q4, and
(e) higher memory reduction than all-Q8.

| Model | Tier | (a) ≥2.0x | (b) 0 violations | (c) cos≥0.995 | (d) beats Q4 | (e) beats Q8 | All 5? |
|---|---|---|---|---|---|---|---|
| 135M | conservative | 2.254x yes | yes | 0.9986 yes | yes (Q4: 0.9671, broke recall) | yes (1.880x) | **yes** |
| 135M | balanced | 2.341x yes | yes | 0.9986 yes | yes | yes | **yes** |
| 135M | aggressive | 2.623x yes | yes | 0.9984 yes | yes | yes | **yes** |
| 360M | conservative | 1.939x no | yes | 0.9999 yes | yes (Q4: 0.9862, broke recall) | yes (1.881x) | no (memory only) |
| 360M | balanced | 1.939x no | yes | 0.9999 yes | yes | yes | no (memory only) |
| 360M | aggressive | 2.098x yes | yes | 0.9997 yes | yes | yes | **yes** |
| Qwen2.5-1.5B | — | not measured (§7) | | | | | not measured |

**Result: both 135M and 360M have at least one tier meeting all five
criteria** (135M meets it at every tier; 360M meets it at `aggressive`).
The user's "at least two models" bar is met. The two 360M tiers that miss
criterion (a) do so by a disclosed, understood margin cause (search
budget, §6), not a quality problem — their cosine is in fact the best of
any tier measured on any model.

## 10. Verification (item 10)

- **Build:** `cmake --build build-llama --target membrane-kv-sensitivity`
  (Release) and `cmake --build build-llama-asan --target
  membrane-kv-sensitivity` (ASan+UBSan) both clean, zero warnings, after
  every change made this phase (including the two bug fixes in §4).
- **Existing test suite:** `ctest --test-dir build-rel` and `ctest
  --test-dir build-asan` both 14/14 passing — this phase added no new
  code to `membrane_core`, only to the standalone
  `membrane-kv-sensitivity` tool, so the core suite is an unchanged
  regression check, not new coverage.
- **Self-test:** `self_test()` (perturbing nothing must reproduce the
  true baseline exactly: top1=100%, top5=100%, cosine=1.0, RMSE=0.0)
  passed on both SmolLM2-135M and SmolLM2-360M before either model's real
  run was trusted.
- **Resumable checkpointing, actually exercised on real interrupted
  work:** the 135M run's checkpoint (180 live decisions across 3 tiers)
  was used for two real resumes mid-phase — once to re-measure with the
  `run_kv_combo` fix, once more to add the all-Q4 quality row — both
  times fast-forwarding all 180 decisions with 0 new live evaluations and
  reproducing byte-identical final metrics, in addition to the smaller
  synthetic interrupt/resume smoke test run before any real model
  (documented in §4).
- **Order-independence (robustness check, reused from Phase 3.5):**
  STABLE (0 mismatches) on both 135M and 360M.
- **Not re-verified this phase:** the Phase 3.3/3.4 per-layer/per-age
  sweep and legacy policies were intentionally skipped
  (`--skip-legacy-comparison`, §1) since item 3 doesn't ask for them; they
  remain validated by their own phase's docs and were not touched by any
  code change in this phase.

## 11. Outputs

- `docs/phase3-cross-model.md` — this document.
- `benchmarks/results/phase3-cross-model-summary.csv` — per-model,
  per-tier aggregate metrics (memory reduction, min cosine/top1/top5,
  hard-gate status, K/V accept-reject counts) plus native-hardware
  performance rows and the transfer-policy verdict.
- `benchmarks/results/phase3-cross-model.jsonl` — machine-readable,
  concatenated from the two completed models' full per-prompt,
  per-config output (`row` records), per-slot search decisions
  (`pareto_decision` records), native performance (`native_perf`
  records), and the transfer-policy verdict (`transfer_policy` record).
  335 lines, all validated as parseable JSON.
- `benchmarks/results/phase3-cross-model/` — per-model raw logs,
  checkpoints (`*.ckpt`, resumable), and exported normalized policies
  (`*-balanced-normalized.json`), kept for reproducibility.
- `benchmarks/results/phase3-cross-model/qwen15b-incomplete-attempt.log`
  — partial log from the stopped Qwen2.5-1.5B attempt (§7), kept as
  evidence rather than deleted.

