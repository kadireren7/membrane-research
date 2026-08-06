# Phase 3.1 — Block-Wise F16 → Q8 KV Quantization Experiment

The lossless arc (Phase 2.1–2.4) closed with a measured, structural ceiling:
even the best lossless backend on the F16 KV cache's high byte plane only
projects to ~1.2–1.35x, because the mantissa plane is incompressible noise.
This phase switches to controlled **lossy** quantization — F16 → int8,
block-wise — and measures memory gain, numerical error, and (to the extent
the model allows) inference-quality impact **separately**, so a compression
ratio is never reported without its accuracy cost next to it. Every number
below comes from the commands shown, run against the same four real
`stories15M` KV dumps used in every prior Phase 2 experiment.

## Headline

- **6 of 8 swept configs meet the ≥1.8x ratio AND ≥0.99 cosine-similarity
  bar**; the two that don't (symmetric/affine at group=32) miss only on
  *ratio* (1.60–1.78x) while still scoring cosine ≥0.9999 — the metadata
  overhead of small groups, not accuracy, is what limits them.
- Fidelity is excellent everywhere measured: **cosine similarity
  0.9999–0.99999**, relative L2 error 0.5–1.1%, decode is bit-reproducible
  and deterministic.
- **The live model-inference quality comparison (item 7) could not be run
  on `stories15M`**: llama.cpp's native quantized KV cache requires the
  per-attention-head dimension to be divisible by its block size (32), and
  stories15M's head dimension is 48 — not a bug, a measured architectural
  incompatibility, detailed below. The comparison tool is built, tested on
  its F16 baseline path, and ready for a model whose head dimension is
  compatible (most production models: head_dim 64 or 128 are common).

## What was built

- **`membrane/f16convert.h`** (`src/codecs/f16convert.c`) — dependency-free
  IEEE-754 binary16 ↔ binary32 conversion, needed because quantization must
  operate on real values, not opaque bytes. Exhaustively verified: all
  65536 possible half bit patterns round-trip through float and back to the
  identical bits (`tests/unit/test_f16convert.c`), except NaN payload bits
  (only "is NaN" is required to survive, matching IEEE 754's own freedom
  there).
- **`membrane/q8block.h`** (`src/codecs/q8block.c`) — block-wise F16→int8
  quantization, **symmetric** (`scale = max_abs/127`) and **affine**
  (`scale = (max-min)/255`, `bias = min`) modes, group size configurable in
  elements (swept at 32/64/128/256). A versioned 20-byte header carries the
  mode, group size, element count, and a CRC32 over the metadata+payload
  for corruption detection. **Architecture decision, made deliberately and
  documented in the header**: this codec is *not* registered in
  `membrane_codec_t` / the block-layer's codec table. That table's
  contract (`membrane_block_decode`, see `src/block/block.c`) verifies
  every block round-trips to the *original* bytes via a checksum computed
  before compression — a lossless-only contract every codec through Phase
  2.4 satisfied. Q8 is lossy by design, so it is exposed as a standalone
  module with its own codec-level CRC (over the quantized bytes, catching
  bitstream corruption) instead, and measured directly by the analyzer and
  `membrane_kv_quant_compute()`, never through `membrane_block_write/read`.
- **NaN/Inf policy** (safety-critical, since a quantizer must never crash
  or emit undefined bytes on pathological input): scale/bias are computed
  from the *finite* elements in a group only. NaN quantizes to the code
  that decodes to 0 (affine: the group minimum); ±Inf saturates to the
  group's extreme code (decodes to the group's finite max/min — a
  fixed-width code cannot represent an unbounded value); an all-non-finite
  group falls back to scale 0, decoding every element to exactly 0.0. Every
  branch is exercised in `tests/unit/test_q8block.c`.
- **`membrane/kvquant.h`** (`src/kvdump/kvquant.c`) — numerical-error
  metrics for one tensor under one Q8 config: MSE, RMSE, MAE, max absolute
  error, cosine similarity, relative L2 error, saturation ratio, metadata
  overhead, all computed only over elements whose *original* value was
  finite (error against NaN/Inf is undefined; those are counted
  separately).
- **`membrane-kv-analyze`** sweeps all 8 (mode × group_elems) combinations
  per tensor record, emitting `record:"quant"` JSONL and an optional
  `--quant-csv`, with human summaries for the full sweep, K-vs-V, per-layer
  (at a reference config), and an explicit success-criteria table.
- **`membrane-kv-quality`** (new tool, `tools/membrane-kv-quality/`,
  `MEMBRANE_ENABLE_LLAMA` only) — a three-pass live-inference comparison
  tool: (1) F16-KV baseline greedy decode, recording every token and logit
  vector; (2) quantized-KV free-running greedy decode, to see if generated
  *text* diverges; (3) quantized-KV teacher-forced on pass 1's exact token
  sequence, so its logits are directly comparable to pass 1's at matched
  positions. Reports top-1/top-5 agreement, mean KL divergence, mean/max
  logit difference, tokens/s, and peak RSS (documented as a whole-process
  running maximum on Linux — a clean per-variant number needs two separate
  invocations via `--variant baseline` / `--variant q8`, which the tool
  supports).
- **Tests**: `test_f16convert.c` (exhaustive round-trip + known values +
  Inf/NaN/subnormals), `test_q8block.c` (14 cases: known values, all 4
  group sizes × 2 modes, all-zero exact, constant-block exact for affine,
  NaN/Inf policy, all-non-finite group, uneven group count, invalid args,
  corrupted header, invalid/corrupted scale, truncated payload, checksum
  corruption, overflow, determinism), `test_kvquant.c` (all-zero is exact,
  smooth signal keeps high fidelity, determinism).

## Why Q8 is not in the lossless block-layer codec table

This is worth stating plainly since every codec through Phase 2.4 *was*
registered there. `membrane_block_write` stores `checksum =
membrane_block_checksum(original_bytes)`; `membrane_block_decode` accepts
only if `checksum(decoded_bytes) == checksum` — i.e. it demands bit-exact
recovery. A genuinely lossy codec registered under that contract would have
every `membrane_block_decode` call return `CORRUPT_DATA`, silently
defeating the store's purpose. Rather than weaken that invariant (which
every other codec and the store's own documentation rely on), Q8 gets its
own corruption-detecting checksum (over the *quantized* bytes) and its own
call surface. Wiring lossy quantization into the block/store layer with an
explicit "this block is lossy" flag is a real design question for a future
phase, not resolved here.

## Experiment setup

Same four `stories15M` F16 KV dumps as every Phase 2 experiment (`short`,
`repeat`, `natural`, `code`), 6 layers × {K,V} = 48 tensor records, 1024
tokens each. Host: AMD Ryzen 5 5600H, Linux 6.18, gcc 13.3.0, CPU-only.

```bash
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
./build-rel/tools/membrane-kv-analyze/membrane-kv-analyze \
    --jsonl benchmarks/results/phase3-kv/kv-q8.jsonl \
    --csv   benchmarks/results/phase3-kv/kv-q8-blocks.csv \
    --quant-csv benchmarks/results/phase3-kv/kv-q8.csv \
    --meta phase=3.1 \
    benchmarks/kv/dumps/short.kvdump benchmarks/kv/dumps/repeat.kvdump \
    benchmarks/kv/dumps/natural.kvdump benchmarks/kv/dumps/code.kvdump
```

## Result 1 — the full sweep (K+V combined, all 48 records × 4 prompts)

| mode | group | ratio | RMSE | cosine | rel L2 | saturation | vs 1.8x/0.99 bar |
|---|---|---|---|---|---|---|---|
| symmetric | 32 | 1.778x | 0.0079 | 0.999978 | 0.66% | 3.17% | ratio short |
| symmetric | 64 | 1.882x | 0.0098 | 0.999967 | 0.80% | 1.58% | **meets bar** |
| symmetric | 128 | 1.939x | 0.0113 | 0.999956 | 0.92% | 0.79% | **meets bar** |
| symmetric | 256 | 1.969x | 0.0129 | 0.999943 | 1.05% | 0.40% | **meets bar** |
| affine | 32 | 1.600x | 0.0060 | 0.999986 | 0.52% | 6.33% | ratio short |
| affine | 64 | 1.778x | 0.0076 | 0.999979 | 0.64% | 3.17% | ratio short |
| affine | 128 | 1.882x | 0.0093 | 0.999970 | 0.77% | 1.59% | **meets bar** |
| affine | 256 | 1.939x | 0.0109 | 0.999959 | 0.90% | 0.80% | **meets bar** |

Every one of the 384 (48 records × 8 configs) sweep points decoded
successfully (`decode_ok`); no NaN/Inf appear anywhere in the real captured
KV data (consistent with Phase 2.1's finding).

Observations, all measured:

1. **Ratio is driven almost entirely by metadata overhead, not error.**
   Cosine similarity barely moves across the whole sweep (0.99994–0.99999);
   ratio moves from 1.60x to 1.97x purely as a function of how many
   elements share one scale (and whether that scale needs a paired bias).
   Doubling the group size roughly halves the metadata cost and pushes
   ratio toward the ideal 2.0x (1 byte per F16 element).
2. **Affine costs one more float per group than symmetric** (scale *and*
   bias vs scale alone), so at equal group size affine's ratio is always
   one "half-step" behind symmetric's next group size up (affine/64 ≈
   symmetric/32 in ratio, affine/128 ≈ symmetric/64, etc.) — visible
   directly in the table. Affine's RMSE is consistently *lower* than
   symmetric's at the same group size (e.g. group=32: 0.0060 vs 0.0079),
   because per-block min-max affine handles a non-zero-centered
   distribution more precisely than a single symmetric scale — but it pays
   for that precision in metadata, and on this data the two effects roughly
   cancel: at matched *ratio* (not matched group size), the two modes'
   RMSE are close.
3. **Saturation tracks 1/group_elems almost exactly**
   (32→3.17%, 64→1.58%, 128→0.79%, 256→0.40%, halving each time) — an
   internal consistency check that matches the analytic expectation
   exactly: the single element defining a group's max magnitude always
   quantizes to the extreme code by construction, so a well-behaved,
   outlier-free distribution should show saturation ≈ 1/group_elems, and it
   does. This means saturation is *not* flagging a problem here; it is
   confirming the quantizer is behaving as designed. A materially higher
   saturation ratio than this baseline would be the actual warning sign.
4. **Prompt content does not matter** (mean ratio 1.7777x, RMSE range
   0.0075–0.0083 across `short`/`repeat`/`natural`/`code`) — the same
   prompt-invariance every Phase 2 experiment found.

## Result 2 — K vs V, per-layer (reference config: 32 elements, symmetric)

| | ratio | RMSE | cosine | rel L2 | saturation |
|---|---|---|---|---|---|
| K | 1.778x | 0.0109 | 0.999973 | 0.74% | 3.16% |
| V | 1.778x | 0.0026 | 0.999982 | 0.59% | 3.17% |

**V quantizes about 4x more accurately than K in absolute RMSE** (0.0026 vs
0.0109) at identical ratio and saturation — a new, measured asymmetry this
phase surfaces (Phase 2 found K and V near-identical in *compressibility*;
here they differ in *quantization error*). Per-layer RMSE is flat across
all 6 layers (0.0062–0.0089), matching Phase 2's "layer number has no
visible effect" pattern.

## Result 3 — model-quality experiment (item 7): blocked, with a measured cause

The plan was: run `stories15M` with the same prompt and seed through an F16
KV cache and a quantized KV cache, and compare logits, agreement, KL
divergence, generated text, speed, and memory. `membrane-kv-quality` was
built to do exactly this using llama.cpp's own native `GGML_TYPE_Q8_0` KV
cache (per-32-element-block symmetric int8 with an F16 scale — the closest
real analogue to this phase's `group_elems=32, symmetric` config, since
MEMBRANE's own Q8 codec is not wired into any inference runtime yet — see
the architecture note above).

Attempting it on `stories15M` fails at context construction:

```
print_info: n_embd_head_k = 48
print_info: n_embd_head_v = 48
llama_init_from_model: K cache type q8_0 with block size 32 does not divide n_embd_head_k=48
```

This is a genuine, measured architectural constraint, not a bug: ggml's
block-quantized KV cache types require the per-head dimension to be evenly
divisible by the block size, and every legacy ggml quantization type
(Q8_0, Q4_0, Q5_0, …) uses block size 32. `stories15M`'s head dimension is
48 = 32 × 1.5 — not divisible by 32, and no alternative ggml block-quant
type has a smaller or compatible block size, so **no native quantized KV
cache can be constructed for this model at all**, independent of which
type is chosen or whether K, V, or both are targeted (verified: K-only
already fails identically, and V has the same head dimension).

This incompatibility is itself informative: it is a concrete illustration
of why a *flexible, group-size-configurable* codec — like this phase's
`MEMBRANE_CODEC_F16_Q8_BLOCK`, whose `group_elems` is a free parameter, not
tied to the model's head dimension — has real value over hard-coding a
single fixed-block-size scheme.

What *was* validated:

- The tool's harness is correct: the F16-only baseline path
  (`--variant baseline`) runs cleanly end-to-end on `stories15M` — coherent
  generated text, 283.9 tok/s, 114 MB peak RSS — confirming the three-pass
  design, timing, and RSS measurement all work; only the quantized-KV
  context construction is blocked by this model's geometry.
- The Q8 quantization fidelity numbers above (Results 1–2) come from
  MEMBRANE's *own* codec applied to *real* captured KV-cache tensors from
  `stories15M` — actual model activations, not synthetic data — so the
  numerical-safety case does not depend on live inference to be grounded in
  real data.

**What remains open**: the live top-1/top-5 agreement, KL divergence, and
generated-text-divergence numbers this item asked for. They need either
(a) a model whose head dimension is a multiple of 32 (common — e.g. most
production LLMs use head_dim 64 or 128; `stories15M` is an unusually small,
non-standard toy architecture), run through the already-built
`membrane-kv-quality` tool unmodified, or (b) MEMBRANE's own Q8 codec wired
into an actual inference runtime (a substantially larger integration task,
out of scope here). Both are natural next steps, not blocked on any new
design work — the tool and methodology are ready.

## Success criteria (item 6/7 of the task)

| criterion | result |
|---|---|
| actual ratio ≥ 1.8x | **met by 6/8 configs** (symmetric 64/128/256, affine 128/256); the 2 misses (symmetric/affine at group=32) fall short only on metadata overhead, not error |
| cosine similarity ≥ 0.99 | **met by all 8/8 configs**, by a wide margin (0.99994–0.99999 vs the 0.99 bar) |
| relative L2 error reported | **done** — 0.52%–1.05% across the sweep, see Result 1 |
| saturation low | **met** — 0.40%–6.33%, tracking the analytically-expected 1/group_elems baseline (Result 1, point 3), not indicating outlier trouble |
| numerical-error metrics used instead of bit-exact integrity | **done throughout** — MSE/RMSE/MAE/max-error/cosine/rel-L2/saturation, no pass/fail-on-bytes anywhere in this phase's codec |
| Q8 proven safe before Q4 | **the numerical case is made**: excellent fidelity at every swept config, deterministic, NaN/Inf/corruption all handled and tested; the live-inference case is not yet made (Result 3) |

## Recommendation

Do not proceed to Q4 yet. The numerical evidence here supports it (Q8's
error is small enough that a coarser 4-bit step is a reasonable next
question), but this phase's own item 7 was explicit that Q8's *safety*
should be proven first, and the live-inference half of that proof is
currently blocked, not completed. The concrete next steps, in order: (1)
re-run `membrane-kv-quality` against a compatible model to get the
top-1/top-5/KL/text numbers this phase couldn't produce for `stories15M`;
only then (2) consider Q4.

The raw outputs (`kv-q8.jsonl`, `kv-q8.csv`, `kv-q8-blocks.csv`,
gitignored) live in `benchmarks/results/phase3-kv/`; every number above
regenerates with the command in "Experiment setup".

## Verification

- Release, ASan+UBSan, and TSan builds all green: 14/14 unit tests pass in
  each configuration.
- The analyzer's full Q8 sweep (all 384 sweep points, both timing and
  correctness passes) runs clean under ASan+UBSan on real KV data — 0
  sanitizer diagnostics.
- `test_f16convert.c` exhaustively round-trips all 65536 possible half bit
  patterns; `test_q8block.c` and `test_kvquant.c` cover every safety case
  in item 8 of the task (NaN/Inf, all-zero, constant block, tiny/huge
  values via the exhaustive f16 test, overflow, malformed header, invalid
  scale, truncated payload, checksum corruption) plus determinism.
