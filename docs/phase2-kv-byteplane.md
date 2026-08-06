# Phase 2.2 — F16 Byte-Plane KV Codec Experiment

This document reports the first tensor-aware codec experiment on real
KV-cache data: splitting F16 values into their two byte planes and
run-length-encoding each plane independently. It follows directly from the
hypothesis raised (and explicitly *not* measured) at the end of
[phase2-kv-analysis.md](phase2-kv-analysis.md). Every number below was
produced by the commands shown; nothing is extrapolated.

## What was built

- **`MEMBRANE_CODEC_F16_BYTEPLANE_RLE`** (`src/codecs/f16_byteplane.c`) — a
  new codec in the existing pluggable registry. It accepts only
  even-length input (whole 16-bit elements); odd or otherwise unsuitable
  lengths return `MEMBRANE_ERR_INVALID_ARG`. Encode deinterleaves the low
  bytes into one plane and the high bytes into another, RLE-compresses each
  plane independently (reusing `MEMBRANE_CODEC_RLE`), and writes a 14-byte
  versioned little-endian header (`version`, `reserved`, `plane_len`,
  `low_comp_len`, `high_comp_len`) followed by the two RLE streams. Decode
  validates the header, RLE-decodes each plane, and re-interleaves the
  bytes back into the original F16 layout. Truncated, overrun, and
  internally-inconsistent streams are rejected as `MEMBRANE_ERR_CORRUPT_DATA`
  before any out-of-bounds access. Because it is a normal codec, the block
  layer's existing RAW fallback applies unchanged: if the codec's output is
  not smaller than the input, the block is stored RAW and the cache never
  expands.
- **Metrics** (`kvmetrics.c`/`.h`) gained per-plane low/high entropy,
  per-plane RLE ratios, the raw byte-plane codec ratio, the adaptive
  (RAW-fallback-aware) byte-plane ratio, and a separate byte-plane
  integrity flag.
- **`membrane-kv-analyze`** now reports, for every record and block size,
  RAW / RLE / F16 byte-plane RLE / adaptive side by side in JSONL and CSV,
  and prints per-K/V, per-layer, and per-prompt byte-plane summaries.
- **Tests** (`tests/unit/test_codec_f16_byteplane.c`): known-sequence and
  header round-trip, random F16, repeating F16, odd/single-length
  rejection, corrupted header (version/reserved), truncated stream,
  plane-length mismatch (over- and under-stated), output-buffer overflow on
  both compress and decompress, RAW fallback through the block layer, and
  stored-byte tamper caught by the block checksum.

## Experiment setup

Identical inputs to Phase 2.1: the four `stories15M` F16 KV dumps
(`short`, `repeat`, `natural`, `code`), 6 layers × {K,V} = 48 tensor
records, 1024 tokens each, analysed at 4 KiB / 16 KiB / 64 KiB / 256 KiB
block sizes. Host: AMD Ryzen 5 5600H, Linux 6.18, gcc 13.3.0, CPU-only.

Reproduction:

```bash
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
./build-rel/tools/membrane-kv-analyze/membrane-kv-analyze \
    --jsonl benchmarks/results/phase2-kv/kv-byteplane.jsonl \
    --csv benchmarks/results/phase2-kv/kv-byteplane.csv \
    --meta phase=2.2 --meta codec=f16bp \
    benchmarks/kv/dumps/short.kvdump benchmarks/kv/dumps/repeat.kvdump \
    benchmarks/kv/dumps/natural.kvdump benchmarks/kv/dumps/code.kvdump
```

## Results (48 tensor records, 64 KiB blocks)

**Headline: the byte-plane split is real — the high-byte plane carries
~2.4 fewer bits/byte of entropy than the low-byte plane — but byte-level
RLE cannot capture that structure, so the codec still expands the data and
adaptive fallback stores every block RAW.**

| Metric (mean over 48 records) | Value |
|---|---|
| Interleaved (whole-tensor) entropy | 7.368 bits/byte |
| **Low-byte plane entropy** | **7.989 bits/byte** (near-random) |
| **High-byte plane entropy** | **5.611 bits/byte** (structured) |
| Low-plane RLE ratio | 0.502x (RLE doubles it) |
| High-plane RLE ratio | 0.515x (RLE still doubles it) |
| **F16 byte-plane RLE ratio (total)** | **0.508x** |
| Byte-plane adaptive (RAW-fallback) | 1.000x — all blocks stored RAW |
| Byte-plane decode integrity | PASS (all records, all block sizes) |

Codec comparison on the same data (mean ratios, 64 KiB blocks):

| Codec | Ratio | Effect |
|---|---|---|
| RAW | 1.000x | baseline |
| RLE | 0.502x | doubles the data |
| F16 byte-plane RLE | 0.508x | doubles the data (marginally less) |
| adaptive (any of the above + RAW fallback) | 1.000x | never expands |

Observations, all measured:

1. **The split is genuinely asymmetric.** The high-byte plane (sign +
   exponent bits of the F16 values) sits at ~5.61 bits/byte, while the
   low-byte plane (low mantissa bits) is ~7.99 bits/byte — essentially
   incompressible noise. The interleaving in Phase 2.1 hid this: the mixed
   stream averaged to 7.37 bits/byte. So the *hypothesis from Phase 2.1 is
   confirmed at the entropy level* — the byte planes are not equally
   random.
2. **RLE is still the wrong backend.** Lower entropy is a property of the
   *symbol distribution*, not of runs. The high plane has no long runs
   either, so RLE doubles it (0.515x) almost exactly as it doubles the low
   plane (0.502x). The transform exposes compressibility that this entropy
   coder cannot spend.
3. **K and V behave identically** (both 0.508x byte-plane, both planes
   within ~0.01 bits/byte), and **layer number has no visible effect**
   across the 6 layers — matching the Phase 2.1 finding.
4. **Prompt content still does not matter.** All four prompts, including
   `repeat`, produce the same 0.508x and the same plane entropies.
5. **Adaptive did its job again.** With the byte-plane codec output ≥ raw
   on every block, all blocks fell back to RAW — 1.000x, never expanding.

## What this means for MEMBRANE

The measured entropies put a hard, information-theoretic ceiling on what a
*perfect* entropy coder over the split planes could achieve on this data:

```
ideal ratio = 1 / (0.5 · 7.989/8 + 0.5 · 5.611/8) ≈ 1.177x  (≈15% smaller)
```

versus only ~1.086x for an ideal coder over the interleaved stream. So the
byte-plane transform roughly doubles the *available* headroom (from ~8% to
~15%) — but realising it requires an actual entropy coder (Huffman / rANS /
range coder), which MEMBRANE does not have yet. RLE captures none of it.

Concretely, the next levers, in order:

- **Entropy-code the high plane.** A static or adaptive entropy coder on
  the ~5.6 bits/byte high-byte plane is the smallest change that would turn
  the confirmed structure into an actual ratio below 1.0x. The low plane
  can stay RAW — it is noise. This is the natural Phase 2.3.
- **Lossy per-block quantization** (roadmap Phase 4) remains the larger
  lever, and the byte-plane view sharpens it: the low-mantissa plane is
  where precision can be dropped almost for free, since it already looks
  random.

Even at ~15%, lossless byte-plane coding alone will not reach the roadmap's
40% KV-memory target — that target still depends on lossy quantization.
This experiment's contribution is a *measured* decomposition of where the
compressible bits actually live in an F16 KV cache.

The raw outputs (`kv-byteplane.jsonl` / `kv-byteplane.csv`, gitignored)
live in `benchmarks/results/phase2-kv/`; the prompts and dumps are the same
versioned inputs as Phase 2.1, so every number here regenerates with the
command above.

## Verification

- Release, ASan+UBSan, and TSan builds all green: 8/8 unit tests pass in
  each configuration (`ctest`), and the analysis binary runs clean under
  ASan+UBSan on the real dumps.
- Decode integrity PASS for the adaptive path and the byte-plane path
  across all 48 records at all four block sizes.
