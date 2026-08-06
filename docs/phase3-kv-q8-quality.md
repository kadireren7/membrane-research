# Phase 3.2 — Live Q8 KV-Cache Model-Quality Validation

Phase 3.1 measured Q8 quantization fidelity offline, against real captured
`stories15M` KV-cache tensors, and found excellent numerical accuracy but
could not run a live model-quality comparison: `stories15M`'s attention
head dimension (48) is not divisible by ggml's fixed KV-cache
quantization block size (32), so llama.cpp could not even construct a
quantized KV cache for it. This phase finds a compatible model and
completes that validation. Every number below comes from the commands
shown, run against real inference, not simulated or extrapolated.

## Headline

**Q8 KV-cache quantization passes every success criterion on every one of
5 prompt types, over 10 runs each: 46.8% KV memory reduction (exceeds the
40% bar), top-1 agreement 96.9%–100% (mean 99.4%, exceeds the 95% bar),
top-5 agreement 100% on all 5, logit cosine similarity 0.9997–0.9999
(exceeds the 0.99 bar), generated text identical or near-identical to the
FP16 baseline including on the hardest (long-context recall) prompt, and a
negligible, sometimes even net-positive, speed effect (0.99x–1.04x).** A
bonus second live data point, 4-bit `Q4_0`, trades more memory (71.8%
reduction) for measurably worse fidelity, including one concrete
information-recall failure the Q8 run did not exhibit — direct evidence of
what "the bar was met" is actually protecting against.

## Scope note carried over from the tool itself (read before the results)

Phase 3.1's own sweep tested `MEMBRANE_CODEC_F16_Q8_BLOCK` at symmetric
group sizes 32/64/128/256 and affine group 128. Building this phase's live
comparison surfaced an architectural fact: **ggml's quantized KV-cache
types (`Q8_0`, `Q4_0`, `Q5_0`, `Q5_1`, `Q4_1`, `IQ4_NL`) all use a block
size fixed at 32, wired into the format itself** -- llama.cpp exposes no
runtime "group size" or "affine vs symmetric" choice for the KV cache.
MEMBRANE's own codec is not wired into any inference runtime yet (that
remains future integration work). So **only one live config exists to
test this way: `GGML_TYPE_Q8_0` (block-32, symmetric) -- the live analogue
of MEMBRANE's `symmetric, group_elems=32`.** The `symmetric/64`,
`symmetric/128`, `symmetric/256`, and `affine/128` configs from Phase 3.1
have no live equivalent and are **not** re-validated here; their numbers
remain the Phase 3.1 offline measurements (cosine similarity ≥0.9999,
ratios 1.78x–1.97x, all measured against real captured KV tensors, just
never run through an actual decode loop). This phase additionally runs
`GGML_TYPE_Q4_0` live, purely as a bonus second data point the same
methodology produces for free.

## Model selection and provenance

| | |
|---|---|
| Model | `SmolLM2-135M-Instruct` (HuggingFaceTB), Apache 2.0 license |
| GGUF build | `bartowski/SmolLM2-135M-Instruct-GGUF`, file `SmolLM2-135M-Instruct-f16.gguf` (F16 weights, no weight quantization, so the *only* variable between baseline and quantized runs is the KV-cache type) |
| Source URL | `https://huggingface.co/bartowski/SmolLM2-135M-Instruct-GGUF/resolve/main/SmolLM2-135M-Instruct-f16.gguf` |
| SHA-256 | `f535f83ec568d040f88ddc04a199fa6da90923bbb41d4dcaed02caa924d6ef57` |
| File size | 270,885,952 bytes (~258 MiB) |
| Architecture | Llama-family, `hidden_size=576`, `num_attention_heads=9` → **head_dim = 576/9 = 64**, divisible by 32 (compatible); `num_hidden_layers=30`, `num_key_value_heads=3` (GQA), vocab 49,152 |
| Not committed to the repo | correct -- `models/` and `*.gguf` are gitignored (verified: `git check-ignore models/SmolLM2-135M-Instruct-f16.gguf`) |

Chosen because: small (fits comfortably in CI-scale hardware and runs
fast), permissively licensed with clear provenance, and -- the specific,
verified requirement here -- its head dimension is a multiple of 32, unlike
`stories15M`'s 48.

## Tool

`tools/membrane-kv-quality/main.cpp`, extended for this phase to sweep
multiple prompts, multiple live KV types, and multiple repeated runs in a
single process (one model load, many context creations). Per
`(prompt, type, run)`:

1. **Baseline (F16 KV)**: greedy-decode `gen_tokens` steps, recording every
   chosen token, its full logit vector, and prefill (TTFT) time.
2. **Quantized, free-running**: greedy-decode independently from the same
   prompt -- does the *generated text* itself diverge over time? The first
   token position where its own choice differs from the baseline's is
   recorded (`first_divergence_pos`; equals `gen_tokens` if it never
   diverged).
3. **Quantized, teacher-forced** on pass 1's exact token sequence: its
   logits are then directly comparable to pass 1's at matched positions,
   isolating the KV-quantization effect from the confound of the two
   passes having walked different token sequences.

**Sampling**: greedy (argmax) throughout, by deliberate choice -- it is
fully deterministic given a token sequence, so there is no seed to fix and
no sampling-randomness confound between the baseline and quantized runs;
any remaining run-to-run variance is attributable only to the inference
engine itself (e.g. multi-threaded floating-point reduction order), which
is exactly what repeating each config 10 times is designed to catch.
**Measured finding: that variance was zero.** Every accuracy metric
(top-1/top-5 agreement, logit cosine, RMSE, KL divergence,
first-divergence position, KV-cache byte count) had `stddev = 0.000000`
across all 10 runs, for every prompt and every type -- llama.cpp's 4-thread
CPU inference was bit-reproducible run-to-run on this model and hardware.
Only wall-clock timing metrics (TTFT, tokens/s) showed real variance, from
ordinary system scheduling noise, not from the computation itself.

KV-cache memory is measured via `llama_state_seq_get_size(ctx, 0)` on the
live context after decoding -- an actual API call against the actual
running KV cache, not a formula.

## Experiment setup

| | |
|---|---|
| llama.cpp submodule commit | `c0bc8591e8815c63cb01dd3f051a8b0df02501c9` (unchanged since Phase 2.1) |
| Context size (`--n-tokens`) | 1024 (comfortably fits the longest prompt, `recall`, at 299 tokens) |
| Generated tokens per run (`--gen-tokens`) | 32 |
| Runs per (prompt, type) | 10 |
| Threads | 4 (`n_threads` / `n_threads_batch`) |
| Live KV types tested | `q8_0` (block-32 symmetric int8), `q4_0` (block-32 symmetric 4-bit, bonus) |
| Host | AMD Ryzen 5 5600H, Linux 6.18, gcc 13.3.0, CPU-only |

Prompts (`benchmarks/kv/prompts/`, plain-text completion, no chat template
applied -- consistent with how every prior KV-cache experiment in this
project has fed prompts):

| name | type | tokens |
|---|---|---|
| `natural.txt` | natural language | 105 |
| `code.txt` | C code | 68 |
| `repeat.txt` | long repeated sentence | 366 |
| `recall.txt` | long-context information recall (a name and a 4-digit code embedded early in a multi-paragraph story, asked for at the end) | 299 |
| `short.txt` | short generation | 13 |

Reproduction:

```bash
cmake -S . -B build-llama -G Ninja -DCMAKE_BUILD_TYPE=Release -DMEMBRANE_ENABLE_LLAMA=ON
cmake --build build-llama --target membrane-kv-quality
./build-llama/tools/membrane-kv-quality/membrane-kv-quality \
    --model models/SmolLM2-135M-Instruct-f16.gguf \
    --prompt-file benchmarks/kv/prompts/natural.txt \
    --prompt-file benchmarks/kv/prompts/code.txt \
    --prompt-file benchmarks/kv/prompts/repeat.txt \
    --prompt-file benchmarks/kv/prompts/recall.txt \
    --prompt-file benchmarks/kv/prompts/short.txt \
    --n-tokens 1024 --gen-tokens 32 --runs 10 \
    --out benchmarks/results/phase3-kv-quality/quality.jsonl
```

## Results: Q8_0 (the live analogue of symmetric, group=32)

Every value below is the mean over 10 runs; `stddev` for every one of
these was `0.000000` except timing (see above), so mean = min = max for
all accuracy metrics.

| prompt | top-1 | top-5 | logit cosine | logit RMSE | KL div | first divergence | KV reduction | speed (q/base) |
|---|---|---|---|---|---|---|---|---|
| natural | 96.9% | 100% | 0.99997 | 0.0592 | 0.0002 | token 31/32 | 1.881x | 1.009x |
| code | 100% | 100% | 0.99994 | 0.0596 | 0.0002 | never (32) | 1.881x | 0.993x |
| repeat | 100% | 100% | 0.99998 | 0.0416 | 0.0000 | never (32) | 1.881x | 1.030x |
| recall | 100% | 100% | 0.99974 | 0.1169 | 0.0004 | never (32) | 1.881x | 1.043x |
| short | 100% | 100% | 1.00000 | 0.0572 | 0.0001 | never (32) | 1.880x | 0.998x |
| **mean** | **99.4%** | **100%** | **0.99992** | **0.0669** | **0.0002** | | **1.881x** | **1.015x** |

**The `recall` prompt -- the hardest test, requiring the model to retrieve
a specific fact from 299 tokens of context -- produced byte-identical
generated text between the F16 baseline and the Q8_0 run**: both answered
"The code to the supply locker was 7429." Q8_0 never diverged from the
baseline's own token choices within the 32 generated tokens on 4 of 5
prompts; the one exception (`natural`) diverged at token 31 of 32 (i.e.
held identical for 30 tokens first).

## Results: Q4_0 (bonus, not requested but produced by the same run)

| prompt | top-1 | top-5 | logit cosine | logit RMSE | KL div | first divergence | KV reduction | speed (q/base) |
|---|---|---|---|---|---|---|---|---|
| natural | 87.5% | 100% | 0.99083 | 0.9264 | 0.0617 | token 12/32 | 3.549x | 0.980x |
| code | 90.6% | 100% | 0.98859 | 0.8043 | 0.0293 | token 7/32 | 3.549x | 0.993x |
| repeat | 100% | 100% | 0.98795 | 1.3068 | 0.0015 | never (32) | 3.550x | 1.026x |
| recall | 96.9% | 100% | 0.97749 | 1.0975 | 0.0178 | token 5/32 | 3.550x | 1.039x |
| short | 96.9% | 100% | 0.99364 | 0.6732 | 0.0307 | token 29/32 | 3.544x | 0.995x |
| **mean** | **94.4%** | **100%** | **0.98770** | **0.9616** | **0.0282** | | **3.548x** | **1.007x** |

**Q4_0 shows a concrete, measured quality failure Q8_0 does not**: on the
`recall` prompt, the baseline and Q8_0 both correctly answered "7429";
Q4_0's free-running generation diverged at token 5 and never recovered the
fact, instead trailing off into an unrelated paraphrase of the story
("Tomas had been searching for a written record of the number, worried
they would run out of food..." -- true to the story, but not the answer
to the question asked). This is exactly the kind of "ciddi metin kalite
bozulması" (serious text-quality degradation) the success criteria asked
to rule out for Q8 -- and it is real, but it belongs to Q4, not Q8.

## Success criteria (item 7 of the task)

| criterion | Q8_0 result |
|---|---|
| KV memory reduction ≥ 40% | **met** -- 46.8%–46.8% (1.880x–1.881x) on every prompt |
| top-1 agreement ≥ 95% (best-effort) | **met on all 5 prompts** -- 96.9%–100%, mean 99.4% |
| top-5 agreement ≥ 99% | **met** -- 100% on every prompt |
| logit cosine ≥ 0.99 | **met by a wide margin** -- 0.99974–1.00000 |
| no serious text-quality degradation | **met** -- identical or near-identical generated text on all 5 prompts, including the recall test |
| speed penalty explicitly measured | **done** -- 0.993x–1.043x (essentially free; sometimes net-positive, likely because smaller KV reads during generation offset dequantization compute at this scale) |

Every criterion is met, on every one of the 5 prompt types, across 10 runs
each -- not a single favorable prompt or a single lucky run.

## Recommendation (item 6 of the task)

The task asked to compare `symmetric/64`, `symmetric/128`, `symmetric/256`,
and `affine/128` and pick the best quality/memory balance. As established
above, **none of those four are live-testable** with the tooling
available (no runtime group-size or affine knob exists in ggml's KV
cache). What this phase *can* recommend, from actual live evidence: **Q8_0
(≈ symmetric, group=32) is validated safe for live use on this model and
prompt suite** -- it clears every bar with room to spare and costs
essentially nothing in speed. Q4_0 is available for roughly 1.9x more
memory savings on top of Q8 (71.8% vs 46.8% reduction), but at a
measured, real quality cost (mean top-1 drops from 99.4% to 94.4%, and one
concrete recall failure was observed) -- **not recommended without
further per-use-case evaluation**, and outside what this phase's success
criteria were written to certify (they targeted Q8 specifically).
Phase 3.1's offline numbers suggest `symmetric/64`–`symmetric/256` and
`affine/128` would likely do at least as well as `symmetric/32` on
fidelity (their offline cosine similarity was equal or higher, and their
ratios higher) -- but this is an inference from offline data, not a live
measurement, and is stated here only as a hypothesis for a future
integration phase to test once MEMBRANE's own codec (or a compatible
runtime hook) can serve a live KV cache.

The raw output (`quality.jsonl`, gitignored) lives in
`benchmarks/results/phase3-kv-quality/`; every number above regenerates
with the command in "Experiment setup".

## Verification

- The standard MEMBRANE test suite (unchanged by this phase -- no new code
  went into `membrane_core`) remains green: Release, ASan+UBSan, and TSan
  all 14/14.
- `membrane-kv-quality`'s new sweep/statistics code was built and run
  under ASan+UBSan against the real model (`SmolLM2-135M-Instruct-f16.gguf`,
  2 prompts × 2 live runs) with **zero sanitizer diagnostics**.
- The full 5-prompt × 2-type × 10-run sweep completed with **zero decode
  failures** and internally consistent results throughout (e.g. `top-5`
  never below `top-1`, KV-byte counts exactly reproducible run to run).
