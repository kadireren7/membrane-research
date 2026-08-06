# EXP-FPGA-DIV-002 canonical result schemas

Field-level documentation for the recurring artifact shapes in
`../canonical/`. Not a formal JSON Schema file (the repository's own CI
schema-validation step checks structural well-formedness — valid JSON,
non-empty CSV headers/rows — not full per-field schema compliance,
disclosed as a real, current limitation of the validator).

## `*-correctness.json`

```
{
  "<variant-key>": {
    "label": "<human-readable variant name>",
    "classification": "MEASURED_BY_TOOL",
    "transactions_checked": <int>,
    "fails": <int>,
    "result": "PASS" | "FAIL",
    "overall_cycles_per_txn": <float>
  },
  ...
  "_meta": {
    "tool": "<Verilator version + testbench file>",
    "classification": "SIMULATED (Verilator cosim against golden C reference)",
    "total_variants_failed": <int>
  }
}
```

## `*-performance.csv`

One row per (variant, profile, mode) triple:

```
variant,profile,mode,count,cycles_per_txn,accepted_per_cycle,min,mean,p50,p95,p99,max,throughput_txn_per_cycle
```

## `*-synthesis.csv`

One row per (target, flow) pair, `flow` in `generic|ecp5|elab`:

```
target,label,flow,classification,total_cells,LUT4,CCU2C,PFUMX,L6MUX21,TRELLIS_FF,TRELLIS_IO,$_DFF_P_,$_DFF_PP0_,$_MUX_,$_NOT_,$_AND_,$_OR_,$_XOR_
```

`classification` is `MEASURED_BY_TOOL` or `UNAVAILABLE` (timeout) — a
row with `UNAVAILABLE` has all cell-count fields blank, never zero
(zero would falsely imply a real, measured empty design).

## `b4-retirement-profile.csv` (Phase B4-specific)

One row per traffic profile, with per-retirement-state cycle counts and
fractions (`cyc_<state>`, `frac_<state>` for each of the 10 states
`methodology.md` documents), plus `accepted`, `retired`, `wrap_events`,
and per-mode `mean_latency_*` columns. `classification: SIMULATED`
(software reference model, not RTL-instrumented — see `methodology.md`).

## `<phase>-run-manifest.json` / `<phase>-promotion-record.json`
(Phase B4-specific)

Provenance records — see `results/canonical/b4-run-provenance.md` for
the full field list and `provenance/repository-contract.md` at this
repo's root for how promotion validation works.
