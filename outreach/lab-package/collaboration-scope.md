# Collaboration scope

What this collaboration is, and explicitly is not.

## What's being asked for

- Hardware/toolchain access (FPGA board + Vivado/Quartus, and/or a CXL
  platform), time-boxed or remote — whatever is realistic for the lab.
- Engineering time to run `hardware/experiment-protocol.md`'s steps and
  report results back honestly, including negative ones.
- Optionally, integration expertise (board-specific DMA framework
  choice, AXI clocking questions) if someone on the team has real
  board-bring-up experience — this project's RTL is deliberately
  interface-only and vendor-IP-free (`hardware/README.md`) so it can be
  adapted without redistributing anything proprietary.

## What's explicitly NOT being asked for

- **No funding.** This is not a funding request.
- **No exclusivity.** Nothing about this collaboration is exclusive —
  the repository is public and Apache 2.0 licensed
  (`docs/licensing.md`); a lab helping with hardware validation gains no
  special claim over the project.
- **No co-authorship obligation.** If a lab's contribution is
  substantial enough to warrant co-authorship on a future paper, that
  would be discussed and credited properly at that time — but no
  contribution is conditioned on agreeing to that in advance.
- **No data-sharing beyond what's public.** All traces/prompts used are
  already committed to the repository and non-sensitive; a lab is not
  being asked to share proprietary data.
- **No commitment to a specific timeline.** The 2-4 week plan in
  `outreach/membrane-technical-brief.md` is a proposal, not a deadline —
  scope and pace should follow what's realistic for the lab's own
  schedule.

## Attribution

If a lab's real hardware results are incorporated into this project
(per `outreach/lab-package/expected-artifacts.md`), the lab/individual's
contribution would be credited explicitly in the relevant document
(`docs/phase8-hardware-validation-plan.md`, `CHANGELOG.md`, and/or
`paper/main.md`'s acknowledgments if a future paper revision includes
one) — exactly how is something to agree on with the specific
contributor at the time, not decided unilaterally in advance here.

## Point of contact

Kadir Eren Altıntaş, creator and lead of MEMBRANE — see
`outreach/kadir-research-profile.md` and the repository's `SUPPORT.md`
for current contact details.
