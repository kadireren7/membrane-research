# Phase 2.3 — Predictive Lossless KV Transform Experiment

This document reports a lossless *predictive* transform on real F16
KV-cache data: replace each value with its XOR against an earlier value
(previous element, previous token, previous row) and measure whether the
residual is more compressible than the raw values. It follows Phase 2.1
(byte-level codecs fail) and Phase 2.2 (the byte-plane split exposes a
lower-entropy high plane that RLE can't spend). Every number below was
produced by the commands shown; nothing is extrapolated, and no entropy
coder is run — only entropy is measured.

## Headline

**No predictor helps. Every XOR predictor *raises* residual entropy above
the no-transform baseline on this data.** The raw F16 values are already
lower-entropy than any XOR residual of them, so lossless XOR prediction is
the wrong lever — the compressible structure lives in the symbol
distribution (the high byte plane, Phase 2.2), not in inter-value
correlation that XOR can exploit.

## What was built

- **Predictor transform** (`include/membrane/f16xor.h`,
  `src/codecs/f16_xor_byteplane.c`) — four modes over the uint16 bit
  patterns: `NONE` (identity), `XOR_PREVIOUS_ELEMENT` (stride 1),
  `XOR_PREVIOUS_TOKEN` and `XOR_PREVIOUS_ROW` (stride = elements per row).
  The transform XORs each value against the one `stride` elements earlier,
  keeping the first `stride` values raw as seeds; it is exactly invertible
  (XOR is its own inverse), so it is lossless for any input. For a
  row-major KV tensor a row is one token, so TOKEN and ROW resolve to the
  same stride — measured to be bit-identical below, as expected.
- **`MEMBRANE_CODEC_F16_XOR_BYTEPLANE`** — an experimental codec that
  applies the (shape-free) element predictor, splits the residual into low/
  high byte planes, and stores both planes RAW behind an 18-byte versioned
  header (`version, predictor, stride_elems, plane_len, low_len, high_len`).
  It is deliberately *not* a compressor yet: RAW planes make the output
  larger than the input, so through the block layer it always falls back to
  RAW. Its job is to prove the transform round-trips losslessly and to
  carry a header general enough for a future entropy-coded version. decode
  is self-describing and rejects malformed/truncated/inconsistent streams
  as `CORRUPT_DATA` with full bounds and overflow checks.
- **Residual metrics** (`include/membrane/kvpredict.h`,
  `src/kvdump/kvpredict.c`) — per predictor: total / low-plane / high-plane
  residual entropy, zero-uint16 ratio, zero-byte ratio, longest zero run,
  an information-theoretic ideal compressed size (`low_bytes·H_low/8 +
  high_bytes·H_high/8`, rounded up), the theoretical ratio it implies, and
  per-token-quartile entropy.
- **`membrane-kv-analyze`** now runs all four predictors on every tensor
  payload and emits `record:"residual"` JSONL plus an optional flat
  `--pred-csv`, with K-vs-V, per-layer, and token-axis summaries.
- **Tests** (`tests/unit/test_codec_f16_xor_byteplane.c`): round-trip in
  all predictor modes, known uint16 sequence and residual bytes, all-zero,
  monotonic bit patterns, random, empty/first-element handling, invalid
  shape (TOKEN without a row width), corrupted header (version, predictor
  id, inconsistent lengths), truncated payload, buffer overflow, RAW
  fallback through the block layer, and checksum tamper.

## Experiment setup

Same inputs as Phase 2.1/2.2: the four `stories15M` F16 KV dumps (`short`,
`repeat`, `natural`, `code`), 6 layers × {K,V} = 48 tensor records, 1024
tokens each. Both K and V are stored row-major, `dims = [576 bytes/row,
1024 tokens]` — each row is one token of 288 F16 elements, so the element
stride for TOKEN/ROW is 288. Host: AMD Ryzen 5 5600H, Linux 6.18,
gcc 13.3.0, CPU-only.

Reproduction:

```bash
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
./build-rel/tools/membrane-kv-analyze/membrane-kv-analyze \
    --jsonl benchmarks/results/phase2-kv/kv-predictive.jsonl \
    --csv   benchmarks/results/phase2-kv/kv-predictive-blocks.csv \
    --pred-csv benchmarks/results/phase2-kv/kv-predictive.csv \
    --meta phase=2.3 \
    benchmarks/kv/dumps/short.kvdump benchmarks/kv/dumps/repeat.kvdump \
    benchmarks/kv/dumps/natural.kvdump benchmarks/kv/dumps/code.kvdump
```

## Results (48 tensor records, whole-payload residuals)

Mean over all 48 records. `ideal ratio` is the entropy ceiling a perfect
order-0 coder over the two residual byte planes would reach — a bound, not
an achieved size.

| Predictor | residual H (bits/byte) | high-plane H | ideal ratio |
|---|---|---|---|
| **NONE (raw)** | **7.37** | **5.618** | **1.176x** |
| XOR previous element | 7.56 | 6.229 | 1.125x |
| XOR previous token | 7.48 | 6.003 | 1.143x |
| XOR previous row | 7.48 | 6.003 | 1.143x |

Per-tensor (K vs V), theoretical ratio:

| Predictor | K ratio | V ratio |
|---|---|---|
| NONE | 1.171x | 1.180x |
| XOR previous element | 1.117x | 1.133x |
| XOR previous token / row | 1.150x | 1.136x |

Observations, all measured:

1. **XOR raises entropy — it does not lower it.** The NONE baseline is the
   best in every case. The transform damages exactly the plane Phase 2.2
   found structure in: the high (sign+exponent) byte plane climbs from
   5.618 bits/byte (raw) to 6.003 (token) or 6.229 (element). Adjacent F16
   values differ enough that XORing their exponent bytes destroys the tight
   exponent distribution instead of concentrating it. The low (mantissa)
   plane stays ~7.99 bits/byte — noise before and after.
2. **The NONE ceiling reproduces Phase 2.2 exactly** (low 7.994, high 5.618,
   ~1.176x), a consistency check that the pipeline measures the same data
   the same way.
3. **TOKEN and ROW are bit-identical** (7.48 / 6.003 / 1.143x, to every
   digit) — confirming that for this row-major layout a row *is* a token, so
   the two axes are the same stride. They are kept as separate modes for
   layouts (e.g. transposed V) where they would differ; this dataset does
   not contain such a layout.
4. **The token axis carries more correlation than the element axis.** Among
   the (net-negative) predictors, TOKEN/ROW is consistently less damaging
   than ELEMENT (high-plane 6.003 vs 6.229; ratio 1.143x vs 1.125x). So
   same-slot values one token apart are more alike than neighbouring
   embedding dimensions within a token — there *is* weak inter-token
   correlation, just not enough for XOR to turn into a net win.
5. **Zero structure appears but stays marginal.** Token/row XOR produces the
   most zero bytes (2.79%, longest zero run 39, vs 0.20% and run 2 for raw)
   and the most zero uint16 values (0.041%), i.e. occasional exactly-equal
   values one token apart. It is nowhere near enough to offset the entropy
   the transform adds elsewhere.
6. **No layer benefits.** NONE is the best predictor at all 6 layers; the
   ceiling barely moves across layers (1.169x–1.178x).
7. **No token-position trend.** For the token predictor, residual entropy is
   essentially flat from the first to the last token quartile (K 7.45→7.42,
   V 7.53→7.51).
8. **Prompt content does not matter**, again: all four prompts rank the
   predictors identically and land within ~0.005x.

## Analysis questions, answered from the measurements

- **Best predictor for K vs V?** Same for both: **NONE**. Among XOR modes,
  the least-bad differs slightly — TOKEN/ROW for K (1.150x), essentially a
  tie between TOKEN/ROW (1.136x) and ELEMENT (1.133x) for V — but neither
  beats no-transform.
- **Correlation on the token axis or the element/row axis?** Higher on the
  **token axis** (TOKEN/ROW residual entropy < ELEMENT residual entropy for
  both K and V), though still below the no-transform bar.
- **Does RoPE make K behave differently from V?** The hypothesis was that
  RoPE would decorrelate K across tokens and make token-prediction *worse*
  for K than V. The data does **not** confirm it: token-XOR raises K's
  entropy by only ~0.05 bits/byte but V's by ~0.17, so token-XOR is
  *less* damaging to K than to V — the opposite direction. Both remain
  net-negative, so the practical conclusion (don't XOR) is unchanged.
- **Does the best predictor change by layer?** No — NONE wins at every
  layer, with negligible layer-to-layer variation.

## What this means for MEMBRANE

Three phases now agree, each measured: byte-level RLE (2.1), a byte-plane
transform + RLE (2.2), and XOR prediction (2.3) all fail to compress F16
KV-cache data losslessly below ~1.0x in practice. The only lossless signal
found is the ~5.6 bits/byte high-byte plane (Phase 2.2), and XOR prediction
actively destroys it. So the lossless levers are exhausted for now; the
remaining paths, in order:

- **Entropy-code the raw high byte plane** (Phase 2.2's ~5.6 bits/byte),
  *without* a predictor. That is still the smallest lossless change with a
  real (if modest, ~15% ceiling) payoff, and this experiment rules out
  spending effort on a predictor stage before it.
- **Lossy per-block quantization** (roadmap Phase 4) remains the larger
  lever and the only route to the 40% target; the byte-plane view says the
  mantissa plane is where precision can be dropped almost for free.

No performance is claimed here: no data was compressed, only its entropy
measured. The contribution is a *measured* elimination of lossless XOR
prediction as a useful transform for this KV cache.

The raw outputs (`kv-predictive.jsonl`, `kv-predictive.csv`,
`kv-predictive-blocks.csv`, gitignored) live in
`benchmarks/results/phase2-kv/`; the dumps and prompts are the same
versioned inputs as Phase 2.1, so every number regenerates with the command
above.

## Verification

- Release, ASan+UBSan, and TSan builds all green: 9/9 unit tests pass in
  each configuration, and the analyzer (which allocates a residual buffer
  per predictor) runs clean under ASan+UBSan on the real dumps.
- All four predictor modes round-trip bit-identically in
  `test_codec_f16_xor_byteplane`, including the block-layer RAW fallback and
  checksum-tamper paths.
