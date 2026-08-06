# Board targets

Candidate boards and the real, disclosed constraints from this
project's existing (yosys-only, no place-and-route) synthesis check.
**No board in this list has actually been used** — this is a target
specification for Level A/B of `docs/phase8-hardware-validation-plan.md`,
inherited from and consistent with
`docs/phase5-synthesizable-fpga.md` §8's original disclosed target
profile, not a new claim.

## Candidate boards

| Board | Family | Notes |
|---|---|---|
| **AMD/Xilinx Alveo U250** (original target) | Virtex UltraScale+ XCU250 | PCIe Gen3 x16 native, commonly deployed behind a Gen4-capable host. Largest LUT budget of the three Alveo candidates; good fit if the FP32 divider needs significant re-pipelining/replication headroom. |
| AMD/Xilinx Alveo U280 | Virtex UltraScale+ XCU280 | Adds HBM2 — not required by this design (no BRAM-depth pressure identified, see below), but could matter if a future iteration wants a larger on-card KV staging buffer. |
| AMD/Xilinx Alveo U55C | Virtex UltraScale+ XCU55C | Smaller/cheaper than U250/U280; a reasonable first-bring-up target if the goal is "does this design fit and close timing at all" before committing to a larger card. |
| Any accessible equivalent (Intel/Altera Stratix 10 or Agilex dev board, or a university lab's existing FPGA testbed) | varies | Would require a Quartus (not Vivado) synthesis flow and a different AXI/DMA IP stack — see "Toolchain" below. Listed because "accessible" matters more than "originally targeted" for a first real board result. |

Board choice should be driven by **what a collaborating lab already
has**, not by re-acquiring the original U250 target specifically — see
`outreach/target-selection.md`.

## Vendor toolchain

- **Xilinx/AMD path**: Vivado (version matching the target Alveo card's
  board-support package — check AMD's release notes for the specific
  card before picking a Vivado version; this project has not verified
  which Vivado version any specific card currently requires).
- **Altera/Intel path**: Quartus Prime (Pro edition for Stratix
  10/Agilex-class devices).
- Neither toolchain is installed or available in this project's current
  development environment (no root access, and these are large,
  license-gated installs) — this is a real, disclosed gap, not an
  oversight.

## PCIe / DMA framework options

- **Xilinx/AMD**: XRT (Xilinx Runtime) + a matching Alveo shell/platform
  — the standard path for Alveo-class cards, provides a DMA subsystem
  MEMBRANE's AXI4-Stream wrapper (`vendor-wrapper/`) would sit behind.
- **Intel/Altera**: OPAE (Open Programmable Acceleration Engine) or a
  board-specific PCIe reference design, depending on the dev board.
- **Either path**: a loopback-DMA test (moving data to/from the card
  with no MEMBRANE logic in the path) should be the very first real
  hardware test run, before introducing this project's own RTL as a
  debugging variable — see `hardware/experiment-protocol.md`.

## Clock target

**Not verified — no P&R tool available in this environment.**
`docs/phase5-synthesizable-fpga.md` §8's disclosed assumption (100-300
MHz, "industry-typical reference point... not a measurement of this
design") stands until Level A actually runs place-and-route. The FP32
divider's un-pipelined ~65-bit combinational critical path
(`membrane_fp_divider`, see below) is the most likely reason a first P&R
attempt does not close timing at the higher end of that range.

## Stream width

**512 bits**, matching `in_data`/`out_data` directly in
`membrane_quant_stream_top.sv` — chosen so the module's native bus width
equals the AXI4-Stream `TDATA` width with no width-conversion logic
needed at the adapter boundary (`vendor-wrapper/axi_stream_adapter.sv`).

## Required BRAM / LUT / DSP (real yosys/ECP5 numbers, not vendor-tool numbers)

From `docs/phase5-synthesizable-fpga.md` §7 — real, measured
`synth_ecp5` technology-mapped cell counts (Lattice ECP5 is this
project's only available open-source synthesis backend; treat these as
**relative sizing signal**, not a Xilinx/Altera LUT count, which a real
vendor synthesis run would produce differently):

| Module | LUT4 | FF | Hard MULT18X18D |
|---|---|---|---|
| `membrane_fp_multiplier` | 330 | 33 | 4 |
| `membrane_fp_adder` | 1,533 | 33 | 0 |
| `q8_maxabs_reduce` | 1,076 | 501 | 0 |
| `q4_scan` | 5,913 | 0 | 0 |
| `membrane_fp_divider` | 37,998 (~73,600 LUT-class cells incl. carry-chain/mux primitives) | 33 | 0 |

`q8_scale`/`q4_scale` each instantiate two `membrane_fp_divider`s in
parallel and are expected (by direct extrapolation, not separately
measured — the standalone run alone took ~150s CPU, dominated by
yosys's `autoname` bookkeeping pass) to land around ~75-80K LUT-class
cells each.

**BRAM**: no analysis exists yet. Neither FIFO in the design
(`stream_fifo.sv`, input/output) is deep enough that this project has
established whether a real vendor tool would map it to block RAM or
distributed/LUT RAM — a real synthesis run's own inference choice,
disclosed as unknown rather than assumed.

**DSP**: only the multiplier's 4 hard `MULT18X18D` instances are
confirmed; a real Xilinx `DSP48E2`/Altera variable-precision DSP block
budget has not been analyzed and would very likely differ from ECP5's
DSP inference choices.

## Expected divider problem

`membrane_fp_divider` is, by a wide margin, both the largest single
module (~73,600 LUT-class cells, vs. ~550 for the multiplier and ~2,530
for the adder) and the most likely real timing-closure failure: its
`num64/den64` division is a single, un-pipelined combinational Verilog
`/` operator over a ~65-bit dividend and ~40-bit divisor. The `DELAY`
parameter in the current RTL only adds *output* register stages after
the combinational result — it does not break the division itself into
multiple cycles. **Disclosed, not worked around**: a real FPGA target
would very likely need this re-implemented as a genuinely pipelined
multi-cycle divider (restoring, SRT, or a smaller-radix iterative
structure) to have a realistic chance of closing timing at any
practical clock frequency. This is Level A's single largest expected
piece of engineering work (see
`docs/phase8-hardware-validation-plan.md` Level A, step 4, and
`hardware/risk-register.md`).

## Pipeline replication limits

`membrane_quant_stream_top` is a single shared-issue pipeline (1
block/cycle, initiation interval 1). To approach PCIe-limited throughput
rather than being capped by one pipeline's per-cycle rate at a realistic
clock, multiple full instances would need to be replicated, each with
its own input/output FIFO, arbitrated onto a wider host-facing AXI/PCIe
DMA fabric (`docs/phase5-synthesizable-fpga.md` §8). Given the FP32
divider's resource cost, replica count is bounded primarily by LUT
budget (each replica needs its own `q8_scale`/`q4_scale` dividers) long
before it would be bounded by DSP or BRAM — a real utilization report
from Level A will give the first real number here; until then, treat
"how many replicas fit" as unknown, not estimated.
