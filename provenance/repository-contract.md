# MEMBRANE two-repository contract

**Status: ADOPTED — Gate B executed.** This document is the corrected,
final record of the repository-split contract. It supersedes the Gate A
draft that first proposed this split (preserved for its own record only
as this document's own git history in `kadireren7/membrane`'s
`migration/repository-contract.md` at the commit predating Gate B) —
that draft used a stale local `main` ref for part of its inventory and
listed five items as "open questions requiring a decision"; all five
were decided explicitly by Kadir before Gate B began, and are recorded
in section 6 below as decisions, not open questions.

This file's canonical location is here, in `membrane-research`'s own
`provenance/` directory — it is deliberately **not** committed to
`membrane` main, per Decision 5 (below): it documents research-side
migration process, not maintained-product policy.

## 1. Why this document exists

`kadireren7/membrane`'s `docs/repository-boundary.md` was an explicit,
reasoned, previously-made decision that MEMBRANE stays one repository,
with research separated from released state by branch, not by
repository. It said, anticipating exactly this situation: "if this
decision is ever revisited, it will be revisited here, explicitly and
disclosed, not implied." Decision 1 (below) revisits it there, in
place, rather than silently superseding it by starting a second repo
without comment.

## 2. Repository responsibilities

### `membrane`

Maintained C/C++ implementation, maintained production RTL, public
headers, tests, CI/sanitizers/CodeQL, build and release tooling, concise
architecture documentation, quick-start experience, a small set of
verified headline results, current limitations, a link to
`membrane-research`, AI-assistance disclosure, contribution guidance.

### `membrane-research`

Experimental RTL, failed/rejected architecture variants, experiment
plans, raw CSV/JSON/log evidence, canonical result manifests, provenance
validators, reproduction scripts, synthesis experiments, CXL/near-memory
studies, CPU quantization studies, FPGA divider experiments, the
academic manuscript, outreach/research-disclosure material, and all
future research branches and experiment records.

## 3. Single-source-of-truth rules

- Maintained source code lives only in `membrane`.
- Experiment evidence lives only in `membrane-research`.
- `membrane-research` references `membrane` code by repository URL,
  commit SHA, and tag — never by copying a live, evolving source tree.
- No duplicated live production trees. Snapshots are permitted only
  where strict reproduction of a specific, already-completed experiment
  requires freezing exact source at the commit it was evaluated
  against — such a snapshot is immutable and clearly labeled as a
  snapshot, not presented as current.

## 4. Classification taxonomy

| Classification | Meaning | Default destination |
|---|---|---|
| `maintained-product` | Released/reviewed code, docs, or CI that a user or interviewer needs to build, test, and understand the current library | `membrane` |
| `active-research` | Evidence, RTL, or tooling for an experiment that has not concluded (`CONTINUE`/open decision) | `membrane-research` |
| `historical-research` | Evidence for a concluded experiment (promoted, rejected, or superseded) | `membrane-research`, in `archive/` |
| `shared-policy` | Governs the process of both repositories | Resolved per-file in Decision 2/3, not a blanket rule |
| `generated-result` | Machine-produced CSV/JSON/log/trace evidence | `membrane-research/experiments/*/results/canonical/` |
| `vendored` | Third-party code brought in as-is | `membrane`, untouched |
| `temporary` | Local build output, smoke-test artifacts, monitor logs | Excluded from both repos (already gitignored, not tracked) |

## 5. Corrected source of truth (Step 0 re-verification)

The Gate A draft's inventory used a stale local `main` git ref for
enumeration (behind `origin/main` by three merged PRs) before ever
running `git fetch`. This was corrected before any copying began:

- **Source refs used**: `origin/main` (`9dbbede255dccf025cc3ecad7f17cd9f52f384a8`),
  `origin/experiment/q8-divider-pipeline` (`61ce8adc4733512a78dcf5c04844e6b85da04b54`),
  `origin/experiment/fp-divider-pipeline` (`4c22efa5bc29f42579f2ea641dd6c9458dec988c`,
  16 unique files not present on either other ref). All other branches
  were checked and contributed zero unique content.
- **Verified against `origin/main` directly** (not inferred from local
  git log): `.coderabbit.yaml`, `.github/workflows/codeql.yml`, and
  `docs/automated-pr-review.md` are present (added by merged PR #4,
  `a67b995`); PRs #4, #5, #6 are all merged, with #6 (`9dbbede`) as
  current HEAD; the TOCTOU fix (`O_NOFOLLOW`, `fchmod(dirfd,...)`,
  `test_dirfd_immune_to_path_swap`) is present in
  `tests/unit/test_store_backend.c`; 0 open CodeQL alerts; the
  `main-protection` ruleset (id `20201573`) is active.
- **Corrected inventory**: 476 total files across the three refs, 0
  duplicates by content SHA256, 0 unclassified paths. 175 → `membrane`,
  301 → `membrane-research`. Full per-file record, including every
  `source_sha256` and (post-copy) `destination_sha256`:
  [`import-manifest.json`](import-manifest.json) in this same directory.

## 6. Decisions (previously listed as open questions in the Gate A draft)

### Decision 1 — Revisiting the single-repository policy

`docs/repository-boundary.md` on `membrane` main is revised in place
(not deleted, not silently superseded) to document the two-repository
model, state why it changed, and mark its status as "Superseded by the
two-repository boundary decision adopted during the repository-focus
migration" — per that document's own Rule 5. This revision is a
`membrane`-side change and is tracked there, not duplicated here.

### Decision 2 — Splitting `docs/reproduction.md`

`membrane` keeps a trimmed reproduction document covering Level 1 only
(unit tests, sanitizers, ggml parity, RTL cosimulation — depends only on
`src/`, `include/`, `rtl/*.sv`, `rtl/tb/*`, all of which stay in
`membrane`). The full three-level document — including Levels 2-3
(KV-cache/CXL tools, multi-hour sweeps) — is reproduced here, tied to
the tools and benchmark data that now live in this repository. This
repository's own full reproduction guide is at the repository root,
`reproduction.md`.

### Decision 3 — CodeQL/CodeRabbit stay `membrane`-only

No separate installation PR was needed: `.coderabbit.yaml`, `.github/
workflows/codeql.yml`, and `docs/automated-pr-review.md` were already
present on `origin/main` (the Gate A draft's claim otherwise was the
stale-ref error corrected in section 5). `membrane-research` gets its
own minimal research CI instead of inheriting these — see
`RESEARCH_POLICY.md`'s "Scope boundary" section for why.

### Decision 4 — `outreach/**` and `scripts/verify-outreach.py`

Both moved to `membrane-research` (copied here first, with
SHA256-verified content; removal from `membrane` main is a separate,
not-yet-authorized step — see this document's own Gate B authorization
boundary, section 8). `outreach/` and its verifier check research
communication material with no role in the maintained release process.

### Decision 5 — This document's own final placement

This file and `import-manifest.json` are valid as final migration
records but are **not** committed to `membrane` main — their audience
and subject matter (research-side migration provenance) belong with the
research record they describe. Final placement:
`membrane-research/provenance/`, as this file itself now demonstrates.

## 7. What Gate A deliberately did not do (historical record)

- No new GitHub repository was created during Gate A.
- No file was copied, moved, or deleted from `membrane` during Gate A.
- No `docs/*.md` was edited during Gate A.
- `destination_sha256` was not computed during Gate A (nothing existed
  at any destination yet). All 301 `membrane-research` destination
  hashes were computed and verified during Gate B — see
  `import-manifest.json`.

## 8. Gate B authorization boundary

Gate B was explicitly authorized to: create `kadireren7/membrane-
research`, populate it, commit and push its initial verified content,
create research CI, and consolidate long documentation into `archive/`
without deleting it, preserving source commit and SHA256 provenance.

Gate B was explicitly **not** authorized to: delete research files from
`membrane` main, merge any future `membrane`-side cleanup PR, delete any
branch, alter any tag, or modify `third_party/llama.cpp` in any way. As
of this document's writing, none of those five actions has been taken —
`membrane`'s `main`, its `experiment/*` branches, and
`third_party/llama.cpp`'s pre-existing local state are all untouched by
this migration.
