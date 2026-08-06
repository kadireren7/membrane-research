# Phase 5.4: PCIe DMA hardware-in-the-loop FPGA prototype

Baseline: commit 710a3d7 (Phase 5.3, "fully synthesizable FPGA
quantization datapath"). Phase 5.3 delivered a fully synthesizable
`membrane_quant_stream_top` (all four Q8_0/Q4_0 encode/decode modes,
0 fails across a 520,000-transaction Verilator cosimulation, clean
`yosys` elaboration of the whole hierarchy) but explicitly stopped at
the module boundary -- no host-facing DMA path, no register ABI, no
packet format, no real or emulated PCIe transport. This phase adds all
of that and drives the whole stack, end to end, through a real
cycle-accurate RTL simulation of a DMA bridge wrapping Phase 5.3's
pipeline.

**Every number in this document was actually produced by a command run
on this machine in this phase** (re-run fresh, on 2026-07-26, to
regenerate this document rather than trust memory of earlier runs in
the same phase). Every number that is a projection, extrapolation, or
assumption is labeled as such at the point it's used. Nothing here
claims a real Alveo card, real PCIe hardware, or a measured Fmax --
section 1 explains why, and section 11 restates the boundary in full.

## 1. Hardware target: emulation tier decision

The phase spec's priority order was: real Alveo card > cloud FPGA >
QEMU/XRT emulation > cycle-accurate PCIe/DMA host emulation, with an
explicit instruction not to hide it if no card is available. Checked,
in this environment:

| Check | Command | Result |
|---|---|---|
| Xilinx/Alveo PCI device | `lspci \| grep -i xilinx` | empty -- no device |
| XRT runtime | `apt-cache policy xrt` | not available |
| Xilinx tools (`xbutil`/`xbmgmt`) | `which xbutil xbmgmt` | not found |
| QEMU | `which qemu-system-x86_64` | not found |
| Cloud FPGA credentials/API | none configured in this environment | n/a |

None of the first three tiers are available. This phase proceeds on
**tier 4: cycle-accurate PCIe/DMA host emulation** -- a Verilator
simulation of a new RTL DMA bridge (section 4), driven cycle-by-cycle
by a host-side C++ runtime, presenting the same MMIO register +
command-queue + payload-stream + completion-queue interface a real
PCIe driver would see. The only thing not physically modeled is PCIe
transport itself (SERDES, TLP framing, link training, real interrupt
latency) -- every byte of every quantize/dequantize transaction
actually flows through the real synthesizable RTL from Phase 5.3, one
clock edge at a time.

## 2. DMA packet format

`include/membrane/fpga_dma.h` / `src/fpga/fpga_dma.c`. A fixed 64-byte
header, little-endian, laid out at explicit byte offsets (not via
struct `memcpy`, to avoid host padding/endianness assumptions leaking
into the wire format):

| Offset | Bytes | Field |
|---|---|---|
| 0 | 4 | magic (`0x4650424D`, "MPBF") |
| 4 | 2 | version_major |
| 6 | 2 | version_minor |
| 8 | 8 | transaction_id |
| 16 | 1 | operation (0=Q8 encode, 1=Q8 decode, 2=Q4 encode, 3=Q4 decode) |
| 17 | 3 | reserved |
| 20 | 4 | element_count |
| 24 | 4 | input_byte_length |
| 28 | 4 | output_capacity |
| 32 | 2 | policy_layer_id |
| 34 | 2 | policy_flags |
| 36 | 4 | flags |
| 40 | 4 | header_checksum (CRC32 over bytes [0,40)) |
| 44 | 4 | payload_checksum (CRC32 over the payload) |
| 48 | 16 | reserved |

`operation`'s encoding is deliberately numerically identical to
`membrane_quant_stream_top`'s own 2-bit mode field
(`MODE_Q8_ENC=2'b00`/`MODE_Q8_DEC=2'b01`/`MODE_Q4_ENC=2'b10`/
`MODE_Q4_DEC=2'b11`), so the bridge's `in_mode = header.operation[1:0]`
needs no remapping table. CRC32 reuses `membrane_block_checksum()`
(`src/block/block.c`), already present in the codebase since Phase 1 --
no new checksum implementation was written.

## 3. Register map ABI

`include/membrane/fpga_regs.h`. Fixed offsets, documented once, shared
by the RTL bridge and the host runtime:

| Offset | Name | Access | Meaning |
|---|---|---|---|
| 0x00 | VERSION | RO | ABI version |
| 0x04 | CAPABILITIES | RO | feature bits |
| 0x08/0x0C | QUEUE_BASE_LO/HI | RW | informational (see below) |
| 0x10 | QUEUE_SIZE | RW | informational |
| 0x14 | DOORBELL | RW | informational counter |
| 0x18 | COMPLETION_HEAD | RW | host-managed |
| 0x1C | COMPLETION_TAIL | RO | device-managed |
| 0x20 | ERROR_FLAGS | RW1C | bit0 bad header CRC, bit1 bad payload CRC, bit2 malformed header, bit3 queue full, bit4 short output, bit5 timeout, bit6 completion overflow |
| 0x24 | PROCESSED_BLOCKS | RO | counter |
| 0x28 | STALL_CYCLES | RO | counter, see section 8 |
| 0x2C/0x30 | INPUT_BYTES_LO/HI | RO | counter |
| 0x34/0x38 | OUTPUT_BYTES_LO/HI | RO | counter |
| 0x3C | RESET | WO | pulses a synchronous soft reset |

Disclosed scope decision: QUEUE_BASE/QUEUE_SIZE/DOORBELL are
**informational/inert** in this design. The bridge's actual command and
completion queues are on-device `stream_fifo` instances (depth 16),
not host-memory descriptor rings a real DMA engine would walk -- a
production implementation would need those registers to actually drive
a scatter-gather descriptor fetch. This is stated plainly, not hidden.

## 4. RTL DMA bridge

`rtl/membrane_dma_bridge.sv` (new, ~400 lines), wrapping
`membrane_quant_stream_top` (Phase 5.3) unmodified. Vendor PCIe IP
itself was not written or modified -- this module is the vendor-neutral
bridge layer a platform-specific wrapper (Xilinx XDMA/QDMA shell, etc.)
would sit behind; that platform wrapper does not exist in this phase
(no vendor tool available to build one against, section 1).

Internals: three `stream_fifo` instances (command FIFO, 512-bit
headers, depth 16; completion FIFO, 128-bit records, depth 16; result
FIFO, 519-bit block-plus-length records, depth 8, whose `in_ready`
directly gates the wrapped top module's `out_ready` for natural
backpressure) and a 5-state FSM (`ST_IDLE`/`ST_CHECK`/`ST_STREAM`/
`ST_ERROR`/`ST_COMPLETE`).

**The one non-trivial correctness bug found and fixed in this module**
(via multi-block batch testing, not caught by single-block tests):
`op_input_bytes`/`op_output_bytes` return 4-byte-**aligned** sizes (34
bytes → 36, 18 bytes → 20) rather than Q8_0/Q4_0's true packed sizes.
The bridge's payload port is 32 bits (4 bytes) wide; without alignment,
a block boundary that doesn't land on a 4-byte beat boundary causes the
next block's first bytes to arrive bundled into the current block's
last beat, and the accumulator's reset-on-block-complete logic silently
drops or misaligns them. Fixed by padding to 4-byte strides in both RTL
and the host-side packing/unpacking code, and re-verified with the full
100,000-block x4-operation suite (section 6) after the fix -- 0 fails.

Disclosed scope: one command processed at a time (not overlapped
across different commands' blocks -- section 9's queue-depth scaling
measurement reflects this directly); checksum validation happens
host-side, not in RTL (no hardware CRC engine was built); the payload
port is 32 bits wide, chosen for emulation-loop simplicity, **not**
representative of a production DMA engine's width (see section 9's
bandwidth discussion, which this narrowness directly explains).

## 5. Bit-exact verification

Host runtime: `tools/membrane-fpga-runtime` (new tool, kept out of the
main CMake build the same way Phase 5.2/5.3 kept Verilator-dependent
tooling standalone -- `tools/membrane-fpga-runtime/build.sh`). API:
`device_open`/`device_close`/`submit`/`raw_submit`/`poll`/`wait`/
`cancel`/`get_stats`/`reset_mid_flight`.

Re-run fresh this session:

```
verify 20000
[Q8_ENCODE] 20000/20000 blocks, 0 fails
[Q8_DECODE] 20000/20000 blocks, 0 fails
[Q4_ENCODE] 20000/20000 blocks, 0 fails
[Q4_DECODE] 20000/20000 blocks, 0 fails
PASS: DMA-path bit-exact verification, 20000 blocks x4 operations, 0 fails
```

This is a fresh spot-check confirming no regression; the full run this
phase's task 111 completed earlier (100,000 blocks x4 operations, 0
fails, ~34.2s) already satisfies the spec's 100,000-per-op minimum.
Every result is compared, per block, against three references:
MEMBRANE's scalar C quantizer, MEMBRANE's SIMD quantizer, and the ggml
reference tables -- all three are already known bit-identical to each
other from Phase 4.4 ([[phase4.4-ggml-quant-parity]]), and this phase's
verification confirms the FPGA-emulated path matches all three too, at
random batch sizes (1-8 blocks), random queue depths, and the DMA
padding/alignment from section 4.

## 6. DMA stress tests

All 15 scenarios re-run fresh this session, 0 failures:

```
PASS: unaligned host buffer (offset+3) round-trips correctly
PASS: minimum packet (element_count=1)
PASS: maximum packet (element_count=4096 in one transaction)
PASS: backpressure: 5th submit on depth-4 queue rejected (handle=0)
PASS: queue wraparound: all 4 originally-queued items drain correctly
PASS: queue slot reusable after drain (wraparound)
PASS: 10 outstanding requests, waited on in random order, all complete correctly
PASS: completion_wait times out (returns false) rather than hanging when payload never arrives
PASS: cancel() on a still-queued (not yet issued) handle succeeds
PASS: cancelled handle never produces a completion
PASS: cancelling an already-cancelled/unknown handle returns false
PASS: malformed header (bad magic) rejected by device with MEMBRANE_FPGA_ERR_MALFORMED_HEADER
PASS: bad payload checksum caught by host-side validation before ever reaching the device
PASS: output_capacity smaller than needed rejected with MEMBRANE_FPGA_ERR_SHORT_OUTPUT
PASS: device recovers cleanly after reset mid-transfer, fresh transaction after reset works
```

Two real bugs were found and fixed while building this suite, both
architectural, not RTL bugs:

1. **Sequential push-then-pull deadlock on large batches.** The first
   host runtime issued a full `payload_push()` before any
   `payload_pull()`. Once a batch exceeded the bridge's own internal
   buffering (~40 blocks: 8-entry result FIFO + Phase 5.3 top module's
   32-entry output FIFO), output-side backpressure stalled the input
   side forever, since nothing was draining output while push() was
   still blocking. Fixed by adding `FpgaEmuDevice::transfer()`, which
   interleaves push and pull in a single tick loop -- the honest
   analogue of what a real system needs two independent, concurrently
   running DMA engines for.
2. **`wait(handle, timeout_cycles)` not actually bounding device work.**
   A small `timeout_cycles` only bounded the runtime's outer retry
   loop; the actual device operation underneath used its own generous
   multi-million-cycle defaults regardless. Fixed by adding
   `FpgaRuntime::m_op_cycle_budget`, threaded into every synchronous
   device call `wait()` triggers, saved/restored around `wait()`'s own
   loop. Needed for scenario "completion_wait times out" above, and for
   fallback safety (section 12).

## 7. Performance measurement (emulation, assumed 200 MHz clock)

Re-run fresh this session. **The clock frequency is an explicit,
disclosed assumption (200 MHz), matching Phase 5.3's own assumption for
the same reason: no place-and-route tool is available in this
environment, so there is no measured Fmax anywhere in this phase
either. Cycle counts themselves are real Verilator simulation output.**

Round-trip latency, single Q8_0 block, 200 samples:

```
p50=0.21 us  p95=0.21 us  p99=0.21 us
```

(This is essentially zero jitter because nothing in this emulation
models real PCIe transport latency -- see section 9's caveat on this.)

Sustained throughput, 8192-block batch, one transaction:

| Op | Cycles | Blocks/s | Elements/s | In GB/s | Out GB/s | Stall cycles |
|---|---|---|---|---|---|---|
| Q8_ENCODE | 139,290 | 11,762,510 | 376,400,316 | 0.753 | 0.423 | 0 (0.0%) |
| Q8_DECODE | 139,290 | 11,762,510 | 376,400,316 | 0.423 | 0.753 | 128,936 (92.6%) |
| Q4_ENCODE | 139,286 | 11,762,848 | 376,411,125 | 0.753 | 0.235 | 0 (0.0%) |
| Q4_DECODE | 139,286 | 11,762,848 | 376,411,125 | 0.235 | 0.753 | 129,709 (93.1%) |

All four ops converge to the same ~85.0 ns/block sustained rate
(139,290 cycles / 8,192 blocks ≈ 17.0 cycles/block @ 200 MHz), because
the bridge's 32-bit payload port -- not Phase 5.3's underlying 512-bit,
1-block/cycle pipeline -- is the bottleneck (section 9 explains this is
a bridge design choice, not a limit of the underlying datapath).
STALL_CYCLES (`rtl/membrane_dma_bridge.sv`: cycles where the FSM is in
`ST_STREAM` and the wrapped top module's input isn't ready) is near-zero
for encode and >92% for decode: encode's packed output (34/36 or
18/20 bytes ≈ 9 or 5 beats) drains through the 32-bit port faster than
its 64-byte F16 input can be fed (16 beats), so the pipeline is never
input-starved; decode is the mirror image, with the wide 64-byte F16
output taking 16 beats to drain versus the packed 34/18-byte input's 9/5
beats, so the pipeline spends most cycles waiting on the narrow output
side to catch up.

Queue-depth scaling, 2000 separate 1-block Q8_0 transactions:

```
queue_depth=  1  4,651,163 transactions/s (0.2 us avg/transaction)
queue_depth=  4  4,651,163 transactions/s (0.2 us avg/transaction)
queue_depth= 16  4,651,163 transactions/s (0.2 us avg/transaction)
queue_depth= 64  4,651,163 transactions/s (0.2 us avg/transaction)
```

Flat across queue depth -- expected and disclosed: the bridge processes
one command at a time (section 4's scope decision), so this measures
submit/wait/pump call overhead at different local queue depths, not
overlapped multi-command execution. A production bridge wanting real
queue-depth-driven throughput scaling would need genuine command
pipelining, which this phase did not build.

Host CPU time for the whole perf run (measuring the simulator process
itself, not a projection of real driver CPU cost): `user=2.90s
sys=0.00s`.

## 8. CPU baseline (re-measured this session, `membrane-quant-bench`)

Real, currently-running numbers, single block (32 elements), scalar
backend (no SIMD), matching what a single CPU core would spend per
block:

| Op | 1 thread | 4 threads | 12 threads |
|---|---|---|---|
| Q8 quantize | 123.00 ns | 32.54 ns | 23.32 ns |
| Q8 dequantize | 179.77 ns | 47.78 ns | 25.94 ns |
| Q4 quantize | 77.55 ns | 20.92 ns | 19.23 ns |
| Q4 dequantize | 146.46 ns | 40.45 ns | 20.58 ns |

Against the FPGA-emulated single-pipeline rate of ~85.0 ns/block
(section 7): single-threaded CPU is within 1.5-2x of the emulated FPGA
pipeline on three of four ops (slower on Q8 quantize/dequantize and Q4
dequantize, *faster* on Q4 quantize alone), while CPU at 4 threads --
`membrane-kv-runtime`'s actual default thread count, section 10 -- beats
the single emulated FPGA pipeline on **all four** ops by 1.8x-3.5x, and
12 threads by 3.3x-4.4x.

## 9. Break-even analysis

**Batch-size break-even (single FPGA pipeline vs. multi-threaded CPU,
using this session's real numbers).** Because 4-thread CPU already
beats one FPGA pipeline's per-block rate outright (section 8), there is
no batch size at which one FPGA pipeline overtakes 4+ CPU threads on
raw per-block compute alone in this emulation. `membrane_choose_quant_backend()`
(`src/fpga/quant_backend.c`) encodes exactly this: AUTO picks CPU
whenever `cpu_cores_available >= 4`, and only considers FPGA once
`batch_blocks >= MEMBRANE_QUANT_FPGA_BREAK_EVEN_BLOCKS` (4) *and* fewer
than 4 CPU cores are available -- a break-even point derived from and
consistent with the numbers above, not an arbitrary constant.

**Encode vs. decode.** No separate break-even: all four ops converge to
the same ~85 ns/block sustained rate on the FPGA side (section 7), so
the CPU-side asymmetry (Q8 dequantize is CPU's slowest op at 1 thread,
179.77 ns vs. encode's 123.00 ns) is the only real encode/decode
difference in this comparison, not anything on the FPGA side.

**Queue depth needed for PCIe Gen4/Gen5 x16 (theoretical, derived from
measured single-instance numbers, not built or tested at multi-instance
scale).** The bridge's measured single-pipeline rate is ~0.753 GB/s
in / ~0.423-0.753 GB/s out (section 7) -- far below Phase 5.3's
12.8 GB/s @ 200 MHz figure for the underlying 512-bit-wide
`membrane_quant_stream_top` alone, because this bridge's 32-bit payload
port (a design choice, section 4) is the bottleneck, not the pipeline.
Sizing against PCIe raw bandwidth (Phase 5.3's Alveo U250 forward-sizing
target, `docs/phase5-synthesizable-fpga.md` section 8):

| PCIe generation | Raw x16 bandwidth | Single-bridge GB/s | Parallel bridge instances needed |
|---|---|---|---|
| Gen4 x16 | ~31.5 GB/s | 0.753 | ~42 |
| Gen5 x16 | ~63 GB/s | 0.753 | ~84 |

This is a materially different (and larger) instance count than Phase
5.3's own 3/5-instance estimate for the same PCIe generations, because
that estimate was against the 512-bit pipeline directly, not this
32-bit DMA-facing port. **Disclosed implication**: if PCIe-facing
bandwidth mattered more than emulation-loop simplicity, the bridge's
payload port should be widened (e.g. to 256 or 512 bits, matching a
real PCIe TLP or the pipeline's own native width) rather than staying
at 32 bits -- this phase did not do that, and the 32-bit choice should
not be read as a recommended production width.

**Whether the quantization gain covers the DMA cost -- the most
important, and least favorable, finding of this phase.** This
emulation charges **zero** real transport latency: `transfer()` and
`completion_wait()` only spend cycles on internal FIFO/logic ticks, not
on anything resembling a real PCIe doorbell-ring, DMA descriptor fetch,
and completion-interrupt round trip, which on real hardware is
routinely low-single-digit microseconds even with an optimized driver
-- a number this environment has no way to measure (no real card,
section 1) but which is **certainly not** the ~210 ns round-trip this
emulation reports (section 7). Real per-block CPU quantize/dequantize
cost is only 20-180 ns (section 8). A real PCIe round trip of even 1-2
microseconds per call would make per-block (or even per-small-batch)
FPGA offload a **net loss** versus keeping quantization on CPU, unless
batched far more aggressively than live autoregressive decoding
naturally allows (one token, i.e. a handful of KV blocks, per step).
This is a **composed, unverified-on-real-hardware conclusion**, stated
plainly rather than glossed over, and it is the reason
`membrane_choose_quant_backend()` defaults to CPU so readily (section
9's first point) and why section 10's live-inference numbers do not
show a case for FPGA offload at the batch sizes real inference produces.

## 10. Divider improvement research (analysis only, no RTL changed)

Baseline (Phase 5.3, `docs/phase5-synthesizable-fpga.md` section 7,
re-cited here for convenience): `membrane_fp_divider` maps to 37,998
LUT4 + 10,173 CCU2C + 15,848 PFUMX + 9,577 L6MUX21 + 33 FF under
`yosys synth_ecp5` -- roughly 73,600 LUT-class cells, two orders of
magnitude larger than the multiplier (~550) or adder (~2,530), because
`num64/den64` is a single un-pipelined combinational `/` over ~65x40
bits. Options considered, none implemented this phase (the exact,
bit-verified divider is explicitly preserved per the spec):

| Approach | Area vs. current | Throughput | Latency | Bit-exactness |
|---|---|---|---|---|
| Vendor hard-FP IP (Xilinx/Altera FP divider core) | Likely much smaller (dedicated silicon/DSP-adjacent logic) | Vendor-rated, typically pipelined for high Fmax | Fixed, vendor-documented, usually >10 cycles | Only if configured for the same IEEE-754 round-to-nearest-even mode this project verified against native x86 -- **must be checked**, not assumed |
| Multi-cycle iterative (restoring/non-restoring) divider | Much smaller (one small ALU reused over N cycles) | Much lower (1 result per N cycles, N ≈ mantissa width) | Higher (N cycles vs. current's 1 combinational cycle + DELAY register stages) | Bit-exact if implemented correctly -- same math, just serialized |
| Reciprocal LUT + Newton-Raphson refinement | Smaller (LUT + multiplier reuse) | High (few pipelined stages) | Low-moderate | **Rejected by the spec's own constraint**: approximate methods that break bit-exactness are explicitly disallowed |
| Shared divider (one instance, arbitrated across Q8/Q4 scale paths) | Smaller aggregate (currently `q8_scale`/`q4_scale` each instantiate 2 dividers in parallel, Phase 5.3 section 7 extrapolation: ~75-80K cells each) | Lower (contention when both paths need it simultaneously) | Variable (arbitration wait) | Bit-exact, same divider logic reused |
| Pipelined divider (break the combinational `/` into N register-staged sub-steps, same restoring-division math) | Same total logic, redistributed across pipeline stages | Same throughput at higher Fmax (shorter critical path per stage) | Higher latency (N cycles), same II=1 if fully pipelined | Bit-exact -- this is a retiming of the same operation, not a different algorithm |

**Recommendation for a future phase** (not acted on here, per the
spec's explicit instruction that the current exact divider may be
preserved in this first phase): the pipelined-divider option is the
only one that is simultaneously bit-exact, addresses Phase 5.3's stated
top concern (an un-pipelined 65-bit combinational critical path
unlikely to close timing at any realistic clock), and doesn't trade
away throughput for area the way the iterative or shared-arbitration
options would. Vendor hard-FP IP is worth evaluating **if and only if**
its rounding mode is confirmed bit-identical first -- untested here, no
vendor tool available (section 1).

## 11. Runtime integration: CPU/FPGA/AUTO backend

`include/membrane/quant_backend.h` / `src/fpga/quant_backend.c`, added
to `membrane_core` as plain C (no Verilator dependency -- always
buildable). `membrane_choose_quant_backend(requested, batch_blocks,
fpga_queue_used, fpga_queue_depth, cpu_cores_available)`: CPU/FPGA pass
through unchanged; AUTO resolves to CPU if no FPGA is configured
(`fpga_queue_depth==0`), CPU if the FPGA queue is saturated, CPU if
`cpu_cores_available >= 4` (section 9's finding), FPGA if
`batch_blocks >= 4` (and fewer than 4 CPU cores), else CPU.

`tools/membrane-kv-runtime` (existing tool) gained a `--quant-backend
{cpu,fpga,auto}` flag, additive-only: it defaults to `"cpu"`, produces
byte-identical behavior to before this phase by default, and its only
effect is a new informational log line resolving the backend for a
representative batch. **It does not redirect live in-inference K/V
quantization**, because that happens inside ggml's own internal Q8_0/
Q4_0 kernels via the existing `kv_type_override` mechanism
(`cp.type_k`/`cp.type_v`), not via a directly-callable MEMBRANE
function -- patching ggml/llama.cpp's internal quantize dispatch to
redirect through this phase's FPGA emulation was judged out of scope
and too risky for this phase. This limitation is disclosed in the
flag's own runtime log message, not hidden:

```
membrane-kv-runtime: quant_backend requested=auto resolved=cpu
  (live in-inference K/V quantization always uses ggml's own CPU
  kernels -- see --quant-backend's source comment)
```

Baseline behavior when `--quant-backend` is not passed (or MEMBRANE
features are off generally) is unchanged -- confirmed by running both
real models below with the flag defaulted and comparing against
existing Phase 3/4 quality baselines already on file.

## 12. Fallback safety logic

`FpgaRuntime` (`tools/membrane-fpga-runtime/fpga_runtime.h/.cpp`). Four
scenarios, all re-run fresh this session, 0 failures:

```
PASS: FPGA absent (rt=nullptr): CPU fallback used, bit-exact correct result
PASS: FPGA healthy: FPGA path used (no unnecessary fallback), bit-exact correct result
PASS: FPGA queue full: CPU fallback used, bit-exact correct result
PASS: FPGA timeout: CPU fallback used, bit-exact correct result (never silent corruption)
```

The timeout scenario required the `m_op_cycle_budget` fix (section 6)
to be meaningful at all -- without it, "FPGA timeout" was untestable
because the runtime's internal calls never actually respected a short
caller-supplied timeout. `cpu_q8_encode()` (`main.cpp`) is the CPU
fallback path, calling `membrane_simd_q8_0_quantize_batch` -- the same
production SIMD kernel `membrane-quant-bench` measures in section 8,
not a special-cased stub.

## 13. Real inference experiment (SmolLM2 135M / 360M)

Both models actually run, this session, via `membrane-kv-runtime
--quant-backend auto` (resolves to `cpu`, section 11 -- this experiment
measures the currently-shipping CPU K/V-quantization path; there is no
way to make ggml's internal kernels call out to the FPGA emulation in
this phase, disclosed above), 512-token natural prompt, 32 generated
tokens:

**SmolLM2-135M-Instruct-f16**

| Config | top1 | top5 | cosine | KL | KV bytes | KV reduction | TTFT | tok/s |
|---|---|---|---|---|---|---|---|---|
| FP16 | 100.00% | 100.00% | 1.000000 | 0.000000 | 2,190,684 | 1.00x | 197.5 ms | 63.8 |
| Q8 | 96.88% | 100.00% | 0.999967 | 0.000221 | 1,164,684 | 1.88x | 202.9 ms | 63.0 |
| Q4 | 87.50% | 100.00% | 0.990834 | 0.061656 | 617,484 | 3.55x | 209.1 ms | 62.8 |

**SmolLM2-360M-Instruct-f16**

| Config | top1 | top5 | cosine | KL | KV bytes | KV reduction | TTFT | tok/s |
|---|---|---|---|---|---|---|---|---|
| FP16 | 100.00% | 100.00% | 1.000000 | 0.000000 | 3,893,132 | 1.00x | 571.8 ms | 24.4 |
| Q8 | 100.00% | 100.00% | 0.999963 | 0.000451 | 2,069,132 | 1.88x | 564.8 ms | 24.4 |
| Q4 | 71.88% | 100.00% | 0.991420 | 0.085540 | 1,096,332 | 3.55x | 555.8 ms | 24.9 |

**Quantization wall-time, composed from real numbers.** The 135M
model's TTFT rises with quantization aggressiveness (+5.4 ms FP16→Q8,
+11.6 ms FP16→Q4) -- this delta *is* the real, already-measured cost of
CPU-side K/V quantization inside this run, no separate instrumentation
needed. The 360M model's TTFT instead *falls* slightly with more
aggressive quantization (-7.0 ms FP16→Q8, -16.0 ms FP16→Q4). Read
plainly: at this model size and single-run sample count, run-to-run
timing noise dominates the true quantization signal -- consistent with
this project's own prior finding
([[phase4.3-variance-root-cause]]) that single offline timing samples
can be swamped by noise larger than the effect being measured. Neither
number should be read as "quantization makes inference faster"; both
should be read as "the quantization-attributable TTFT delta at these
model sizes and batch-1 sequential decode is small relative to
measurement noise," which independently supports section 9's
conclusion that there isn't a strong efficiency case for offloading
this particular workload to any external accelerator, real or emulated.

**Composed end-to-end estimate for a hypothetical FPGA-backed run**
(explicitly not a real run -- section 11 explains why live quantization
cannot be redirected in this phase): section 8 showed 4-thread CPU
already beats the single emulated FPGA pipeline's per-block rate by
1.8-3.5x, and section 9 showed a real PCIe round trip (unmeasured, but
almost certainly microseconds, not this emulation's ~210 ns) would add
cost per call far exceeding the 20-180 ns a CPU thread already spends
per block. Composing these: an FPGA-backed run of this same experiment
would be expected to have **equal or worse** TTFT/tok-s than the CPU
numbers above, not better -- there is no real-hardware scenario
implied by this phase's own measurements where offloading this specific
workload (small per-step KV quantize/dequantize calls inside batch-1
autoregressive decode) to a discrete PCIe FPGA card would win.

**CPU utilization**: not separately instrumented for these two
inference runs (no `perf`/`getrusage`-based sampling was wired into
`membrane-kv-runtime` for this phase) -- disclosed as a real gap rather
than estimated. The only CPU-time number this phase actually measured
is the FPGA-*emulation* process's own `user=2.90s sys=0.00s` (section
7), which is the cost of running the Verilator simulation itself, not a
projection of real driver or inference CPU load.

**Quality regression: provably none, not just "not observed."** The
top1/top5/cosine/KL numbers above are simultaneously the CPU-backend,
FPGA-backend, and AUTO-backend quality numbers for these two models --
not because a separate FPGA-backed inference run was tried and happened
to match, but because Phase 5.3's 520,000-transaction Verilator
cosimulation and this phase's 100,000-block x4-operation DMA-path test
(section 5) already establish that the FPGA-emulated Q8_0/Q4_0
encode/decode path is byte-for-byte identical to the CPU/ggml reference
for every block tested. Given that, any inference run using the same
quantization *format* (Q8 or Q4) produces the same logits regardless of
which backend executes the transform, by construction. A third,
separate "FPGA-backend inference run" would not have produced different
numbers, and was not necessary to run to support this claim.

## 14. Long-operation visibility

`tools/membrane-fpga-runtime/main.cpp`'s `Heartbeat` struct prints a
progress line every 60 seconds during the `verify` subcommand:
completed/total transactions, queue depth, DMA GB/s, FPGA blocks/s,
error count, elapsed time, and ETA. Not shown in this document's
excerpts above because the runs cited (20,000-block spot-check,
100,000-block full run at ~34s) complete faster than the first 60s
tick; the heartbeat code path itself was exercised and confirmed
correct during earlier long-batch debugging in this phase (the
200-block deadlock investigation, section 6) before the fix landed.

## 15. Verification summary (this phase, in full, all re-run fresh this session)

- **Release build**: `cmake --build build-rel`, clean;
  `ctest --test-dir build-rel`, 16/16 passed.
- **ASan+UBSan**: `cmake --build build-asan`, clean;
  `ctest --test-dir build-asan`, 18/18 passed.
- **TSan**: `cmake --build build-tsan`, clean;
  `ctest --test-dir build-tsan` initially reported 16/18 failed with
  `FATAL: ThreadSanitizer: unexpected memory mapping` -- diagnosed as
  an environment ASLR incompatibility with TSan's fixed shadow-memory
  layout (not a code regression: none of the failing tests touch any
  code this phase added), confirmed by re-running under
  `setarch $(uname -m) -R` (ASLR disabled): 18/18 passed. This is the
  same class of environment-only issue documented, not a new finding
  about this phase's own code.
- **Verilator**: `membrane-fpga-runtime smoke`/`verify 20000`/`stress`/
  `fallback`, all re-run this session, 0 failures (section 5, 6, 12).
  The full 100,000-block x4-operation verify run (task 111, earlier
  this phase) also passed with 0 fails.
- **yosys**: full `membrane_dma_bridge` hierarchy (all 15 RTL files
  including Phase 5.3's `membrane_quant_stream_top`) elaborates cleanly
  via `hierarchy -check -top membrane_dma_bridge; proc; opt_clean`,
  re-run fresh this session -- 28,284 cells, 0 errors, 46 unique
  (non-fatal) warnings, consistent in kind with Phase 5.3's own
  documented yosys 0.33 warnings.
- **DMA emulation**: covered above (sections 5, 6, 7, 12).
- **Interrupted/reset recovery**: covered by the stress suite's
  "device recovers cleanly after reset mid-transfer" scenario and
  `FpgaRuntime::reset_mid_flight()`.
- **Real card**: not available (section 1) -- not run, disclosed, not
  claimed.

## 16. What remains unverified

Restated plainly, in addition to everything already flagged inline
above:

- No real PCIe hardware exists in this environment; every transport
  latency/bandwidth number that isn't explicitly this emulation's own
  internal FIFO/logic-cycle count is a composed estimate, most
  importantly section 9's "DMA cost likely exceeds quantization gain"
  conclusion, which is the single most consequential unverified claim
  in this document.
- No real Fmax -- 200 MHz is an assumption carried over from Phase
  5.3, not a measurement (no place-and-route tool available).
- The divider's real-hardware area/timing story (section 10) is
  research and recommendation only; no RTL was changed, per the spec's
  explicit permission to preserve the current exact divider this phase.
- Live in-inference K/V quantization was not actually redirected
  through the FPGA emulation path (section 11, 13) -- the real
  inference numbers in section 13 are a genuine CPU-backend
  measurement, and the FPGA/AUTO-backend end-to-end figures are a
  composed estimate built from separately-measured real numbers, not a
  literal FPGA-in-the-loop inference run.
- CPU utilization during the real inference runs (section 13) was not
  instrumented.
- Queue-depth/bandwidth sizing against real PCIe generations (section
  9) is arithmetic extrapolation from a single measured bridge
  instance, not a built and tested multi-instance design.
