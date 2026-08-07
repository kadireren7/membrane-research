# Evidence of ownership

Concrete, checkable evidence that this project's direction and
validation decisions are Kadir's, not an agent's — every item below
names a real commit, file, or documented incident, not a general
assurance.

## Commit history

Every commit on `kadireren7/membrane`'s `main` and
`experiment/q8-divider-pipeline` is authored solely as Kadir Eren
Altıntaş/Altintas, no `Co-Authored-By` line — a project-wide, enforced
convention (`no-claude-in-commits`), not an accident:

```
61ce8ad research: reduce Q8 retirement pressure
6c033c9 research: make Q8 experiment artifacts provenance-safe
e416e36 research: reduce Q8 input head-of-line blocking
ecd996d research: reduce Q8 divider collateral stalls
5f05b4f research: evaluate dual exact radix-4 Q8 dividers
c26e834 research: baseline Q8 divider architecture
9dbbede fix: harden store backend test file handling
d72476d docs: record active main protection
a67b995 chore: add CodeRabbit review and CodeQL scanning
```

## A real, documented correction of AI-produced work

During the repository-focus migration (this document's own project),
an initial audit pass used a stale local `main` git ref instead of
fetching `origin/main` first, and incorrectly reported CodeQL,
CodeRabbit, and `docs/automated-pr-review.md` as missing from `main`
when they were already present (added by merged PR #4). **Kadir caught
this, rejected the incorrect report, and issued an explicit correction**
requiring a formal "Step 0" re-verification (fetch, resolve the real
SHA, verify each of 7 specific claims against `origin/main` and the
GitHub API directly) before any further migration work was authorized
to proceed. This is direct, checkable evidence of independent review
catching a real AI-produced error — not a hypothetical.

## The TOCTOU fix

`tests/unit/test_store_backend.c`, commit `9dbbede`: a real time-of-
check-to-time-of-use / symlink-substitution vulnerability in test file
handling, fixed by switching from path-based `chmod` to descriptor-based
`fchmod`, with a new adversarial regression test
(`test_dirfd_immune_to_path_swap`) proving the fix. Verified with
CodeQL, sanitizers, and stress runs per the commit message.

## Explicit, standing project rules that show ongoing judgment, not a one-time review

These are enforced across every session's work in this project, not
just stated once — evidence of a maintained standard, not a single
approval:

- No `Co-Authored-By` in any commit (project-wide).
- Never fabricate a measurement; never imply physical FPGA/CXL
  hardware, measured Fmax, timing closure, or power without real
  evidence (`RESEARCH_POLICY.md`).
- A negative result is reported at the same level of detail as a
  positive one, in the same document (`RESEARCH_POLICY.md`,
  demonstrated directly in `experiments/EXP-FPGA-DIV-002/README.md`'s
  Phase B3/R1/R2 entries).
- Destructive git operations, merges, branch deletion, and third-party
  submodule changes all require explicit authorization per action, not
  a blanket standing approval.

## The two-repository split's own decision record

The repository-focus migration itself is a five-decision, explicitly
authorized-in-stages process, not a single blanket instruction:
`membrane-research/provenance/repository-contract.md` sections 6-8
record each specific decision (revising `docs/repository-boundary.md`
in place, splitting `docs/reproduction.md`, keeping CodeQL/CodeRabbit
`membrane`-only, moving `outreach/**`, this document's own final
placement) and the exact boundary of what Gate B/C/D were and were not
authorized to do at each stage.

## What this document does not claim

It does not claim Kadir wrote every line of code by hand — see
`outreach/ai-assistance-disclosure.md` for the accurate, non-overclaimed
framing of what AI assistance did and didn't do. This document's purpose
is narrower: showing that the *decisions* (what to build, what counts as
valid, when to report a result as negative, what gets promoted) are
real, checkable, and his.
