# Paper figures

Rendered figures exist under `paper/figures/generated/*.svg`, produced by
`paper/scripts/generate-figures.py` from the committed CSV/JSONL
artifacts named below — never hand-authored. Regenerate with:

```bash
python3 paper/scripts/generate-figures.py
```

| Figure | Paper section | Source | File |
|---|---|---|---|
| System architecture | §3 System design | hand-laid-out schematic (not data-driven — matches `docs/architecture.md` diagram A) | `system_architecture.svg` |
| Bytes/token comparison | §5 RQ5 | `benchmarks/cxl-sim/unified-sweep.csv` | `bytes_per_token_comparison.svg` |
| Capacity across device sizes | §5 RQ1 | `benchmarks/cxl-sim/unified-sweep.csv` | `capacity_across_device_sizes.svg` |
| p99 vs. host-cache size | §5 RQ6 | `benchmarks/cxl-sim/unified-sweep.csv` | `p99_vs_host_cache.svg` |
| Pipeline-count sensitivity | §5 RQ7 | `benchmarks/cxl-sim/unified-sweep-hardware-sensitivity.csv` | `pipeline_sensitivity.svg` |
| Oracle vs. real predictor gap | §5 RQ5 | `benchmarks/cxl-sim/unified-sweep.csv` | `oracle_vs_predictor_gap.svg` |
| Mixed-precision quality/capacity Pareto | §5 RQ2 | `benchmarks/cxl-sim/unified-sweep.csv` + `benchmarks/cxl-sim/quality-reverify/*.jsonl` | `quality_capacity_pareto.svg` |

The full four-diagram Mermaid set (end-to-end system, KV lifecycle, FPGA
datapath, exact retrieval sequence) lives in `docs/architecture.md` and
is not duplicated here as a static image — `system_architecture.svg`
above is a simplified schematic for the paper specifically, not a
replacement for those diagrams.

## Known limitation: SVG, not PDF

This environment has no `matplotlib`, no working `pip install` (PEP 668
externally-managed, no venv set up for this project), and no
`rsvg-convert`/`inkscape`/`cairosvg` to convert SVG to PDF. `paper/main.tex`
includes each figure via `\IfFileExists{...pdf}{...}{fallback text}`, so
it still compiles cleanly (once a LaTeX toolchain is available) even
without a PDF version of these figures — see `paper/build.sh` and
`docs/phase7-academic-paper.md` for the full disclosure.

No figure should ever be generated from anything other than the
committed CSV/JSONL artifacts — see `benchmarks/MANIFEST.json` for the
SHA-256-tracked source of each, and `paper/scripts/verify-paper.py` for
the automated check that figures match their declared sources.
