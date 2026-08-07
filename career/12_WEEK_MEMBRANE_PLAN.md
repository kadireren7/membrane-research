# 12-week MEMBRANE plan

6-10 hours/week maximum on MEMBRANE specifically. Research continues
(see `ROADMAP.md`), but scope stays bounded — this plan exists so
"research continues" doesn't quietly become an unbounded time sink.
Each week names a real deliverable tied to a real file or checklist
item, not a vague theme.

## Weeks 1-2 — Ownership baseline

Work through `OWNERSHIP_CHECKLIST.md` in full — every item, not a
sample. Goal: be able to build, test, and explain the maintained repo
without any agent assistance in the room. This is the highest-priority
block; everything after it assumes it's done.

## Weeks 3-4 — Interview readiness

Work through `MEMBRANE_INTERVIEW_GUIDE.md`'s question list until the
30-second and 2-minute explanations are fluent without notes, and at
least 10 of the 20 exact questions can be answered from memory (technical
answer + evidence file, not just the simple answer). Run
`DEMO_SCRIPT.md`'s spine out loud at least twice, ideally recorded so it
can be reviewed.

## Weeks 5-6 — R2: production promotion exercise

Start `ROADMAP.md`'s R2 track: pick one already-mature research result
and take it through a real PR against `kadireren7/membrane`, reviewing
it personally (not delegating the review). Goal is process fluency on a
low-risk change, not a new architectural result — don't let this expand
into new research scope.

## Weeks 7-8 — R3: external reproduction

Run `ROADMAP.md`'s R3 track: reproduce `EXP-FPGA-DIV-001/reproduction/README.md`
or `EXP-FPGA-DIV-002/reproduction/README.md` end to end, personally,
against a fresh clone of both repositories. This is also the first real
test of the `MEMBRANE_PRODUCTION_ROOT`/`MEMBRANE_ROOT` path-resolution
gap disclosed in `ROADMAP.md`'s "Track 4" — expect to hit and document a
real issue, not a clean pass on the first try.

## Weeks 9-10 — Evidence and glossary maintenance

Re-read `EVIDENCE_OF_OWNERSHIP.md` and `TECHNICAL_GLOSSARY.md` against
whatever changed in weeks 1-8; update anything stale (a moved file, a
superseded number). This is deliberately scheduled, not left to "I'll
update it when I notice" — documentation like this rots quietly if
nobody owns keeping it current.

## Weeks 11-12 — Buffer and R5 scoping

No new deliverable required — this buffer exists because weeks 1-10
above will slip, and a plan with no slack teaches the wrong lesson. If
ahead of schedule, use this time to scope (not start) `ROADMAP.md`'s R5
— write down what a new experiment ID for the retirement-pressure
bottleneck would actually test, without committing to executing it
inside this 12-week window.

## What this plan explicitly does not include

Real hardware validation (`ROADMAP.md` R4) — gated on hardware/tool
access this plan doesn't assume exists yet, and not worth bounding time
against until it does. Paper submission — tracked separately in
`ROADMAP.md`'s "Also tracked" section, on its own timeline. Either can
displace a buffer week above if it becomes ready, but neither is a
committed deliverable of this plan.
