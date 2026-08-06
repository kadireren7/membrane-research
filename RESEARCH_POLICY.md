# Research policy

This document states the standards this repository's research record is
held to, independent of any single experiment. It exists because a
research repository has no compiler to catch the equivalent mistake —
these rules are the enforcement mechanism.

## Negative and null results are reported, not reframed

`experiments/EXP-FPGA-DIV-002` is the clearest example in this
repository: five phases, one `PROMOTE_CANDIDATE` and four `CONTINUE`
decisions, one phase (B3's lookahead candidates) where the result was
the *opposite* of the hypothesis, and one phase (B4's R1 candidate)
where a change made things 84.3% worse on an adversarial pattern. All of
that is reported at the same level of detail as a positive result, in
the same document, not moved to a footnote. A negative result that
disproves a hypothesis is a completed experiment, not a failed one — it
is documented exactly like a positive one would be.

## Every claim traces to a real artifact

A number in a `README.md`, a paper table, or an outreach document must
be drawn from a file in that experiment's own `results/canonical/`, not
independently re-derived or rounded from memory. Where automated
verification exists (`scripts/verify-exp-q8-divider-002-results.py`,
`paper/scripts/verify-paper.py`, `scripts/verify-outreach.py`), it
checks this mechanically; where it doesn't yet cover a given document,
that gap is disclosed rather than assumed away.

## Measurement classification is mandatory, not advisory

See the top-level `README.md`'s "Measurement classification" section
for the four labels (`MEASURED_BY_TOOL` / `SIMULATED` / `ESTIMATED` /
`UNAVAILABLE`) and their exact meaning. A result with no classification
is treated as a defect in the document, not an acceptable omission.
Nothing in this repository is described as physical FPGA behavior,
physical CXL hardware behavior, measured Fmax, timing closure, or
measured power unless a real board or vendor toolchain actually
produced it — synthesis-tool cell counts (Yosys generic or
`synth_ecp5`) are proxy results for relative comparison, never
physical-utilization claims.

## Nothing is deleted when a conclusion changes

When a later phase supersedes an earlier one's approach or conclusion,
the earlier phase's document moves to that experiment's `archive/`
directory. It is never deleted and never silently rewritten. The
current README links to the current understanding; the archive
preserves how that understanding was reached, including the parts that
turned out to be wrong.

## Scope boundary with `kadireren7/membrane`

This repository is evidence and exploration. It does not carry the
maintained runtime, and a result here reaching `PROMOTE_CANDIDATE` (an
experiment-internal research verdict) is explicitly not the same thing
as that result being merged to `kadireren7/membrane`'s `main` — a merge
is a separate, later, product-repository decision, made via a normal PR
against that repository, gated by its own CI, CodeQL, and CodeRabbit
review. This repository does not run CodeQL or CodeRabbit itself: there
is no shipped binary or user-facing attack surface here for either tool
to usefully gate, and adding them would duplicate review infrastructure
without a corresponding integrity benefit. This is a decision, not an
oversight, and is revisited explicitly (in this document) if that ever
stops being true.

## AI assistance is disclosed, and does not change authorship

Kadir Eren Altıntaş is the project lead, decision-maker, and author of
record for every experiment, conclusion, and document in this
repository. AI coding agents are used heavily and disclosed plainly in
`outreach/ai-assistance-disclosure.md` — they are never described as
independent authors or autonomous project owners, and their output is
held to the same artifact-traceable, classification-labeled standard as
any other contribution before it is trusted.

## Corrections

If a result in this repository is later found to be wrong — a bug in
the reference model, a misclassified measurement, a broken reproduction
script — the fix is a new, disclosed correction (a new commit, and
where the error was material, a note in the relevant experiment's
`README.md`), not a silent edit to the original artifact.
