# Phase 2.4 — F16 High-Plane Entropy Coding Experiment

This is the decisive lossless experiment: actually compress the F16 KV
high byte plane with a real entropy coder and measure the delivered ratio
and speed, rather than only its entropy. It closes the lossless arc of
Phase 2 (2.1 byte-level RLE fails, 2.2 the byte-plane split exposes a
~5.6 bits/byte high plane, 2.3 XOR prediction only hurts). Every number
below was produced by the commands shown; nothing is extrapolated except
where a projection is labelled as such.

## Headline

Order-0 canonical Huffman on the high plane delivers **1.13x–1.17x**
end-to-end, at **~99% of its own entropy ceiling** and bit-exact — so the
codec works and meets the >1.10x bar. But two measured facts cap the whole
lossless path:

1. **The ceiling itself is low.** The low (mantissa) byte plane is ~8-bit
   noise and stays RAW, so even a perfect high-plane coder tops out around
   ~1.3x end-to-end.
2. **Order-0 Huffman is not even the best high-plane backend.** An offline
   comparison shows the high plane has higher-order structure that
   general LZ+entropy coders exploit and Huffman cannot: on a single
   tensor's high plane, Huffman ceilings at 1.41x while zstd reaches 1.78x
   and xz 1.99x — and zstd *decodes ~9x faster*.

Lossless KV compression is therefore a **limited-benefit** path (~1.17x
delivered, ~1.3x realistic ceiling). The recommendation is to move to
Phase 3 lossy quantization for anything approaching the roadmap's 40%
target.

## What was built

- **Canonical Huffman coder** (`include/membrane/huffman.h`,
  `src/codecs/huffman.c`) — dependency-free, order-0, 8-bit symbols. Code
  lengths are limited to 15 bits (zlib-style Kraft repair) and stored as a
  fixed 128-byte nibble-packed table; the stream is a symbol count, the
  table, then an MSB-first bitstream. Decode validates the table (rejects
  over-subscription) and truncated bitstreams.
- **`MEMBRANE_CODEC_F16_HIGHPLANE_HUFFMAN`**
  (`src/codecs/f16_highplane_huffman.c`) — 18-byte versioned header
  (`version, flags, element_count, low_len, high_comp_len, crc`), RAW low
  plane, Huffman-coded high plane, re-interleave on decode, whole-payload
  CRC32 verify, full bounds/overflow checks. When the total is not smaller
  than the input the block layer stores RAW, so the cache never expands.
- **Metrics + analyzer** — deterministic per-block Huffman size/overhead/
  integrity in `kvmetrics`, plus an analyzer timing pass (encode/decode
  GB/s), theoretical-ratio and efficiency columns, a per-block-size
  summary, and a `--dump-highplane` option that writes the raw high-plane
  bytes for the offline backend comparison.
- **Tests** — `test_huffman.c` (empty, single symbol, full alphabet,
  random, skewed input that triggers the length limiter, over-subscribed
  table, truncated bitstream, invalid header, overflow) and
  `test_codec_f16_highplane_huffman.c` (round-trips, compressible shrink,
  odd length, corrupted header, codec-CRC tamper, block-layer RAW fallback
  and checksum tamper).

## Experiment setup

Same inputs as 2.1–2.3: the four `stories15M` F16 KV dumps, 6 layers ×
{K,V} = 48 records, 1024 tokens each, at 4 KiB / 16 KiB / 64 KiB / 256 KiB
blocks. Host: AMD Ryzen 5 5600H, Linux 6.18, gcc 13.3.0, CPU-only.

```bash
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
./build-rel/tools/membrane-kv-analyze/membrane-kv-analyze \
    --jsonl benchmarks/results/phase2-kv/kv-entropy.jsonl \
    --csv   benchmarks/results/phase2-kv/kv-entropy.csv \
    --dump-highplane benchmarks/results/phase2-kv/highplane.bin \
    --meta phase=2.4 \
    benchmarks/kv/dumps/short.kvdump benchmarks/kv/dumps/repeat.kvdump \
    benchmarks/kv/dumps/natural.kvdump benchmarks/kv/dumps/code.kvdump
```

## Result 1 — the built codec (order-0 Huffman, per block)

Delivered over all 48 records, K+V. `ceiling` is the order-0 entropy
bound; `effic` is achieved/ceiling; `meta%` is the fixed codec+table
overhead as a fraction of the codec output. Speeds are wall-clock over the
whole dataset (naive scalar bit I/O; approximate).

| block | actual (adaptive) | ceiling | efficiency | enc GB/s | dec GB/s | meta% |
|---|---|---|---|---|---|---|
| 4 KiB   | 1.127x | 1.187x | 0.950 | ~0.08 | ~0.08 | 4.13% |
| 16 KiB  | 1.161x | 1.179x | 0.985 | ~0.10 | ~0.09 | 1.06% |
| 64 KiB  | 1.170x | 1.177x | 0.994 | ~0.10 | ~0.09 | 0.27% |
| 256 KiB | 1.171x | 1.176x | 0.996 | ~0.11 | ~0.09 | 0.09% |

Decode integrity: **PASS** for every record at every block size.

Observations, all measured:

1. **Huffman nearly reaches its own ceiling** — 95% at 4 KiB rising to
   99.6% at 256 KiB. The order-0 coder is doing its job; the gap to the
   ceiling is almost entirely metadata, not coding loss.
2. **Metadata is the small-block tax.** The 18-byte codec header + 132-byte
   Huffman table = 150 bytes per block is 4.1% of a 4 KiB block's output
   but 0.09% of a 256 KiB block's, which is exactly the 4 KiB→256 KiB
   ratio climb from 1.127x to 1.171x.
3. **The end-to-end ratio decomposes cleanly.** The low plane is RAW (1.0x)
   and the high plane compresses ~1.41x, so the whole is
   `2 / (1 + 1/1.41) = 1.17x` — the low plane, half the bytes and
   incompressible, sets the cap.
4. **Throughput is poor** (~0.1 GB/s). This is a naive scalar
   Huffman (per-block tree build on encode, bit-at-a-time decode); it is
   ~100x below memory bandwidth and, as Result 2 shows, ~9x slower to
   decode than a production LZ codec.

## Result 2 — offline backend comparison (which coder is best?)

Compressing the **high plane alone** (`--dump-highplane`), so the low
plane's noise does not dilute the comparison. This is the "prove the best
backend before integration" step. Order-0 Huffman is represented by the
measured high-plane entropy (H ≈ 5.68 bits/byte → 1.41x ceiling, which the
codec reaches to ~99%).

One tensor's high plane (294 912 bytes, no cross-record redundancy):

| backend | ratio |
|---|---|
| order-0 Huffman (this codec) | ~1.41x |
| gzip -9 | 1.601x |
| zstd -19 | 1.776x |
| xz -9 | 1.993x |

One 64 KiB block's high plane (32 768 bytes): gzip 1.555x, zstd 1.702x,
xz 1.829x — the same ordering holds at block scale.

Speed (zstd `-b19` on the 13.5 MB high plane vs the codec's timing pass):

| coder | high-plane ratio | decode speed |
|---|---|---|
| this Huffman | ~1.41x | ~0.09 GB/s |
| zstd -19 | 1.78x | ~0.80 GB/s |

**Conclusion: order-0 Huffman is not the best backend.** The high plane has
real higher-order structure (neighbouring exponent bytes are correlated;
LZ finds matches even within one tensor) that an order-0 model cannot see.
zstd and xz beat Huffman by 25–40% on ratio *and* zstd decodes ~9x faster.
Phase 2.2's entropy figure (which is an order-0 quantity) therefore
*understated* the lossless headroom.

Projected end-to-end lossless ratio if the high plane used each backend and
the low plane stayed RAW (`2 / (1 + 1/high_ratio)`):

| high-plane backend | projected end-to-end |
|---|---|
| Huffman (1.41x) | 1.17x (measured) |
| zstd (1.78x) | ~1.28x (projected) |
| xz (1.99x) | ~1.33x (projected) |

## Success criteria (Phase 2.4 item 7)

| criterion | result |
|---|---|
| actual ratio > 1.10x | **met** — 1.127x (4 KiB) to 1.171x (256 KiB) |
| bit-exact integrity | **met** — PASS, all records, all block sizes |
| decode speed measured & reported | **met** — ~0.09 GB/s (and ~0.80 GB/s for zstd) |
| small-block metadata cost shown | **met** — 4.13% at 4 KiB vs 0.09% at 256 KiB |

## Verdict and recommendation

The bar is met, but the win is small and structurally capped:

- **Delivered ~1.17x**, and even swapping in the best available backend
  (xz on the high plane) only reaches ~1.33x end-to-end — because the low
  mantissa plane is ~8-bit noise and roughly half of every F16 value.
  No lossless scheme escapes that: the incompressible mantissa is a hard
  wall well under 2x.
- **The dependency-free order-0 Huffman leaves ratio on the table**
  (1.41x vs zstd's 1.78x on the high plane) and is slow (~0.1 GB/s).
  Reaching the ~1.33x ceiling would mean taking a heavyweight LZ+entropy
  dependency, against the project's standalone-C11 principle, for a still
  modest gain.

**Lossless KV compression is a limited-benefit path** — real, bounded at
roughly 1.2–1.35x, and cheap only in the sense that it is exact. The next
lever, and the only route to the roadmap's ≥40% (≈1.7x+) target, is
**Phase 3 lossy per-block quantization** (F16 → Q8 → Q4). The byte-plane
view sharpens where to spend precision: the mantissa plane — the wall for
lossless — is exactly the part that can be truncated with controlled error,
while the ~5.6-bit exponent plane is the part worth keeping. Phase 3 should
quantize the mantissa and measure the quality/memory trade-off per block.

No performance beyond the measured ratios and speeds is claimed. The raw
outputs (`kv-entropy.jsonl`, `kv-entropy.csv`, `highplane.bin`, gitignored)
live in `benchmarks/results/phase2-kv/`; the dumps and prompts are the same
versioned inputs as Phase 2.1, so every number regenerates with the command
above (external-backend rows need `gzip`/`zstd`/`xz` on the extracted
`highplane.bin`).

## Verification

- Release, ASan+UBSan, and TSan builds all green: 11/11 unit tests pass in
  each configuration, and the analyzer (Huffman encode/decode over real
  data, per block size) runs clean under ASan+UBSan.
- The high-plane Huffman codec round-trips bit-identically for random,
  compressible, and degenerate inputs, and the block-layer RAW fallback and
  both checksum paths (codec CRC and block CRC) are covered by tests.
