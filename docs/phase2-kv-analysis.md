# Phase 2.1 — Real KV-Cache Capture and Analysis

This document describes the pipeline that extracts real KV-cache tensors
from a running llama.cpp model and the first measured results. Every number
below was produced by the commands shown; nothing is extrapolated.

## Pipeline

```
GGUF model ─▶ llama.cpp (submodule) ─▶ membrane-kv-capture ─▶ .kvdump files
                                                                  │
                                              membrane-kv-analyze ┴▶ JSONL + CSV + report
```

- **`membrane-kv-capture`** (built only with `-DMEMBRANE_ENABLE_LLAMA=ON`)
  loads a GGUF model, feeds a prompt (repeated to fill the requested token
  count), then exports the per-layer K and V tensors from
  `llama_state_seq_get_data()` into MEMBRANE's versioned kvdump format.
  The state blob layout is internal to llama.cpp, so the parser targets
  exactly the submodule commit pinned in `third_party/llama.cpp`
  (`c0bc8591`) and bounds-checks every read; a layout change aborts with an
  error instead of exporting garbage. Both V layouts (row-major when flash
  attention is enabled, transposed otherwise) are supported.
- **kvdump format** (`include/membrane/kvdump.h`): a sequence of records,
  each a 144-byte little-endian header (magic `MKV1`, version, model name,
  layer, K/V type, token range, element dtype, shape, payload size, payload
  CRC32, header CRC32) followed by the raw tensor bytes. Corrupted or
  truncated records are rejected by CRC/size checks (covered by
  `tests/unit/test_kvdump.c`).
- **`membrane-kv-analyze`** computes, per record and per block size
  (4 KiB / 16 KiB / 64 KiB / 256 KiB): RAW size, RLE size/ratio, the
  adaptive (RLE-with-RAW-fallback) result via the real `membrane_block`
  path, Shannon entropy, zero-byte ratio, run-length distribution, and
  end-to-end decode integrity. Output: JSONL (with an env record carrying
  OS/CPU/RAM/compiler and `--meta` key=values), CSV, and a human summary.

## Experiment setup

| | |
|---|---|
| Model | `stories15M.gguf` (TinyStories LLaMA, 6 layers, n_embd 288, F32 weights) from `ggml-org/models` |
| KV cache dtype | F16 (llama.cpp default; 576-byte rows per cell) |
| Context | 1024 tokens per prompt (prompt repeated to fill) |
| Prompts | `short` (one sentence), `repeat` (one sentence ×60), `natural` (story paragraph), `code` (C snippet) — in `benchmarks/kv/prompts/` |
| llama.cpp | submodule commit `c0bc8591` |
| MEMBRANE | commit `a3b8219` (analysis code added on top) |
| Host | AMD Ryzen 5 5600H, 5.7 GB RAM, Linux 6.18, gcc 13.3.0, CPU-only |

Reproduction:

```bash
cmake -S . -B build-llama -G Ninja -DCMAKE_BUILD_TYPE=Release -DMEMBRANE_ENABLE_LLAMA=ON
cmake --build build-llama
./build-llama/tools/membrane-kv-capture/membrane-kv-capture \
    --model models/stories15M.gguf \
    --prompt-file benchmarks/kv/prompts/natural.txt \
    --out natural.kvdump --n-tokens 1024
./build-rel/tools/membrane-kv-analyze/membrane-kv-analyze \
    --jsonl out.jsonl --csv out.csv natural.kvdump
```

## Results (48 tensor records: 4 prompts × 6 layers × {K,V})

**Headline: real F16 KV-cache data is near-maximum-entropy at the byte
level, and byte-level RLE does not compress it at all.**

| Metric (64 KiB blocks) | K | V |
|---|---|---|
| Shannon entropy | 7.376–7.396 bits/byte | 7.327–7.402 bits/byte |
| Zero-byte ratio | ~0.2% | ~0.2% |
| Longest byte run | 4 | 4 |
| Pure RLE ratio | 0.502x (doubles the data) | 0.502x |
| Adaptive (RLE + RAW fallback) | 1.000x — all blocks stored RAW | 1.000x |
| Decode integrity | PASS (all records, all block sizes) | PASS |

Additional observations, all measured:

1. **Prompt content does not matter.** Even the `repeat` prompt (one
   sentence repeated 60 times) produces KV entropy of 7.375 bits/byte —
   indistinguishable from natural text (7.370) or code (7.357). Identical
   tokens do *not* produce identical KV rows because rotary position
   embeddings mix the token position into K, decorrelating repeats.
2. **Block size does not matter** for RLE: 0.5020x at 4 KiB, 16 KiB,
   64 KiB and 256 KiB alike.
3. **K vs V are near-identical** in compressibility (K marginally higher
   entropy than V, ~0.03 bits/byte). Layer number has no visible effect in
   this 6-layer model.
4. **The adaptive fallback did exactly its job**: with RLE useless on this
   data, every block was stored RAW — the engine never *expanded* the
   cache, which blind RLE would have (2x).

## What this means for MEMBRANE

The lossless byte-level codecs of Phase 0/1 are the wrong lever for
KV-cache compression — that is now a measured fact, not an assumption.
The levers the roadmap should reach for next, in order of expected value:

- **Tensor-aware transforms** before entropy coding (e.g. byte-plane
  splitting of F16 values: exponent bytes across a row are far more
  correlated than interleaved sign/mantissa bytes — this is the standard
  trick behind float-specialized compressors and is *not* measured here
  yet, so it is a hypothesis to test, not a claim).
- **Lossy per-block quantization** (F16 → Q8/Q4, roadmap Phase 4), where
  llama.cpp already demonstrates the memory headroom, and MEMBRANE's
  contribution is deciding *per block* what precision each part of the
  cache deserves.

The full raw outputs (JSONL/CSV, gitignored) live in
`benchmarks/results/phase2-kv/`; the prompts are versioned in
`benchmarks/kv/prompts/` so every number here can be regenerated with the
two commands above.
