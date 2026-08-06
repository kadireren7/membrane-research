# Reproduction

Full reproduction guide for everything in this repository. For the
maintained runtime's own quick verification (unit tests, sanitizers,
ggml quantization parity, RTL cosimulation against the *production*
top-level module) see `kadireren7/membrane`'s own `docs/reproduction.md`
Level 1 — that content is not duplicated here; it depends only on files
that live in `membrane`, and reproducing it needs a `membrane` checkout,
not this one.

Everything below assumes:

```bash
git clone https://github.com/kadireren7/membrane-research
cd membrane-research
```

Several steps additionally need a `membrane` checkout for shared
production source or model weights not duplicated here — each such step
says so explicitly.

---

## Level 2 — Model-backed KV/attention verification

**Goal:** validate real KV/attention trace capture and quality-
preserving quantization against actual llama.cpp inference on both
SmolLM2 checkpoints.

**Dependencies:** a `membrane` checkout, built with
`-DMEMBRANE_ENABLE_LLAMA=ON` (see that repository's Level 1.3), plus two
real GGUF checkpoints under `membrane/models/` (not committed anywhere —
gitignored in both repositories; see `membrane`'s `docs/licensing.md`).
The tool binaries themselves (`membrane-kv-capture`,
`membrane-kv-attn-trace-capture`, `membrane-kv-quality`) are built from
this repository's `tools/`.

```bash
# One-time: obtain and convert the real checkpoints (HuggingFaceTB/
# SmolLM2-135M-Instruct and HuggingFaceTB/SmolLM2-360M-Instruct) via
# membrane's vendored llama.cpp converter -- see membrane's own
# docs/reproduction.md Level 2 for the exact convert_hf_to_gguf.py
# invocation.

# Build this repository's own capture/quality tools against the
# membrane checkout's headers/libs:
cmake -S tools/membrane-kv-capture -B build/kv-capture \
  -DMEMBRANE_ROOT=/path/to/membrane -DCMAKE_BUILD_TYPE=Release
cmake --build build/kv-capture -j

./build/kv-capture/membrane-kv-capture \
  --model /path/to/membrane/models/SmolLM2-135M-Instruct-f16.gguf \
  --prompt-file benchmarks/kv/prompts/natural.txt \
  --out /tmp/repro-135m.kvdump --n-tokens 128
```

- **Expected time:** ~10-30 seconds per model on CPU (135M), a few
  minutes for 360M.
- **Success signal:** process exits 0, output file non-empty; compare
  shape against the committed `benchmarks/cxl-sim/traces/*.kvtrace`.

Quality validation (`membrane-kv-quality`, both models, 3 runs each for
determinism) follows the same build-then-run pattern against
`benchmarks/kv/prompts/recall.txt` and
`benchmarks/results/phase3-kv-quality/quality.jsonl` as its expected-
shape reference — see `docs/phase3-kv-q8-quality.md` for the full
original run this reproduces.

**Known gap**: the `-DMEMBRANE_ROOT=` build flag above is the intended
fix for these tools now building against an external `membrane`
checkout instead of a co-located source tree; it has not yet been
independently re-verified end to end since the split — tracked in
`ROADMAP.md` Track 4, same disclosed gap as the FPGA divider
experiments below.

---

## Level 3 — Full research reproduction (multi-hour)

**Goal:** reproduce the full 128K-context x 512-concurrency unified
sweep (462 scenarios across both models) using the out-of-core simulator
backend, including checkpoint/resume and artifact integrity
verification.

**Dependencies:** this repository's own build only (`-DMEMBRANE_ENABLE_LLAMA=OFF`
is sufficient — the unified sweep only needs the already-committed
`.attntrace` captures, not live model inference).
**Expected disk:** ~1-2 GiB transient (`.attntrace3` out-of-core
synthetic traces, regenerated deterministically, gitignored).
**Expected RAM:** a few hundred MiB-2 GiB (`--memory-budget-mib`),
verified on a 5.6 GiB machine — see `docs/phase6-out-of-core-simulator.md`
§3.
**Expected time:** multi-hour (this sweep took multiple real sessions to
complete originally, including real OOM-driven restarts — see
`docs/phase6-unified-stress.md`'s "Completion history" section).
Checkpoint/resume makes it safe to interrupt.

```bash
cmake -S tools/membrane-kv-exact-sim -B build/kv-exact-sim -DCMAKE_BUILD_TYPE=Release
cmake --build build/kv-exact-sim -j

./build/kv-exact-sim/membrane-kv-exact-sim \
  --trace-135m-long benchmarks/cxl-sim/traces/SmolLM2-135M-Instruct-f16-long.attntrace \
  --trace-360m-long benchmarks/cxl-sim/traces/SmolLM2-360M-Instruct-f16-long.attntrace \
  --out benchmarks/cxl-sim/unified-sweep.csv \
  --checkpoint benchmarks/cxl-sim/unified-sweep.ckpt \
  --backend streaming --memory-budget-mib 768 --trace-cache-mib 256 \
  --workers 1
```

Interrupt and re-run the identical command to resume from the last
checkpointed scenario (trace/config-hash staleness detection refuses and
restarts fresh on any mismatch, never silently reuses a stale
checkpoint). Verify with:

```bash
./build/kv-exact-sim/membrane-kv-exact-sim-verify \
  --checkpoint benchmarks/cxl-sim/unified-sweep.ckpt \
  --csv benchmarks/cxl-sim/unified-sweep.csv
```

- **Success signal:** `0 problem(s) found; 462 unique scenarios in
  checkpoint, 462 CSV data rows`.
- **Full detail**: `docs/phase6-out-of-core-simulator.md`.

### 3.1 Hardware-sensitivity sub-sweep (smaller)

```bash
cmake -S tools/membrane-cxl-sim -B build/cxl-sim -DCMAKE_BUILD_TYPE=Release
cmake --build build/cxl-sim -j
./build/cxl-sim/membrane-cxl-sim \
  --trace benchmarks/cxl-sim/traces/SmolLM2-135M-Instruct-f16.kvtrace \
  --out /tmp/repro-hardware-sensitivity.csv
```

Compare against `benchmarks/cxl-sim/unified-sweep-hardware-sensitivity.csv`.
Success signal is the tool's own printed success-criteria section,
reported as-is — see `docs/phase6-cxl-near-memory.md` §12.

---

## FPGA divider experiments (EXP-FPGA-DIV-001, EXP-FPGA-DIV-002)

Each experiment owns its own reproduction guide with exact
`--phase`/`--quick`/`--full` commands:

- [`experiments/EXP-FPGA-DIV-001/reproduction/README.md`](experiments/EXP-FPGA-DIV-001/reproduction/README.md)
- [`experiments/EXP-FPGA-DIV-002/reproduction/README.md`](experiments/EXP-FPGA-DIV-002/reproduction/README.md)

Both disclose the same known gap: `MEMBRANE_PRODUCTION_ROOT` is the
intended path-resolution fix for the experimental/production RTL split
across two repositories now, not yet independently re-verified — see
`ROADMAP.md` Track 4.

## Synthesis experiments

Yosys 0.33 generic and `synth_ecp5` synthesis is invoked by the FPGA
divider experiment scripts above (see each experiment's own
`methodology.md` "Synthesis methodology" section for exact flags,
including Phase B4's isolated-wrapper `-noshare` approach). No separate
synthesis-only reproduction path exists outside those scripts.

## Canonical result promotion and provenance

`EXP-FPGA-DIV-002`'s Phase B4 provenance-safety layer
(`scripts/run-exp-q8-divider-002.sh --promote-results`,
`scripts/gen-run-manifest.py`,
`scripts/verify-exp-q8-divider-002-results.py`) is itself reproducible —
see `experiments/EXP-FPGA-DIV-002/reproduction/README.md` and
`experiments/EXP-FPGA-DIV-002/results/canonical/b4-run-provenance.md`
for the full mechanism.

This repository's own migration provenance (not experiment provenance)
is verified separately — see `provenance/import-manifest.json` and
`provenance/source-map.md`.

## Paper

```bash
paper/build.sh
python3 paper/scripts/verify-paper.py
```

`verify-paper.py` checks every quantitative claim in `paper/main.md`
against a real source artifact in `experiments/*/results/canonical/` or
`benchmarks/results/`; a claim it cannot trace is reported as a failure,
not silently allowed. See `paper/claim-audit.md` for the last full audit
this tool backs.

## Outreach

```bash
python3 scripts/verify-outreach.py
```

Checks every factual claim in `outreach/*.md` against the same
source-of-truth artifacts the paper verifier uses. Moved here from
`membrane` alongside `outreach/` itself (Decision 4,
`provenance/repository-contract.md`) — it has no maintained-repo release
role to fill anymore.
