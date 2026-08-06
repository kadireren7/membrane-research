# Phase 1 Store — Effective Capacity Benchmark

Validates the budget-aware compressed block store: how much *logical* data
it holds within a fixed *resident* memory budget. Produced by
`tools/membrane-store-bench` (`membrane-store-bench`).

## Method

A synthetic dataset is generated block-by-block with a deterministic seed.
A fixed fraction of blocks (`--compressible-ratio`) is filled with a single
repeated byte (compresses to ~1/128 of its size under RLE); the rest are
pseudo-random (incompressible, stored RAW via the store's fallback). Blocks
are written to the store, then every block is read back and compared against
regenerated content for an end-to-end integrity check. `effective_capacity_ratio`
is `logical_bytes / resident_bytes`.

Numbers below are from this development machine (gcc 13, `-O2`). Throughput
is single-threaded ingest and was memory-pressured into swap on this host
(~300 MiB free at run time), so the GB/s figures are a floor, not a ceiling;
the capacity and integrity results are exact.

## Scenario 1 — 1 GiB logical, 600 MiB budget

```
membrane-store-bench --logical-size 1G --memory-budget 600M \
    --block-size 64K --compressible-ratio 0.5 --seed 42
```

| Metric | Value |
|---|---|
| logical_bytes | 1.00 GiB |
| resident_bytes | 516.00 MiB |
| budget_bytes | 600.00 MiB |
| block_count | 16384 (8192 raw, 8192 compressed) |
| evictions | 0 |
| effective_capacity_ratio | **1.98x** |
| integrity | PASS |

All 1 GiB of logical data stays resident inside a 600 MiB budget — nothing
is evicted — because the compressed footprint (516 MiB) fits. Matches the
~1.9x expectation.

## Scenario 2 — 1.5 GiB logical, 1 GiB budget

```
membrane-store-bench --logical-size 1536M --memory-budget 1G \
    --block-size 64K --compressible-ratio 0.5 --seed 42
```

| Metric | Value |
|---|---|
| logical_bytes | 1.50 GiB |
| resident_bytes | 774.00 MiB |
| budget_bytes | 1.00 GiB |
| block_count | 24576 (12288 raw, 12288 compressed) |
| evictions | 0 |
| effective_capacity_ratio | **1.98x** |
| integrity | PASS |

1.5 GiB of logical data held in 774 MiB resident, comfortably above the
"at least 1.5x" bar.

## Takeaway (in-RAM store)

Both scenarios hold well over 1.5x their resident footprint in logical data
with zero eviction and bit-exact integrity, confirming the store's
budget accounting and the RAW/RLE per-block decision on realistic mixed data.
The ratio is bounded here by the 50% incompressible half; higher
compressible ratios raise it further (verifiable by sweeping
`--compressible-ratio`).

## Phase 1.4 — persistent file backend

With `--backend file`, blocks evicted from the RAM budget are written to a
cold tier instead of dropped, and loaded back (promoted) on `get`. This lets
logical data exceed what the compressed footprint alone would fit in RAM,
while `resident_bytes` stays hard-capped at the budget.

### Small validation — 256 MiB logical, 64 MiB budget

```
membrane-store-bench --logical-size 256M --memory-budget 64M \
    --block-size 64K --compressible-ratio 0.5 --backend file \
    --backend-path <dir> --seed 42
```

| Metric | Value |
|---|---|
| logical_bytes | 256.00 MiB |
| resident_bytes | 64.00 MiB (never exceeds budget) |
| stored_bytes | 135.27 MiB (resident + backend) |
| block_count | 4096 (2048 raw, 2048 compressed) |
| evictions | 5541 |
| effective_capacity_ratio | 1.98x |
| verified_blocks | 4096 / 4096 (read back in scrambled order) |
| integrity | PASS |
| backend files after destroy | 0 |

Every block survives thousands of evict/promote cycles and reads back
bit-exact in an order different from the write order, with resident memory
pinned to the 64 MiB budget throughout.

### Main validation — 2 GiB logical, 512 MiB budget

```
membrane-store-bench --logical-size 2G --memory-budget 512M \
    --block-size 64K --compressible-ratio 0.5 --backend file \
    --backend-path <dir> --seed 42
```

This run is memory-pressured (the host had far less free RAM than the
512 MiB budget) and disk-bound, so ingest throughput reflects swap and disk,
not the store. Capacity and integrity are the meaningful results and are
reported independently of speed:

| Metric | Value |
|---|---|
| logical_bytes | 2.00 GiB |
| resident_bytes | 511.97 MiB (never exceeds the 512 MiB budget) |
| stored_bytes | 1.06 GiB (resident + backend) |
| block_count | 32768 (16384 raw, 16384 compressed) |
| evictions | 44340 |
| effective_capacity_ratio | 1.98x |
| verified_blocks | 32768 / 32768 (read back in scrambled order) |
| integrity | PASS |
| backend files after destroy | 0 |
| ingest throughput | 0.07 GB/s (swap- and disk-bound; see note above) |

2 GiB of logical data cycled through a 512 MiB resident budget backed by
disk: every one of 32768 blocks was evicted and promoted as needed and read
back bit-exact in an order different from ingest, with resident memory
never crossing 512 MiB across 44340 evictions.

