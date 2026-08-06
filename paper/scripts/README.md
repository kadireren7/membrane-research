# paper/scripts

Reproducible generation for everything in `paper/figures/generated/` and
`paper/tables/` — no figure or table number in the manuscript is
hand-typed; every one is derived from a committed CSV/JSONL artifact (or,
for the related-work comparison, from `paper/related-work-matrix.md`).

- **`svg_chart.py`** — tiny, dependency-free SVG chart helper (bar/line/
  scatter). Not matplotlib/pandas: this environment has no network
  access to install them, and this project favors minimal dependencies
  generally. Produces real vector graphics from real input data.
- **`generate-figures.py`** — the 7 required figures (system
  architecture schematic, bytes/token comparison, capacity across
  device sizes, p99 vs. host-cache size, pipeline-count sensitivity,
  oracle-vs-predictor gap, mixed-precision quality/capacity Pareto),
  reading `benchmarks/cxl-sim/*.csv` and `benchmarks/cxl-sim/quality-
  reverify/*.jsonl` directly. `--check` diffs against the committed
  output without overwriting it.
- **`generate-tables.py`** — the 6 required tables (major results,
  hardware datapath verification, unified stress results, negative-
  result summary, limitations, related-work comparison), same
  `--check` convention.
- **`verify-paper.py`** — the paper-specific audit (see its own
  docstring): manifest coverage, figure/table input consistency,
  135M/360M mixups, partial/superseded data usage, REAL/SIMULATED label
  correctness, bibliography completeness, leftover citation-needed
  markers, prohibited overclaim phrases.

## Known limitation: SVG, not PDF

This environment has no `matplotlib`, no working `pip install` (PEP 668
externally-managed, no venv set up for this project), and no
`rsvg-convert`/`inkscape`/`cairosvg` to convert SVG to PDF. Figures are
generated as standalone SVG files. `paper/build.sh` checks for an
SVG->PDF converter and uses one if present; if not, it reports this
limitation explicitly (see `docs/phase7-academic-paper.md`) rather than
faking a PDF or silently omitting figures from the LaTeX build.

## Regenerating everything

```bash
python3 paper/scripts/generate-figures.py
python3 paper/scripts/generate-tables.py
python3 paper/scripts/verify-paper.py
```

Or all at once via `paper/build.sh`.
