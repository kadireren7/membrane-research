# Related-work comparison matrix

Comparison is presented as measured/described in each cited primary
source (see `paper/references.bib` for full metadata). Where a source's
publication doesn't state a figure or property, the cell says "not
stated" rather than guessing. This matrix does not attempt to show
MEMBRANE ahead on every row — several rows below are honestly worse for
MEMBRANE than the cited work, and are left that way.

| Work | Year | System category | KV compression | KV selection/eviction | Exact or approximate | Memory tier | Hardware acceleration | Real hardware | Context/concurrency scale | Quality validation | Relationship to MEMBRANE |
|---|---|---|---|---|---|---|---|---|---|---|---|
| H2O (Zhang et al.) | 2023 | Inference runtime | None (FP16 KV) | Eviction (heavy-hitter + recent, submodular) | **Approximate** — evicted tokens are gone | GPU only | None | Real GPU (OPT/LLaMA/GPT-NeoX) | Not a concurrency study; single-stream generation | Perplexity/downstream task accuracy on real 6.7B–30B models | MEMBRANE's exact-retrieval design is a direct response to this category of lossy eviction; MEMBRANE never discards a genuinely-needed block, but has not run on models anywhere near this scale |
| Scissorhands (Liu et al.) | 2023 | Inference runtime | Optional 4-bit quant on retained KV | Eviction ("persistence of importance" heuristic) | **Approximate** | GPU only | None | Real GPU | Not a concurrency study | Real downstream-task quality on production-scale models | Same relationship as H2O — MEMBRANE's Phase 6.2 (`docs/phase6-attention-working-set.md`) independently measured a real recall failure mode for this class of approximate eviction, consistent with the persistence-of-importance approach needing care on recall-shaped prompts |
| StreamingLLM (Xiao et al.) | 2023 (ICLR 2024) | Inference runtime | None | Eviction, but keeps fixed initial "sink" tokens + sliding window | **Approximate** (older mid-sequence tokens are dropped, not attended to at all) | GPU only | None | Real GPU (Llama-2, MPT, Falcon, Pythia) | Streaming, up to 4M tokens on a single stream | Perplexity in streaming setting | MEMBRANE's own capacity-accounting sections independently need an attention-sink-like structure at 128K context; unlike StreamingLLM, MEMBRANE never drops a block outright — it demotes to Q8/Q4 or fetches it back exactly |
| Quest (Tang et al.) | 2024 (ICML 2024) | Inference runtime | None | Query-aware page selection (top-K pages per query, min/max key summary) | **Approximate** (only top-K pages attended) | GPU only | None | Real GPU | Long-context (32K–100K single-sequence) benchmarks | Real downstream long-context benchmark accuracy | Closest sparse-attention analogue to MEMBRANE's predictor; MEMBRANE's predictor also estimates block relevance, but MEMBRANE's compulsory-miss path makes every genuinely-attended block exact rather than permanently excluded — Quest does not have this fallback because it targets GPU compute reduction, not tiered memory residency |
| PagedAttention / vLLM (Kwon et al.) | 2023 (SOSP 2023) | Serving system | None | None (no eviction; virtual-memory-style paging of full KV) | Exact (no data loss; only fragmentation eliminated) | GPU only | None | Real GPU, production serving workloads | Real multi-request concurrency, production-scale (13B–66B models) | Real serving throughput vs. FasterTransformer/Orca | The strongest real-hardware, real-concurrency result in this table; MEMBRANE's exact-retrieval design shares the "never lose data" property but at far smaller model scale and in simulation, not a real multi-tenant GPU server — this is a genuine capability gap, not a wash |
| FlexGen (Sheng et al.) | 2023 (ICML 2023) | Offloading runtime | 4-bit weight+KV quantization | None (full KV kept, just tiered) | Exact retrieval of tiered data (quantization is lossy, but nothing is evicted) | GPU + CPU + disk (real, on one machine) | None | Real single-GPU hardware | Large batch, single machine, not concurrency in the serving sense | Real accuracy loss reported for 4-bit compression | Real precedent for MEMBRANE's own hot/warm/cold tiering idea, but FlexGen tiers to real CPU RAM/NVMe on one machine today, while MEMBRANE's near-memory/CXL tier is simulated, not implemented against real hardware |
| InfiniGen (Lee et al.) | 2024 (OSDI 2024) | Offloading runtime | None | Speculative prefetch (rehearsal-based, predicts important tokens ahead of the layer that needs them) | Approximate prefetch decision, but full KV retained (not evicted) | GPU + CPU (real) | None | Real GPU+CPU serving system | Real multi-request serving | Real accuracy improvement over prior KV managers reported | The closest real-hardware analogue to MEMBRANE's predictor+prefetch+compulsory-fetch design; InfiniGen's speculative rehearsal is a different prediction mechanism than MEMBRANE's working-set predictor, and InfiniGen is validated on a real serving system where MEMBRANE is not |
| KIVI (Liu et al.) | 2024 (ICML 2024) | KV quantization | 2-bit (per-channel K, per-token V) | None | Exact algorithm application, lossy compression | GPU only | None | Real GPU (Llama/Falcon/Mistral) | Larger batch size via memory savings | Real downstream quality at 2-bit reported near-lossless | Closest quantization-scheme analogue to MEMBRANE's Q8/Q4 tiers; KIVI's 2-bit result is more aggressive than MEMBRANE's Q4 floor, but is not verified bit-exact against a reference implementation the way MEMBRANE's Q8_0/Q4_0 path is verified against ggml |
| KVQuant (Hooper et al.) | 2024 (NeurIPS 2024) | KV quantization | Sub-4-bit, non-uniform/per-channel quantization | None | Exact algorithm application, lossy compression | GPU only | None | Real GPU | Up to 10M single-sequence context length reported | Real perplexity/accuracy reported at sub-4-bit | MEMBRANE's context scale (128K) is far smaller than KVQuant's stated 10M-token target; MEMBRANE's contribution is not context length but concurrency (512 simultaneous sequences) combined with exact retrieval, which KVQuant's single-sequence quantization result does not address |
| Landmark Attention (Mohtashami & Jaggi) | 2023 (NeurIPS 2023) | Attention mechanism | None | Learned landmark-token block retrieval (trained, not a runtime heuristic) | Retrieval is trained/approximate at the block level | GPU only | None | Real GPU, fine-tuned LLaMA-7B | Extends usable context to ~32K via fine-tuning | Real task accuracy after fine-tuning | Conceptually close to MEMBRANE's "retrieval instead of eviction" framing, but requires model fine-tuning; MEMBRANE's exact retrieval works against an unmodified model's real attention pattern, calibrated from a captured trace, not a trained retrieval head |
| TRACE (Xie et al.) | 2025 (cs.AR) | CXL near-memory architecture | Precision-scaling + lossless compression inside a CXL device | None (bandwidth amplification, not eviction) | Exact (compression is lossless per the source's own description) | CXL (device-side compression/precision-scaling) | CXL device-side logic | Described as a CXL architecture proposal; this project has not independently verified whether results are from real CXL silicon, an FPGA prototype, or simulation — flagged here rather than assumed | Long-context LLM serving, figures reported at the serving-system level | Reported end-to-end serving throughput/context-length/footprint improvements | The closest published analogue to MEMBRANE's own CXL near-memory + quantization simulation (`docs/phase6-cxl-near-memory.md`); MEMBRANE's own CXL layer is explicitly simulated with assumed link parameters, so this comparison should not be read as MEMBRANE validating against real CXL hardware either |
| PNM-CXL (Kim et al.) | 2025 (cs.AR) | CXL near-memory architecture | None described at abstract level | Processing-near-memory token/page selection accelerator | Approximate (page/token selection is a selection heuristic) | CXL (processing-near-memory accelerator) | CXL-side accelerator | Described as CXL-enabled architecture; this project has not independently verified real-silicon vs. simulated/FPGA-prototype results beyond the abstract | 1M-token target, GPU-limit-scale serving | Reported throughput/energy/cost figures at the serving-system level | Same category as MEMBRANE's CXL appliance simulation (`tools/membrane-cxl-sim`); MEMBRANE's version explicitly discloses zero real CXL hardware and assumed link parameters, whereas this source's real-vs-simulated hardware split was not independently confirmed by this project |
| F-BFQ (Haris & Cano) | 2025 (LG-ARC @ ISCA 2025) | FPGA quantization accelerator | Block floating-point quantization | None (accelerator, not a KV-cache manager) | N/A (accelerator design, not a retrieval scheme) | FPGA-resident | FPGA | Real FPGA per source (edge-device-targeted); this project did not independently re-verify board-level results | Not a long-context/concurrency system | Not primarily a quality-validation paper (accelerator throughput/area focus) | Closest FPGA-quantization analogue to MEMBRANE's own synthesizable datapath (`rtl/`); MEMBRANE's datapath is cosimulated in Verilator and confirmed synthesizable under yosys but has never been run on a physical FPGA board — F-BFQ's real-board claim (per its own abstract) is a capability MEMBRANE does not yet have |
| CXL Consortium specification | 2023–2026 (CXL 3.1 / 4.0) | Industry standard | N/A | N/A | N/A | Defines the CXL memory-expansion/pooling standard itself | N/A (standards document) | N/A — this is the standard MEMBRANE's own CXL simulation assumes conformance to, not a system under test | N/A | N/A | MEMBRANE's simulated link latency/bandwidth figures are drawn from this standard's published, industry-typical ranges (see `docs/phase6-cxl-near-memory.md`), not from an implementation of the standard |

## Honest summary of where MEMBRANE is NOT ahead

- **No real-hardware serving validation.** Every LLM-runtime paper in
  this table (H2O, Scissorhands, StreamingLLM, Quest, PagedAttention,
  FlexGen, InfiniGen, KIVI, KVQuant) reports results on real GPUs, most
  at 6.7B–66B model scale. MEMBRANE's exact-retrieval and mixed-precision
  results are simulated/CPU-measured at 135M–360M scale — a genuinely
  smaller and less production-representative evaluation.
- **No real CXL or FPGA hardware.** TRACE, PNM-CXL, and F-BFQ each claim
  some form of real-hardware or FPGA-board validation in their own
  abstracts (not independently re-verified by this project). MEMBRANE's
  equivalent components are simulated (CXL) or cosimulated-but-not-
  synthesized-to-silicon (FPGA).
- **Smaller context/model scale than several cited works.** KVQuant's
  10M-token target and Landmark Attention's real fine-tuned extension to
  32K on a 7B model both exceed MEMBRANE's 128K-context, 135M/360M-model
  evaluation in that specific dimension.

## Where MEMBRANE's approach differs constructively

- **Exact rather than approximate retrieval.** H2O, Scissorhands,
  StreamingLLM, and Quest all permanently exclude some KV content from
  future attention. MEMBRANE's compulsory-miss path never does — every
  block attention actually needs is fetched exactly, at a measured
  latency cost (see `docs/phase6-unified-stress.md` §12.1). This is a
  design choice with real tradeoffs (more bytes moved than a pure
  eviction policy at the same cache size — see this project's own
  `docs/results-summary.md`), not an unqualified improvement.
- **Bit-exact CPU/FPGA math parity as a stated, verified property.**
  None of the cited FPGA/CXL hardware sources describe validating their
  hardware datapath bit-for-bit against a CPU reference across
  100,000+ randomized cases the way MEMBRANE's Phase 4.4/5.3 do (per
  their own abstracts) — this is a narrower, more mechanical claim than
  "faster" or "more accurate," but it is one this project can support
  directly from `tests/unit/test_ggml_quant_parity.c` and
  `rtl/tb/tb_top_verilator.cpp`.
- **Disclosed negative results as a first-class deliverable.** No cited
  source's abstract foregrounds a null/negative finding the way this
  project's `docs/results-summary.md` §4 does (blind compression
  failure, PCIe offload net loss, eviction recall failure, 10ms bound
  miss, micro-batching null result) — this is a difference in reporting
  discipline, not a technical superiority claim.
