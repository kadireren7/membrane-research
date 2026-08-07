# Interview demo script

For a live walkthrough in an interview or portfolio call — different
from `outreach/demo-video-script.md` (a recorded external video). This
one assumes an interviewer asking questions in real time, so it's a
shorter spine with room to go deep wherever they steer it, not a fixed
narration.

## Before the call

- Have both repos cloned locally, `membrane` built (`cmake --build
  build -j`), so nothing live depends on network/build time.
- Have `experiments/EXP-FPGA-DIV-002/README.md` open in a tab — it's the
  single best document for "walk me through a real negative result."
- Know the two-repository URLs by heart:
  `github.com/kadireren7/membrane`,
  `github.com/kadireren7/membrane-research`.

## Spine (5-8 minutes if uninterrupted; expect interruptions)

1. **(30s)** Give the 30-second explanation from
   `MEMBRANE_INTERVIEW_GUIDE.md`.
2. **(60s, live terminal)** `./scripts/demo.sh --quick` in `membrane` —
   let it run unedited, narrate what each of the four steps actually
   checks while it runs (build, ggml quant parity, FPGA Verilator cosim,
   exact-retrieval scenario).
3. **(90s)** Open `experiments/EXP-FPGA-DIV-001/README.md` in
   `membrane-research` — this is the result that *did* reach production.
   Show the phase table, then jump to
   `results/canonical/b1-differential.json` and point at
   `"mismatches_d": 0` — say explicitly that "0" here means checked and
   zero, not unchecked.
4. **(90s)** Open `experiments/EXP-FPGA-DIV-002/README.md` — the related
   result that *didn't* reach production. Walk through the Phase B3
   lookahead entry specifically: state the hypothesis, state that it was
   wrong, state the real measured cause. This is the moment to
   demonstrate comfort with negative results, not rush past it.
5. **(60s)** Show the two-repository split — `README.md`'s "Relationship
   to kadireren7/membrane" section in `membrane-research`, then
   `docs/repository-boundary.md` in `membrane`. Explain the split
   preserves an earlier explicit decision *against* a second repo by
   revising that document in place, not deleting it.
6. **(remaining time)** Follow wherever the interviewer goes — the
   `MEMBRANE_INTERVIEW_GUIDE.md` question list covers the most likely
   directions (RTL detail, AI-assistance boundary, what's measured vs.
   simulated).

## If asked "what would you do differently"

Have a real answer ready, not a rehearsed platitude — e.g. the B4
retirement-pressure ceiling (`ROADMAP.md`'s "Closed experiments"
section) is a legitimate place to say "a larger reorder structure would
plausibly help, and here's why every phase explicitly excluded trying
it." Naming a real, scoped, still-open technical question is stronger
than a vague "I'd add more tests."

## What not to do

- Don't narrate a number you can't currently see on screen — if asked
  for a figure you don't remember exactly, open the file and read it
  live rather than guessing confidently.
- Don't claim physical FPGA/CXL measurement exists anywhere — if asked
  "did you run this on real hardware," the honest answer is no, and
  `RESEARCH_POLICY.md`'s measurement classification is *why* that
  question has a fast, confident answer instead of an awkward one.
- Don't over-credit or under-credit the AI assistance — use
  `outreach/ai-assistance-disclosure.md`'s framing verbatim if unsure
  how to phrase it under pressure.
