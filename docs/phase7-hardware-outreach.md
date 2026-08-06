# Phase 7.3: Hardware Validation Outreach Package

Baseline: commit `e74d719` (Phase 7.2, academic manuscript). This phase
produced **no new experimental results and made no outbound contact
with any real person or institution.** Its job was to prepare a
scoped, claim-limited outreach and physical-validation-planning package
that could be sent to labs/companies in the future — nothing in this
phase was sent, and no relationship, response, or prior conversation is
claimed anywhere in the produced materials.

## Packages created

- **Outreach materials** (`outreach/`): a technical brief (2-4 pages),
  a one-page summary, four email templates (university professor, FPGA
  lab, CXL/memory-systems team, company research team), a personal
  research profile for Kadir, a demo video script, a research-talk
  outline, a target-selection framework (no named targets), and an
  empty contact tracker template.
- **Hardware validation planning** (`docs/phase8-hardware-validation-plan.md`,
  `hardware/`): a 3-level plan (FPGA sim/impl, real board, CXL
  platform), candidate board/toolchain research, a real risk register,
  a sequential experiment protocol, and a JSON schema for physical
  results.
- **Vendor-integration interface** (`hardware/vendor-wrapper/`):
  AXI4-Stream adapter, AXI4-Lite control-register adapter, a
  platform-independent DMA command/completion interface, and a
  top-level wrapper — all interface-only, no vendor IP.
- **Claim governance** (`outreach/hardware-claim-gates.md`): 8 explicit
  gates mapping "which test must pass" to "which sentence becomes
  allowed," enforced automatically by `scripts/verify-outreach.py`.
- **Release/CI infrastructure**: a GitHub Actions paper-build workflow
  (`.github/workflows/paper.yml`) and a release-candidate checklist
  (`docs/release-candidate-checklist.md`, checklist only — no tag or
  release was created).

## Evidence used (nothing new; all from Phase 0-7.2)

Every technical claim in the outreach materials traces to an artifact
that already existed before this phase: `benchmarks/MANIFEST.json`'s
33 tracked artifacts, `paper/claim-audit.md`'s 11 audited claims, and
`docs/phase5-synthesizable-fpga.md`'s real (yosys/ECP5) synthesis cell
counts — reused directly in `hardware/board-targets.md` rather than
re-derived. The one genuinely new verification step this phase
performed: **elaborating the new `hardware/vendor-wrapper/*.sv` files
together with the full existing, cosimulation-verified `rtl/` hierarchy
under yosys 0.33** (`hierarchy -top membrane_core_wrapper`) — this
succeeded with 0 errors and 0 warnings attributable to the new files (all
46 unique warnings in the combined run are pre-existing, from already-
verified `rtl/` modules). This confirms the wrapper's port/parameter
wiring is structurally consistent with the verified core — it does
**not** confirm synthesizability on a real vendor toolchain, timing
closure, or anything about real silicon.

## Which hardware claims remain prohibited

Per `outreach/hardware-claim-gates.md`, as of this phase:

- **"Hardware bit-exact"** — prohibited; only "simulation bit-exact" is
  earned (Gate 0/1 passed, Gate 4 not).
- **"FPGA-deployed" / "runs on an FPGA board" / "board-verified"** —
  prohibited; no board exists (Gate 3 not passed).
- **Any specific achieved clock frequency** — prohibited; only the
  100/200/300 MHz *assumption* from `docs/phase5-synthesizable-fpga.md`
  may be cited, always labeled as an assumption (Gate 2 not passed).
- **Any throughput/latency/power number presented as a hardware
  measurement** — prohibited entirely; every such number in this
  project is a simulation or cosimulation output (Gate 5/6 not passed).
- **"Real CXL acceleration" / "CXL-accelerated"** — prohibited; only
  "CXL architecture simulation" is earned (Gate 7 not passed).

`scripts/verify-outreach.py` enforces the last two automatically (a
"gated claim" scan across `outreach/` and `hardware/`, 9/9 checks
passing as of this phase, including three negative tests: a fake
`REAL_HARDWARE`-labeled result, an injected prohibited claim, and a
removed required lab-package file were each deliberately introduced and
confirmed to be caught, then reverted).

## Resources needed from a lab

Summarized in `outreach/lab-package/required-hardware.md`; in order of
value: (1) FPGA board + Vivado/Quartus toolchain access (even
time-boxed/remote) for a real place-and-route result; (2) if that
succeeds, board bring-up + real DMA/throughput/parity measurement; (3)
a real CXL Type-3 device or emulation platform access, the single
hardest resource to obtain per `hardware/risk-register.md`.

## Physical-validation success criteria

Defined per level in `docs/phase8-hardware-validation-plan.md`:
- **Level A**: real timing closure at some frequency + bit-exact
  post-route simulation.
- **Level B**: 0 unexplained mismatches on a 100,000+-block real-
  hardware parity test, plus a real (not estimated) throughput/latency/
  power measurement, whichever direction it points.
- **Level C**: a real device successfully serving the exact-retrieval
  workload end-to-end, plus a written comparison against this project's
  simulated CXL assumptions — valuable even if the comparison shows the
  simulation was wrong.

## LaTeX/PDF build status

**Unchanged from Phase 7.2: no LaTeX toolchain exists in this
development environment** (`pdflatex`/`xelatex`/`lualatex` all absent).
`paper/main.pdf` has still never been built locally. This phase adds a
**GitHub Actions workflow** (`.github/workflows/paper.yml`) as the
disclosed, explicit path to a real PDF build: it installs TeX Live on
the CI runner, regenerates figures/tables, runs
`paper/scripts/verify-paper.py`, runs `paper/build.sh`, and uploads
`paper/main.pdf` as a build artifact. **This workflow has not been
executed** (that requires pushing to GitHub and letting Actions run,
which is outside this phase's scope) — its YAML was syntactically
validated (parses correctly as a well-formed GitHub Actions workflow:
`name`/`on`/`jobs` keys present, one job with `runs-on` and a real step
sequence) but not exercised end-to-end. No PDF is claimed to exist
anywhere in this repository.

## Verification performed

- `scripts/verify-outreach.py`: 9/9 checks (lab-package completeness,
  internal link resolution, results-schema validity, example-fixture
  schema conformance, zero `REAL_HARDWARE` records present, zero hype
  words, zero unhedged gated claims, gate-table self-consistency,
  email-template placeholder-safety).
- `paper/scripts/verify-paper.py`: 11/11 (unchanged, re-confirmed).
- `scripts/verify-results.py`: 13/13 (unchanged, re-confirmed).
- `scripts/prepare-release.sh --dry-run`: passes except the (expected,
  pre-commit) dirty-tree check.
- Existing Release test suite: 28/28 (unchanged; no C/C++ source was
  modified this phase).
- yosys elaboration of `hardware/vendor-wrapper/*.sv` + the full
  existing `rtl/` hierarchy: succeeds, 0 new errors/warnings.
- Three negative tests (fake `REAL_HARDWARE` result, prohibited
  hardware claim, missing lab-package file): all three deliberately
  introduced and confirmed caught by `scripts/verify-outreach.py`,
  then reverted.

## What's still missing (disclosed, not hidden)

- No physical FPGA board, CXL device, or vendor toolchain has been
  used — every hardware-adjacent claim in this repository remains
  simulation, cosimulation, or an assumption.
- The GitHub Actions paper-build workflow has not actually been run
  (no PDF has been produced by it yet).
- No lab or company has been contacted; the outreach materials are
  unsent templates.
- `hardware/vendor-wrapper/`'s AXI4-Lite responder is explicitly
  flagged in its own file as an unverified hand-written FSM, not a
  protocol-checker-verified implementation.

## Authorship and AI-assistance wording correction (follow-up pass)

A follow-up pass, before anything in this package was sent to anyone,
reviewed the outreach/paper/docs wording for honesty and naturalness
and corrected several things:

- **Authorship framing.** The original materials described Kadir as
  "sole author" of MEMBRANE in a way that, read alongside phrases like
  "every line of code... was authored... by Kadir" and "written from
  scratch," could be read as claiming he hand-typed the entire
  codebase without AI assistance. In practice, this project makes heavy
  use of AI coding agents (Claude Code) for implementation and
  documentation, under Kadir's direction. Every occurrence of this
  framing across `outreach/`, `paper/main.md`, and `paper/main.tex` was
  rewritten to the more accurate: Kadir created and leads MEMBRANE,
  directing architecture, experimental design, validation criteria, and
  technical decisions, and reviewing every commit — while an AI coding
  agent drafted most of the actual code/documentation text under that
  direction. A new `outreach/ai-assistance-disclosure.md` spells out
  this division of labor explicitly, including what a human still needs
  to understand and defend versus what automated tooling checks.
- **"Independent researcher."** Removed everywhere it appeared
  (`outreach/email-templates.md`, twice) — Kadir is not a credentialed
  or institutionally-affiliated researcher yet, and the phrase read as
  more claim than is accurate. Replaced with "systems programming
  student (42 İstanbul)" framing, consistent with
  `outreach/kadir-research-profile.md`.
- **"Production quantizer"/"production reference implementation."**
  Replaced with "ggml's own reference implementation" / "ggml reference
  quantizer" in `paper/main.md`, `paper/main.tex`, and
  `outreach/one-page-summary.md` — ggml is a real, widely-used
  open-source reference implementation, but "production" implied a
  formality/certification that was never actually claimed or verified.
- **CXL-value sourcing.** Some CXL link latency/bandwidth figures are
  informed by standard PCIe-generation bandwidth math, not literally
  quoted from the CXL Consortium's specification text. Wording that
  said figures were "drawn from the CXL Consortium's own published
  specification" was corrected, everywhere it appeared, to "informed by
  the CXL Consortium's specification and standard PCIe-generation
  bandwidth figures, not a literal quote from either."
- **Demo duration.** `scripts/demo.sh --quick` was documented as "~25
  seconds" based on an early, warm-build-cache measurement. A fresh,
  cold-cache re-measurement during this pass showed it can take up to
  ~51 seconds when the `MEMBRANE_ENABLE_LLAMA` build needs fresh
  configuration (27s warm-cache vs. 51s cold-cache, both real
  measurements taken during this pass). Every mention across
  `outreach/`, `paper/main.md`, `paper/main.tex`, and
  `docs/phase7-research-release.md` (the latter kept as a historical
  record with an added caveat, not silently rewritten) now states a
  ~25–50 second range instead of a single cherry-picked number.
- **Email templates.** Shortened from ~250 words to ~120–180 words
  each, first paragraph simplified, metric-stacking reduced, and
  repeated defensive disclaimers ("no obligation," "not a sales
  conversation," "as a favor") trimmed to appear at most once per
  email. A new `outreach/primary-email-template.md` provides a single,
  general-purpose ~150-word template for recipients who don't cleanly
  fit one of the four category-specific ones.
- **Verification.** `scripts/verify-outreach.py` gained three new
  checks (no phrase implies unassisted hand-authorship; "sole
  author"/"independent researcher" only in accurate/qualified form;
  every demo-duration mention is qualified) — 12/12 checks pass. Three
  new negative tests were run and confirmed caught before being
  reverted: an injected "I personally wrote every line of code"
  phrase, an injected fake `REAL_HARDWARE`-labeled result, and an
  injected unqualified "~25 seconds" demo claim.

None of this changes any technical/experimental claim's substance —
462/462 scenarios, 520,000 transactions, 100,000+-block parity, the
187x-405x traffic reduction figures, and the ~73,600-LUT-class-cell
divider estimate are unchanged and re-verified. This pass only
corrected how the project's authorship, development process, and two
sourcing details were described.

## Update: GitHub Actions verification repair (`58ec90b`, follow-up pass)

This document's "LaTeX/PDF build status" section above (and the
corresponding bullet under "What's still missing") originally reported
that the `Paper Build` GitHub Actions workflow "has not been executed."
That is now out of date: a follow-up pass diagnosed and fixed two real
CI bugs (a hardcoded test timeout tuned to one machine's speed, and a
log-scoping bug in `paper/build.sh`'s citation check — see the
`fix: repair GitHub Actions verification` commit for full detail),
and confirmed via real
`gh run` logs that both `CI` (Debug, ASan+UBSan, TSan) and `Paper
Build` now pass on the actual GitHub Actions runner, with
`paper/main.pdf` genuinely produced as a real CI build artifact. The
original text above is left as written (an accurate account of this
phase's own state at the time) rather than silently edited — this note
is the correction, following the same disclosure pattern as the
authorship section above it.
