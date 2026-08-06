# Hardware experiment protocol

The exact, sequential protocol to run **once a real FPGA board is
obtained** (Level B of `docs/phase8-hardware-validation-plan.md`). Run
in order — later steps assume earlier ones passed. Every measurement
records to `hardware/results-schema.json`'s format with
`result_label: "REAL_HARDWARE"`; nothing in this file has been run yet,
and no artifact from it exists in this repository beyond the schema and
its documented example (`hardware/results-example.json`).

| # | Step | Command (illustrative — adapt to the real board's toolchain) | Input | Metric | Expected invariant | Artifact path | Pass/fail rule |
|---|---|---|---|---|---|---|---|
| 1 | **Environment capture** | `uname -a; lspci -vv \| grep -A20 Xilinx; vivado -version` (or `quartus_sh --version`) | none | full env dump | env dump is non-empty and includes board PCIe device id | `hardware/runs/<date>/env.txt` | FAIL if the board's PCIe device does not enumerate at all — stop here, this is a hardware/driver problem, not a MEMBRANE problem |
| 2 | **Board identification** | vendor tool's board-query command (e.g. `xbutil examine`) | none | board serial, part number, installed shell/platform version | matches the board named in `hardware/board-targets.md` (or is recorded as a deviation) | `hardware/runs/<date>/board-id.txt` | FAIL if the installed shell version doesn't match what Level A's bitstream was built against |
| 3 | **Bitstream hash** | `sha256sum <bitstream>.bit` (or `.pof`/`.sof` for Intel) | Level A's compiled bitstream | SHA-256 | recorded, immutable per run | `hardware/runs/<date>/bitstream.sha256` | N/A (recording step) |
| 4 | **Host configuration** | `cat /proc/cpuinfo`, `free -h`, `lspci -t` | none | CPU model, RAM, PCIe topology | non-empty | `hardware/runs/<date>/host-config.txt` | N/A (recording step) |
| 5 | **Loopback DMA** | vendor DMA test utility (e.g. `xbutil validate`) with no MEMBRANE logic in the path | a fixed test pattern | round-trip byte match | 100% byte match | `hardware/runs/<date>/loopback.log` | FAIL stops the whole protocol — do not proceed to step 6 with unverified DMA |
| 6 | **Known-vector test** | replay `rtl/tb/tb_top_verilator.cpp`'s existing golden vectors (`/tmp/top_x_120k.txt` etc., regenerated per `docs/reproduction.md` Level 1.4) through real DMA to the bitstream | the same fixed, deterministic golden vectors already used in simulation | byte-exact match vs. CPU reference | 0 mismatches on a small (e.g. 1,000-block) subset first | `hardware/runs/<date>/known-vector.csv` | FAIL on any mismatch not attributable to the documented round-to-even vs. round-half-away-from-zero difference already disclosed in `docs/phase4-ggml-quant-parity.md` |
| 7 | **100K random-block parity** | same replay mechanism, full 100,000+-block randomized set (matching `tests/unit/test_ggml_quant_parity.c`'s scale) | randomized blocks, fixed seed for reproducibility | 0 mismatches | 100% match | `hardware/runs/<date>/parity-100k.csv` | This is the gate for the "hardware bit-exact" claim (`outreach/hardware-claim-gates.md`) — FAIL blocks that claim |
| 8 | **Sustained throughput** | drive the core continuously at max achievable rate for >=60s | continuous stream of valid blocks | bytes/sec sustained | within an order of magnitude of the PCIe link's theoretical bandwidth at the achieved clock (sanity check, not a target to hit) | `hardware/runs/<date>/throughput.csv` | FAIL only if throughput is inconsistent across repeated runs (investigate host/driver config before blaming MEMBRANE) |
| 9 | **Queue-depth scaling** | repeat step 8 at queue depths {1, 4, 16, 64} in-flight descriptors | same | throughput and p50/p95/p99 latency per depth | throughput should not decrease monotonically as depth increases (a decrease would indicate a real contention/backpressure bug) | `hardware/runs/<date>/queue-depth-scaling.csv` | FAIL if throughput *decreases* as queue depth increases |
| 10 | **Batch-size scaling** | repeat step 8 at batch sizes {1, 8, 64, 512} blocks per DMA descriptor | same | throughput vs. batch size | throughput should increase (or plateau) with batch size, consistent with §RQ4's PCIe-round-trip-amortization finding | `hardware/runs/<date>/batch-size-scaling.csv` | Record as-is even if it doesn't increase — a flat or decreasing curve here would itself be a real, reportable finding |
| 11 | **CPU comparison** | run `tests/unit/test_ggml_quant_parity.c`'s equivalent workload on the same host's CPU, same block count | same random blocks as step 7 | CPU throughput/latency | comparable methodology to steps 8-10 | `hardware/runs/<date>/cpu-comparison.csv` | N/A (comparison baseline, not a pass/fail gate) |
| 12 | **Power measurement** | vendor power-sensor read (e.g. `xbutil examine -r electrical`) during step 8's sustained load | none | board power (W) | non-zero, plausible for the card's rated envelope | `hardware/runs/<date>/power.csv` | FAIL if the reading is 0 or exceeds the card's rated maximum (sensor/config problem) |
| 13 | **Thermal measurement** | vendor temperature-sensor read during step 8's sustained load | none | junction/board temperature | below the card's rated thermal limit throughout | `hardware/runs/<date>/thermal.csv` | FAIL (stop the run) if temperature approaches the card's rated limit — protect the hardware over completing the protocol |
| 14 | **Failure injection** | deliberately send a malformed descriptor (e.g. zero-length, out-of-range mode) | one malformed transaction | core/host behavior | core does not hang; either rejects cleanly or the host driver times out and recovers | `hardware/runs/<date>/failure-injection.log` | FAIL if the core hangs and requires a power cycle to recover (vs. a clean reset) |
| 15 | **Reset/recovery** | issue `soft_reset` via the AXI-Lite control register (`hardware/vendor-wrapper/axi_lite_ctrl.sv`'s CTRL register), then repeat step 6 | same known vectors as step 6 | post-reset correctness | identical result to step 6, confirming reset genuinely clears state | `hardware/runs/<date>/reset-recovery.csv` | FAIL if post-reset behavior differs from a fresh bring-up |

## Notes

- Steps 1-7 are the minimum bar for any "hardware bit-exact" claim.
  Steps 8-13 are the minimum bar for any real throughput/latency/power
  claim. See `outreach/hardware-claim-gates.md` for the exact allowed
  wording at each stage.
- Every artifact path above is illustrative (`hardware/runs/<date>/...`)
  — this directory does not exist yet and should be created fresh per
  real run, never committed with fabricated contents.
- If any step's FAIL condition triggers, stop and report it as a real,
  disclosed finding (consistent with this project's negative-result
  discipline) rather than silently retrying until it passes.
