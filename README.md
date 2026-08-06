# MEMBRANE Research

This repository is the full research record for MEMBRANE: every phased
experiment, simulator, exploratory FPGA datapath, the project paper, and
the outreach package built around it. It is **not** the maintained
product. The maintained, production MEMBRANE runtime lives at
[kadireren7/membrane](https://github.com/kadireren7/membrane) and is not
duplicated here — this repository references it by commit, never by
copy.

If you're looking for the runtime itself (build it, run it, integrate
it), go to `kadireren7/membrane`. If you're evaluating the research
behind it — what was tried, what worked, what didn't, and what the raw
evidence actually says — you're in the right place.

## Why two repositories

`kadireren7/membrane` carries only what a user or contributor needs to
build and run the maintained product, plus the CI, security scanning
(CodeQL), and review tooling (CodeRabbit) that guard it. Every
experimental branch, simulator, negative result, and abandoned
candidate that MEMBRANE's own development produced lives here instead,
so the product repository stays legible and the research record stays
complete. Nothing here is deleted when the direction of an experiment
changes — the `archive/` folder inside each experiment directory
preserves every phase's original document, even after that phase is
superseded.

The full reasoning behind this split, including the earlier decision
against splitting at all and why it was revisited, is documented in
`kadireren7/membrane`'s `docs/repository-boundary.md`.

## Structure

```
experiments/     phased research records (see below)
rtl/experimental/ exploratory RTL that never shipped to production, plus
                  its own Verilator testbenches
tools/            simulators and analysis tools built for this research
                  (KV-cache exact/working-set/variance sims, CXL/near-
                  memory sim, FPGA runtime harness, policy export)
benchmarks/       traces, prompts, and result sets the tools/ above
                  consume or produce
hardware/         real-FPGA validation plan: board targets, protocol,
                  risk register — planning documents, not measured
                  hardware results (see hardware/README.md)
paper/            the project paper: source, figures, tables, claim
                  audit, build script
outreach/         research communication package: technical brief,
                  talk materials, AI-assistance disclosure, and the
                  verifier that checks every claim in it against the
                  repository
docs/             phase-by-phase research narrative (phase2 through
                  phase8) written as the work happened
provenance/       how this repository's content maps back to its
                  kadireren7/membrane source branches, with SHA256
                  verification
career/           evidence-of-ownership and interview-preparation
                  material tied to this research record
scripts/          experiment drivers and result-verification tooling
```

## How to read an experiment

Each directory under `experiments/` follows the same shape:

- `README.md` — index: executive summary, phase timeline, what (if
  anything) reached production, and links to everything else.
- `methodology.md` — test design, exactness rules, toolchain, and the
  measurement-classification discipline (below).
- `results/canonical/` — the result artifacts every claim in the README
  is drawn from, plus `results/schemas/README.md` documenting their
  fields.
- `reproduction/README.md` — exact commands to reproduce those results,
  including any known gap in doing so (disclosed, not glossed over).
- `archive/` — every phase document in its original form, preserved
  even where a later phase supersedes its conclusion.

Two experiments are fully restructured into this shape so far:
[`EXP-FPGA-DIV-001`](experiments/EXP-FPGA-DIV-001/README.md) (the
exact-radix-4 Q4_0 divider that *did* reach production — see its
"Provenance" section for the merge record) and
[`EXP-FPGA-DIV-002`](experiments/EXP-FPGA-DIV-002/README.md) (the
related Q8_0 dual-divider and scheduler investigation, currently at
`CONTINUE`, nothing merged).

## Measurement classification

Every quantitative claim in this repository — in a README, a paper
table, an outreach document, or a raw result file — is labeled one of:

- **MEASURED_BY_TOOL** — a real tool (Verilator, Yosys) produced this
  number against real input. Still not physical hardware unless stated.
- **SIMULATED** — a software model stands in for hardware or a system
  that wasn't run directly (e.g. the retirement-pressure taxonomy
  model, the CXL near-memory simulator).
- **ESTIMATED** — derived from measured or simulated numbers via an
  explicit, disclosed method (e.g. a delta between two equally-inflated
  synthesis counts).
- **UNAVAILABLE** — the intended measurement did not complete (most
  commonly a synthesis timeout) and is reported as missing, never as a
  zero standing in for "not measured."

No result anywhere in this repository implies a physical FPGA board,
physical CXL hardware, measured Fmax, timing closure, or measured
power unless explicitly labeled as such — `hardware/` documents a
*plan* for obtaining real numbers, not results.

## AI-assistance disclosure

MEMBRANE's implementation, experiments, and this repository's own
documentation were built with heavy use of AI coding agents (Claude
Code) under Kadir Eren Altıntaş's direction. What the agents did,
what Kadir directed and decided, and what automated verification
does and doesn't cover, is disclosed in full in
[`outreach/ai-assistance-disclosure.md`](outreach/ai-assistance-disclosure.md).
AI agents are not described anywhere in this repository as independent
authors or autonomous project owners.

## Provenance

Every file in this repository that originated in `kadireren7/membrane`
is tracked back to its exact source branch, commit, and content hash in
[`provenance/import-manifest.json`](provenance/import-manifest.json)
and [`provenance/source-map.md`](provenance/source-map.md). No source
branch was deleted or rewritten by this migration.

## License

Apache License 2.0 — see [`LICENSE`](LICENSE).

## Citation

See [`CITATION.cff`](CITATION.cff). For the maintained runtime, cite
`kadireren7/membrane` instead.
