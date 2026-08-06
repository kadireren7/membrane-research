# AI-assistance disclosure

MEMBRANE was built with heavy use of AI coding agents (Claude Code).
This document says plainly what that means in practice, so a reader
doesn't have to guess or take a vague "AI-assisted" label on faith. It
does not make a legal ruling on authorship or IP ownership — that's a
question for a lawyer if it ever matters, not something this document
resolves.

## What Kadir directed and decided

- The overall research question (does a per-block precision + exact-
  retrieval decision engine help, and where does it not) and the
  project's phased scope.
- System architecture: which components exist (mixed-precision
  runtime, exact sparse retrieval, CXL/near-memory simulation, FPGA
  datapath, out-of-core simulator infrastructure) and how they relate.
- Experimental design: what to measure, which baselines to compare
  against, what counts as a valid test, and when a result should be
  reported as negative rather than reframed to look better.
- Validation criteria: the REAL/SIMULATED/EXTRAPOLATED/ORACLE/ASSUMED
  labeling discipline used throughout the project, and the decision to
  build automated claim-checking tooling
  (`scripts/verify-results.py`, `paper/scripts/verify-paper.py`,
  `scripts/verify-outreach.py`) rather than rely on manual review alone.
- Every technical decision that shows up as a disclosed tradeoff in the
  docs (e.g. choosing exact retrieval over eviction, choosing a
  streaming out-of-core backend after hitting a real memory ceiling)
  was a real decision made in response to a real finding, not a
  cosmetic choice.
- What gets published where, what gets sent to whom, and — specific to
  this outreach package — the decision to write this disclosure at all
  rather than leave the AI-assistance question implicit.

## What coding-agent assistance actually did

- Wrote the bulk of the C11/C++17/SystemVerilog implementation, test
  code, and documentation prose, under direction and iterative review.
- Ran builds, tests, and simulators; proposed fixes when something
  failed; drafted docs summarizing results.
- Performed literature search for `paper/related-work-matrix.md` and
  drafted the comparison table (each source was independently checked
  against its own arXiv/ACM page before being trusted, not taken on the
  model's word).
- Drafted this outreach package's materials from real repository
  content, then had them checked by the same automated tooling
  (`scripts/verify-outreach.py`) that checks everything else.

## What automated verification does (and doesn't) cover

`scripts/verify-results.py`, `paper/scripts/verify-paper.py`, and
`scripts/verify-outreach.py` check that specific numbers in the docs
match specific source artifacts, that labels (REAL/SIMULATED/etc.) are
used consistently, that citations resolve, and that a defined list of
prohibited overclaim phrases doesn't appear unhedged. This catches a
real, meaningful class of error — and has caught real ones during this
project's own history (see `docs/phase7-research-release.md` and
`docs/phase7-academic-paper.md` for two documented examples where the
verifier found and fixed a real transcription error before it shipped).

It does **not** catch: whether the underlying experiment design is
sound, whether a simulator's assumptions are reasonable, or whether a
sentence is technically accurate but misleading in framing. That's why
this project still needs a human — Kadir — who understands the design
well enough to defend it in a real conversation, not just to point at a
green checkmark.

## What a human needs to understand and defend

Concretely: if a lab, reviewer, or collaborator asks a hard question
about *why* the exact-retrieval design was chosen over eviction, *why*
the CXL figures are assumptions rather than measurements, or *what
exactly* the FPGA cosimulation does and doesn't prove, the answer needs
to come from Kadir's own understanding of the system — not from
re-reading a doc on the spot. That's the standard this project holds
itself to, and it's a different (and higher) bar than "the code
compiles and the tests pass."

## Authorship vs. line-by-line hand-writing

This project's repository, paper, and outreach materials list Kadir
Eren Altıntaş as the sole human author/owner — that is a claim about
**direction, responsibility, and ownership**, not a claim that every
character of every file was typed by hand without AI assistance. Most
of the literal text and code in this repository was drafted by an AI
coding agent under Kadir's direction and review. Both things are true
at once, and this document exists specifically so no one has to infer
which one is meant when the word "author" appears elsewhere in this
project.
