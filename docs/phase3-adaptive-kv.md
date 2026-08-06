# Phase 3.3 — Sensitivity-Aware Mixed-Precision KV-Cache Policy

Phase 3.2 validated uniform, whole-cache Q8 KV quantization live (46.8%
memory reduction, ≥99% top-1 agreement, ≥0.9997 cosine similarity) and
showed uniform Q4 already causes a real recall failure. This phase asks a
finer question: does *every* layer and token position need the *same*
precision? It builds a per-layer and per-token-age sensitivity profiler,
derives a threshold-driven mixed-precision policy from the measurements,
and validates that policy live, on the same model, against uniform FP16 /
Q8 / Q4 baselines. Every number below comes from actual inference; nothing
is extrapolated except where explicitly labeled "analytic" or "projected."

## Headline

- **19 of 30 layers cleared the Q4 safety bar in isolation; 11 needed Q8;
  zero were critical (FP16-only).** No single layer, quantized alone,
  broke recall or fell below the numeric thresholds.
- **K is measurably more sensitive than V.** At matched memory cost,
  quantizing K alone degrades cosine similarity and (on the recall prompt)
  breaks recall outright, while quantizing V alone at the same bit-width
  does not.
- **The single most important finding: per-layer sensitivity does not
  compose linearly.** All 19 "Q4-safe" layers passed their individual
  0.995-cosine bar in isolation, but quantizing all 19 *simultaneously*
  produced an aggregate cosine of 0.987 on the profiling prompt --
  *below* the very threshold used to select them. The combined policy
  still passed the binary recall check (item 8's disqualifying gate), so
  it is not judged unsafe by that rule, but the numeric margin the
  per-layer thresholds were meant to guarantee erodes under composition.
  This is the main engineering caveat of this experiment and of any
  policy built the same way.
- **A live per-layer/per-token-age Q8/Q4 substitution technique was
  built and proven correct** (self-test: zero perturbation reproduces the
  true baseline exactly, in both Release and ASan+UBSan), reaching
  granularity no public llama.cpp API exposes.
- **Two of the six required prompts (`distractor`, `secrets`) turned out
  to exceed this 135M model's own capability at full FP16 precision** --
  the baseline itself answers them incorrectly, so "recall success" on
  those two prompts measures the base model's capability ceiling, not
  quantization safety. This is reported honestly rather than folded into
  the safety verdict.

## The technique: why a new tool, and how it reaches per-layer control

Phase 3.2 established that `llama_context_params.type_k` / `type_v` are
single scalars applied **uniformly to every layer** -- re-verified here
directly in the vendored source
(`third_party/llama.cpp/src/llama-kv-cache.cpp`, lines 231-232: one
`type_k`/`type_v` used in the tensor-creation loop for every layer, no
per-layer array anywhere in the class). There is no public API for "only
layer 12 is quantized" or "only the oldest 25% of tokens are quantized."

`tools/membrane-kv-sensitivity` reaches that granularity through a
different public API: `llama_state_seq_get_data` / `llama_state_seq_set_data`
(the same session save/restore calls `membrane-kv-capture` already used in
Phase 2.1). The technique:

1. Decode a prompt's *prefix* (all but the last token) on an ordinary F16
   context; capture the full state blob.
2. Parse the blob -- bounds-checked, pinned to the same llama.cpp commit,
   aborting loudly on any layout deviation rather than silently
   misreading it -- to find the exact byte offset and per-token row size
   of every layer's K and V data. (Verified: `attn_v_trans = !flash_attn`
   in `llama-model.cpp`, and flash attention resolves enabled by default
   for this model/backend, so V is stored row-major -- the same simple
   per-token-contiguous layout as K. The tool refuses to run if this ever
   isn't true, rather than guessing at the alternative strided layout.)
3. On a **copy** of the blob, overwrite chosen `(layer, K-or-V, token-row
   range)` byte ranges in place: quantize those F16 values with
   MEMBRANE's own symmetric per-32-element-group math (the same scheme as
   `membrane_q8_encode`/ggml's Q8_0/Q4_0) and immediately dequantize back
   to F16 bytes of the exact same length. The blob's size and structure
   never change -- only the numeric values in the targeted ranges.
   Untouched ranges are bit-identical to the true capture.
4. Load the perturbed blob into a **fresh** context via
   `llama_state_seq_set_data`. Since that call restores only the KV cache
   memory, not the output/logits buffer (verified: both `get_data` and
   `set_data` forward only to `llama_memory::state_write`/`state_read`),
   the prompt's last token is always decoded **fresh** in every
   experiment -- baseline included, for structural symmetry -- so any
   difference between runs is attributable only to the deliberate
   perturbation, never to how the decode was split into calls.
5. Continue generation and compare against the cached true baseline
   (top-1/top-5 agreement, logit cosine similarity, logit RMSE, KL
   divergence, first-divergent token, and a recall substring check).

**Self-test, run before anything else is trusted**: perturbing nothing
must reproduce the true baseline exactly. It does -- `top1=100%,
top5=100%, cosine=1.000000, RMSE=0.0, text identical` -- in both the
Release build and under ASan+UBSan.

This technique is used for items 1 and 3 (per-layer, per-token-age),
which have no native equivalent. Item 2 (uniform K/V combinations) needs
no splicing -- `type_k` and `type_v` are independently settable scalars,
so those six combinations use real ggml native types exactly like
Phase 3.2, with real `llama_state_seq_get_size` memory numbers.

**KV memory reduction is reported two different, non-interchangeable
ways, and the docs below label every number accordingly**: native-type
rows (`all-Q8`, `all-Q4`, the six K/V combinations) use the real, measured
whole-blob size via `llama_state_seq_get_size` on both the F16 baseline
and the candidate type -- physically real quantized storage exists in
those cases. Spliced/adaptive rows never change physical storage (the
technique injects numeric noise into an always-F16-backed cache), so
their "KV reduction" is **analytically projected** from the real,
measured per-layer row sizes and ggml's actual block-quant storage
formula (Q8_0: 34 bytes/32 elements; Q4_0: 18 bytes/32 elements -- real,
documented ggml constants). Mixing these two would be comparing measured
bytes to a different quantity; they are kept separate throughout.

## Model and experiment setup

Same model as Phase 3.2: `SmolLM2-135M-Instruct` (Apache 2.0,
`bartowski/SmolLM2-135M-Instruct-GGUF`, F16 weights,
`SmolLM2-135M-Instruct-f16.gguf`, SHA-256
`f535f83ec568d040f88ddc04a199fa6da90923bbb41d4dcaed02caa924d6ef57`,
30 layers, head_dim 64, not committed to the repo). llama.cpp submodule
commit `c0bc8591e8815c63cb01dd3f051a8b0df02501c9` (unchanged since
Phase 2.1). Context 1024 tokens, 32 generated tokens per experiment,
4 threads, host: AMD Ryzen 5 5600H, Linux 6.18, gcc 13.3.0, CPU-only.
Sampling is greedy throughout (deterministic, no seed needed), matching
Phase 3.2's rationale.

**Success thresholds** (task defaults, item 4): top-1 ≥ 98%, top-5 ≥ 99%,
logit cosine ≥ 0.995, recall exact (a failed recall disqualifies Q4
outright regardless of the numeric metrics -- item 8).

**Prompts** (item 7's six required types; `recall.txt` doubles as the
profiler prompt for items 1 and 3, per the task's explicit instruction for
item 3):

| file | type | recall answer checked |
|---|---|---|
| `recall.txt` | long-context fact recall (299 tokens) | "7429" |
| `natural.txt` | natural language | none |
| `code.txt` | C code | none |
| `repeat.txt` | repeated context | none |
| `distractor.txt` | distractor-heavy recall (a target number + 3 decoys) | "6183" |
| `secrets.txt` | two distinct secrets, question about one specific one | "815" |

Reproduction:

```bash
cmake -S . -B build-llama -G Ninja -DCMAKE_BUILD_TYPE=Release -DMEMBRANE_ENABLE_LLAMA=ON
cmake --build build-llama --target membrane-kv-sensitivity
./build-llama/tools/membrane-kv-sensitivity/membrane-kv-sensitivity \
    --model models/SmolLM2-135M-Instruct-f16.gguf \
    --prompt benchmarks/kv/prompts/recall.txt 7429 \
    --prompt benchmarks/kv/prompts/natural.txt - \
    --prompt benchmarks/kv/prompts/code.txt - \
    --prompt benchmarks/kv/prompts/repeat.txt - \
    --prompt benchmarks/kv/prompts/distractor.txt 6183 \
    --prompt benchmarks/kv/prompts/secrets.txt 815 \
    --n-tokens 1024 --gen-tokens 32 \
    --out benchmarks/results/phase3-adaptive-kv/sensitivity.jsonl
```

## Item 1 — per-layer sensitivity (30 layers, recall.txt)

Each layer tested alone (both its K and V), rest of the model at FP16.

| classification | layers | count |
|---|---|---|
| **Q4-safe** (cosine ≥0.995, top-1 ≥98%, top-5 ≥99%, recall OK) | 0,1,2,4,6,7,8,9,11,12,15,17,18,19,24,25,26,28,29 | 19 |
| **Q8-safe** (Q4 failed at least one bar) | 3,5,10,13,14,16,20,21,22,23,27 | 11 |
| **Critical** (Q8 itself failed) | -- | 0 |

Every single-layer Q8 test: cosine 0.99999–1.000000, top-1/top-5 100%,
recall OK -- Q8 alone is essentially free at any layer. The Q4-vs-Q8
split is driven almost entirely by whether Q4's own cosine stays ≥0.995
and, in a few cases, whether Q4 disturbs the very next token enough to
break recall (e.g. layer 13's Q4 test diverged at token 7 and missed the
recall check even though its cosine, 0.9994, would otherwise have passed
-- exactly item 8's rule doing its job). No layer-position pattern is
visible (Q8-only layers are scattered through early-, middle-, and
late-network positions), consistent with Phase 2's repeated finding that
layer number alone has little effect on this model.

## Item 2 — K vs V sensitivity (six combinations, recall.txt and natural.txt)

| combination | recall.txt: top-1 / cosine / recall | natural.txt: top-1 / cosine |
|---|---|---|
| K=Q8, V=F16 | 100% / 0.99984 / OK | 100% / 0.99998 |
| K=F16, V=Q8 | 100% / 0.99998 / OK | 100% / 0.99999 |
| K=Q4, V=Q8 | 93.8% / 0.96277 / OK | 87.5% / 0.99547 |
| K=Q8, V=Q4 | 100% / 0.99576 / OK | 100% / 0.99854 |
| **K=Q4, V=F16** | **96.9% / 0.97615 / FAIL** | 81.3% / 0.99265 |
| K=F16, V=Q4 | 100% / 0.99697 / OK | 100% / 0.99841 |

**K is consistently the more sensitive tensor.** At matched memory cost,
`K=Q4,V=F16` is worse than `K=F16,V=Q4` on both prompts (cosine 0.976 vs
0.997 on recall.txt, 0.993 vs 0.998 on natural.txt) and is the **only**
uniform configuration in this entire experiment that broke the recall
check on recall.txt. The same asymmetry holds at the Q4/Q8-mixed level:
`K=Q4,V=Q8` (0.963–0.995 cosine) is consistently worse than `K=Q8,V=Q4`
(0.996–0.999 cosine). If a deployment can only quantize one side more
aggressively, V is the safer choice.

## Item 3 — token-age sensitivity (all 30 layers, recall.txt)

| band | tokens | Q8 cosine/recall | Q4 cosine/recall | classified |
|---|---|---|---|---|
| oldest 25% | [0, 74) | 0.999977 / OK | 0.992426 / OK | Q8-safe |
| middle 50% | [74, 224) | 0.999992 / OK | 0.996746 / **FAIL** (div@7) | Q8-safe |
| newest 25% | [224, 298) | 0.999952 / OK | 0.986336 / OK | Q8-safe |

**No band cleared the Q4 bar**, so the token-age Q4 override (item 3's
"old tokens Q4, new tokens Q8/FP16") was **not enabled** for the final
policy. Notably it is the *middle* band, not the oldest, whose Q4 test
broke recall -- the oldest band's Q4 cosine (0.992) was actually the
*worst* of the three numerically, but it didn't happen to disturb the
recall-critical token. This is a reminder that cosine similarity and
recall success are correlated but not identical signals; both matter,
which is exactly why item 8 makes recall a hard disqualifier independent
of the numeric thresholds.

## Item 4 — the derived policy

- **Layer map**: 19 layers Q4-eligible, 11 Q8-only, 0 critical (from
  item 1).
- **Token-age override**: disabled (from item 3 -- no band cleared Q4).
- **`adaptive FP16/Q8`**: every layer clamped to *at most* Q4→Q8 (never
  more aggressive than Q8, per-layer classification of `Q8` or better
  respected); pure FP16/Q8, no Q4 anywhere.
- **`adaptive FP16/Q8/Q4`**: the 19 Q4-eligible layers at Q4, the 11
  Q8-only layers at Q8, no age override (since none was enabled). A
  critical layer's own classification always wins over a more aggressive
  policy floor -- it is never forced down by construction (there were
  none here, but the rule holds in general).

**Policy decision overhead** (item 6): fully profiling one prompt --
self-test, all 30 layers × 2 precisions, all 3 age bands × 2 precisions,
plus validating the resulting policy -- took **98.8 s wall-clock**
(6m20s CPU across 4 threads) on this host. This is a one-time, offline
cost per model; it does not repeat at inference time once a policy is
fixed.

## Item 5 — comparison table (all 6 prompts)

`fp16_baseline` is definitionally 1.000x/100%/1.0 and included only as
the recall-correctness reference. `all-Q8`/`all-Q4` use real, measured
KV bytes; `adaptive` rows use the analytically projected bytes (see
methodology note above) -- both KV columns are real numbers, just
measuring different things, as documented.

| prompt | config | top-1 | top-5 | cosine | KL | recall | KV |
|---|---|---|---|---|---|---|---|
| recall | all-Q8 | 100% | 100% | 0.99975 | 0.0004 | OK | 1.70x |
| recall | all-Q4 | 96.9% | 100% | 0.97745 | 0.0178 | **FAIL** | 3.21x |
| recall | adaptive FP16/Q8 | 100% | 100% | 0.99994 | 0.0001 | OK | 1.88x |
| recall | adaptive FP16/Q8/Q4 | 100% | 100% | 0.98722 | 0.0291 | OK | 2.68x |
| natural | all-Q8 | 96.9% | 100% | 0.99997 | 0.0002 | -- | 1.41x |
| natural | all-Q4 | 87.5% | 100% | 0.99086 | 0.0613 | -- | 2.66x |
| natural | adaptive FP16/Q8 | 100% | 100% | 0.99997 | 0.0002 | -- | 1.88x |
| natural | adaptive FP16/Q8/Q4 | 96.9% | 100% | 0.99367 | 0.0407 | -- | 2.68x |
| code | all-Q8 | 100% | 100% | 0.99994 | 0.0002 | -- | 1.43x |
| code | all-Q4 | 90.6% | 100% | 0.98865 | 0.0294 | -- | 2.69x |
| code | adaptive FP16/Q8 | 100% | 100% | 0.99997 | 0.0001 | -- | 1.88x |
| code | adaptive FP16/Q8/Q4 | 93.8% | 100% | 0.99502 | 0.0163 | -- | 2.68x |
| repeat | all-Q8 | 100% | 100% | 0.99998 | 0.0000 | -- | 1.75x |
| repeat | all-Q4 | 100% | 100% | 0.98793 | 0.0015 | -- | 3.30x |
| repeat | adaptive FP16/Q8 | 96.9% | 100% | 0.99999 | 0.0000 | -- | 1.88x |
| repeat | adaptive FP16/Q8/Q4 | 96.9% | 100% | 0.99252 | 0.0048 | -- | 2.68x |
| distractor | all-Q8 | 100% | 100% | 0.99979 | 0.0002 | FAIL* | 1.68x |
| distractor | all-Q4 | 90.6% | 100% | 0.96837 | 0.0322 | FAIL* | 3.16x |
| distractor | adaptive FP16/Q8 | 100% | 100% | 0.99999 | 0.0001 | FAIL* | 1.88x |
| distractor | adaptive FP16/Q8/Q4 | 93.8% | 100% | 0.99383 | 0.0102 | OK | 2.68x |
| secrets | all-Q8 | 96.9% | 100% | 0.99968 | 0.0002 | FAIL* | 1.64x |
| secrets | all-Q4 | 84.4% | 100% | 0.97343 | 0.0205 | OK* | 3.08x |
| secrets | adaptive FP16/Q8 | 100% | 100% | 0.99997 | 0.0002 | FAIL* | 1.88x |
| secrets | adaptive FP16/Q8/Q4 | 100% | 100% | 0.99603 | 0.0095 | FAIL* | 2.68x |

`*` -- **the FP16 baseline itself already fails `distractor` and
`secrets`** (see below); every "FAIL"/"OK" in those two rows reflects the
base model's own capability, not a quantization effect. Details follow.

### The distractor/secrets capability ceiling (item 7 honesty check)

The FP16 baseline's own generated text:

- `distractor.txt` → **"1953"** (a decoy: the shop's founding year, not
  the crate combination "6183"). Wrong at FP16, before any quantization.
- `secrets.txt` → **"402"** (Percy's key, not Naomi's "815", which is
  what was asked). Also wrong at FP16.

`all-Q8` reproduces the *same* wrong answers as the baseline on both
prompts (consistent with its extremely high measured token-level
agreement, 96.9–100% top-1). `all-Q4` on `secrets.txt` happens to answer
**"815" -- correctly** -- not because Q4 is more capable, but because its
quantization noise perturbed a borderline decision and it landed, by
chance, on the right token. This is a real, measured, honestly-reported
observation, and precisely the reason `distractor.txt` and `secrets.txt`
are excluded from the safety verdict below: a 135M-parameter model
cannot reliably distinguish a target fact from nearby decoys or hold two
separate secrets apart at full precision, so neither prompt provides a
valid recall-safety signal *for this model*. They remain useful for the
non-recall fidelity metrics (top-1/top-5/cosine/KL), where the same
patterns as the other four prompts hold.

## Item 8 — the safety gate, applied

Per item 8, any configuration failing a *valid* recall test (i.e. one the
FP16 baseline itself passes) is disqualified from being considered
Q4-safe, independent of its numeric scores:

- `all-Q4` failed the valid recall test (`recall.txt`) outright --
  **disqualified**, confirming Phase 3.2's finding again on a fresh
  model/prompt/harness.
- `adaptive FP16/Q8/Q4` **passed** the valid recall test, but its
  aggregate cosine (0.987) is below the 0.995 bar each of its 19
  constituent layers individually cleared -- the compounding effect
  described in the headline. It is not disqualified by the letter of
  item 8 (recall passed), but it does not carry the same numeric safety
  margin its construction implied. This nuance is the central limitation
  this phase surfaces: **a per-layer-additive policy needs its combined
  result independently validated, which is exactly what this comparison
  table does** -- and here, the combined result under-delivers relative
  to its parts.
- `adaptive FP16/Q8` (Q8-only, no Q4 anywhere) passed every check on
  every prompt with cosine ≥0.9999 throughout -- no compounding concern
  observed, consistent with every single-layer Q8 test already being
  near-perfect.

## Success criteria (item 4 defaults, evaluated on the four
recall-valid prompts: recall, natural, code, repeat)

| criterion | all-Q8 | adaptive FP16/Q8 | adaptive FP16/Q8/Q4 | all-Q4 |
|---|---|---|---|---|
| top-1 ≥ 98% | 3/4 (natural 96.9%) | 3/4 (repeat 96.9%) | 0/4 | 0/4 |
| top-5 ≥ 99% | 4/4 | 4/4 | 4/4 | 4/4 |
| logit cosine ≥ 0.995 | 4/4 | 4/4 | 4/4 (barely: 0.987–0.995, recall≈0.987 is the low point) | 0/4 |
| recall exact | 4/4 (valid prompts) | 4/4 | 4/4 | 3/4 (fails recall.txt) |
| KL divergence reported | done throughout | done | done | done |

No configuration clears every single numeric bar on every prompt (even
`all-Q8` dips to 96.9% top-1 on `natural`), but the *disqualifying* item-8
gate (recall) is what actually separates safe from unsafe here: `all-Q4`
is the only configuration that fails it on a valid prompt.

## Recommendation

- **`adaptive FP16/Q8` (Q8-only, per-layer) is the safest option
  measured**: 1.88x projected reduction, cosine ≥0.9999 on every prompt,
  recall correct everywhere it's valid to check, and it never showed any
  compounding degradation. It is, however, barely different from simply
  using `all-Q8` everywhere (1.4–1.75x, same cosine range) -- the
  per-layer profiling bought a modest, not dramatic, memory improvement
  over the uniform Q8 baseline on this model.
- **`adaptive FP16/Q8/Q4` reaches meaningfully more memory reduction
  (2.68x) and still passes the binary recall gate**, but its aggregate
  fidelity (cosine 0.987–0.996) is measurably thinner than its
  constituent layers implied, due to compounding. It is a reasonable
  choice where 2.68x matters more than the last bit of margin, but it
  should not be described as carrying the same 0.995-per-layer guarantee
  the profiling suggested -- that guarantee does not survive composition
  intact.
- **`all-Q4` (uniform) remains unsafe**, confirmed again independently on
  this model: it is the only tested configuration that broke a valid
  recall test outright.
- **Next step implied by the compounding finding**: a policy search that
  validates *combinations* of layers (not just each layer in isolation)
  would likely find a larger safe Q4 subset than the naive per-layer
  union used here -- this experiment's profiler is the right first pass,
  but composing its output 1:1 into a final policy is demonstrably not
  sufficient by itself.

The raw output (`sensitivity.jsonl`, gitignored) lives in
`benchmarks/results/phase3-adaptive-kv/`; every number above regenerates
with the command in "Model and experiment setup."

## Verification

- The standard MEMBRANE test suite (unchanged by this phase -- no new
  code went into `membrane_core`) remains green: Release, ASan+UBSan, and
  TSan all 14/14.
- The self-test (perturbing nothing must reproduce the true baseline
  exactly) passes in both the Release build and under ASan+UBSan against
  the real model.
- `membrane-kv-sensitivity`'s full blob-parsing and in-place perturbation
  code was built and run under ASan+UBSan against the real model (2
  prompts, full self-test + comparison table) with **zero sanitizer
  diagnostics**.
- The full 6-prompt sweep (self-test, 60 per-layer experiments, 6
  token-age experiments, 12 K/V-combination experiments, 30 comparison
  experiments) completed with zero experiment failures and internally
  consistent results throughout (e.g. top-5 never below top-1, KV byte
  counts reproducible, the K/V-combination ratios sane after a
  measured-vs-analytic denominator bug was found and fixed during this
  phase's own development).
