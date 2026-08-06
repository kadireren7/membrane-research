# Experiment checklist

The concrete, ordered checklist for a lab that has decided to help at
some level. Full detail (commands, expected invariants, artifact paths,
pass/fail rules) is in `hardware/experiment-protocol.md` — this is the
short, actionable checklist version.

## Level A checklist

- [ ] Elaborate `rtl/membrane_quant_stream_top.sv` (or
      `hardware/vendor-wrapper/membrane_core_wrapper.sv`) in your vendor
      toolchain.
- [ ] Run synthesis targeting your available device part.
- [ ] Run place-and-route at 100 MHz, 200 MHz, and 300 MHz (this
      project's disclosed assumption range — report the real achieved
      Fmax even if it's outside this range).
- [ ] If timing doesn't close: note where in the design the critical
      path lies (this project's own analysis names
      `membrane_fp_divider` as the most likely culprit — confirm or
      correct that).
- [ ] Run post-route simulation against the same golden vectors
      `rtl/tb/tb_top_verilator.cpp` already uses (regenerate via
      `docs/reproduction.md` Level 1.4).
- [ ] Record resource utilization (LUT/FF/BRAM/DSP) and the tool's
      pre-bitstream power estimate.
- [ ] Report back: pass or fail, real numbers either way.

## Level B checklist (only after Level A succeeds)

- [ ] Environment capture, board identification, bitstream hash.
- [ ] Loopback DMA test (no MEMBRANE logic in the path yet).
- [ ] Wire `hardware/vendor-wrapper/` behind your board's real shell/DMA
      engine (see that directory's integration checklist).
- [ ] Known-vector test (small fixed set) — byte-exact match required.
- [ ] 100K-random-block parity test — 0 unexplained mismatches required
      for any "hardware bit-exact" claim.
- [ ] Sustained throughput, queue-depth scaling, batch-size scaling.
- [ ] CPU-vs-FPGA comparison on the same host.
- [ ] Power and thermal measurement under sustained load.
- [ ] Failure injection + reset/recovery test.
- [ ] Report back via `hardware/results-schema.json`-conformant
      records, `result_label: "REAL_HARDWARE"`.

## Level C checklist (independent of A/B, if a CXL platform is available)

- [ ] Memory-tier integration smoke test (allocate/read/write the CXL
      device's exposed memory, no MEMBRANE logic yet).
- [ ] Port the exact-retrieval predictor/prefetch/compulsory-fetch logic
      to issue real reads/writes against the device.
- [ ] Replay a real captured trace (scaled to what the platform
      supports) and record real bandwidth/p99.
- [ ] Compare against `docs/phase6-cxl-near-memory.md`'s assumed CXL
      figures — report the delta, in whichever direction it goes.

## After any level

- [ ] Send results back (raw logs + schema-conformant JSON preferred
      over a summary) — see `outreach/collaboration-scope.md` for how
      attribution/disclosure of the results would work.
