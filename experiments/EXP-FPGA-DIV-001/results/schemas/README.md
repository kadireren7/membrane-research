# EXP-FPGA-DIV-001 canonical result schemas

Field-level documentation for the recurring artifact shapes in
`../canonical/`.

## `*-differential.json`

```
{
  "cases_run": <int>,
  "mismatches": <int>,
  "classification": "MEASURED_BY_TOOL",
  ...
}
```

Exact field set varies slightly by phase (see each phase's own archived
document for the specific keys its own differential run emits) — this
experiment predates the standardized manifest format Phase B4 of
EXP-FPGA-DIV-002 later introduced.

## `*-full-datapath.json`

Full-datapath Verilator cosimulation summary: transaction count, fail
count, per-mode latency statistics, real measured wall-clock time.

## `*-synthesis.csv`

Yosys `stat` cell-count breakdown per (target, flow), same convention as
EXP-FPGA-DIV-002's own schema (see that experiment's own
`results/schemas/README.md`) — `classification` is `MEASURED_BY_TOOL` or
`UNAVAILABLE`, never a zero standing in for a timeout.

## `*-comparison.md`

Hand-written narrative tables comparing candidates for one phase — not
machine-generated, but every number in them is drawn directly from the
same-phase `.json`/`.csv` artifact next to it, not independently
re-derived.
