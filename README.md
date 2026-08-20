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

## Relationship to `kadireren7/membrane`

`kadireren7/membrane` carries only what a user or contributor needs to
build and run the maintained product, plus the CI, security scanning
(CodeQL), and review tooling (CodeRabbit) that guard it. Every
experimental branch, simulator, negative result, and abandoned
candidate that MEMBRANE's own development produced lives here instead,
so the product repository stays legible and the research record stays
complete. The two repositories share no live source tree: maintained
code lives only in `membrane`; experiment evidence lives only here;
either side references the other by URL and commit, never by copying a
live, evolving tree. The full reasoning behind the split, including the
earlier decision against splitting at all and why it was revisited, is
documented in `kadireren7/membrane`'s `docs/repository-boundary.md`.

## Structure

```
experiments/     phased research records (see "How experiments are organized")
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

## How experiments are organized

Each directory under `experiments/` follows the same shape — see
[`experiments/README.md`](experiments/README.md) for the full navigation
table (every experiment, its question, status, decision, and whether
anything reached production):

- `README.md` — index: problem, baseline, tested candidates, key
  measurements, final decision, limitations, links to canonical
  evidence.
- `methodology.md` — test design, exactness rules, toolchain, and the
  measurement-classification discipline (below).
- `results/canonical/` — the result artifacts every claim in the README
  is drawn from, plus `results/schemas/README.md` documenting their
  fields.
- `reproduction/README.md` — exact commands to reproduce those results,
  including any known gap in doing so (disclosed, not glossed over).
- `archive/` — every phase document in its original form, preserved
  even where a later phase supersedes its conclusion.

Two experiments are fully restructured into this shape:
[`EXP-FPGA-DIV-001`](experiments/EXP-FPGA-DIV-001/README.md) (the
exact-radix-4 Q4_0 divider that *did* reach production — see its
"Provenance" section for the merge record) and
[`EXP-FPGA-DIV-002`](experiments/EXP-FPGA-DIV-002/README.md) (the
related Q8_0 dual-divider/scheduler investigation — five phases
complete, `RESEARCH_COMPLETE_NO_PROMOTION`, nothing merged).

Two more (Phase 10, KV cache storage precision) were migrated from
`kadireren7/membrane`'s research branches, each with an additional
`MANIFEST.json` (machine-readable provenance/artifact-hash record) and
`patches/` (the prototype code, as a `git diff` against its real base
commit — not new source files):
[`EXP-KV-Q4-STORAGE-10A`](experiments/EXP-KV-Q4-STORAGE-10A/README.md)
(real memory win, real quality cost too high — `Q4_MEMORY_WIN_QUALITY_TOO_HIGH_COST`,
not promoted) and
[`EXP-KV-Q5-EVALUATION-10B`](experiments/EXP-KV-Q5-EVALUATION-10B/README.md)
(Q5_1 clears the quality bar Q4 missed — `Q5_PRODUCT_CANDIDATE`,
shipped as `--kv q5` via [kadireren7/membrane#19](https://github.com/kadireren7/membrane/pull/19)).

A third,
[`EXP-KV-RAM-VRAM-TIERING-12A`](experiments/EXP-KV-RAM-VRAM-TIERING-12A/README.md),
preserves Phase 12A: a real, working GPU↔host KV copy mechanism was
proven, but true KV-only placement (independent of weight placement)
was proven *not* achievable with the public API available at the time
(`TIERING_REQUIRES_UPSTREAM_CHANGE`) — this finding directly motivated
the later Phase 12B device-override patch (not yet migrated here).

## How canonical results work

A result is "canonical" only once it lives in an experiment's own
`results/canonical/` — nothing else counts, no matter how it looks.
Quick/smoke-mode runs (`--quick`, used for fast iteration and CI-sized
checks) are explicitly **not** canonical and are never promoted into
that directory; only a full research-scale run (`--full`), passed
through each experiment's own promotion/validation step, is. Once
promoted, a canonical result is immutable except via a disclosed
correction (a new commit, with the reason stated in the affected
experiment's own docs) — never a silent edit. See `RESEARCH_POLICY.md`
for the full discipline this repository holds every result to.

## Provenance policy

Every file in this repository that originated in `kadireren7/membrane`
is tracked back to its exact source branch, commit, and content hash in
[`provenance/import-manifest.json`](provenance/import-manifest.json)
and [`provenance/source-map.md`](provenance/source-map.md). No source
branch was deleted or rewritten by this migration. Every experiment
additionally pins the exact `membrane` commit its own results were
measured against (each experiment's own README "Provenance" section) —
a claim about a promoted or rejected result is only ever made against a
named, fixed commit, never "the current state of membrane."

## Reproduction

See [`reproduction.md`](reproduction.md) at this repository's root for
the full guide (KV/attention model-backed verification, the multi-hour
out-of-core unified sweep, both FPGA divider experiments, paper and
outreach claim verification). The maintained runtime's own Level-1
quick verification (unit tests, sanitizers, ggml parity, production RTL
cosimulation) stays documented in `kadireren7/membrane`'s own
`docs/reproduction.md` — not duplicated here, since it depends only on
files that live there.

## Current active research tracks

See [`ROADMAP.md`](ROADMAP.md) for full detail and status. In brief: R1
(finishing this repository split and the ownership material in
`career/`); R2 (a production promotion exercise on an already-mature
result, to build the full research→PR→review→merge process on a
low-risk change); R3 (external reproduction of one experiment by
someone other than the original session — Kadir himself, running a
reproduction guide end to end, counts); R4 (vendor tooling/real
hardware validation, gated on actual hardware access, no FPGA board or
CXL hardware exists in this project's environment yet); and R5 (any new
architectural attempt at `EXP-FPGA-DIV-002`'s unresolved bottleneck gets
its own new experiment ID, not a `B5`). A separately-tracked gap:
reproduction-tooling repair for the two-repository split itself
(`MEMBRANE_PRODUCTION_ROOT`/`MEMBRANE_ROOT` path resolution, disclosed
as not yet re-verified end to end — R3 is its first real test).
`EXP-FPGA-DIV-002` itself is closed for its current architecture family,
not being iterated on further under that experiment ID — see its own
README's "Status".

## How failed hypotheses are preserved

A negative result is a completed experiment, not a failed one, and is
documented at the same level of detail as a positive result. Phase B3's
lookahead candidates (`EXP-FPGA-DIV-002`) are the clearest example: the
hypothesis was that bounded lookahead would reduce collateral stalls; it
measurably made them worse, and that result — root cause included — is
in the current `README.md`, not buried in `archive/`. Every phase
document that a later phase supersedes moves to that experiment's
`archive/` when the *narrative* is superseded, never when a result is
merely unflattering; nothing is deleted or rewritten to make a
conclusion look cleaner in hindsight.

## Limitations

No real FPGA board, no vendor place-and-route toolchain, and no
physical CXL hardware exist anywhere in this repository's development
environment — every synthesis number is a Yosys 0.33 generic or
`synth_ecp5` synthesis-tool proxy result, and every CXL/near-memory
number is a software simulation, never a physical measurement. No
result anywhere in this repository implies measured Fmax, timing
closure, or measured power unless explicitly labeled as such —
`hardware/` documents a *plan* for obtaining real numbers, not results.
See `RESEARCH_POLICY.md` for the full classification discipline
(`MEASURED_BY_TOOL`/`SIMULATED`/`ESTIMATED`/`UNAVAILABLE`) this applies
to every quantitative claim in the repository.

## AI-assistance disclosure

MEMBRANE's implementation, experiments, and this repository's own
documentation were built with heavy use of AI coding agents (Claude
Code) under Kadir Eren Altıntaş's direction. What the agents did,
what Kadir directed and decided, and what automated verification
does and doesn't cover, is disclosed in full in
[`outreach/ai-assistance-disclosure.md`](outreach/ai-assistance-disclosure.md).
AI agents are not described anywhere in this repository as independent
authors or autonomous project owners.

## License

Apache License 2.0 — see [`LICENSE`](LICENSE).

## Citation

See [`CITATION.cff`](CITATION.cff). For the maintained runtime, cite
`kadireren7/membrane` instead.
