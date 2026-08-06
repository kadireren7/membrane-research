# Phase 6.5: out-of-core memory-bounded unified simulator

Baseline: commit 58fb9b5 (Phase 6.4, "research: validate unified 128K
concurrent KV workload"). Phase 6.4 shipped the unified 128K x 512
sweep but left
SmolLM2-360M at 46% (107/231) because a single model's fully
in-memory synthetic-extended trace (~2.1 GiB for SmolLM2-135M, ~3.7
GiB for SmolLM2-360M) left too little headroom on this machine's 5.6
GiB RAM, causing real, repeated kernel OOM kills. This phase's job:
replace that fully-resident design with a real chunked, bounded-memory
trace format and streaming reader, verify it produces identical
results to the old in-memory path, then use it to actually finish the
missing SmolLM2-360M scenarios and recover the lost 135M tail-sample
artifact -- under a declared, enforced memory budget, on the same
machine, without raising the RAM ceiling or shrinking the workload.

Labeling discipline, unchanged from Phase 6.1-6.4: **REAL** (an actual
measurement), **SIMULATED** (this project's own discrete-event
engines, actually run on this machine), **ASSUMED** (an explicit,
cited estimate -- no real CXL hardware exists anywhere in this
project).

## 1. Simulator correctness

### 1.1 Out-of-core trace format (.attntrace3)

`include/membrane/attntrace3.h` / `src/attntrace/attntrace3.c`. Keeps
v2's compact per-entry encoding (uint16 block_id + uint8 fixed-point
score, 3 bytes/entry) but splits the payload into independently
checksummed, independently (DEFLATE-)compressible chunks of
`--chunk-steps` decode-steps each:

```
[0, 256)                        fixed header (magic "AMT3", geometry,
                                 chunk_steps/chunk_count, index/payload
                                 offsets, file_sha256)
[index_offset, +index_size)     chunk_count fixed-size (32B) index
                                 entries, increasing step order
[payload_offset, EOF)           chunk payloads, same order, each with
                                 its own CRC32 (over the UNCOMPRESSED
                                 compact bytes) and optional DEFLATE
```

`header.file_sha256` covers `[index_offset, EOF)` -- the index and
every chunk payload -- computed via ONE bounded streaming pass
(`membrane_sha256_update` in fixed 64 KiB reads; the underlying SHA-256
was already internally incremental, just not previously exposed --
see `include/membrane/hash.h`'s `membrane_sha256_init/update/final`).
The whole point of this format is that no reader or writer ever needs
the full payload resident to check or produce it.

Tests: `tests/unit/test_attntrace3.c` -- multi-chunk round-trip
(compressed/uncompressed), out-of-order chunk-write rejection
(deterministic ordering enforced, not just conventional), one
corrupted chunk's CRC32 catching it WITHOUT disturbing any other
chunk's read, file-level SHA-256 catching index corruption that a
per-chunk CRC32 cannot see, oversized-block-id rejection, uneven last
chunk.

### 1.2 Streaming reader (`attn_trace_reader_t`)

`tools/membrane-kv-workingset-sim/attn_trace_reader.h/.cpp`. One
interface (`open/get_metadata/at/read_step_range/read_layer_range/
prefetch_chunk/release_chunk/set_prefetch_depth/close`), three
backends:

- `in_memory` -- wraps an already-resident `attn_trace_t` (parity
  baseline, small/test traces, or an explicit opt-out).
- `mmap` -- maps the whole file read-only, decodes a chunk straight
  from the mapped pointer on demand (no intermediate copy of the
  compressed bytes).
- `streaming` (**the default** -- see 2.3 for why) -- `fseek`/`fread`
  into a small buffer, no mmap.

A key simplification the real access pattern allows: the only real
consumer of the wide per-entry trace
(`wssim::run_scenario_calibration_impl`, see 1.3) walks decode steps
strictly sequentially, one at a time, needing every layer/head/top_k
entry of the CURRENT step only -- never revisits an earlier step. So
every backend only ever needs to keep a small, bounded number of
decoded chunks resident, never the whole trace.

**Bounded chunk cache** (`attn_trace_chunk_cache_t`, same file): LRU
(hashmap + intrusive list), refcounted pin/unpin so an in-use chunk
can't be evicted mid-read, a "loading" placeholder + condition
variable so two threads racing to decode the SAME missing chunk
collapse into ONE decode (`duplicate_loads_avoided` stat), hit/miss/
eviction/resident-bytes counters. **Shared across every worker thread
reading the same model's trace** (not per-thread -- a per-thread cache
would multiply the memory budget by worker count).

`--prefetch-depth N`: on every chunk-boundary crossing, warms the next
N chunks ahead of when the sequential walk will actually need them,
and drops any previously-warmed chunk that fell behind. Verified with
a real, measured effect (not asserted, measured):
`test_attn_trace_reader.cpp`'s
`test_prefetch_depth_turns_boundary_misses_into_hits` shows 0 cache
hits at chunk boundaries with depth=0 vs. 7 hits (out of 8 boundaries)
with depth=2, on the exact same walk.

### 1.3 One calibration loop, not two

`wssim::run_scenario_calibration_impl` (templated on a trace-view
struct + a generic `(step,layer,head) -> entry*` accessor,
`tools/membrane-kv-workingset-sim/engine.cpp`) is the ONE real
implementation of the 8-stage per-decode-step pipeline.
`run_scenario_calibration(const attn_trace_t&, ...)` (existing,
untouched signature/behavior) and `run_scenario_calibration_streamed
(attn_trace_reader_t&, ...)` (new) are both thin wrappers around it --
an out-of-core trace and a fully in-memory one run the literal same
compiled logic, not two maintained copies that could quietly drift
apart. `exactsim::calibrate()`/`calibrate_streamed()` mirror the same
split one level up.

### 1.4 Exact parity, verified

`test_attn_trace_reader.cpp` and `test_calibrate_streamed.cpp`: same
small/medium trace, in-memory vs. mmap vs. streaming backend,
bit-identical `scenario_result_t`/`calibrated_profile_t` output
(percentiles, queue stats, capacity stats, predictor precision/
recall, coalescing counts, layer/head hit rates) -- these are exact
selection/aggregation algorithms in every backend, not approximations,
so parity is bit-exact, not "close." One real, disclosed exception:
`extend_synthetic()`'s jitter could push a near-1.0 native score
slightly above 1.0 in the wide in-memory representation while any
on-disk encoding (v2 or v3) has always clamped scores to [0,1] before
quantizing -- fixed by clamping in the shared per-step generator
(`generate_synthetic_step`, `attn_workload.cpp`) so both paths agree
exactly, not just to within a tolerance. Confirmed harmless to every
reported metric: `score` is never read by the actual simulation logic
(`ground_truth_blocks` only extracts `block_id`; `regroup_to_block_size`
is the only score-reading code path and is unreachable in the unified
sweep, which never changes block size).

## 2. Memory optimization (real measurements)

All numbers below are from real `--audit-memory` runs against the
real, committed `SmolLM2-135M-Instruct-f16-long.attntrace` (8,847,360
entries) on this machine, sampling actual `/proc/self/status` VmRSS
before/after each real phase -- not sizeof-based estimates.

### 2.1 Synthetic trace generation: ~2.1 GiB -> ~14 MiB

| phase | RSS delta |
|---|---|
| load native capture (8.85M entries) | +69.3 MiB |
| stream-generate the 130,560-step synthetic trace to disk, one chunk resident at a time | **+13.9 MiB** |
| (for comparison: what the pre-6.5 fully in-memory `extend_synthetic()` would hold resident) | 2,151 MiB |

The synthetic-extension step is where Phase 6.4's OOM kills actually
came from (`extend_synthetic_to_file`, `attn_workload.cpp`, shares its
per-step generator with the still-present in-memory
`extend_synthetic()` -- see 1.4). Out-of-core generation reduces this
phase's resident footprint by ~99.4%.

### 2.2 A real, unaudited hotspot the first `--audit-memory` run caught

The first working version bounded `seq_latencies`/`all_latencies` and
`tracked_resource_t::wait_samples_ns` (see 2.4) but a real
`--audit-memory` run still showed **+656 MiB** for a single
`calibrate()` call -- `wssim::channel_predictor_t`'s per-channel state
is itself bounded (`max_tracked_blocks`, pruned every `observe()`
call), but `run_scenario_calibration_impl`'s own `ever_fetched`
(`std::unordered_set<uint64_t>`, tracking every (layer, kv_group,
block_id) tuple EVER fetched, purely to detect a later redundant
re-fetch) is not: at the unified sweep's 130,560-step synthetic
extension, block ids keep shifting forward every ~4096-step cycle (see
`extend_synthetic`'s "block 0 stays fixed, everything else shifts"
comment), so the union of distinct tuples touched grows roughly
LINEARLY with context length instead of staying bounded by working-set
size the way the predictor/hot-cache state does.

Fixed by bounding `ever_fetched` to `kEverFetchedCap` (1,000,000)
tuples, FIFO-evicted (`engine.cpp`) -- a disclosed approximation:
`total_redundant_fetches` becomes a LOWER BOUND once the cap is
exceeded (a genuine redundant fetch of a tuple evicted from this
tracking window looks like a fresh one), not an exact count. No other
metric reads `ever_fetched`.

### 2.3 mmap vs. streaming: a real, measured RSS-accounting difference

Both backends are byte-for-byte identical in what they compute (1.4).
They are NOT identical in how the OS accounts their memory. Two
`--audit-memory` runs, same scenario, same 512 MiB `--trace-cache-mib`
budget derivation:

| backend | `calibrate()` RSS delta |
|---|---|
| mmap | 656-660 MiB |
| streaming | **233 MiB** (~2.75x less) |

Root cause: `run_scenario_calibration_impl` sequentially touches every
chunk of the trace at least once. The `mmap` backend's chunk *cache*
is exactly as bounded as `streaming`'s (both show
`chunk_cache_resident ~= 212 MiB` for this trace/budget), but `mmap`'s
underlying file mapping means the OS keeps the touched pages of the
*whole 431 MiB source file* resident (as clean, evictable-but-not-yet-
evicted, shared file-backed pages) independent of when this process's
own cache decides a chunk is no longer needed -- those pages count
toward `VmRSS` for as long as the kernel leaves them resident, which
under normal (non-memory-pressure) operation can be indefinitely.
`streaming`'s plain `fseek`/`fread` never maps anything, so its RSS
tracks this process's own bounded structures much more tightly.

**Consequence**: `--backend streaming` is the default (`main.cpp`),
not `mmap`, specifically because `mem_guard_t`'s budget enforcement
(3) reads real process RSS, and mmap's RSS is a measurably less
reliable proxy for "memory this process would actually free under
pressure" at this scale. `mmap` remains available via `--backend mmap`
for anyone who wants its (real, but less predictable under a strict
budget) potential repeat-access speed from page-cache reuse.

### 2.4 Per-scenario transient state: `run_concurrent()`

`exact_engine.cpp`'s `run_concurrent()` used to collect EVERY completed
step's latency into `seq_latencies` (a
`std::vector<std::vector<double>>`, one inner vector per sequence)
then flatten into `all_latencies` just to sort it once -- up to ~66.8M
doubles (~536 MiB) at the full 512 x 130,560 scale, doubled
transiently by the flatten. THREE more plain `std::vector<double>`
(`tracked_resource_t::wait_samples_ns`, one each for the link/device-
DRAM/quant-engine resources) grew the same unbounded way with every
dispatched fetch -- and since prefetch can fire on nearly every step
for every one of 512 sequences, this was, in practice, the LARGER of
the two problems (caught by the same real memory-guard smoke test
described in 3, an 846 MiB RSS observed against a 768 MiB declared
budget before this fix; overshoot narrowed to a stable ~10% band
after).

Fixed by `bounded_quantile_accumulator_t`
(`tools/membrane-kv-exact-sim/bounded_quantile.h/.cpp`): holds samples
in RAM up to `in_memory_cap` (default 2,000,000, ~16 MiB); beyond that,
further samples spill to a temp file, and the final p50/p95/p99 come
from an external (file-based) multi-target quickselect -- median-of-
(strided-)sample pivot, with an "equal to pivot" bucket resolved
WITHOUT ever materializing it. That last property matters concretely
here: the sweep's own documented common shape (Phase 6.4's finding
that p50=p95=p99 frequently pin to the model's compute floor) is
exactly the "almost every sample is bit-identical" case that would
defeat a naive fixed-bucket histogram (nearly all mass in one bucket)
but resolves in a single round via the equal-bucket short-circuit --
verified directly in `test_bounded_quantile.cpp`'s
`test_spilled_compute_floor_pinned` (200,000 identical + 500 distinct
tail samples) against a plain-`std::sort` reference. Below the
in-memory cap, behavior is the untouched original in-RAM sort --
parity with the pre-6.5 code is exact there by construction, not
approximated.

Five such accumulators exist per scenario (total-latency,
KV-overhead-only for item 13's new metric, and the three per-resource
queue-waits) -- worst case `5 x 2,000,000 x 8 bytes = 80 MiB`
in-memory before any of them would need to spill.

### 2.5 What stayed bounded already (no change needed)

- `tail_tracker_t`: O(cap) (default 20) min-heap of worst-latency
  samples, unchanged from Phase 6.4 -- already exactly the pattern the
  rest of this phase applied elsewhere.
- `wssim::channel_predictor_t`'s heavy-hitter state: bounded to
  `max_tracked_blocks` (128) per channel, pruned every `observe()`
  call -- pre-existing, unaffected by 2.2's fix (a DIFFERENT map in
  the same function).
- `calibrated_profile_t.steps`: one `per_step_calib_t` (16 bytes) per
  decode step, replayed (not per-sequence) -- 130,560 steps = ~2 MiB,
  independent of concurrency.

## 3. Real memory-budget enforcement (`mem_guard.h/.cpp`)

Reads `/proc/self/status` (VmRSS), `/proc/meminfo` (MemAvailable,
SwapTotal/SwapFree), `/proc/self/stat` (majflt) -- real numbers, no
estimation, checked after every scenario completion (not just on the
60-second heartbeat). Escalates by RSS-vs-budget ratio (and,
independently, system-wide MemAvailable / swap pressure):

| condition | action |
|---|---|
| ratio > 0.75, or swap used > 60% | shrink the shared chunk cache to ~30% of budget (idempotent target, not repeated halving) |
| ratio > 0.90, or swap used > 60% | lower the active-worker slot limit by one (floored at `--min-workers`) |
| ratio > 1.05, or MemAvailable < 128 MiB, or swap used > 85% | stop claiming new scenarios, let in-flight ones finish, exit cleanly with `kMemGuardExitCode` (3) -- checkpoint is already durable (every scenario's row is `fflush`ed immediately), so this is a deliberate, resumable stop, never a kernel OOM-kill standing in for control flow |

Worker throttling is cooperative, not preemptive: each worker thread
checks its own rank against the current limit before claiming a new
scenario index, sleeping and rechecking rather than spinning; a
throttled-out worker still periodically checks whether the OTHER
workers have already drained the queue, so it exits normally instead
of blocking `join()` forever.

Real, measured behavior (streaming backend, `SmolLM2-135M`,
`--memory-budget-mib 768`, 2 workers): mem_guard triggered
`CHECKPOINT_AND_EXIT` at 846 MiB RSS -- **10% over budget**, not the
~74% overshoot (1337 MiB vs. 768 MiB) observed before the `ever_fetched`
and `wait_samples_ns` fixes in 2.2/2.4. The remaining ~10% is the real,
expected granularity cost of checking once per scenario completion
rather than continuously -- documented here as the honest slack
factor, not hidden.

### 3.1 Worker scaling (item 9)

Real, measured (not modeled) -- three fixed 240-second windows,
identical setup each time (`SmolLM2-135M`, `--memory-budget-mib 1024`,
`--chunk-steps 512`, fresh scratch checkpoint, streaming backend),
`--workers` varied:

| workers | real scenarios completed (excl. 6 instant analytical) | peak RSS | mean RSS | chunk-cache hit rate at window end |
|---|---|---|---|---|
| 1 | 5 | 469 MiB | 465 MiB | 0.000 |
| 2 | 7 | 857 MiB | 501 MiB | 0.18-0.34 |
| 4 | 13 | **787 MiB** | 574 MiB | 0.41 |

All three stayed under the 1024 MiB budget with zero `mem_guard`
interventions -- this is throughput scaling headroom, not a forced
choice. 4 workers gave the highest real throughput (13 vs. 7 vs. 5 in
the same wall-clock window) while its PEAK RSS was actually lower than
2 workers' (787 vs. 857 MiB): with more workers cycling through
scenarios against the SAME shared chunk cache (3.1's whole reason for
being shared, not per-thread), the cache warms up and gets reused
more, so per-worker redundant decode work drops faster than the
added per-worker transient state (bounded_quantile_accumulator_t
instances, predictor state) costs. This machine has 12 logical CPUs;
system-wide `free` showed real, pre-existing swap pressure throughout
(2.1 GiB of 9.6 GiB swap in use, ~170 MiB genuinely free at the
tightest point) independent of these test runs, consistent with the
5.6 GiB total RAM this whole phase is designed around.

**4 workers, `--memory-budget-mib` in the 1024-2048 range, was the
choice used for the real SmolLM2-360M completion run (section 6)** --
`mem_guard`'s adaptive throttling (3) is the actual safety net if
360M's larger per-scenario footprint (more layers/kv-heads than 135M)
needed it, not a manual re-tune.

## 4. Recovered/verified artifacts and real bugs this rework's own
   testing caught

- **CSV-durability-on-early-kill bug**: the checkpoint-replay-into-CSV
  loop at startup (`main.cpp`) wrote rows via `fprintf` but never
  `fflush`ed before the first NEW scenario completed. A process killed
  before completing even one new scenario (exactly what a real
  `mem_guard` `CHECKPOINT_AND_EXIT`, or a real OOM-kill, or this
  phase's own interrupted-resume test does) lost every previously-
  checkpointed row from the CSV even though the checkpoint genuinely
  still had them -- caught by a real smoke test
  (`membrane-kv-exact-sim-verify` flagged "8 checkpoint records have no
  matching CSV row" against real artifacts), fixed with one explicit
  `fflush(csv)` after the replay loop, re-verified clean.
- **Checkpoint duplicate-id rebuild bug**: `load_checkpoint()`
  (`checkpoint.h`, unchanged) does not itself deduplicate scenario
  records with the same id; the pre-6.5 CSV-rebuild-from-checkpoint
  loop would have faithfully turned a duplicated checkpoint record
  into an actual duplicate CSV row. Fixed by deduplicating (keep-last)
  during rebuild (`main.cpp`) -- also the exact repair strategy
  `membrane-kv-exact-sim-verify --repair` uses independently.
  Note: the "checkpoint header can go stale mid-file" concern in the
  original planning for this phase was investigated and found NOT to
  be a real issue -- `checkpoint_writer_t::open()` only ever writes a
  header on a non-append (fresh) file, so a real checkpoint from this
  codebase's actual write path never accumulates more than one header
  line.
- **135M's lost 900 tail samples**: `--tail-recovery-only` re-runs
  ONLY `SmolLM2-135M`'s 45 `exact-predictor-prefetch` scenarios (the
  comparison the tail-samples CSV targets), against a SEPARATE
  checkpoint (`unified-tail-recovery.ckpt`), appending only to the
  tail-samples CSV -- the already-complete 231/231 main-sweep rows for
  135M are untouched. See section 6 for the real completion status.
- **A genuinely truncated real CSV row, self-healed by the checkpoint-
  as-source-of-truth design**: migrating the real, existing 338-row
  Phase 6.4 checkpoint (below) and rebuilding its CSV surfaced that the
  ORIGINAL `unified-sweep.csv`'s very last line
  (`SmolLM2-360M,exact-predictor-prefetch,fp16,4294967296,109951162777`)
  was cut off mid-write -- one digit short of the real
  `device_total_bytes` value (`1099511627776`), almost certainly from
  one of Phase 6.4's own disclosed real OOM kills landing mid-`fwrite`.
  The checkpoint's own copy of that exact record was complete and
  correct. Rebuilding the CSV from the checkpoint (main.cpp's own
  startup behavior, exercised here by hand ahead of the real 6.5 run)
  silently repaired it -- a real, unplanned demonstration of why the
  checkpoint, not the CSV, is this design's source of truth.

### 4.1 Real checkpoint schema migration (pre-6.5 -> 6.5)

The real, existing 338-scenario checkpoint (135M 231/231 + 360M
107/231, both genuinely completed by the Phase 6.4 binary) predates
this phase's 8 new columns entirely (41 vs. this phase's 49). A
one-time migration (`/tmp/migrate_legacy_checkpoint.py` at the time
this was run, not part of the committed tree -- a data migration, not
a simulator feature) backfilled each legacy row with:

- `model_compute_floor_ns`: exactly computable from `sim_config.h`'s
  real, already-measured tok/s constants (63.8 / 24.4) -- not
  re-measured, not approximated.
- `trace_hash8`/`config_hash8`: the real checkpoint header's own
  hashes, which apply sweep-wide, not per-row.
- `sim_version="phase6.4-legacy"`, `backend="in-memory"`: accurate
  historical labels (Phase 6.4's binary held the whole synthetic trace
  resident; no backend choice existed then).
- `completion_checksum`: a real, freshly computed CRC32 over the
  migrated row.
- `incremental_kv_p99_ns`, `hidden_under_compute_fraction`: honest
  `"n/a"` -- these require the full per-step latency distribution,
  which Phase 6.4 never persisted. Not fabricated as 0 or any other
  number.

Verified before touching the real files: a `--dry-run` pass, a CRC32
cross-check against Python's `zlib.crc32` (matches
`membrane_block_checksum`'s algorithm exactly -- confirmed against a
real row's already-known checksum), a full run of
`membrane-kv-exact-sim-verify` against the migrated (scratch-copy)
result (0 problems), an actual real invocation of
`membrane-kv-exact-sim` against the migrated scratch checkpoint
confirming `"338 already complete"`, and a byte-for-byte diff of the
original 41 columns against the migrated file's first 41 columns
(1 real difference found -- the truncated row above, correctly
repaired, not corrupted). Only then applied to the real
`benchmarks/cxl-sim/unified-sweep.{csv,ckpt}`, with the pre-migration
files kept as a backup during the session.

### 4.2 Real bugs found by code review (not by testing)

An independent review pass over this phase's diff, run WHILE the real
SmolLM2-360M completion (section 6) was already in progress, found
three more real bugs testing had not caught:

- **An uncaught exception from one corrupted/truncated chunk would
  `std::terminate()` the entire sweep**, not just fail one scenario --
  defeating this whole phase's checkpoint-and-exit-cleanly design.
  `attn_trace_reader.cpp`'s `decode()` throws `std::runtime_error` on a
  real CRC/decompress failure; nothing caught it. Fixed by wrapping
  `process_one()`'s call in `main.cpp`'s worker loop in a try/catch --
  a failed scenario is now logged, skipped (never written to CSV/
  checkpoint, so it stays eligible for retry), and the sweep continues;
  the run only refuses to mark itself complete if any scenario failed.
- **The chunk cache's "loading" placeholder was never cleared if the
  loader threw**, which would hang every OTHER thread already waiting
  on that same chunk_id in `m_cv.wait()` forever --
  `attn_trace_chunk_cache_t::acquire()` now catches, erases the
  placeholder, and notifies all waiters before re-throwing to its own
  caller. Directly tested:
  `test_throwing_loader_unblocks_concurrent_waiters` (8 threads racing
  on one chunk whose first decode throws; all 8 terminate, at least
  one recovers) and `test_throwing_loader_does_not_leave_slot_stuck`.
- **A stale `m_cur_chunk_id` after a thrown `acquire()` could dereference
  a chunk the reader no longer held a pin on** on a later call reusing
  the same reader instance (workers reuse one reader across every
  scenario they process) -- fixed by sentinel-ing `m_cur_chunk_id`/
  `m_cur_ptr` to "none" BEFORE calling `acquire()`, not after, so a
  thrown exception can never leave them pointing at a released chunk.

Two lower-severity findings from the same review were also fixed:
`bounded_quantile.cpp`'s external quickselect had no NULL check on
temp-file handles (a real crash risk under fd exhaustion/disk-full,
now throws a catchable exception instead) and used a large
stack-resident I/O buffer in a recursive function (moved to the heap,
removing a theoretical stack-exhaustion path on adversarially skewed
input); `bounded_quantile_accumulator_t::finish()` is now safe to call
twice (returns zero, was previously undefined behavior). All fixes
re-verified: full Release/ASan+UBSan/TSan suites, 100% pass, before
resuming reliance on the running completion sweep's NEXT checkpoint
resume (the already-running process kept using its already-loaded
pre-fix binary uninterrupted -- Linux does not retroactively affect a
running process's code when the file at its path is rebuilt).

## 5. Verification matrix

- Release, ASan+UBSan, TSan: full `ctest` suite, 100% pass on all
  three (28/30/30 tests respectively as of this phase -- the two extra
  under sanitizer builds are the sanitizer-specific
  `test_store`/`test_block` allocator-return-null configurations
  already established pre-6.5).
- In-memory vs. mmap vs. streaming backend parity: bit-identical on
  real fixture traces (1.4).
- Deterministic replay: `test_exact_engine.cpp`'s pre-existing
  `test_deterministic_replay`, still passing after the `tracked_
  resource_t`/`ever_fetched` rework (2.2/2.4).
- Interrupted/resumed sweep + corrupted-checkpoint rejection:
  `test_interrupted_resume.sh`, against the REAL binaries and REAL
  (small, committed) native trace file, not mocks -- SIGKILLs a real
  in-progress sweep, confirms the checkpoint AND CSV both durably
  survived, resumes and confirms prior progress is honored, corrupts
  one checkpoint record's checksum and confirms
  `membrane-kv-exact-sim-verify` catches it, then confirms `--repair`
  restores a clean pair. This is the test that caught the CSV-
  durability bug in section 4.
- Forced-low-memory / peak-RSS: real, not simulated -- every
  `--memory-budget-mib` number in sections 2.3 and 3 is an actual
  `/proc`-read RSS from an actual run on this machine, including the
  before/after comparison across the `ever_fetched` and mmap-vs-
  streaming fixes.
- Note on this machine's TSan + mmap interaction: ThreadSanitizer's
  shadow-memory layout can reject an ordinary, correct `mmap()` call
  as an "unexpected memory mapping" purely depending on where ASLR
  happens to place it (observed on EVERY TSan-instrumented binary on
  this kernel, not just ones calling mmap directly -- e.g. `test_block`
  hit it too). Not a data race; worked around by running the whole
  `ctest --test-dir build-tsan` invocation under `setarch $(uname -m)
  -R` (disables ASLR for the process tree), not by changing the mmap
  backend's real behavior.

## 6. Completed 360M results

The real completion run finished. SmolLM2-360M is now **231/231**,
matching SmolLM2-135M -- **462/462 total across both models**, all via
this document's out-of-core path, none extrapolated from 135M or from
Phase 6.4's partial 107/231. Full per-section detail (capacity,
queue/contention, tail-latency, layer/head predictor accuracy, main
comparisons, compute-normalized latency, success criteria) lives in
`docs/phase6-unified-stress.md`; this section gives the headline
numbers specific to what this document's rework actually delivered.

- **Scope closed**: the remaining 124 scenarios (`exact-predictor-
  prefetch`'s last 34/45, `exact-predictor-coalescing`'s full 45/45,
  `oracle`'s full 45/45) were genuinely computed under the streaming
  backend, worker count chosen from the real 1/2/4 micro-benchmark
  (3.1), staying within the enforced memory budget for the whole run
  (`mem_guard`, section 3) -- no OOM kill during this phase's actual
  completion run, unlike all four of Phase 6.4's attempts at the same
  scenarios under the old eager-load path.
- **Bytes/token**: all five comparisons now real for 360M, 130x-405x
  reduction vs. full-scan-cxl / 130x-215x vs. compressed-full-scan-cxl
  depending on comparison, meeting the >=100x target across the board
  (`docs/phase6-unified-stress.md` section 12).
- **Capacity**: 360M's complete 3-device-size table confirms the
  larger, real finding that 360M's fp16 working set never fits any
  tested device (`cap_effective_capacity_ratio` = 0.7998 even at 2TiB)
  -- a genuinely worse result than 135M's, not something the earlier
  partial run could have shown (section 3 of the unified-stress doc).
- **Tail latency**: 360M's real worst captured sample -- sequence 477,
  step 53576, fp16, 64MiB host/2TiB device, 24.6M prefetch + 16.7M
  compulsory-miss bytes, 53.54ms link wait / 54.19ms total latency --
  came from this phase's out-of-core run, not Phase 6.4's (section 5
  of the unified-stress doc).
- **Compute-normalized latency (new fields, 124 real 360M rows)**:
  `hidden_under_compute_fraction` spans 0.001-0.983 across those rows,
  confirming the field genuinely discriminates tight-cache
  (mostly-exposed KV latency) from generous-cache (mostly-hidden)
  scenarios rather than being a constant (section 12.1 of the
  unified-stress doc).
- **10ms p99 bound**: confirmed, exhaustively, not met by any of the
  225 real (non-analytical) 360M rows -- same structural cause as
  135M (the model's own real decode speed, ~4.1x the bound at its own
  compute floor), now confirmed across the full matrix rather than a
  representative sample (section 10 of the unified-stress doc).
- **Tail-sample recovery**: both models now have exactly 900 real tail
  samples each (1,800 total) in `unified-tail-samples.csv`, zero
  duplicate rows -- 135M's via a dedicated recovery run reusing this
  phase's own out-of-core path (section 4), 360M's via the same path
  seeded with 34 already-real rows from the main completion run so
  only the genuinely-missing 11 were recomputed, not all 45 (avoiding
  a duplicate-row risk caught and killed before it could write any
  data).
- **Integrity**: `membrane-kv-exact-sim-verify` against the final
  artifacts reports 0 problems -- `unified-sweep.csv`/`.ckpt` at
  462/462 unique scenarios, both tail-recovery checkpoint/CSV pairs
  clean at 45/45 records each.

## 7. Hardware assumptions / still-unverified real CXL claims

Unchanged from Phase 6.1/6.4: every CXL link latency/bandwidth figure
in the hardware-sensitivity matrix (10 named points) is **ASSUMED**
(published industry-typical ranges) except pipeline count, which Phase
5.3's own real RTL simulation calibrated. No real CXL hardware exists
anywhere in this project; this phase adds NO new hardware claims --
it is purely a software/memory-architecture change to the simulator
itself, verified against this development machine's real, measured
RAM/CPU behavior, not against any physical CXL device.
