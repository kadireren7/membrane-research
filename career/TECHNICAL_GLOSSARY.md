# Technical glossary

Short, precise definitions tied to how each term is actually used in
this project — not textbook-generic. See `MEMBRANE_INTERVIEW_GUIDE.md`
for the full simple/technical/evidence treatment of the most
interview-relevant subset of these.

**KV cache** — the per-token key/value vectors an attention layer
computed for previous tokens, cached so generation doesn't recompute
them. Grows linearly with context length and concurrency; the thing
this whole project is about managing.

**Q8_0 / Q4_0** — ggml's own per-block quantization formats (one scale
factor per block of values, 8-bit or 4-bit quantized values). MEMBRANE
reuses this exact math so results are bit-comparable to ggml, not a
custom scheme.

**Bit-exact / exactness** — identical down to every bit vs. a trusted
reference, checked with zero tolerance across every differential test
case in this project — not "close enough" or "within some ULP budget."

**ULP** — unit in the last place; the smallest possible difference
between two adjacent floating-point values at a given magnitude. This
project's differential tests report ULP distance when a mismatch is
found, but "exact" always means 0 ULP, not "small ULP."

**Radix-4 divider** — an iterative divider computing 2 quotient bits per
clock cycle (vs. radix-2's 1 bit/cycle), trading latency (many cycles
instead of one) for synthesized area (far fewer cells than a
combinational divider). Used in both `EXP-FPGA-DIV-001` (production) and
`EXP-FPGA-DIV-002` (experimental).

**Ready/valid handshake** — a two-signal streaming protocol: sender
asserts `valid`, receiver asserts `ready`, a transfer happens only on a
cycle where both are true. Used throughout this project's RTL for
backpressure-safe streaming.

**Head-of-line (HOL) blocking** — when a blocked transaction at the
front of a queue prevents transactions behind it from making progress,
even if the system has capacity to handle them. The problem Phase B2/B3
of `EXP-FPGA-DIV-002` address.

**Retirement / retirement pressure** — the process of a completed
transaction leaving in-flight state and producing its output, under a
strict in-order constraint. Phase B4 found this, not HOL blocking,
dominates residual stalls once B3's split queues are in place.

**Collateral slowdown** — how much *other*, untouched transaction
classes slow down because of a change made to one transaction class's
own handling — the central metric `EXP-FPGA-DIV-002` optimizes against.

**Synthesis (Yosys / synth_ecp5)** — a real EDA tool processing real
RTL and producing a real, tool-verified cell count. Not a physical
device measurement; a proxy for area used for relative comparison.

**Synthesis-tool proxy** — a synthesis output (cell count) presented
explicitly as a stand-in for physical area, never as an actual
LUT/FF/Fmax/power number on real silicon.

**MEASURED_BY_TOOL / SIMULATED / ESTIMATED / UNAVAILABLE** — this
project's four-way result-classification discipline. See
`RESEARCH_POLICY.md` for exact definitions; a result with no label is
treated as a defect in the document, not an acceptable omission.

**TOCTOU (time-of-check-to-time-of-use)** — a bug class where a
condition is checked (e.g. "is this the expected path?") and then acted
on later, with a window in between where the checked thing could have
changed. Fixed for real in `tests/unit/test_store_backend.c` (commit
`9dbbede`) by switching from path-based `chmod` to descriptor-based
`fchmod`.

**ASan / UBSan / TSan** — AddressSanitizer (memory errors: use-after-
free, buffer overflow), UndefinedBehaviorSanitizer (UB: signed overflow,
misaligned access), ThreadSanitizer (data races) — three different
runtime instrumentation tools catching three different bug classes, all
run in this project's CI.

**CodeQL** — static analysis over a compiled snapshot of the source,
without running the program — a different bug-finding method than the
sanitizers above (pattern-based analysis vs. runtime instrumentation),
run as its own CI workflow, currently 0 open alerts on `main`.

**CodeRabbit** — an AI-assisted PR review tool configured as advisory
(non-blocking) on this project — its findings inform review but don't
themselves gate merge, unlike CI/CodeQL.

**Canonical result** — a result artifact that has passed an
experiment's own full-run + promotion/validation step and lives in that
experiment's `results/canonical/` directory. Quick/smoke-mode results
are never canonical, regardless of how they look.

**Provenance** — the record of exactly which source branch, commit, and
content hash a migrated or referenced file came from — tracked in
`membrane-research/provenance/import-manifest.json`, re-verified in CI.
