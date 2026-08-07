# MEMBRANE interview guide

Real answers, tied to real files and real results — not talking points.
Every "evidence" line names a file or artifact that actually exists in
`kadireren7/membrane` or `kadireren7/membrane-research`; if you can't
find it, the answer is wrong or stale and this document needs fixing,
not the other way around.

## 30-second explanation

"MEMBRANE is a research prototype for LLM KV-cache storage: mixed-
precision quantization (Q8_0/Q4_0), exact block-level retrieval, and
CXL/near-memory simulation, plus a synthesizable FPGA datapath for the
quantization step. One result already reached production — an exact
radix-4 divider that cut the Q4_0 datapath's divider area by over 90%
with bit-exact output, verified against millions of test cases. A
related experiment on the wider Q8_0 case didn't reach that bar and is
documented as a negative result, not hidden."

## 2-minute explanation

Start with the 30-second version, then add: "The core problem is that
LLM inference keeps a growing KV cache in memory per request, and that
cache dominates memory and bandwidth at long context lengths. MEMBRANE
attacks this from three angles: CPU-side quantization (storing cache
entries at lower precision, using the same math ggml uses so results
are bit-comparable), an exact sparse-retrieval layer (only paging in the
KV blocks a query actually attends to, verified byte-for-byte against a
full scan, not approximated), and an FPGA/RTL exploration of whether the
quantization step itself is worth offloading to hardware. Every result
is labeled by how it was obtained — measured by a real tool, simulated
in software, estimated from other measurements, or unavailable — because
a synthesis-tool cell count is not a physical FPGA measurement and I
don't want to imply otherwise. The project is split into two
repositories: `membrane`, the maintained implementation, and
`membrane-research`, the full experiment record, so a recruiter or
contributor doesn't have to wade through five phases of a rejected
scheduler design to find the build instructions."

## 10-minute deep dive

Walk through, in order, pointing at real files as you go:

1. **The problem** — KV cache growth, `docs/architecture.md` (product
   repo) for the current system diagram.
2. **Quantization** — `src/quant/quant_simd.c`, and
   `tests/unit/test_ggml_quant_parity.c` (bit-exact against real ggml
   math, not a hand-rolled approximation of it — see
   `[[phase4.4-ggml-quant-parity]]`-equivalent history in
   `membrane-research/docs/phase4-ggml-quant-parity.md`).
3. **Exact retrieval** — `membrane-research/docs/phase6-exact-sparse-retrieval.md`:
   real concurrency/capacity sweep, a real p99-contention finding that
   reversed to null after a mid-session artifact-size constraint forced
   a top-k reduction — walk through *why* it reversed, that's the part
   that shows real understanding, not memorized numbers.
4. **The FPGA divider work** — `EXP-FPGA-DIV-001` (promoted) vs.
   `EXP-FPGA-DIV-002` (not promoted). Explain why one worked and the
   other didn't (see Q5-Q12 below) — this is the single best test of
   whether you actually understand the RTL, not just the headline
   numbers.
5. **The two-repository split** — why it happened, what stayed, what
   moved, and that nothing was deleted (`membrane-research/provenance/`).
6. **What's real vs. simulated** — walk through one MEASURED_BY_TOOL
   number and one SIMULATED number side by side and explain the
   difference without hedging.

## Exact questions

### What problem does MEMBRANE solve?

- **Simple answer**: LLM inference servers run out of memory and
  bandwidth because the KV cache grows with context length; MEMBRANE
  explores ways to shrink and manage that cache without losing
  correctness.
- **Technical answer**: mixed-precision block quantization (Q8_0/Q4_0,
  same math as ggml) for the cache itself, an exact (not approximate)
  sparse-retrieval layer that only pages in attended blocks, and a
  CXL/near-memory simulation layer for out-of-core scenarios, plus an
  FPGA exploration of offloading the quantization step.
- **Evidence**: `README.md` (product repo) "Why MEMBRANE";
  `membrane-research/docs/phase6-exact-sparse-retrieval.md`.
- **Wrong answer to avoid**: "it's a faster LLM" — MEMBRANE doesn't
  touch model compute; it's entirely about KV-cache storage/retrieval
  and one FPGA datapath experiment on the quantization step.

### What is KV cache?

- **Simple answer**: the per-token key/value vectors an attention layer
  computed for every previous token in the sequence, kept around so the
  model doesn't recompute them for every new token.
- **Technical answer**: for each layer and attention head, one key
  vector and one value vector per token, stored so each new token's
  attention only needs one new key/value pair plus the cached ones —
  O(1) work per new token instead of O(n).
- **Evidence**: `membrane-research/docs/phase2-kv-analysis.md`.
- **Wrong answer to avoid**: confusing it with the model's weights —
  weights are fixed per model; KV cache is per-request, per-token state
  that grows during generation.

### Why Q8/Q4?

- **Simple answer**: storing cache entries at 8-bit or 4-bit precision
  instead of 16/32-bit cuts memory and bandwidth roughly in proportion,
  if the quality loss is acceptable.
- **Technical answer**: Q8_0/Q4_0 are ggml's own per-block quantization
  formats (one scale factor per block of values) — MEMBRANE reuses the
  exact same math (not a re-derivation) so quantized values are
  bit-comparable to what ggml itself would produce, verified by
  `test_ggml_quant_parity`.
- **Evidence**: `membrane-research/docs/phase4-ggml-quant-parity.md` —
  including the real finding that "correct" ggml-matching math can make
  offline *quality prediction* worse under extreme perturbation, a
  genuine, disclosed complication, not a clean win.
- **Wrong answer to avoid**: claiming Q8/Q4 quantization has no quality
  cost — it does, quantified and disclosed, not hand-waved away.

### What is bit-exactness?

- **Simple answer**: the quantized/dequantized/transformed result is
  identical, down to every bit, to a trusted reference — not "close
  enough," not "within tolerance."
- **Technical answer**: every differential test in this project (RTL vs.
  C reference, quantized vs. ggml) checks for zero mismatches across
  millions of cases; a single 1-ULP difference fails the whole run. This
  is a much stricter bar than typical floating-point "close enough"
  testing, and several real candidates in `EXP-FPGA-DIV-002` were
  rejected specifically because they were *not* bit-exact.
- **Evidence**: `experiments/EXP-FPGA-DIV-002/results/canonical/feasibility-differential-full.txt`
  (Phase A: 25.04%/28.17%/4.54% real mismatch rates, all rejected).
- **Wrong answer to avoid**: "it passed the tests" without being able to
  say what the test actually checked (exact equality, not statistical
  similarity).

### Why didn't 1/d reconstruct the other Q8 scale exactly?

- **Simple answer**: `d` and `id` are both derived from the same
  `amax`, and they're *mathematically* reciprocals of each other, but
  floating-point division doesn't preserve that relationship bit-for-bit
  — computing `1/d` in floating point is not the same operation as
  computing `id` directly, and rounds differently.
- **Technical answer**: `d = amax/127.0` and `id = 127.0/amax` are each
  a single correctly-rounded IEEE-754 division; `1/d` is a *second*
  division (`1.0/d`) applied to an already-rounded intermediate result,
  which compounds rounding error — the two paths agree only when the
  rounding happens to cancel out, which Phase A measured at ~75% of
  cases for one direction and ~72% for the other, meaning ~25-28%
  actually diverge by 1 ULP or more.
- **Evidence**: `experiments/EXP-FPGA-DIV-002/results/canonical/feasibility-differential-full.txt`
  (candidate B: 25.04% mismatch; candidate "reconstructed 1/id": 28.17%).
- **Wrong answer to avoid**: "floating point is imprecise" as a whole
  non-answer — the real answer is specifically about compounded
  rounding across two dependent divisions, not general FP fuzziness.

### What did radix-4 change?

- **Simple answer**: replaced a wide, single-cycle combinational divider
  with an iterative divider that computes 2 quotient bits per cycle
  over several cycles — much smaller, but no longer instant.
- **Technical answer**: `membrane_fp_divider.sv` (baseline) computes a
  full division combinationally in one cycle, which synthesizes to a
  very wide, cell-heavy circuit. `membrane_fp_divider_radix4.sv`
  computes the same IEEE-754 division bit-exactly but iteratively (an
  FSM: `S_IDLE` → 13× `S_ITER` → `S_ROUND` → `S_DONE`), trading latency
  (single-cycle → ~15-cycle) for area.
- **Evidence**: `rtl/experimental/q8_div/q8_scale_dual_radix4.sv`;
  `experiments/EXP-FPGA-DIV-002/results/canonical/b1-synthesis.csv` row
  B (standalone radix-4 divider: 1,509 ECP5 cells vs. 73,629 for the
  baseline standalone divider).
- **Wrong answer to avoid**: "radix-4 is just a faster algorithm" — it's
  the opposite trade here: smaller and slower, not faster.

### Why did area proxy fall dramatically?

- **Simple answer**: a combinational divider needs enough hardware to
  compute the whole result in one cycle; an iterative divider reuses a
  small amount of hardware across many cycles, so it needs far less of
  it.
- **Technical answer**: the standalone radix-4 divider synthesizes to
  1,509 ECP5 cells vs. 73,629 for the baseline (-97.95%); the full dual-
  instance `q8_scale_dual_radix4` integration measured 2,775 cells vs.
  123,742 for the baseline `q8_scale` (-97.76%) — below even the naive
  2× estimate, because Yosys/ABC shares logic between the two parallel
  radix-4 instances.
- **Evidence**: `experiments/EXP-FPGA-DIV-002/results/canonical/b1-synthesis.csv`.
- **Wrong answer to avoid**: presenting these numbers as physical FPGA
  utilization — they are Yosys synthesis-tool cell counts, a proxy for
  area, not a measured LUT/FF count on a real device.

### Why did latency increase?

- **Simple answer**: trading a one-cycle combinational divider for a
  many-cycle iterative one directly increases per-operation latency —
  that's the mechanism the area reduction comes from, not a side effect.
- **Technical answer**: measured mean latency 14.888 cycles under random
  backpressure (max 34), with a measured initiation interval of 16
  cycles (single-in-flight per divider) — vs. the baseline's fixed
  1-cycle latency. This single-in-flight behavior is exactly what forces
  full-serialization scheduling against other in-flight transaction
  classes in Phase B1, the problem Phases B2-B4 spend their entire scope
  trying to reduce.
- **Evidence**: `experiments/EXP-FPGA-DIV-002/results/canonical/b1-differential.json`
  (`latency_cycles`, `initiation_interval`).
- **Wrong answer to avoid**: treating latency increase as a bug — it's
  the direct, understood, accepted cost of the area trade, not an
  unexplained regression.

### Why did B3 lookahead regress?

- **Simple answer**: letting the scheduler peek ahead and issue a
  ready transaction out of order sounds like it should only help, but
  the bookkeeping cost of doing that safely turned out bigger than the
  benefit.
- **Technical answer**: 2-entry and 4-entry lookahead (`b3l2`/`b3l4`)
  measurably made density-sweep collateral *worse* than Phase B2's
  simpler scheduler at every density/mode tested — the real cause was
  shadow-retirement contention plus a constant per-issue lookahead/
  compaction overhead that outweighed the head-of-line bypass benefit
  the design was meant to capture. This was the opposite of the
  hypothesis, and it's documented as such, not reframed.
- **Evidence**: `experiments/EXP-FPGA-DIV-002/results/canonical/b3-candidate-comparison.md`
  (20%-density Q8_DEC: B2 +12.39%, `b3l2` +19.73%, `b3l4` +37.28%).
- **Wrong answer to avoid**: claiming lookahead is "always" a bad idea —
  the finding is specific to this scheduler's own retirement/compaction
  overhead, not a general claim about lookahead scheduling.

### What did split queues improve?

- **Simple answer**: giving Q8_0/Q4_0-encode transactions their own
  input queue, separate from other modes, removes the head-of-line
  dependency directly instead of trying to bypass it.
- **Technical answer**: `b3_split`'s mode-split ingress queues met the
  ≤10% collateral bound at 20% Q8_0-encode density on all three affected
  modes (+9.58%/+0.59%/+9.79%), and ran 17.8-21.4% faster than Phase B2
  on the 20-25%-density profiles specifically — the only Phase B3
  candidate that actually helped.
- **Evidence**: `experiments/EXP-FPGA-DIV-002/results/canonical/b3-candidate-comparison.md`.
- **Wrong answer to avoid**: conflating this with lookahead — split
  queues and lookahead were two different, separately-tested hypotheses
  in the same phase, with opposite outcomes.

### What did B4 direct-retire improve?

- **Simple answer**: letting a completed transaction skip completion
  storage entirely when the consumer downstream can accept it
  immediately, instead of always buffering it first.
- **Technical answer**: R3 (direct-retire bypass) ran 4.5-4.8% faster
  than `b3_split` overall, +4.1% faster on the adversarial-retirement
  pattern, and was the only B4 candidate to meet the ≤10% collateral
  bound at 20% density — but not at 25%, and R1/R2 (the other two
  candidates, which reduced completion-storage capacity instead) made
  things *worse*, R1 severely (-84.3% on the adversarial pattern).
- **Evidence**: `experiments/EXP-FPGA-DIV-002/results/canonical/b4-candidate-comparison.md`.
- **Wrong answer to avoid**: implying B4 solved the problem — it's a
  real, partial improvement, explicitly not enough to meet the
  promotion bar (see next question).

### What still prevents Q8 promotion?

- **Simple answer**: no candidate across any phase met the full set of
  required success criteria at the same time (collateral bound at both
  20% and 25% density, overall-improvement bound, adversarial-reduction
  bound) — R3 gets closest but falls short at 25% density.
- **Technical answer**: a software retirement-state model found strict
  in-order retirement (not head-of-line blocking, which B3 already
  solved) now dominates: 65.6-80.5% of stall cycles across density
  profiles. Closing that gap further would plausibly need a larger
  reorder/completion structure, which every phase in this experiment
  explicitly excluded from scope ("no depth 4/8 ROB sweep").
- **Evidence**: `experiments/EXP-FPGA-DIV-002/results/canonical/b4-retirement-analysis.md`;
  `ROADMAP.md`'s "Closed experiments" section.
- **Wrong answer to avoid**: "it just needs more testing" — the
  bottleneck is architectural and named, not a coverage gap.

### What is TOCTOU?

- **Simple answer**: time-of-check-to-time-of-use — a bug where you
  check something about a file (e.g. "is this the right path?") and then
  act on it later, and between those two steps, something else could
  have swapped what that path points to.
- **Technical answer**: the real fix in `tests/unit/test_store_backend.c`
  (commit `9dbbede`) replaced a path-based `chmod()` sequence (check the
  path, then act on the same path string later — vulnerable to a
  symlink swap in between) with descriptor-based `fchmod(dirfd, ...)`,
  which acts on the already-open file descriptor and can't be redirected
  by a later filesystem change. A new adversarial regression test
  (`test_dirfd_immune_to_path_swap`) proves the fix.
- **Evidence**: `tests/unit/test_store_backend.c`, commit `9dbbede`.
- **Wrong answer to avoid**: describing it as a generic "race condition"
  without naming the specific check-then-use gap.

### What CodeQL issue was fixed?

- **Simple answer**: the same TOCTOU/symlink-substitution issue above —
  verified fixed with CodeQL as part of the fix commit, alongside
  sanitizers and stress runs.
- **Technical answer**: commit `9dbbede`'s own message states the fix
  was verified with CodeQL, sanitizers, and stress runs; current state
  is 0 open CodeQL alerts on `main`.
- **Evidence**: commit `9dbbede` message; `gh api repos/kadireren7/membrane/code-scanning/alerts` (0 results).
- **Wrong answer to avoid**: naming a *different* CodeQL finding not
  actually documented anywhere in this project — if you don't remember
  the specific one, say "the TOCTOU fix in test_store_backend.c" and
  stop there rather than inventing detail.

### What is ready/valid?

- **Simple answer**: a standard two-signal handshake in digital design —
  the sender asserts `valid` when it has data, the receiver asserts
  `ready` when it can accept data, and a transfer only happens on a
  cycle where both are true at once.
- **Technical answer**: used throughout this project's RTL
  (`rtl/stream_fifo.sv`, every quant-stream module) for backpressure-safe
  streaming — neither side is ever forced to accept or produce data it
  isn't ready for, which is what makes the differential testbenches'
  "random backpressure" testing meaningful (real, varying `ready`
  patterns, not a fixed-latency assumption).
- **Evidence**: `rtl/membrane_quant_stream_top.sv`; any experimental top
  module in `rtl/experimental/`.
- **Wrong answer to avoid**: confusing it with a simple enable signal —
  ready/valid is bidirectional and a transfer requires *both* sides
  agreeing on the same cycle.

### What has actually been physically measured?

- **Simple answer**: nothing. No real FPGA board, no vendor place-and-
  route tool, and no physical CXL hardware exist in this project's
  development environment, in any experiment.
- **Technical answer**: every synthesis number is a Yosys 0.33 generic
  or `synth_ecp5` synthesis-tool proxy result (real tool, real RTL,
  real output — but not a physical device); every CXL/near-memory
  number is a software simulation. This is stated explicitly and
  repeatedly in every experiment's own limitations section, not a
  detail you have to dig for.
- **Evidence**: `RESEARCH_POLICY.md`'s "Measurement classification is
  mandatory" section; every experiment's own "Limitations" section.
- **Wrong answer to avoid**: any answer that names a specific Fmax,
  power, or LUT-utilization number as if it were measured — none exist.

### What is simulated?

- **Simple answer**: any result where a software model stands in for
  something that wasn't actually run on real hardware — the CXL near-
  memory simulator, the retirement-pressure/HOL-blocking taxonomy
  models, the out-of-core KV simulator.
- **Technical answer**: labeled `SIMULATED` throughout, distinct from
  `MEASURED_BY_TOOL` (a real tool like Yosys or Verilator produced the
  number) and `ESTIMATED` (derived from measured/simulated numbers via
  a disclosed method) — see `RESEARCH_POLICY.md`'s terminology section
  for why these three are kept distinct rather than used interchangeably.
- **Evidence**: `scripts/b3-hol-model.py`, `scripts/b4-retirement-model.py`
  (both explicitly software reference models, not RTL-instrumented).
- **Wrong answer to avoid**: calling a Verilator cosimulation
  "simulated" in the same sense as the software-only models above — a
  Verilator run executes the real compiled RTL and is classified
  `MEASURED_BY_TOOL`, a meaningfully different (stronger) claim.

### Why split into two repositories?

- **Simple answer**: so a recruiter or contributor can understand and
  build the maintained product without wading through five phases of
  research history, while nothing about that history gets deleted.
- **Technical answer**: `membrane` carries the maintained implementation,
  its CI/CodeQL/CodeRabbit, and enough documentation to build and use it;
  `membrane-research` carries every experiment, simulator, rejected
  candidate, and negative result, with full SHA256-verified provenance
  back to its original source branches (none of which were deleted).
  The decision explicitly revisits and supersedes an earlier, reasoned
  decision *against* a second repository — documented in place, not
  silently reversed.
- **Evidence**: `docs/repository-boundary.md` (product repo);
  `membrane-research/provenance/repository-contract.md`.
- **Wrong answer to avoid**: "to make membrane look better" — the actual
  stated reasons are navigation/lifecycle, not image management, and
  the split is explicit about that.

### What did AI agents do?

- **Simple answer**: wrote the bulk of the implementation, tests, RTL,
  and documentation prose, under direction and iterative review — they
  didn't decide what to build or what counts as a valid result.
- **Technical answer**: AI agents ran builds/tests/simulators, proposed
  fixes, drafted docs from real repository content, and had that output
  checked by automated verification tooling
  (`scripts/verify-outreach.py`, `paper/scripts/verify-paper.py`) built
  specifically so claims aren't taken on the model's word alone.
- **Evidence**: `outreach/ai-assistance-disclosure.md`.
- **Wrong answer to avoid**: "AI built the project" or "I wrote all the
  code manually" — both are false; the accurate framing is direction
  and validation by Kadir, implementation heavily AI-assisted.

### What decisions remain Kadir's responsibility?

- **Simple answer**: the research question, the architecture, what
  counts as a valid test, when a result is reported as negative instead
  of reframed, what gets promoted to production, and what gets
  published or sent externally.
- **Technical answer**: see `outreach/ai-assistance-disclosure.md`'s
  "What Kadir directed and decided" section for the full list — it's
  written out explicitly rather than left implicit, specifically so this
  question has a real, checkable answer instead of a vague assurance.
- **Evidence**: `outreach/ai-assistance-disclosure.md`.
- **Wrong answer to avoid**: "I reviewed everything line by line" if
  that isn't literally true — see `career/OWNERSHIP_CHECKLIST.md` for
  what independent review has actually been done, and don't claim more.
