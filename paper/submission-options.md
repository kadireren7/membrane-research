# Submission positioning

Research notes on where this manuscript could plausibly be submitted,
and what stands between the current draft and a real submission at each.
**No acceptance probability is claimed anywhere below** — this is a fit
and readiness analysis, not a prediction. Venue names and general scope
are real, well-known venues in their respective communities; specific
submission deadlines are deliberately **not** stated here, since CFP
dates change year to year and this document was not produced by
checking each venue's live, current call for papers. Before submitting
anywhere, verify the actual current deadline/format/page-limit
requirements against that venue's own official website.

## Category 1: Systems (OSDI, SOSP, EuroSys)

- **Topic fit**: strong. PagedAttention/vLLM (SOSP 2023) and InfiniGen
  (OSDI 2024) — two of this paper's closest related works — were
  published in exactly this category. KV-cache memory management is an
  actively reviewed topic here.
- **Evidence bar**: these venues expect real-system implementation and
  measurement — typically a working integration with a real serving
  stack (or at least a real standalone system under real load), real
  end-to-end throughput/latency numbers, and comparison against strong
  real baselines at production-relevant model scale (7B+ typically).
- **Real-hardware requirement**: high for the systems claims (CPU/GPU
  behavior must be real); the CXL/FPGA components would likely need to
  be scoped as "future work" or removed entirely for this venue, since
  systems venues are typically skeptical of simulation-only hardware
  claims presented as a systems contribution.
- **Is the current project ready?** No. MEMBRANE's exact-retrieval and
  mixed-precision runtime are evaluated in simulation/CPU-benchmark at
  135M/360M scale, not as an integrated component of a real multi-tenant
  serving system at production model scale. This is the single largest
  gap for this category.
- **Gaps before submission**: real serving-stack integration (e.g. as a
  vLLM KV-manager plugin), evaluation at 7B+ model scale, real
  concurrent-request throughput/latency measurement, removal or
  significant re-scoping of the CXL/FPGA claims to avoid an
  evidence-bar mismatch.

## Category 2: Computer architecture (ISCA, MICRO, HPCA, ASPLOS)

- **Topic fit**: strong for the CXL near-memory and FPGA-datapath
  components specifically — TRACE and PNM-CXL (both cs.AR arXiv
  preprints as of this writing) target exactly this kind of venue.
- **Evidence bar**: architecture venues generally expect either (a) real
  silicon/FPGA-board results, or (b) a well-validated simulator with
  clearly justified timing models and sensitivity analysis presented as
  a simulation study, not conflated with a hardware result. MEMBRANE's
  hardware-sensitivity analysis (quant-pipeline count vs. CXL link
  generation, RQ7) is exactly this kind of contribution.
- **Real-hardware requirement**: high if framed as an accelerator
  contribution; lower if honestly framed as a simulation/design-space-
  exploration study (which is what this paper actually is).
- **Is the current project ready?** Partially. The negative PCIe-offload
  result (RQ4) and the pipeline-count sensitivity result (RQ7) are
  exactly the kind of disclosed, quantified finding this community
  values. The FPGA datapath's bit-exact cosimulation is a genuine,
  narrow architecture contribution. What's missing is either real
  silicon results or a more extensive design-space exploration
  (multiple FPGA families, multiple CXL device configurations) than
  this paper currently has.
- **Gaps before submission**: either real FPGA board results (Fmax,
  power, area) or an explicit reframing as "RTL cosimulation + synthesis
  check," never described ambiguously; a broader hardware-sensitivity
  sweep than the current 135M-only, one-scenario-point analysis.

## Category 3: ML systems (MLSys, NeurIPS/ICML Systems tracks)

- **Topic fit**: strong. KIVI, KVQuant, H2O, Quest, and Scissorhands —
  five of this paper's fourteen cited sources — were all published at
  ML-focused venues (ICML, NeurIPS) or their associated systems tracks.
- **Evidence bar**: real accuracy/quality numbers on real models (most
  cited work here uses 6.7B+ models and standard benchmark suites,
  e.g. perplexity, LongBench-style long-context evaluation) plus
  real throughput numbers on real GPUs.
- **Real-hardware requirement**: moderate — GPU results are expected,
  but a strong simulation-based systems argument (as MEMBRANE has) is
  more acceptable in this category than in Category 1, especially if
  framed as complementary to (not competing head-to-head with) the
  cited GPU-serving-scale work.
- **Is the current project ready?** Partially. MEMBRANE's bit-exact
  quantization verification (Q8/Q4 vs. ggml) is a real, defensible
  quality-correctness contribution at any model scale. The gap is model
  scale (135M/360M vs. the 7B+ standard in this literature) and the lack
  of a standard long-context benchmark suite (LongBench or similar) in
  the evaluation.
- **Gaps before submission**: evaluation at a larger model scale (at
  minimum a 7B-class open model); a standard long-context quality
  benchmark suite rather than this project's own authored prompt
  categories.

## Category 4: FPGA / reconfigurable computing (FCCM, FPL, FPGA)

- **Topic fit**: strong for the RTL/datapath contribution specifically.
  F-BFQ (this paper's cited FPGA-quantization source) was accepted at an
  ISCA-affiliated workshop in exactly this space.
- **Evidence bar**: typically expects real synthesis results (area,
  Fmax, power) on a named FPGA family, and often real board measurements
  — cosimulation alone is usually treated as a necessary but
  insufficient step, not the final result.
- **Real-hardware requirement**: high. This is the category where
  MEMBRANE's current "cosimulated + synthesizability-checked, not on
  silicon" status is the most direct mismatch with the venue's typical
  evidence bar.
- **Is the current project ready?** No, for a full paper. The bit-exact
  cosimulation methodology itself (520,000 transactions, verified
  against a real CPU reference) is a genuinely relevant, well-executed
  piece of evidence, but this community will expect real
  place-and-route results before treating it as a complete contribution.
- **Gaps before submission**: real synthesis run (yosys $\to$
  nextpnr or a vendor toolchain) producing Fmax/area/power numbers on a
  named, real FPGA part; ideally a real board bring-up.

## Category 5: Workshop / demo tracks

- **Topic fit**: strong, and the most realistic near-term option. Most
  major systems/architecture/ML conferences run affiliated workshops
  (e.g. ML-and-systems workshops at NeurIPS/ICML/MLSys, hardware-aware
  ML workshops at ISCA/MICRO) that explicitly welcome early-stage,
  simulation-heavy, or negative-result-focused work.
- **Evidence bar**: substantially lower than a full paper at any of the
  above venues — workshops routinely accept work-in-progress,
  reproducibility-focused, or reflective/negative-result submissions.
  This paper's negative-results section (§6 in `paper/main.md`) and
  disclosed limitations would likely be seen as a strength here rather
  than a gap, since workshop audiences specifically value honest,
  well-instrumented null results.
- **Real-hardware requirement**: low to none — a well-documented
  simulation study with a clear artifact/reproducibility story (which
  this project already has: `docs/reproduction.md`,
  `benchmarks/MANIFEST.json`, `scripts/verify-results.py`) is typically
  sufficient.
- **Is the current project ready?** **Yes, closest to ready of all five
  categories.** The manuscript's current scope, disclosure discipline,
  and artifact-verification infrastructure already meet what most
  workshop/demo tracks look for.
- **Gaps before submission**: primarily formatting (page limit,
  specific venue's LaTeX template) and a final check against that
  specific workshop's actual, current CFP for scope fit — not a
  substantive research gap.

## Overall recommendation

Given the honest gaps above, a workshop or demo track (Category 5) is
the most realistic near-term submission target for this manuscript as
it stands today; Categories 1–4 (systems, architecture, ML systems,
FPGA) are plausible **future** targets once the specific gaps listed
under each are closed — most of all, real-hardware validation (CXL,
FPGA board) and evaluation at a larger model scale. No specific venue or
deadline is recommended as ready-to-submit-today without first checking
that venue's own, current call for papers.
