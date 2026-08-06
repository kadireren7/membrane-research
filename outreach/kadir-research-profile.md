# Research profile — Kadir Eren Altıntaş

A short, factual introduction for anyone deciding whether to respond to
an outreach email or review the MEMBRANE repository. No academic degree,
institutional title, or publication is claimed here — none exist yet.

## Background

- Student at **42 İstanbul**, part of the 42 network's project-based,
  peer-learning software engineering curriculum (no lectures, no
  professors — project- and peer-review-based, C/C++-heavy in its early
  curriculum).
- Primary technical interest: **C/C++ and systems-level development**
  — memory management, concurrency, and low-level performance work,
  which is what pulled me toward KV-cache memory as a problem in the
  first place.
- A brief prior background involving **İHA-1** (unmanned aerial
  vehicle) work — mentioned only because it's where the real-time/
  embedded-systems experience relevant to this project's hardware
  ambitions comes from, not elaborated further since it isn't the focus
  of this research.

## Role on MEMBRANE

I created and lead MEMBRANE as an AI-assisted systems research project.
I directed the architecture, experimental design, validation criteria,
technical decisions, and release process, while using AI coding agents
to assist with implementation and documentation. I'm the sole human
owner and maintainer of the project — there are no other people
involved as of this writing — and every commit, benchmark, and claim in
the repository was reviewed and directed under that ownership before it
went in. I did not hand-write every line myself, and I'm not going to
claim otherwise; what I take responsibility for is understanding the
design well enough to defend it, and catching it when a claim goes
further than the evidence supports (see
`outreach/ai-assistance-disclosure.md` for the fuller breakdown of what
that division of labor actually looked like).

## Research interests

- KV-cache memory management for LLM inference: precision tiering,
  exact vs. approximate retrieval, and where each approach's tradeoffs
  actually show up in measurement rather than intuition.
- The boundary between software simulation and physical hardware
  validation — specifically, being honest about which claims a
  simulator can and cannot support, and building the tooling
  (`scripts/verify-results.py`, `paper/scripts/verify-paper.py`) to
  enforce that boundary automatically rather than by discipline alone.
- Synthesizable RTL design for numerical/quantization datapaths, and
  bit-exact verification methodology between a CPU reference
  implementation and its hardware counterpart.
- Near-memory and CXL-based memory-tiering architectures for AI
  workloads, at the level of "what would actually bind first" rather
  than a general survey of the space.

## What kind of collaboration is being sought

Not funding, not a job, not co-authorship on unrelated work. Specifically:

1. **Hardware access** — FPGA board + synthesis toolchain
   (Vivado/Quartus), even time-boxed or remote, to run real
   place-and-route and, if that succeeds, a real board bring-up. See
   `outreach/membrane-technical-brief.md` and
   `docs/phase8-hardware-validation-plan.md`.
2. **CXL platform access** — a real CXL Type-3 memory device or an
   accessible emulation/prototyping platform, to check whether this
   project's near-memory simulation resembles real device behavior.
3. **Engineering input** on board-specific integration questions (DMA
   framework choice, AXI clocking constraints) from anyone with real
   board-bring-up experience — this project's RTL is deliberately kept
   interface-only and vendor-IP-free (`hardware/README.md`) so it can be
   adapted without redistributing anything proprietary.
4. **Critical review** — anyone willing to point out where a claim in
   `paper/main.md` or `docs/results-summary.md` is overstated, or where
   the related-work comparison (`paper/related-work-matrix.md`) is
   missing something relevant, is exactly the kind of engagement this
   project is looking for, independent of whether hardware access
   follows.

## Contact and links

- Repository: https://github.com/kadireren7/membrane
- Paper draft: `paper/main.md` / `paper/main.tex`
- Quick demo (no model download): `scripts/demo.sh --quick` — takes
  roughly 25–50 seconds depending on whether the ggml-parity build
  needs fresh configuration.
- Contact: see the repository's `SUPPORT.md` / `SECURITY.md` for the
  current maintainer contact address.
