# Phase 5.1 item 13: hardware-friendly datapath for a future FPGA Q8_0/Q4_0 engine

Status: architecture and interface specification only. No RTL, no synthesis,
no hardware target chosen. This document exists to record the datapath a
future FPGA (or ASIC) block-quantization pipeline would need to implement to
stay bit-exact with ggml's own Q8_0/Q4_0 quantize/dequantize math -- the same
math `src/quant/quant_simd.c` (Phase 5.1) and `src/quant/ggml_quant.c`
(Phase 4.4) already implement in software, cross-verified against real ggml
across 100,000+ random blocks. Per the governing spec for this phase, the CPU
datapath was built and measured first (`tools/membrane-quant-bench`,
`docs/phase5-quant-engine.md`); this document is the forward-looking output
of that work, not a substitute for it.

## 1. Why hardware at all

The CPU profiling in `docs/phase5-quant-engine.md` found that, at the real
32-element ggml block size, single-thread SIMD gives only a modest speedup
for Q8_0 quantize and essentially none for Q8_0 dequantize or Q4_0
quantize/dequantize -- the vectorizable multiply/round step is cheap; the
bottleneck is the *scalar* F16<->F32 conversion per element (and, for Q4_0,
the tie-sensitive sequential amax scan that item 4 of the governing spec
requires to stay scalar in every CPU backend, to avoid a SIMD reduction
picking a different tied element and diverging the sign of the scale). A
fixed-function hardware pipeline does not have this asymmetry: F16<->F32
conversion is a fixed-latency combinational operation in hardware (a small
exponent/mantissa shift-and-bias network, not a branchy software routine),
and the amax scan can be pipelined as a single left-to-right systolic reduce
that preserves first-occurrence-on-tie by construction (see 5.3), so neither
of the two CPU-side bottlenecks identified in this phase's profiling is
structural in hardware.

## 2. Input block format (unchanged from the software oracle)

Every block is exactly `MEMBRANE_QSIMD_BLOCK_ELEMS` = 32 contiguous F16
(binary16) elements, matching ggml's own block size (`QK8_0` / `QK4_0` = 32
in the pinned llama.cpp commit, `docs/phase4-ggml-quant-parity.md`). A row of
`elems_per_row` elements is `elems_per_row / 32` independent blocks; blocks
within a row, and rows within a batch, have no cross-block dependency, so
the entire input can be streamed to hardware as a flat sequence of 32-element
frames with no reordering.

- **Q8_0 block**: 32 x F16 in (64 bytes) -> 1 x F16 scale + 32 x int8 out
  (34 bytes). Compression ratio 1.882x by construction (independent of
  data).
- **Q4_0 block**: 32 x F16 in (64 bytes) -> 1 x F16 scale + 16 bytes of
  packed 4-bit pairs out (18 bytes). Compression ratio 3.556x.

## 3. Functional pipeline stages (Q8_0 quantize)

1. **F16 ingest**: 32 lanes wide, one F16 word per lane per cycle at full
   throughput (see 6). No stall here; this stage is purely a wire fan-out
   from the input FIFO to 32 parallel F16->F32 converters.
2. **F16->F32 widen**: combinational per-lane (sign/exponent/mantissa
   shift-and-bias, exactly the bit manipulation in
   `membrane_f16_to_f32()`, `src/codecs/f16convert.c`), including the
   Phase 5.1 quiet-NaN fix (payload OR 0x00400000 when the F16 mantissa is
   nonzero) so hardware output matches the same corrected reference the
   software engine now uses -- the PRE-fix behavior must never be
   replicated in hardware.
3. **abs + reduce-max (amax)**: a 32-wide, 5-stage binary-tree reduction
   (`|x0|` vs `|x1|` -> ... -> single amax), order-independent for Q8_0
   since only the magnitude is kept (unlike Q4_0's signed-max, see 5.3).
   Depth log2(32) = 5 compare-select stages, fully pipelable.
4. **scale compute**: `d = amax / 127`, `id = amax != 0 ? 127/amax : 0`.
   Two F32 dividers (or one shared divider timesliced across id/d if area
   is constrained instead of latency) -- this is the one non-trivial
   arithmetic block in the pipeline; a pipelined Newton-Raphson or
   lookup-table-seeded divider is the standard IP block for this, not a
   custom design.
5. **F16-round the scale**: `d` is stored as F16 in the output block, so
   this stage is another F32->F16 narrow (round-to-nearest-even, matching
   `membrane_f32_to_f16()`'s normal-path rounding -- NOT the NaN
   special-case, since `d` is never NaN for finite, non-empty-block
   input).
6. **quantize + round-half-to-even + saturate**: per lane, `q = x * id`,
   rounded round-to-nearest-even (a hardware round-to-nearest-even
   multiply-then-round unit, matching `_mm256_round_ps(...,
   _MM_FROUND_TO_NEAREST_INT)` -- NOT C's `roundf()`, which is
   half-away-from-zero and would silently break parity, see
   `docs/phase4-ggml-quant-parity.md` for why this distinction is load-
   bearing). Then saturate to int8 range, replicating the x86
   "integer-indefinite" CVTPS2DQ + double-saturating-pack sentinel
   behavior for NaN/+Inf/-Inf (all three -> -128, see
   `sat_i8_from_rounded_f32()`/`sat_i8_from_i32()` in
   `src/quant/quant_simd.c`) -- in hardware this is a simple explicit
   `is_nan_or_out_of_range -> force -128` mux ahead of the normal
   clamp-to-[-128,127] compare-select, cheaper than replicating the x86
   sentinel path CPU-side needed to (hardware gets to specify this
   behavior directly rather than inheriting it from an ISA quirk).
7. **pack**: 32 x int8 + 1 x F16 scale -> 34-byte output frame, written to
   the output FIFO.

Q4_0 quantize follows the same ingest/widen stages, but stage 3 becomes a
**sequential** (non-tree) left-to-right scan that keeps the signed value at
the first strictly-greater-magnitude position (`if (amax < |v|)`, matching
`q4_0_quant_block_scalar()`) -- see 5.3 for why this cannot be a tree
reduction. Stage 6 becomes the Q4_0 pack: `qi = trunc(x*id + 8.5)` (a
truncating cast, not round-to-nearest -- deliberately different from Q8_0),
`xi = min(15, qi) mod 256` (an 8-bit modular wrap on negative `qi`, no lower
clamp -- both details are ggml's actual, slightly asymmetric behavior,
reproduced exactly rather than "corrected", per `docs/phase4-ggml-quant-
parity.md`), then two 4-bit lanes pack into one byte, halving the output
width relative to Q8_0's byte-per-lane packing.

Dequantize (either format) is the pipeline run in reverse: unpack ->
per-lane int8/int4 -> F32 multiply by the block's scale -> F32->F16 narrow.
No amax/reduce stage is needed, so dequantize hardware is shallower and
higher-throughput than quantize for the same lane width -- consistent with
what the CPU benchmark already shows (dequantize's *software* bottleneck is
the scalar F16 narrow step, not the multiply; in hardware neither step is
scalar, so dequantize should end up meaningfully faster than quantize per
lane, unlike the CPU numbers in `docs/phase5-quant-engine.md` where they're
close because both are dominated by the same scalar conversion cost).

## 4. Rounding pipeline detail (the part most likely to silently break parity)

This is the single highest-risk area for a hardware implementation to
diverge from the software oracle, because "round to nearest, ties to even"
has more than one common hardware realization and only one of them matches
ggml:

- Q8_0's `rint()`-equivalent step must round ties to **even**, not away
  from zero. An IEEE-754 FPU's default rounding mode (RNE) already does
  this correctly if the multiply-and-round is done as a single fused
  operation in that mode; a naive `truncate(x + 0.5*sign(x))` "add-half-
  and-truncate" implementation (a common cheap hardware rounding trick)
  rounds ties away from zero and WOULD silently produce wrong output for
  every exact-half input -- this is exactly the class of bug found and
  fixed in `f16convert.c` during this phase's own bit-parity testing (see
  `docs/phase5-quant-engine.md` for the two pre-existing bugs found there),
  and the same failure mode applies directly to a hardware rounder design.
- Q4_0's `+ 8.5` step is a **truncating** cast, deliberately not
  round-to-nearest -- a hardware design that "fixes" this to be a proper
  round (reasoning that it looks like a rounding-bias constant) would
  diverge from ggml. This one detail has no principled derivation; it is
  simply what ggml's C code does, and hardware must replicate the exact
  arithmetic, not the apparent intent.
- Verification path for any future hardware or RTL simulation: the same
  100,000+ random-block, all-zero, constant, extrema, halfway-rounding, and
  NaN/Inf test vectors in `tests/unit/test_quant_simd_parity.c` are the
  right co-simulation vectors -- a hardware model's outputs must match
  `membrane_ggml_quant`'s oracle byte-for-byte on all of them before any
  hardware unit is considered correct, exactly as the two software
  backends already had to.

## 5. Pipeline stages, buffering, and backpressure

### 5.1 Stage count and latency budget

Quantize: ingest -> widen -> reduce-max (5 sub-stages) -> scale-compute
(divider latency, IP-dependent, typically 8-16 cycles pipelined) ->
scale-narrow -> quantize-round-saturate -> pack. Call it roughly 20-30
pipeline stages end to end for a fully-pipelined quantize block, dominated
by the divider. Since blocks are independent, a new block can enter the
pipeline every cycle once filled -- latency is per-block wall time, but
sustained *throughput* is one block/cycle regardless of the ~20-30 cycle
per-block latency, exactly the deep-pipeline-hides-latency pattern that
makes this workload hardware-friendly.

### 5.2 Required throughput and memory bandwidth

Using the CPU numbers as the floor a hardware design should be measured
against (not a target it must hit -- no throughput claim is made here since
no RTL exists to measure): `docs/phase5-quant-engine.md`'s best-measured CPU
throughput at block=4096, 12 threads, AVX2 was ~4.36 GB/s (Q8 quantize,
input-side bytes/sec). A hardware pipeline processing 32 F16 lanes/cycle at,
conservatively, 200 MHz would ingest `32 * 2 bytes * 200e6 = 12.8 GB/s`
input-side -- above the best-measured CPU figure, but this is a napkin
estimate for a single 32-lane pipeline at a deliberately conservative clock,
not a promised number; real FPGA fabric timing, place-and-route, and DSP
slice availability for the dividers would need to be measured against an
actual target part before this number means anything. This is flagged
explicitly per item 12's "no unmeasured performance claims" -- everything in
this section is a first-order estimate for sizing buffers and interfaces,
not a benchmark result.

Memory-bandwidth-wise, the workload is inherently bandwidth-light relative
to compute: Q8_0 quantize reads 64 bytes and writes 34 bytes per block (1.88x
compression), so a design bottlenecked on the ingest side needs less write
bandwidth than read bandwidth, and dequantize is the mirror image (34 bytes
read, 64 bytes written). Neither direction requires random access -- both
are pure streaming, which is the ideal DMA pattern for feeding an FPGA
accelerator over PCIe or a SoC's AXI-stream fabric.

### 5.3 Why Q4_0's amax scan cannot be a tree reduction

Q4_0's amax scan keeps the **signed** value at the first element whose
magnitude strictly exceeds the running max (`if (amax < |v|)`  -- strict
less-than, so a later tie does NOT replace an earlier one). A tree
reduction (as used safely for Q8_0's magnitude-only max) evaluates
comparisons in a different order than a sequential left-to-right scan, so
on a tie between two elements at different tree levels, a tree reduction
can retain a different tied element than the sequential scan would -- and
since a tie means both candidates have the *same* magnitude but possibly
*different signs*, this changes the sign of `d`, which changes the sign of
every output element in the block. This is precisely why item 4 of the
governing spec calls out keeping order-sensitive reductions scalar rather
than "fast but different" -- in hardware, the fix is not to avoid
pipelining the scan, but to pipeline it as a strictly sequential systolic
chain (each stage forwards its own answer only after strictly comparing
against the one before it, never combining two independent partial
reductions out of order), which still achieves one-block-per-cycle
throughput at steady state -- it just cannot be restructured into a
balanced tree the way the Q8_0 reduction can.

### 5.4 Buffer depth and backpressure

- **Input FIFO**: needs to absorb the divider's pipeline latency (~20-30
  cycles) worth of incoming blocks without stalling the upstream DMA/AXI-
  stream source, i.e. at least 32 blocks (32 x 64 bytes = 2 KB) of headroom;
  doubled for ping-pong buffering against the host-side batch API's
  row-at-a-time framing (`membrane_simd_*_batch*` in
  `include/membrane/quant_simd.h`) would put this around 4 KB, comfortably
  inside a single FPGA BRAM block.
- **Output FIFO**: same reasoning, sized to the packed (smaller) output --
  34 bytes/block for Q8_0, 18 bytes/block for Q4_0 -- so it can be
  proportionally shallower than the input FIFO for the same block-count
  headroom.
- **Backpressure**: since every block is independent and the pipeline is
  fully streaming with no block-to-block feedback, backpressure is a
  standard ready/valid handshake at the ingest and egress boundaries only
  -- no mid-pipeline stall logic is needed beyond gating the ingest-side
  ready signal off the input FIFO's fill level, which is the same pattern
  the software batch API already uses at a coarser grain (the caller-
  provided scratch buffer in `membrane_simd_*_batch()` bounds how much
  work is in flight at once, see `docs/phase5-quant-engine.md` section on
  the batch API).

## 6. Interface contract with the existing software engine

A future FPGA offload would sit behind the exact same
`membrane_simd_q{8,4}_0_{quantize,dequantize}_batch[_parallel]()` entry
points in `include/membrane/quant_simd.h` as a new `MEMBRANE_SIMD_FPGA`
backend value, selected the same way `membrane_simd_best_backend()`
currently probes for AVX2/SSE4.1 support -- probing instead for a bound PCIe/
SoC accelerator device and falling back to the existing scalar/SIMD engine
if none is present or if a self-check block fails, consistent with how the
scalar path is already the permanent safe fallback for every existing
backend (item 2 of the governing spec). No change to that public API's
contract (bit-exactness, batch framing, scratch-buffer sizing) would be
needed to add this backend -- the block format and batch semantics were
deliberately kept implementation-agnostic in Phase 5.1 for exactly this
reason.

## 7. What is explicitly out of scope here

No RTL, no HLS source, no vendor toolchain, no synthesis or timing closure,
no target part selection, and no power/area estimate. This document is the
functional and interface specification a hardware implementation would need
to satisfy to stay bit-exact with the software oracle; it is not itself an
implementation, and none of the throughput figures above are measured --
they are sizing estimates only, clearly labeled as such per item 12's
requirement not to present unmeasured numbers as results.
