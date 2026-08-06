# Contributing to MEMBRANE Research

This repository holds a research record, not a product. Before adding
anything, read the top-level `README.md`'s "Measurement classification"
section — it is the one rule every contribution here is held to.

## What belongs here vs. `kadireren7/membrane`

- New experiments, simulators, exploratory RTL, paper work, and outreach
  material belong here.
- Anything intended to become part of the maintained runtime does not
  start here — it starts as an experiment here (if it needs one), and
  moves to `kadireren7/membrane` via a PR against that repository once
  it has evidence behind it. This repository never becomes the source
  of truth for code that ships.
- Do not duplicate `kadireren7/membrane` source files here to make an
  experiment self-contained. Reference the production file by path and
  commit instead (see any existing experiment's `README.md`
  "Provenance" section for the pattern) — duplication is exactly what
  the two-repository split was meant to avoid.

## Adding a new experiment

Follow the shape every existing experiment under `experiments/` uses:

```
experiments/EXP-<ID>/
  README.md              executive summary, phase/result timeline,
                          what (if anything) reached production
  methodology.md         test design, exactness rules, toolchain,
                          measurement classification
  results/canonical/     the actual result artifacts every claim cites
  results/schemas/README.md   field docs for those artifacts
  reproduction/README.md exact commands, including any known gap
  archive/               every phase document, preserved even after
                          superseded — never deleted, only archived
```

Do not write a result into a README or paper table that isn't backed by
a file in that experiment's own `results/canonical/`. Do not report a
synthesis timeout, a missing measurement, or a null/negative result as
a zero or as silently absent — see the classification rules.

## Reproducibility

Every experiment's `reproduction/README.md` must give exact,
copy-pasteable commands, and must disclose any known gap in actually
running them (a path assumption, a version pin, an unverified flag) —
"this hasn't been re-verified end to end" is an acceptable thing to
write here; a reproduction doc that silently omits a known-broken step
is not.

## Measurement classification (enforced, not optional)

Every quantitative claim is one of `MEASURED_BY_TOOL`, `SIMULATED`,
`ESTIMATED`, or `UNAVAILABLE`. Never imply physical FPGA hardware,
physical CXL hardware, measured Fmax, timing closure, or measured power
unless a real board/toolchain produced the number — see `hardware/` for
the (currently unexecuted) plan to obtain real hardware numbers, and
don't anticipate its results in any other document.

## AI-assisted contributions

This project is built with heavy AI-agent assistance under human
direction — see `outreach/ai-assistance-disclosure.md` for exactly what
that means in practice here. If you're contributing with AI assistance
yourself, the same standard applies: you are the author and decision-
maker of record, the tool is not, and every claim it drafts gets
checked against a real source artifact before it's committed.

## Pull requests

Changes to `main` go through a PR. Research CI (link validation, JSON/
CSV schema checks, canonical-hash verification, provenance validation)
must pass before merge — see `.github/workflows/`. CodeQL and
CodeRabbit are `kadireren7/membrane`-side tools and are not run here
(see `RESEARCH_POLICY.md` for why); do not add them to this repository
without an explicit decision to do so.

## Commit attribution

Kadir Eren Altıntaş is the sole author of record for this project.
Commits do not carry `Co-Authored-By` lines for AI assistance.
