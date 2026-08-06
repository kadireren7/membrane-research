# Phase 8: Hardware Validation Plan

This document plans real physical validation of the FPGA/CXL claims in
`docs/phase5-synthesizable-fpga.md`, `docs/phase6-cxl-near-memory.md`,
and `paper/main.md` — it does not report any new hardware result. As of
this writing, no FPGA board, no place-and-route tool, and no CXL
hardware exist in this project's development environment; every number
in the plan below that looks like a hardware fact (clock targets, board
names, PCIe generation) is drawn from the *target specification*
already disclosed in `docs/phase5-synthesizable-fpga.md` §8, not a new
claim.

Three levels, each strictly gated on the previous one succeeding —
see `outreach/hardware-claim-gates.md` for exactly what can be said
publicly at each stage.

---

## Level A — FPGA simulation and implementation (no board required)

Goal: get a real place-and-route result (pass or fail) for
`membrane_quant_stream_top` on a real vendor toolchain, replacing this
project's current yosys-only, no-P&R synthesis check.

### Prerequisites
- Repository at or after commit `e74d719` (or later); `rtl/` unchanged
  from what's cosimulated in `rtl/tb/tb_top_verilator.cpp`.
- A machine with a supported OS for the chosen vendor toolchain
  (Vivado: RHEL/Ubuntu LTS or Windows; Quartus: similar).

### Hardware
- None required for this level — synthesis and place-and-route run
  entirely in the toolchain, targeting a specific device part number
  (no physical board needed to get a P&R result and a timing report).

### Software
- **Xilinx/AMD path**: Vivado (version matching the target Alveo
  card's supported release — check AMD's board support package for the
  specific card before choosing a Vivado version).
- **Altera/Intel path**: Quartus Prime (Pro edition for higher-end
  devices), if targeting an Intel FPGA instead.
- This project's RTL (`rtl/*.sv`) plus a synthesis-only top-level
  wrapper (see `hardware/README.md`'s vendor-wrapper section) — no
  vendor IP is vendored in this repository.

### Procedure
1. Elaborate the full `membrane_quant_stream_top` hierarchy in the
   vendor tool (should succeed — already confirmed to elaborate under
   yosys 0.33; a second, independent elaboration in a different tool is
   itself useful signal).
2. Run synthesis targeting the chosen device part (see
   `hardware/board-targets.md` for candidates).
3. Run place-and-route at each of the three assumed clock targets this
   project has already disclosed as assumptions (100/200/300 MHz —
   `docs/phase5-synthesizable-fpga.md` §10), to find the real achievable
   Fmax rather than assuming one.
4. If timing does not close at any tested frequency: re-pipeline
   `membrane_fp_divider` (the real, disclosed dominant resource cost —
   ~73,600 LUT-class cells per the yosys/ECP5 measurement, and an
   un-pipelined ~65-bit combinational divide) into a multi-cycle
   structure and re-run steps 2–3. This is expected engineering work,
   not a contingency — see `hardware/risk-register.md`.
5. Run **post-route (post-implementation) simulation** — cosimulate the
   post-P&R netlist (with real extracted timing) against
   `rtl/tb/tb_top_verilator.cpp`'s same golden vectors, not just the
   pre-synthesis RTL.
6. Record resource utilization (LUT/FF/BRAM/DSP, real vendor-tool
   numbers, not yosys/ECP5 estimates) and the vendor tool's own power
   estimate (pre-bitstream, estimation-only — real power measurement is
   Level B).

### Success criteria
- Synthesis and place-and-route complete without errors on at least one
  tested clock target.
- Post-route simulation matches the same golden vectors
  `tb_top_verilator.cpp` already validates, bit-exact, 0 mismatches.
- A real Fmax number exists (even if lower than the 100 MHz floor this
  project assumed).

### Failure criteria
- Timing does not close at any frequency down to a practical floor
  (e.g. 50 MHz) even after re-pipelining the divider — in which case,
  report this honestly as a real, disclosed finding (see
  `outreach/hardware-claim-gates.md`), not as a project failure to hide.
- Post-route simulation produces a single bit-exactness mismatch vs.
  the pre-synthesis golden vectors — this would indicate a real
  synthesis/timing bug and must block progression to Level B.

### Estimated engineering time
2–5 days for a clean run; 1–3 additional weeks if the divider needs
re-pipelining (a real RTL redesign, not a config change).

### Risks
See `hardware/risk-register.md`: RTL divider resource use, timing
closure, tool licensing.

### Produced artifacts
- Vendor P&R report (timing, utilization, power estimate).
- Post-route simulation log.
- Updated `hardware/board-targets.md` with the real (not assumed) Fmax.

---

## Level B — Real FPGA board

Goal: bring the design up on physical silicon and get real
bit-exactness and throughput/latency numbers.

### Prerequisites
- Level A complete: timing closes at some real, measured frequency.
- A physical board matching (or close to) the Level A target device.

### Hardware
- **Target**: AMD/Xilinx Alveo U250, U280, or U55C (or an accessible
  equivalent — see `hardware/board-targets.md` for the tradeoffs).
  Alveo U250 was this project's original target specification
  (`docs/phase5-synthesizable-fpga.md` §8: Virtex UltraScale+ XCU250,
  PCIe Gen3 x16 native, commonly deployed behind a Gen4-capable host).
- A host machine with a free PCIe slot matching the card's electrical
  and mechanical requirements, and enough airflow/power delivery for a
  data-center-class accelerator card (these cards are not
  desktop-friendly — check the card's power/cooling spec before
  assuming a normal workstation suffices).

### Software
- Vendor board-support package / shell (e.g. AMD's XRT + a matching
  Alveo platform shell) — this project does not vendor or redistribute
  this.
- Host-side driver + a DMA test harness (see
  `hardware/experiment-protocol.md` for the loopback-DMA-first
  approach).
- This project's AXI4-Stream/AXI-Lite adapter (`hardware/` — see the
  vendor-wrapper section) wired to the board's actual shell interface.

### Procedure
1. **Host DMA loopback** (no MEMBRANE logic yet) — confirm the host can
   move data to and from the card at all, before introducing any of
   this project's own logic as a debugging variable.
2. **AXI4-Stream wrapper integration** — wire
   `membrane_quant_stream_top` behind the project's own
   platform-independent AXI adapter (`hardware/README.md`).
3. **Known-vector test** — replay a small, fixed set of golden vectors
   (the same ones `tb_top_verilator.cpp` uses) through real DMA and
   compare byte-for-byte against the CPU reference.
4. **100K-random-block parity test** — the real-hardware analogue of
   this project's existing 100,000+-block CPU/ggml parity test and
   520,000-transaction Verilator cosimulation; this is the step that
   would upgrade "simulation bit-exact" to "hardware bit-exact" (see
   `outreach/hardware-claim-gates.md`).
5. **Throughput/latency measurement** — sustained throughput at
   increasing queue depth and batch size, real p50/p95/p99, real host
   CPU utilization during the transfer.
6. **Power/thermal measurement** — real board power (vendor
   tool/sensor, not an estimate) and junction/board temperature under
   sustained load.

### Success criteria
- 100% match on the 100K-random-block parity test (0 mismatches) —
  this is the bar for claiming "hardware bit-exact," not a lower
  threshold.
- A real, measured throughput/latency number exists, in whatever
  direction it points (even if it's worse than the CPU-only baseline —
  report it either way, consistent with this project's negative-result
  discipline).

### Failure criteria
- Any parity mismatch on the 100K-random-block test that cannot be
  attributed to a known, disclosed IEEE-754 rounding-mode difference
  (this project's own quantizer already documents one such case,
  round-to-even vs. round-half-away-from-zero — see
  `docs/phase4-ggml-quant-parity.md`) is a real bug and must block any
  "hardware bit-exact" claim.
- DMA throughput that cannot be explained (e.g. wildly inconsistent
  across identical runs) should be investigated as a possible
  host/driver configuration issue before being reported as a MEMBRANE
  result.

### Estimated engineering time
1–2 weeks for bring-up + known-vector test (assuming the board-support
package and driver already work, which is itself often the hardest
part); 1–2 additional weeks for the full throughput/scaling/power
measurement suite in `hardware/experiment-protocol.md`.

### Risks
PCIe setup overhead, DMA batching granularity, board availability,
vendor lock-in, simulation-to-hardware mismatch — see
`hardware/risk-register.md`.

### Produced artifacts
- `hardware/results-schema.json`-conformant real result records
  (label: `REAL_HARDWARE`), one per experiment in
  `hardware/experiment-protocol.md`.
- Bitstream + its hash (for reproducibility — see
  `hardware/experiment-protocol.md`'s environment-capture step).

---

## Level C — CXL platform

Goal: check whether MEMBRANE's near-memory/CXL discrete-event
simulation (`docs/phase6-cxl-near-memory.md`) resembles real CXL device
behavior, and, if feasible, run the exact sparse retrieval design
against a real memory tier.

### Prerequisites
- Level B complete (or, if a CXL platform becomes available before
  Level B, this level can proceed independently on the *simulation
  fidelity* question alone — the exact-retrieval logic itself doesn't
  require the FPGA datapath to be on real silicon to test).

### Hardware
- A real CXL Type-3 memory device (an accelerator/expander that
  exposes host-manageable device memory), or an accessible CXL
  emulation/prototyping platform (e.g. a vendor's own dev kit or a
  university lab's existing CXL testbed) — this project has access to
  neither today.

### Software
- Host OS with CXL Type-3 support (recent Linux kernel with CXL core
  driver support) and whatever vendor tooling the specific device
  requires.
- `tools/membrane-cxl-sim`'s calibration inputs (real captured attention
  traces, already committed) adapted to drive real memory-tier
  placement decisions against the real device instead of the simulator.

### Procedure
1. **Memory-tier integration smoke test** — confirm the host can
   allocate, read, and write to the CXL device's exposed memory range at
   all, independent of any MEMBRANE logic.
2. **Exact sparse retrieval against the real tier** — port the
   predictor/prefetch/compulsory-fetch logic
   (`tools/membrane-kv-exact-sim`) to issue real reads/writes against
   the CXL device instead of the simulator's modeled queues.
3. **Multi-sequence workload** — replay a real captured trace (scaled
   down from the full 512-concurrency simulation to whatever the real
   platform can sustain) and record real bandwidth and real p99 latency.
4. **Compare against the simulation** — the single most valuable
   output of this level: does `membrane-cxl-sim`'s assumed link
   latency/bandwidth model predict the real device's behavior at all,
   and by how much is it wrong if not?

### Success criteria
- The real device successfully serves the exact-retrieval workload
  end-to-end with correct (bit-exact) data.
- A real bandwidth and p99 number exists for at least one scenario
  point, comparable (even if not identical in scale) to a
  corresponding simulated point.

### Failure criteria
- If the real device's latency/bandwidth characteristics are so
  different from the assumed CXL Consortium ranges that no meaningful
  comparison is possible, this is itself a valid, reportable finding
  (the simulation's assumptions were wrong) — not a reason to suppress
  the result.

### Estimated engineering time
Highly platform-dependent — realistically 2–6 weeks depending on how
much of the host-side CXL memory management is already working on the
provided platform versus needing to be built from scratch.

### Risks
CXL hardware availability (the largest single risk in this entire
plan), vendor lock-in, simulation-to-hardware mismatch — see
`hardware/risk-register.md`.

### Produced artifacts
- Real bandwidth/latency measurements, `hardware/results-schema.json`-
  conformant, label `REAL_HARDWARE`.
- A written comparison against `docs/phase6-cxl-near-memory.md`'s
  assumed figures, updating that document's disclosure section either
  way.

---

## Cross-level notes

- No level in this plan should be attempted out of order — Level B's
  claims are only valid once Level A's timing closure is real, and
  Level C's "real CXL" claims are only valid once Level C's own
  hardware is actually in hand (not before).
- Every artifact produced at any level must conform to
  `hardware/results-schema.json` and pass
  `hardware/experiment-protocol.md`'s pass/fail rules before being cited
  anywhere in `paper/main.md`, `README.md`, or any outreach material —
  see `outreach/hardware-claim-gates.md` for the enforcement mechanism.
