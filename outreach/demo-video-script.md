# Demo video script (5–7 minutes)

A recording plan, not a recording — nothing here has been filmed.
Every segment below only shows what can genuinely be shown live on
screen today (a real terminal running real commands against this
repository); segments about hardware that doesn't exist yet are
explicitly scripted as **spoken future work**, never staged or implied
as a live demo.

## Segment 1 — Problem (45s, talking head or slides, no screen capture needed)

"LLM inference servers are usually compute-rich but memory-poor. KV-cache
size grows linearly with context length and concurrent requests, and
GPU memory — not compute — is often what runs out first. I'm going to
show a research prototype, MEMBRANE, that explores whether a per-block
memory decision engine can help, and where it explicitly doesn't."

## Segment 2 — Repository overview (60s, screen capture: browsing GitHub or a local clone)

- Show `README.md`'s first screen (the five-capability list, the status
  disclaimer).
- Scroll to the "Key results" table — point out that every row cites a
  source artifact and a doc section.
- Briefly show `docs/architecture.md`'s four diagrams.

*Script note: say explicitly, on camera, "every number in this table
links to a real file in this repo" — don't just imply it.*

## Segment 3 — Quick demo, live (90s, screen capture: real terminal)

```bash
./scripts/demo.sh --quick
```

- Let it run on camera, unedited, showing the four real steps (build,
  quant parity, FPGA Verilator cosim, exact-retrieval scenario) and
  their PASS lines.
- Point out the elapsed time (roughly 25–50 seconds depending on
  whether the build cache is warm) and that no model was downloaded.

*Script note: this is the one segment that is a completely live,
unedited demonstration — no cuts that could hide a failure.*

## Segment 4 — FPGA RTL (60s, screen capture: code + terminal)

- Show `rtl/membrane_quant_stream_top.sv`'s port list briefly.
- Show (or re-run on camera, since it only takes ~8s per the numbers
  already in `docs/reproduction.md`) the Verilator cosimulation log
  ending in "520000 transactions, 0 fails."
- **Say explicitly on camera**: "this is a simulation — cosimulated
  against the same CPU math it's meant to accelerate, confirmed to
  elaborate under yosys, but never placed, routed, or run on a real
  FPGA board. That's exactly what this outreach is asking for help
  with."

## Segment 5 — Unified simulator (60s, screen capture: CSV/terminal)

- Show `benchmarks/cxl-sim/unified-sweep.csv`'s row count (462) and a
  few columns.
- Briefly explain: discrete-event simulation, 128K context, 512
  concurrent sequences, calibrated from real captured attention traces.
- **Say explicitly**: "this models queueing and contention — it is not
  a measurement from a real CXL device. No real CXL hardware exists in
  this project."

## Segment 6 — Results (45s, slides or screen capture of `paper/tables/major-results.md`)

- 187x–405x KV-traffic reduction (name which baseline, which model).
- 520,000-transaction, zero-mismatch cosimulation.
- 100,000+-block bit-exact quantization parity.

## Segment 7 — Negative findings (45s, slides or `docs/results-summary.md` §4 on screen)

Read out, plainly, without softening:
- Blind lossless compression fails on real KV data.
- PCIe FPGA offload is a net loss at small batch sizes.
- Naive KV eviction breaks recall-shaped prompts.
- The 10ms p99 target is never met — bounded by compute, not retrieval.
- Micro-batching shows no measurable benefit.

*Script note: this segment should not feel like an apology — frame it
as "here's what a disclosure-first research project looks like."*

## Segment 8 — Requested hardware access (30–45s, talking head)

"Everything hardware-adjacent in this project today is either a
cosimulation or an assumption. What I'm asking for: FPGA board +
toolchain access, and if possible a CXL platform, to actually answer
whether this design works on real silicon. Details, a scoped 2-4 week
plan, and exactly what claims I can and can't make at each stage are in
the repository — links in the description."

## Post-production notes

- Total target length: 5–7 minutes; the segment times above sum to
  ~7.5 minutes including talking-head segments — trim segment 2 or 6
  first if over time, not segment 3 (the live demo) or segment 7 (the
  negative results).
- Do not add background music or a "hype" edit style (fast cuts, zoom
  punches) — this is a technical credibility piece, not a promotional
  video.
- Caption or verbally flag every simulated/assumed claim at the moment
  it's shown, not just once at the start.
