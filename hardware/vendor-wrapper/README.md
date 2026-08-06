# vendor-wrapper/

Interface-only RTL skeletons for integrating `rtl/membrane_quant_stream_top.sv`
(the cosimulation-verified, 520,000-transaction-tested quantization
datapath) behind standard bus interfaces, so a real board integration
never has to modify the verified core itself.

**Verification status**: these four files, together with the full
existing `rtl/` hierarchy, were elaborated under yosys 0.33
(`hierarchy -top membrane_core_wrapper`) and resolve cleanly — 0 new
errors or warnings attributable to these files (all 46 unique warnings
in the combined elaboration are pre-existing, from the already-verified
`rtl/` modules; none originate from the files in this directory). This
confirms the port/parameter wiring is structurally consistent, **not**
that the design synthesizes, closes timing, or works on real silicon —
no place-and-route tool or FPGA board was used. See
`docs/phase8-hardware-validation-plan.md` Level A for what would
actually confirm that.

## Files

| File | Role | Status |
|---|---|---|
| `axi_stream_adapter.sv` | Wraps the core's native valid/ready/mode/id/data ports behind a standard AXI4-Stream slave+master pair | Skeleton, elaborates under yosys, not synthesized/simulated on real hardware |
| `axi_lite_ctrl.sv` | Minimal AXI4-Lite control/status register responder (soft reset, ready/FIFO status, error/transaction counters) | Skeleton, hand-written FSM, **not verified against a real AXI4-Lite protocol checker** — flagged as a TODO in the file itself |
| `dma_command_if.sv` | Platform-independent `interface` (SystemVerilog construct) defining a command/completion handshake a real DMA engine would implement | Signal-grouping only — no real DMA engine implemented or modeled |
| `membrane_core_wrapper.sv` | Top-level tying the above together behind one vendor-agnostic module boundary | Skeleton; ties verified pieces together, itself unverified beyond elaboration |

## Interface spec: `membrane_axi_stream_adapter`

| Signal | Direction | Width | Meaning |
|---|---|---|---|
| `aclk` | in | 1 | Clock (single clock domain assumed — no CDC in this skeleton) |
| `aresetn` | in | 1 | Active-low synchronous reset |
| `s_axis_tvalid/tready/tdata/tuser/tlast` | slave | 1/1/512/2+ID_WIDTH/1 | Host-to-core stream; `tuser` = `{mode[1:0], id[ID_WIDTH-1:0]}`; `tlast` expected always 1 (single-beat transactions) |
| `m_axis_tvalid/tready/tdata/tuser/tlast` | master | 1/1/512/2+ID_WIDTH/1 | Core-to-host stream, same framing |
| `m_axis_tuser_error` | master out | 1 | Passive status bit (`out_error`, per `rtl/membrane_quant_stream_top.sv`) — a numerically-exceptional (NaN/Inf) result, not a protocol error; see that module's own documentation for why this is not a retry/drop signal |

## Interface spec: `membrane_axi_lite_ctrl`

See the register map table in `axi_lite_ctrl.sv`'s own header comment
(CTRL/STATUS/ERR_CNT/TXN_CNT at offsets 0x00/0x04/0x08/0x0C). This is
the minimum register set needed for
`hardware/experiment-protocol.md`'s environment-capture and
loopback-DMA steps, not a complete production register map.

## Interface spec: `membrane_dma_cmd_if`

A parameterized SystemVerilog `interface` with `initiator`/`engine`
modports; see the file's own header for the command/completion field
list and the ordering-guarantee caveat that must be checked against
whatever real DMA engine a target platform provides.

## Integration checklist (for whoever wires this to a real board)

1. **Confirm clock strategy.** This skeleton assumes a single clock
   domain (`aclk`) for both the data and control paths. If the target
   vendor shell's DMA/AXI-Lite clocks differ, add real clock-domain-
   crossing logic — none exists here, and using this skeleton across
   clock domains without adding CDC would be a real bug, not a missing
   feature to configure around.
2. **Replace or verify `axi_lite_ctrl.sv`.** Either substitute a
   vendor-generated AXI4-Lite slave (recommended — e.g. Vivado's AXI
   Lite IP template) wired to the same register semantics, or run the
   hand-written version here through a real AXI4-Lite protocol checker
   before trusting it on hardware.
3. **Bind `membrane_dma_cmd_if` to the real platform's DMA engine.**
   XRT (Alveo) and OPAE (Intel) both expose their own descriptor/
   completion mechanisms — write a thin shim mapping the target
   platform's real interface onto this file's `engine` modport, rather
   than expecting the platform to natively speak this exact handshake.
4. **Decide single-beat vs. multi-beat framing.** This skeleton assumes
   `tlast` is always 1 (one 512-bit beat per transaction, matching
   `membrane_quant_stream_top`'s native granularity). If the host DMA
   engine bursts multiple beats per descriptor, either configure it for
   single-beat transfers or add real multi-beat framing — not silently
   assumed.
5. **Wire real FIFO-status signals**, once exposed. `membrane_core_wrapper.sv`
   currently stubs `in_fifo_almost_full`/`out_fifo_almost_empty` to
   constants (see its own TODO comments) since
   `membrane_quant_stream_top` doesn't expose FIFO occupancy at its
   top-level ports today — a real integration wanting real backpressure
   visibility needs to add those output ports to the core itself first.
6. **Re-cosimulate with the wrapper in the loop.** Before trusting any
   of this on real hardware, extend `rtl/tb/tb_top_verilator.cpp` (or a
   copy of it) to drive `membrane_core_wrapper` through the AXI4-Stream
   ports instead of the core's native ports directly, and confirm the
   same golden-vector bit-exactness result holds through the adapter.
7. **Run Level A** (`docs/phase8-hardware-validation-plan.md`) before
   assuming any of the above is sufficient for a real bring-up.

## What this directory deliberately does not include

No vendor PCIe IP, no vendor DMA engine implementation, no vendor
AXI-Lite IP, no board-support package, no bitstream. See
`hardware/README.md` for why.
