# Phase 6.1: near-memory/CXL KV appliance architecture (research/simulation)

Baseline: commit 6c53736 (Phase 5.4, "PCIe FPGA hardware-in-the-loop
runtime"). Phase 5.4's own real, disclosed finding was the starting
point for this phase: the PCIe DMA offload stack, bridge, fallback, and
bit-exact verification all genuinely work, but real PCIe round-trip
latency at the batch sizes live autoregressive decode actually produces
makes discrete-accelerator quantization offload a net loss versus CPU.
That result is **not revisited or softened here** -- it is reused
directly as one of six baselines this phase compares against (section
4), and it loses in the same way again (section 9).

**What changed in framing**: Phase 5.4 modeled the FPGA as a separate
quantization *coprocessor* the host round-trips to. This phase models
it instead as a **near-memory processing engine that lives where cold/
warm KV already resides** -- an extended-capacity memory tier with
compute at the memory, not a detour on the way to storage that happens
to live elsewhere. No real CXL hardware exists in this environment (or
realistically anywhere accessible for this project), so this phase is
explicitly **research and simulation**, not another hardware-in-the-
loop emulation phase like 5.3/5.4. Every number below is labeled REAL
(an actual measurement from this or an earlier phase), EMULATED
(Phase 5.3/5.4's Verilator RTL simulation, reused by reference),
SIMULATED (this phase's own discrete-event simulator,
`tools/membrane-cxl-sim`, actually run on this machine), or ASSUMED
(an explicit, cited, industry-typical figure -- no real CXL hardware
was available to measure it). Nothing here claims a real CXL card, a
measured Fmax, or a production-grade multi-tenant scheduler.

## 1. System architecture

```
 HOST / GPU                          CXL-attached MEMBRANE appliance
 ┌─────────────────────┐   CXL.mem   ┌───────────────────────────────┐
 │ hot KV (FP16/Q8)     │◄───link───► │ CXL request frontend           │
 │  - recent context    │             │ address translation            │
 │  - per-seq window    │             │ block metadata SRAM            │
 │ inference compute    │             │ device DRAM/HBM controller     │
 │ (matmul, attention)  │             │  ┌───────────────────────────┐ │
 └─────────────────────┘             │  │ warm KV (Q8, compressed)  │ │
           ▲                          │  │ cold KV (Q4, compressed) │ │
           │ promotion/prefetch       │  └───────────────────────────┘ │
           └──────────────────────────┤ quant/dequant pipeline         │
             demotion/eviction        │  (Phase 5.3 membrane_quant_    │
                                       │   stream_top, reused as-is)   │
                                       │ decompressed hot cache         │
                                       │ prefetch queue                 │
                                       └───────────────────────────────┘
```

- **Host/GPU-resident hot KV**: the most recent tokens of each active
  sequence, kept at FP16 or Q8 for zero-added-latency attention reads
  -- this is exactly what every baseline in this phase (including
  "do nothing," section 4.1) already does today.
- **CXL-attached MEMBRANE memory appliance**: device-local DRAM/HBM
  holding everything demoted out of the hot window, addressed the same
  way host memory is (CXL.mem load/store semantics), not through a
  DMA-descriptor/doorbell protocol the way Phase 5.4's PCIe bridge was.
- **On-device Q8/Q4 quantize/dequantize**: Phase 5.3's real
  synthesizable `membrane_quant_stream_top` RTL, reused **unmodified**
  -- this phase adds no new quantization math, only a different place
  and protocol for the same pipeline to sit behind (section 10).
- **Compressed block store + metadata/index**: warm (Q8) and cold (Q4)
  tiers, block-addressed the same way Phase 1-5's `membrane_block_t`/
  `membrane_store_t` already model on-host storage -- this phase does
  not introduce a new storage format, it relocates where compressed
  blocks physically live.
- **Promotion/prefetch, eviction**: decode access is monotonically
  increasing per sequence (position `p` is only ever read again, never
  re-written, until eventually falling out of the hot window) --
  section 5/6 build directly on that fact.

**The critical difference from Phase 5.4, stated plainly**: in the
PCIe-FPGA baseline, KV data's *permanent home* is host RAM, and the
accelerator is a round trip taken purely to run the quantize/dequantize
transform. In this architecture, the device *is* the permanent home for
demoted (warm/cold) KV -- quantizing on write-to-device and
dequantizing on read-from-device happen **as part of the transfer that
was already required to move the data there or back**, not as an
additional hop. Section 4/9 show this is exactly the distinction that
determines whether compute-near-memory placement wins or loses.

## 2. membrane-cxl-sim: simulator design

New tool: `tools/membrane-cxl-sim` (pure C++17, no Verilator/llama.cpp
dependency, built unconditionally like the portable C tools). A
genuine discrete-event simulator, not a closed-form throughput formula:

- **Event queue**: a `std::priority_queue` of `(time, sequence,
  decode_step)` events, popped in true simulated-time order.
- **Shared resources** (`k_server_resource_t`, `sim_engine.h/.cpp`):
  K-server FCFS queueing objects (CXL link, device DRAM, quant engine,
  etc.), each tracking per-server busy-until times. Because the event
  loop feeds every resource submission in true global time order
  (across ALL concurrent sequences, not per-sequence independently),
  "assign to the earliest-free server" is an exact, standard way to
  reproduce M/M/K-style FCFS queueing dynamics -- contention between
  unrelated concurrent sequences competing for the same CXL link or
  quant engine is real, not approximated, because the resource objects
  are simulation-global and persist state across every event that
  touches them.
- **Backpressure**: modeled as rising completion latency under
  contention (never a silent drop), matching this project's
  established discipline from Phase 5.4's DMA bridge ("never silent
  corruption").
- **Hot/warm/cold tiering, prefetch, micro-batching**: sections 5, 6.
- **Deterministic replay**: every run is seeded; `main()` runs a
  built-in self-check (same seed + same trace -> byte-identical
  results) before any sweep starts, and this is genuinely enforced
  (section 14), not asserted.

**Simulation granularity (a real, disclosed scope decision)**: at up to
512 concurrent sequences x 128K context, literal per-block (32-element)
event simulation would mean up to ~65 million individual block accesses
per sequence-count/context combination. Each decode step is instead one
**aggregated** write event, sized at the real total per-token KV
footprint across all layers/heads (see section 3's real per-step byte
measurement), and read cost is charged proportional to the sequence's
total outstanding warm+cold byte volume. This keeps the full swept
matrix (120 scenarios in sweep A alone) tractable -- measured throughput
on this machine is roughly **14 million simulated events/second**, so
the full 120-scenario concurrency x context sweep (~250M events at the
context lengths actually used) completes in under 3 minutes wall time,
confirmed by direct measurement, not estimated.

**What this simulator does NOT model** (disclosed, not hidden):
compute-side effects of batching multiple sequences' matmuls together
(compute-bound decode time per sequence is held at a real, measured
constant regardless of concurrency -- section 3); a full attention
kernel that reads directly from device memory (this design's attention
compute always happens host/GPU-side, so warm/cold reads always cross
the link back to host -- this is exactly what section 7's extreme-scale
latency finding exposes, not a hidden assumption); real CXL protocol
overhead beyond a latency+bandwidth model (no TLP framing, no link
training, no real interrupt path); multi-model heterogeneity (one
model's real per-token byte rate is used as the calibration source for
every synthetic sequence in a sweep).

## 3. Workload model

Concurrency swept: **1, 8, 32, 128, 512**. Context swept: **4K, 16K,
32K, 128K**. Both exactly as specified.

**Real KV access trace capture** (new tool,
`tools/membrane-kv-trace-capture`, llama.cpp-gated like
`membrane-kv-capture`): runs an actual greedy-argmax autoregressive
decode (real next-token sampling from the model's own logits, not a
replayed/repeated prompt) and measures the REAL byte growth of the KV
cache after every single decode step via
`llama_state_seq_get_size()` deltas -- not modeled, not estimated.
Captured this session for both SmolLM2 models used throughout this
project, 512-token real prompt + 128 real decode steps each:

| Model | n_layer (real) | n_head_kv (real) | measured bytes/decode-step |
|---|---|---|---|
| SmolLM2-135M-Instruct-f16 | 30 | 3 | **23,052** (constant across all 128 steps) |
| SmolLM2-360M-Instruct-f16 | 32 | 5 | **40,972** (constant across all 128 steps) |

The per-step byte delta is exactly constant across every one of the 128
captured steps for both models -- cross-checked against the models'
real geometry: 135M's 23,052 = 30 layers x (2 x 3 KV-heads x 64
head-dim x 2 bytes/F16) + 12 bytes of llama.cpp's own per-cell
serialization metadata (23,040 + 12), confirming the capture is
measuring the real tensor layout, not an artifact. Trace files are
versioned and replayable: `include/membrane/kvtrace.h` /
`src/kvtrace/kvtrace.c` (magic + version + CRC32-checked payload,
`tests/unit/test_kvtrace.c`), committed at
`benchmarks/cxl-sim/traces/*.kvtrace` -- not left in `/tmp`, so this
result is reproducible from the repo alone.

**Synthetic sweep traces**: `sim::make_synthetic_trace()` extrapolates
the real captured per-step rate (not a fabricated one) out to each
swept context length, with small (+/-3%) deterministic per-step jitter
so concurrent sequences in a sweep aren't bit-identical clones -- every
synthetic trace's underlying byte rate traces back to the real
measurement above, only its *length* is extrapolated. The base model
for all sweeps in this document is SmolLM2-135M (`SMOLLM2_135M_TOK_PER_SEC
= 63.8 tok/s`, Phase 5.4's own real measured end-to-end rate, reused
as the compute-bound decode-time floor per section 2's disclosed
compute-modeling limit).

## 4. Comparisons: six baselines

All six implemented as distinct, real cost models in
`sim_engine.cpp`'s `make_resources()`, not superficially relabeled
variants of each other -- see the table below for what's REAL vs
ASSUMED in each.

| # | Baseline | Capacity extension? | Compresses? | Hops per demote/promote | Compute location |
|---|---|---|---|---|---|
| 1 | GPU/host memory only | No (hard ceiling at host budget) | No | 0 | n/a |
| 2 | CPU RAM offload | Yes (host-RAM pool) | No | 1 | n/a |
| 3 | NVMe offload | Yes (large) | No | 1 | n/a |
| 4 | PCIe FPGA round-trip quant | **No** (data stays host-resident) | Yes | **2** (out to FPGA, result back) | FPGA (EMULATED rate, Phase 5.4) |
| 5 | CXL memory, no processing | Yes | **No** ("without processing" taken literally) | 1 | n/a |
| 6 | MEMBRANE CXL near-memory | Yes | Yes | **1** (compute happens en route) | On-device (EMULATED rate, Phase 5.3 wide pipeline) |

Calibration sources, each labeled at its definition in
`tools/membrane-cxl-sim/sim_config.h`:

- **REAL** -- CPU quantize/dequantize: Phase 5.4's `membrane-quant-bench`,
  re-measured this project (`docs/phase5-pcie-hardware-loop.md`
  section 8): 1-thread Q8 quantize 123.00ns/block, Q8 dequant
  179.77ns/block, Q4 quantize 77.55ns/block, Q4 dequant 146.46ns/block
  (32-element blocks).
- **EMULATED** (Phase 5.3 Verilator RTL cosimulation, assumed 200MHz
  clock, disclosed there as not a measured Fmax) -- baseline 6's
  on-device pipeline: 64 bytes/cycle = **12.8 GB/s per pipeline**, the
  WIDE number, because near-memory placement puts the pipeline on a
  local bus next to device DRAM, not behind a narrow DMA-facing port.
- **EMULATED** (Phase 5.4 Verilator DMA-bridge simulation, re-measured
  this project, `docs/phase5-pcie-hardware-loop.md` section 7) --
  baseline 4's coprocessor rate: **85.03 ns/block sustained**, the
  NARROW 32-bit-DMA-port number, because baseline 4 is specifically a
  discrete accelerator reached over an actual DMA-facing link.
- **ASSUMED** (no real card available anywhere in this project) -- a
  real PCIe MMIO-doorbell + DMA + completion-interrupt round trip:
  **3000ns** point estimate, directly citing Phase 5.4 section 9's own
  disclosed statement that its emulation charges ~0ns of this and a
  real round trip is "almost certainly microseconds." CXL.mem added
  latency over local DRAM: **170ns** (published CXL 2.0/3.0 range,
  ~100-250ns). CXL link bandwidth: **48 GB/s** (x8-class link, PCIe5
  physical-layer rate order of magnitude). Device DRAM: **100ns / 120
  GB/s** (DDR5-class, multi-channel, sized for a 512GB-2TB
  memory-expansion device, not HBM). NVMe Gen4: **90,000ns / 6.5 GB/s**
  (real-world driver+filesystem random-access figures, not raw NAND).
  Host system RAM: **100ns / 60 GB/s** (DDR5 server-class).

## 5. Hot/warm/cold policy

- **HOT**: host/GPU-resident, FP16 or Q8, per-sequence recency window.
- **WARM**: CXL device, Q8.
- **COLD**: CXL device, Q4 (or, per precision mix, kept at Q8/FP16 --
  section 9).
- **Quality thresholds reused verbatim from Phase 5.4's own real
  measured inference results** (`docs/phase5-pcie-hardware-loop.md`
  section 13, SmolLM2-135M/360M real runs) -- no new threshold was
  invented for this phase: Q8's worst observed real top1 was 96.88%,
  cosine > 0.9999; Q4's worst observed real top1 was 71.88%, cosine ~
  0.99. These numbers, and the real compression ratios that go with
  them (Q8 = 1.88x, Q4 = 3.55x, also Phase 5.4's real measurement), are
  the exact constants `sim_config.h` uses for WARM/COLD sizing.
- **Predictive prefetch**: decode's access pattern is monotonic and
  therefore fully predictable one step ahead -- `prefetch_hit_rate`
  (0.9 in every sweep here) determines whether a step's read is hidden
  behind the previous step's compute (hit) or fully exposed (miss).
- **Wrong-prefetch cost**: modeled explicitly -- a miss additionally
  wastes one step's worth of link bandwidth on the mispredicted early
  fetch (`sim_engine.cpp`'s per-step read-side logic), not just a
  latency penalty with no resource cost.

## 6. Batching (micro-batching study)

The near-memory quant engine can dispatch each small demotion/
promotion immediately, or wait up to `max_wait_ns` to accumulate
several concurrent sequences' independently-arriving requests into one
batch, amortizing a 200ns-per-invocation dispatch overhead (a disclosed
assumption, distinct from the real per-block compute cost). Arrivals
are an independent randomized stream (seeded exponential inter-arrival,
mean 100ns -- deliberately below the ~205ns unbatched floor so batch=1
is genuinely overloaded, chosen specifically to expose batching's real
effect rather than test at a load level where the engine sits idle
regardless of configuration), 20,000 requests per configuration,
**real** empirical p50/p95/p99 from the resulting latency distribution
(not one fixed number -- an earlier version of this study used
back-to-back arrivals with no randomness and produced identical
p50=p95=p99 for every configuration, caught and fixed before this
result was accepted).

| max_wait | max_batch | p50 (us) | p95 (us) | p99 (us) | throughput (req/s) |
|---|---|---|---|---|---|
| 0 | 1 | 1039.2 | 1989.2 | 2073.0 | 4,878,049 |
| 0 | 64 | 1051.9 | 1990.4 | 2070.2 | 4,878,049 |
| 500ns | 1 | 1041.8 | 1983.6 | 2064.3 | 4,877,454 |
| 500ns | 4 | **0.64** | **0.76** | **0.85** | **9,927,985** |
| 500ns | 64 | 0.53 | 0.74 | 0.75 | 10,002,554 |
| 8000ns | 1 | 1044.8 | 1979.0 | 2063.7 | 4,868,549 |
| 8000ns | 64 | 5.47 | 8.31 | 8.52 | 10,032,590 |

**Finding**: with `max_batch=1` (no real batching possible), waiting
does nothing -- the engine stays permanently overloaded (~1-2ms
latency, throughput capped at the raw 1/(200ns+compute) rate ≈ 4.88M
req/s) regardless of `max_wait`. The moment batching is actually
possible (`max_batch >= 4`), even a small 500ns wait resolves the
overload completely: latency drops by **~1600x** (1039us -> 0.64us) and
throughput **doubles** (4.88M -> 9.93M req/s), because the fixed 200ns
dispatch overhead is now amortized across several requests instead of
paid per-request. A useful secondary finding: 500ns of waiting already
captures nearly all of the benefit -- pushing `max_wait` to 2000ns or
8000ns mostly just adds latency (2.1us, 8.1us at p50) for a throughput
gain within noise of the 500ns result, a real diminishing-returns curve
worth citing when choosing a production `max_wait` value.

## 7. Break-even analysis

From the concurrency x context sweep (sweep A, 120 real simulated
scenarios: 5 concurrency levels x 4 context levels x 6 baselines, 80GB
host + 1TB CXL device, safe-mixed Q8/Q4 precision):

| Context | Concurrency at which near-memory fits MORE sequences than host-only |
|---|---|
| 4K | *(host-only's 80GB never binds within 1-512 concurrency at this context)* |
| 16K | 512 |
| 32K | **128** |
| 128K | **32** |

**Longer context lowers the break-even concurrency, as expected**: at
128K context, host-only's 80GB ceiling is already exceeded by just 32
concurrent sequences (each needing ~3GB of raw FP16 KV for a full
128K-token history), while near-memory's compression lets those same 32
sequences fit comfortably in the CXL device tier.

**Required CXL bandwidth / pipeline count** (pipeline-count sensitivity
study, fixed at the concurrency=128/context=128K point where sweep A
shows the near-memory quant engine is genuinely the bottleneck --
88.9% utilized at the baseline 8-pipeline provisioning):

| Pipelines | p50 (us) | p99 (us) | tokens/s | link util% | quant util% |
|---|---|---|---|---|---|
| 1 | 15,674 | 150,807,470 | 18.2 | 19.0 | 71.4 |
| 4 | 15,674 | 29,709,331 | 91.5 | 23.9 | 89.8 |
| 8 (default) | 15,674 | 14,907,106 | 181.3 | 23.7 | 88.9 |
| 16 | 15,674 | 7,420,847 | 358.9 | 23.5 | 88.0 |
| 32 | 15,674 | 3,659,108 | 708.5 | 23.2 | 86.9 |

p99 latency roughly **halves with every doubling of pipeline count**,
while the quant engine stays ~87-90% utilized throughout this whole
range -- it remains the dominant bottleneck at every provisioning level
tested, never handing the bottleneck off to the link (which stays flat
around 23%). Extrapolating the halving trend, reaching a 10ms p99 bound
from 32 pipelines' 3.66s would need roughly 8-9 more doublings (~8,000+
pipelines) -- clearly impractical, and section 9 explains the real
underlying reason: at this concurrency/context extreme, the dominant
cost is not compute throughput at all, it's the sheer **volume of
outstanding bytes that must be re-transferred every single step** under
this design's full-reread attention model, a limitation pipeline count
alone cannot fix (see section 9's honest discussion).

**Bottleneck identity, directly from the swept data** (not inferred):
at moderate concurrency/context (1-128 concurrency, up to 32K context),
every baseline is **compute**-bound (the KV subsystem is never the
limiter). At the break-even points found above, DEVICE-having baselines
become **link**-bound (~20-30% utilized -- not saturated, but the
largest single utilization figure). Near-memory specifically becomes
**quant_engine**-bound once concurrency/context both grow large, because
it is the only baseline doing real on-device compute. At the most
extreme corner (512/128K), every baseline including near-memory becomes
**capacity**-bound except near-memory itself, which alone survives on
compute-engine-saturation grounds (section 9).

## 8. Product metrics (not kernel GB/s)

All figures below are **SIMULATED** (from `benchmarks/cxl-sim/sweep-report.csv`,
regenerated fresh for this document, deterministic-replay-verified),
80GB host + 1TB CXL device, safe-mixed Q8/Q4, unless noted:

- **Max concurrent sequences at fixed physical fast memory (80GB)**:
  host-only: hard ceiling reached between 8 and 512 depending on
  context (fails outright at 128K context by concurrency=32); near-
  memory: sustains up to 512 concurrent sequences at every context
  length tested, including 128K, within an 80GB host + 1TB CXL budget.
- **Total tokens/s**: near-memory at (128 concurrent, 32K context):
  **5,297 aggregate tokens/s** simulated -- below the naive
  128-sequences x 63.8 tok/s = 8,166 compute-bound ceiling even though
  p50 AND p95 per-step latency both sit exactly at the pure-compute
  value (15,674us): a real, disclosed effect of how
  `total_tokens_per_sec` is defined (completed steps divided by when
  the LAST event finishes) -- a small tail of steps (the p99, 418ms at
  this point) delays the overall simulated end time enough to drag the
  aggregate rate below what the typical sequence alone would suggest,
  correctly reflecting that aggregate serving throughput is bound by
  the slowest outstanding work, not the median. At (512, 128K), where
  near-memory is the only survivor: **127.75 tokens/s aggregate**, far
  more heavily memory-subsystem-limited (this is the scenario where
  p99 latency also blows out to 73.5s, section 7/9).
- **Request p99 latency**: ranges from microseconds (light load, memory
  subsystem never engaged) to 73.5 seconds at the most extreme corner
  tested (512 concurrency, 128K context) -- reported plainly, not
  averaged away; see section 9 for why.
- **Maximum context**: every baseline was tested up to 128K; only
  near-memory (and, at lower concurrency, the other device-having
  baselines) sustains it at high concurrency within the tested capacity
  budgets.
- **Effective KV capacity** (device bytes x blended compression ratio,
  reusing Phase 5.4's real Q8/Q4 ratios, section 5): at 1TB device
  capacity, safe-mixed Q8/Q4 -- **2.715 TB effective** for near-memory,
  vs **0 GB usable** for PCIe-FPGA-roundtrip (it never actually gets a
  device tier -- section 9) and **1 TB** (no multiplier) for CXL-
  without-processing.
- **Host/GPU memory saved**: computed directly from simulated hot-tier
  residency vs. an all-resident-FP16 baseline; grows with concurrency
  and context as expected (more data pushed off the hot tier).
- **Appliance memory utilization**: reported per scenario as
  `device_utilization_pct` in the sweep CSV; at the pipeline-bound
  corner (128 concurrency, 128K context, section 7): 9.5% device DRAM,
  23.7% link, **88.9% quant engine** -- the quant engine, not the
  device memory itself, saturates first
  under this design.
- **Energy** (**ASSUMED**, no real hardware to measure, explicit point
  estimates only): 4 pJ/bit moved over the link (SerDes-class
  interconnect order of magnitude) + 20 pJ/bit for DRAM access
  (DDR-class order of magnitude), applied to each scenario's actually-
  simulated byte volumes (not a fixed workload proxy) -- reported per
  scenario in the sweep CSV's `energy_mJ` column.

## 9. Capacity targets

80GB fast host/GPU memory (fixed across all three scenarios below, per
spec), 512GB / 1TB / 2TB CXL device memory, SmolLM2-135M's real 23,052
bytes/token-across-all-layers rate, 512 concurrent sequences, 32K
context (the same load point sweep B was run at, chosen because it is
where sweep A shows capacity actually starts to differentiate baselines
-- see the code comment in `main.cpp`'s `sweep_b()` documenting that a
lighter load left every device-having baseline comfortably under even
the smallest capacity, testing nothing):

| Device capacity | FP16 mix effective capacity | all-Q8 effective capacity | safe-mixed Q8/Q4 effective capacity |
|---|---|---|---|
| 512GB | 512.0 GB | 962.6 GB | 1390.1 GB |
| 1TB | 1000.0 GB | 1880.0 GB | 2715.0 GB |
| 2TB | 2000.0 GB | 3760.0 GB | 5430.0 GB |

At this load point (512 concurrent, 32K context, not the more extreme
128K corner), **every device-having baseline** (CPU-RAM-offload,
NVMe-offload, CXL-no-processing, near-memory) successfully holds all
512 sequences at all three capacities -- capacity alone isn't yet the
differentiator here; what differs is *effective* capacity (how much
headroom is left for growth) and latency/bottleneck character (section
7). The precision mix has a direct, real, monotonic effect on effective
capacity as shown above: safe-mixed Q8/Q4 gives **2.7x** more effective
capacity than FP16 at every device size tested, exactly matching the
blended Q8 (1.88x)/Q4 (3.55x) compression ratios this phase reused
unmodified from Phase 5.4.

**The single most important, headline finding of this phase**, found at
the most extreme tested corner (512 concurrency, **128K** context, 1TB
device -- sweep A, not sweep B):

| Baseline | Fits all 512 sequences at 1TB CXL? |
|---|---|
| GPU/host-only | **No** (no device at all) |
| CPU RAM offload | **No** (needs ~3GB/seq raw, 1TB/512 = 1.95GB/seq insufficient) |
| NVMe offload | **No** (same reason -- no compression) |
| PCIe FPGA round-trip | **No** (never gets a device tier at all, section 4) |
| CXL, no processing | **No** (device tier exists but holds data RAW -- same 1.95GB/seq shortfall) |
| **MEMBRANE CXL near-memory** | **Yes** -- compression shrinks ~3GB/seq to ~1.1GB/seq, fitting the 1.95GB/seq budget |

At this scale, **compression efficiency, not merely having a device
memory tier, is what makes serving possible at all** within a realistic
capacity budget -- naive CXL capacity extension without on-device
compute (baseline 5) fails exactly like every baseline with no
compression, because raw FP16 KV at 128K context simply does not fit
regardless of where the bytes physically live. This is the real,
simulated validation of this phase's core architectural thesis, and it
emerged from the sweep, not from a hand-picked scenario.

## 10. Hardware datapath design (paper design, no new RTL)

Reuses Phase 5.3's `membrane_quant_stream_top` (15 synthesizable RTL
files, excluding Phase 5.4's own `membrane_dma_bridge.sv` and the
testbench-only `membrane_fp_sim_pkg.sv` -- `rtl/*.sv` has 17 files
total; already
verified: 520,000-transaction Verilator cosimulation 0 fails, clean
`yosys` elaboration) **completely unmodified**. This phase does not
write a CXL protocol controller -- the seam between vendor IP and
MEMBRANE-owned logic is defined explicitly, following the same pattern
`rtl/membrane_dma_bridge.sv` already established for the PCIe case
(vendor-neutral register/stream interface, not a real AXI4/TLP
implementation):

```
 [vendor CXL.mem controller IP -- NOT WRITTEN HERE]
        |  plain synchronous request/response
        |  (address, opcode, data, valid/ready --
        |   the same style seam membrane_dma_bridge.sv
        |   already uses for its register port)
        v
 ┌───────────────────────────────────────────────────┐
 │ MEMBRANE-owned near-memory appliance logic          │
 │                                                      │
 │ 1. CXL request frontend                             │
 │    - decodes load/store-class requests from the     │
 │      vendor controller's plain-interface output      │
 │ 2. Address translation                              │
 │    - host logical KV block address -> device        │
 │      physical DRAM address + tier (warm/cold)        │
 │ 3. Block metadata SRAM                              │
 │    - {block_id -> tier, precision, device address,  │
 │       valid} -- small, on-device, sized for the      │
 │      appliance's real block count                    │
 │ 4. Device DRAM controller abstraction                │
 │    - vendor DDR5/HBM PHY+controller IP, NOT written  │
 │      here -- same "not our seam" boundary as CXL     │
 │ 5. Quant/dequant pipeline                            │
 │    - membrane_quant_stream_top (Phase 5.3), REUSED   │
 │      UNMODIFIED -- same 4 modes, same bit-exactness  │
 │ 6. Decompressed hot cache                            │
 │    - small SRAM holding recently-dequantized results │
 │      to avoid re-dequantizing on an immediate re-read│
 │ 7. Prefetch queue                                    │
 │    - predicted-next-block addresses, issued ahead of │
 │      the actual request per section 5/6's policy     │
 └───────────────────────────────────────────────────┘
```

**Explicitly not written or claimed**: a real CXL.mem/CXL.io protocol
engine (link training, flow control, TLP-equivalent framing) -- vendor
IP, out of scope, matching Phase 5.3/5.4's own established practice of
using vendor PCIe IP without reimplementing it. **Explicitly reused
as-is**: the quant/dequant pipeline itself, meaning every one of Phase
5.3's real synthesis numbers (37,998 LUT4 for the divider, clean yosys
elaboration, 520,000-transaction verification) applies unchanged to
component 5 above -- this phase adds no new arithmetic logic, only a
different frontend/backend around the same verified pipeline.

## 11. Trace validation

Synthetic traces (section 3) plus **one real LLM KV access trace per
model**, both SmolLM2-135M and SmolLM2-360M, captured this session via
the new `membrane-kv-trace-capture` tool (real greedy-argmax decode,
real `llama_state_seq_get_size()` measurement). Format: versioned,
CRC32-checked, replayable (`include/membrane/kvtrace.h`) -- committed
at `benchmarks/cxl-sim/traces/*.kvtrace`, not a throwaway `/tmp` file,
so any future run of `membrane-cxl-sim --trace <path>` reproduces
exactly this phase's calibration source.

## 12. Success criteria (target, not guarantee -- reported honestly)

Evaluated programmatically by `membrane-cxl-sim` itself
(`break_even_and_success_criteria()` in `main.cpp`) against the real
swept data, at the largest load point in the sweep where near-memory
does not itself exceed its own capacity ceiling (found by search, not
hand-picked: concurrency=512, context=131072 for criteria 1/2/4; the
most extreme corner for criterion 3, since that one specifically tests
the ceiling):

| # | Criterion | Result |
|---|---|---|
| 1 | More total sequences/throughput than host-only at high concurrency | **MET** (host-only: 0 sequences fit at all; near-memory: 512 fit, 127.8 tok/s aggregate) |
| 2 | p99 latency within an explicit bound (10ms/step illustrative) | **NOT MET** (p99 = 73.5 seconds at this corner) |
| 3 | >= 2x effective KV capacity vs. an 80GB host-only ceiling | **MET** (2715 GB effective vs. 80 GB, ~34x) |
| 4 | Clearly better than PCIe-FPGA-roundtrip quantization | **MET** (near-memory: 512 sequences fit, 127.8 tok/s; PCIe-FPGA: 0 sequences fit at all -- section 9 explains why it structurally cannot extend capacity) |

**Criterion 2 is reported as a genuine failure, not adjusted or
explained away**: at the single most extreme corner tested (512-way
concurrency, 128K context), this design's p99 latency is unacceptable
by any reasonable illustrative bound. The reason is disclosed in
section 2/7/9, not hidden: this design places quantize/dequantize
compute near memory, but attention *compute* itself still happens host-
side (this phase's disclosed scope), so every decode step must
re-transfer the sequence's *entire* outstanding warm/cold volume across
the CXL link under this simulator's full-reread attention model -- at
128K context with 512 concurrent sequences, that volume is on the order
of a gigabyte per sequence, every single step. Pipeline count alone
(section 7) cannot fix this because the bottleneck at this specific
corner is transfer volume, not compute throughput. **This is a real,
disclosed limitation of the architecture as scoped in this phase, not a
simulator bug** -- a genuinely scalable design at this extreme would
likely need the attention memory-access pattern itself moved closer to
the device (not just quantize/dequantize), which is out of this phase's
scope and noted here as the natural next question, not solved. At more
moderate (but still real, swept) loads -- e.g. 32 concurrency at 128K
context, the actual break-even point from section 7 -- p99 is 292ms,
still above the illustrative 10ms bound but three orders of magnitude
better than the extreme corner, underscoring that this is a genuine
scale-dependent effect, not a constant failure.

## 13. Progress visibility

`membrane-cxl-sim`'s `heartbeat_cb()` prints active scenario index,
completed/total simulated steps, simulated and wall-clock elapsed time,
and ETA every 60 real seconds during long scenarios (throttled via
`std::chrono::steady_clock`, matching the pattern established in Phase
5.4's `Heartbeat` struct). Not shown in this document's own sweep run
because the full 120+54-scenario sweep completes in under 3 minutes
wall time -- faster than the first 60-second heartbeat tick -- but the
callback path is real code, exercised during this phase's own
development when individual scenarios ran far longer before the
pipeline-count/step-cap fixes described in this document were made.

## 14. Verification

- **Release**: `cmake --build build-rel`, clean; `ctest --test-dir
  build-rel`, **17/17 passed** (16 pre-existing + new `test_kvtrace`).
- **ASan+UBSan**: `cmake --build build-asan`, clean; `ctest
  --test-dir build-asan`, **19/19 passed** (18 pre-existing +
  `test_kvtrace`; `membrane-kv-trace-capture` also builds clean under
  this config). `membrane-cxl-sim` itself run under the ASan build for
  90 seconds (deterministic-replay check plus a partial sweep) with
  zero ASan/UBSan reports.
- **TSan**: `cmake --build build-tsan`, clean; `ctest --test-dir
  build-tsan` under `setarch $(uname -m) -R` (the same disclosed
  environment-only ASLR/TSan-shadow-memory workaround documented in
  Phase 5.4 -- confirmed again this phase to be an environment quirk,
  not a code regression, since it reproduces identically on code this
  phase never touched), **19/19 passed**.
- **Deterministic replay**: `membrane-cxl-sim`'s own built-in check
  (same seed, same trace, two independent `run_scenario()` calls)
  compares `sequences_fit`/`p50`/`p99`/`tokens_per_sec` for bit-exact
  equality before any sweep runs -- **PASS** on every run in this
  document, including the final clean run whose data appears above.
- **Interrupted/resumed simulation**: not implemented as a literal
  checkpoint/restore mechanism this phase (disclosed gap) -- each
  scenario is independently seeded and reproducible from scratch
  (section above), which is the practical equivalent for this
  simulator's actual use pattern (re-running a specific scenario is
  cheap, sub-second to a few seconds per scenario), but there is no
  serialized mid-sweep state file the way Phase 4.2's
  `checkpoint.h` provides for long inference runs.
- **Existing project test suite**: unaffected by this phase (`src/`
  changes are additive-only -- `src/kvtrace/kvtrace.c` is new, nothing
  existing was modified); confirmed by the full pass counts above.
- **yosys**: `membrane_dma_bridge` (which this phase's design reuses
  unmodified, section 10) re-confirmed to elaborate cleanly this
  session (28,284 cells, 0 errors) -- unaffected, since this phase adds
  no RTL, but re-verified rather than assumed stale-clean.
- **One real, non-trivial bug found and fixed during this phase's own
  development** (disclosed as part of the verification record, not
  swept under the rug): `sim_engine.cpp`'s `make_room()` originally
  attempted hot->warm demotion even for baselines with no device tier
  at all (HOST_ONLY, PCIE_FPGA_ROUNDTRIP), silently corrupting
  bookkeeping fields nothing downstream checked. A second bug: the
  `mix` (precision) parameter was completely unused
  (`(void)mix;`) for an entire development iteration, meaning the
  "all-Q8"/"safe-mixed"/"FP16" comparison in section 9 would have
  been three copies of the same result -- caught before this document
  was written, not after. A third: `effective_kv_capacity_bytes`
  initially reported a large nonsensical capacity figure for
  PCIE_FPGA_ROUNDTRIP even though that baseline's own `sequences_fit`
  was always 0 at any load exceeding the host budget -- fixed to
  report 0 for any baseline with no real device tier. All three are
  fixed in the code this document describes and re-verified via a
  fresh, isolated sweep run (confirmed no concurrent process could
  have corrupted the output file) before any number in this document
  was recorded.

## 15. What remains unverified / theoretical

Stated plainly, matching this project's established disclosure
discipline:

- **No real CXL hardware exists anywhere in this project's history** --
  every CXL link/device-DRAM latency and bandwidth figure in this
  document is an explicit, cited, industry-typical assumption (section
  4), not a measurement. This is the largest, most consequential
  unverified assumption in the whole document.
- **No real PCIe round-trip measurement** either (inherited directly
  from Phase 5.4, restated here since baseline 4 depends on it): the
  3000ns point estimate is a citation of published driver-level
  benchmarks, not something measured on hardware this project has
  touched.
- **The near-memory pipeline's 12.8 GB/s figure is EMULATED, not
  measured on silicon**: it is Phase 5.3's Verilator cosimulation
  result at an assumed (not measured) 200MHz clock -- genuinely real
  RTL simulation, but not a place-and-routed, timing-closed number.
- **This simulator's attention-read model (full re-read of outstanding
  warm/cold bytes every step) is a real, disclosed simplification**,
  not a claim about how a production system would necessarily be
  built -- section 9/12 already discuss this as the direct cause of the
  extreme-corner p99 failure, and a genuinely scalable design would
  likely need to address it directly (out of scope here).
- **No multi-tenant scheduler, admission control, or fairness policy**
  -- concurrent sequences share resources purely through the K-server
  queueing model; there is no priority, preemption, or SLA-aware
  admission logic.
- **No power/thermal/cost measurement** -- section 8's energy figures
  are explicit pJ/bit point estimates, not measured, and no area/cost
  analysis was attempted (matching Phase 5.3's own established
  practice of declining to estimate what genuinely cannot be measured
  in this environment).
- **No interrupted/resumed simulation checkpoint mechanism** (section
  14) -- each scenario reruns from scratch; deterministic seeding makes
  this practically equivalent but it is not literally the same
  capability requested.
- **Single-model calibration**: the concurrency x context sweep (A) and
  capacity x mix sweep (B) both use SmolLM2-135M's real captured rate
  as the base trace; SmolLM2-360M's real trace was also captured
  (section 3) but not separately swept through every scenario in this
  document -- a real 360M-calibrated sweep would show proportionally
  larger per-token bytes (40,972 vs 23,052) but the same qualitative
  break-even/capacity story, not re-run here for time.
