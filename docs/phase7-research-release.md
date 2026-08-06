# Phase 7.1: Research Release

Baseline: commit `6f26b6b` (Phase 6.5, "perf: add out-of-core unified KV
simulator"). This phase produced **no new experimental results** — its
job was to take the already-verified Phase 0–6.5 work (462/462 unified
sweep scenarios, bit-exact quantization parity, FPGA cosimulation, near-
memory/CXL simulation) and package it as a research release a
researcher, engineer, investor, or hiring reviewer can understand
quickly, re-run, and verify — without introducing a single new headline
claim that wasn't already measured.

## What changed

- **`README.md`**: fully rewritten from a stale Phase 0 ("pre-alpha,
  lossless block store only") description to the specified section order
  (what/problem/idea/results/architecture/demo/reproduction/layout/
  limitations/roadmap/citation/license), with the five-capability
  first-screen summary and a sourced "Key results" table (artifact +
  doc section + reproduce command per row).
- **`docs/architecture.md`**: fully rewritten from Phase 0-era content to
  four Mermaid diagrams (end-to-end system, KV lifecycle, FPGA datapath,
  exact sparse retrieval path) matching the actual current code, plus a
  phase-history summary.
- **New `docs/reproduction.md`**: three levels (quick/model-backed/full),
  each with working directory, dependencies, expected time, expected
  output, and success signal.
- **New `docs/results-summary.md`**: a dense, sourced technical summary
  including the five null/negative findings, kept as visible as the
  positive ones.
- **New `docs/licensing.md`**: license boundary between MEMBRANE's own
  Apache 2.0 code, the MIT-licensed `llama.cpp` submodule, model
  artifacts, and captured traces — with unresolved questions (e.g.
  whether a captured activation trace is a "derivative work") flagged
  explicitly rather than asserted away.
- **New `docs/github-presentation.md`**: suggested repository
  description, topics, and social-preview text/layout (no image
  generated).
- **New `scripts/demo.sh`**: a single quick-start command (build → quant
  parity → FPGA Verilator cosim → small exact-retrieval scenario →
  summary), using only small committed fixtures.
- **New `scripts/verify-results.py`**: 13 automated checks
  cross-referencing README's headline numbers against the actual CSVs/
  docs/source files that back them.
- **New `scripts/generate-benchmark-manifest.py`** and
  **`benchmarks/MANIFEST.json`**: 33 tracked artifacts, each with
  path/phase/model/workload/label/SHA-256/generating command/related
  doc/status, deterministically regenerable.
- **New `scripts/prepare-release.sh`**: clean-tree, test suite,
  verify-results, manifest freshness, docs-link, large-file, and
  license-file checks, with `--dry-run`.
- **New `paper/` skeleton**: `main.md`, `references.bib` (empty by
  design), `figures/README.md` — Related Work section left with
  `[citation needed]` markers throughout.
- **New release metadata**: `CITATION.cff`, `CHANGELOG.md`,
  `SECURITY.md`, `SUPPORT.md`, `CODE_OF_CONDUCT.md`; `CONTRIBUTING.md`
  updated to reflect the current multi-language project (was still
  describing C11-only, RAW/RLE-codec-only contribution rules).
- **README badges**: CI status (from the real `.github/workflows/ci.yml`
  workflow) and a static Apache 2.0 license badge — no benchmark or
  performance badges added.
- **`.gitignore`**: added `demo-output/` (scripts/demo.sh's working
  directory).

## A real inconsistency this phase found and fixed

Writing `scripts/verify-results.py` and running it against the freshly
written README caught a real transcription error: the README's first
draft stated "130x–405x" for KV-traffic reduction vs. full-scan-CXL, but
130x was actually the *compressed-baseline* ratio for one comparison, not
a full-scan-CXL ratio at all — conflated while summarizing two different
columns from `docs/phase6-unified-stress.md`'s §12 tables. The verifier
computed the real representative-point ranges directly from
`benchmarks/cxl-sim/unified-sweep.csv` (187.2x–404.7x vs. full-scan-CXL;
99.6x–215.3x vs. the compressed baseline) and flagged the mismatch;
README.md and `docs/results-summary.md` were corrected to the verified
figures before this phase was considered done. This is exactly the kind
of check `scripts/verify-results.py` exists to run on every future claim
change, not a one-time fix.

## Which results were highlighted

The seven rows in README's "Key results" table: the 462/462 unified
sweep completion, the 187x–405x (and 99.6x–215.3x vs. compressed)
bytes/token reduction, the separate 985.7x–1,281.1x 4K-context result,
exact retrieval's preserved precision/recall, the compute-floor-hidden
latency fraction, the 520,000-transaction FPGA cosimulation, the
100,000+-block quantization parity, and the pipeline-count hardware
sensitivity. Each was chosen because it is independently verifiable from
a single committed artifact — not because it was the most impressive
number available.

## Which claims were deliberately NOT used

- **"Production-ready" / "real CXL acceleration"** — explicitly disclaimed
  in README's status line and Technical Limitations section; no physical
  CXL/PCIe/FPGA hardware exists in this project.
- **Extrapolating SmolLM2-360M results from SmolLM2-135M**, or vice
  versa, anywhere a model-specific number is cited — every row in
  README's Key results table names which model(s) it covers.
- **A single unqualified "up to 1,281x" headline** — the two different
  sweeps (unified 128K×512 vs. the smaller 4K-context capacity-bound
  scenario) are kept as two separate, separately-sourced rows rather than
  merged into one number, since they measure different scenarios.
- **FPGA Fmax/power/area claims** — never measured (RTL cosimulation and
  yosys synthesizability are two different, both-disclosed claims; see
  README's Technical Limitations).
- **A literature-backed Related Work section** in `paper/main.md` —
  deliberately left as `[citation needed]` rather than populated with
  unverified or half-remembered citations; a real survey is explicitly
  scoped as future work (see `docs/results-summary.md` §7).

## Demo scope

`scripts/demo.sh --quick` (default): build, bit-exact quantization parity
(100,000+ blocks vs. ggml), the full 520,000-transaction FPGA Verilator
cosimulation, and a small committed-fixture exact-retrieval scenario
(`test_exact_engine`). Measured on the development machine: **~25
seconds** end-to-end in that specific run (a later re-measurement found
this varies roughly 25–50 seconds depending on whether the
llama.cpp-enabled build cache is warm — see
`docs/phase7-hardware-outreach.md`'s wording-correction note). `--full`
additionally runs the complete ctest suite
(28 tests); measured at **~85 seconds**. Neither mode downloads a model
or runs a multi-hour sweep — those are Level 2/3 in
`docs/reproduction.md`, run manually and explicitly. The quant-parity and
FPGA-Verilator steps are best-effort: each has a one-time environment
dependency (the `llama.cpp` submodule; a Verilator binary) that, if
missing, is reported as `SKIP` with instructions rather than a silent
omission or a hard failure.

## Reproduction levels

Three, documented in `docs/reproduction.md`: **Level 1** (unit tests,
quant parity, small simulator replay, ~2–5 min), **Level 2** (real
KV/attention trace capture and quality validation on both SmolLM2
checkpoints, ~30–90 min, requires obtaining/converting the real GGUF
checkpoints), **Level 3** (the full 128K×512 unified sweep via the
out-of-core backend, multi-hour, with checkpoint/resume and
`membrane-kv-exact-sim-verify` artifact-integrity verification).

## Verification results

Run at the end of this phase (see the "Full verification pass" section
of this document's accompanying task log):

- Release build: **28/28** tests passed.
- ASan+UBSan build: **30/30** tests passed.
- TSan build (under `setarch -R`): **30/30** tests passed.
- `scripts/demo.sh --quick`: all 4 steps PASS, ~25s in that run (see
  the note above — later measurements found ~25–50s depending on cache
  state).
- `scripts/verify-results.py`: **13/13** checks passed (after the fix
  described above).
- `scripts/prepare-release.sh --dry-run`: all non-git-state checks
  passed; the clean-tree and docs-link checks correctly failed while
  this phase's own changes were still uncommitted, and passed once
  committed (see below).
- Negative test: `scripts/verify-results.py` against a deliberately
  corrupted `benchmarks/MANIFEST.json` (one SHA-256 byte flipped) —
  correctly reports the exact artifact and hash mismatch, exit code 1.
- Negative test: `scripts/verify-results.py` with a required artifact
  (`benchmarks/cxl-sim/unified-sweep.csv`) temporarily renamed away —
  correctly reports it missing, exit code 1.
- Documentation links: `scripts/prepare-release.sh`'s link checker found
  and this phase fixed a real gap (this document itself, initially
  linked from README.md and docs/architecture.md before it existed).

## Remaining gaps

- `paper/main.md`'s Related Work section is a placeholder
  (`[citation needed]` throughout) — a real literature survey is
  explicitly future work, not attempted here.
- `docs/licensing.md` flags one genuinely unresolved question (whether a
  captured activation trace is a "derivative work" of the model that
  produced it) rather than asserting a legal conclusion — this remains
  open.
- No figures have been rendered yet for `paper/figures/` — the README
  lists exactly what each should show and its data source
  (`paper/figures/README.md`), but generating them was out of scope for
  this phase (no new artifacts, only presentation of existing ones).
- GitHub repository description/topics/social-preview
  (`docs/github-presentation.md`) are suggestions only — applying them
  requires GitHub UI/API access this phase did not have.
- The out-of-core simulator's ~10% memory-budget overshoot slack factor
  (Phase 6.5) remains specific to this development machine and unverified
  elsewhere — unchanged by this phase, disclosed again here for
  visibility.
