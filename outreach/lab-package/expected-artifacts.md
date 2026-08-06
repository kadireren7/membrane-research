# Expected artifacts

What a completed run at each level should produce, and where it would
go if merged back into this repository.

## Level A

- Vendor place-and-route report (timing summary, resource utilization,
  pre-bitstream power estimate) — raw tool output preferred, plus a
  short summary.
- Post-route simulation log showing pass/fail against the existing
  golden vectors.
- An updated `hardware/board-targets.md` "Clock target" section with
  the real (not assumed) Fmax result, attributed to the run that
  produced it.

## Level B

- One `hardware/results-schema.json`-conformant JSON record per
  experiment in `hardware/experiment-protocol.md` (steps 6-13), with
  `result_label: "REAL_HARDWARE"` — see
  `hardware/results-example.json` for the shape (that file is a
  fictional illustration only, never itself a real result).
- Raw logs per `hardware/experiment-protocol.md`'s illustrative
  `hardware/runs/<date>/...` paths (the actual directory structure is
  up to whoever runs it — the schema is what matters for consistency).
- Bitstream file (or at minimum its SHA-256, recorded in every result
  record) for reproducibility.

## Level C

- Real bandwidth/p99 measurements, same schema/labeling convention as
  Level B.
- A written comparison against `docs/phase6-cxl-near-memory.md`'s
  assumed link-latency/bandwidth figures.

## What happens to these artifacts

If a lab is willing, real results would be proposed as a pull request
(or shared directly with Kadir to incorporate) updating:
- `docs/phase8-hardware-validation-plan.md` (marking the relevant level
  complete, with real numbers replacing the plan's assumptions).
- `outreach/hardware-claim-gates.md` (updating the gate table to reflect
  which gates are now actually passed).
- `README.md` / `paper/main.md` (only once the corresponding gate is
  passed, per `outreach/hardware-claim-gates.md` — no claim gets
  promoted ahead of its evidence).

No result is required to be positive to be valuable — a real, honest
"timing does not close" or "the CXL simulation's assumptions were wrong
by 3x" is exactly the kind of finding this project's existing
negative-results discipline (`docs/results-summary.md` §4) already
values.
