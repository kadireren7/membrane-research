# Research release freeze

## Freeze baseline

**`v0.1.0-research`, commit `8298e953b792c78aa8604c7558ef701b2b862b28`.**
This document was originally written before that tag existed, describing
`58ec90b` as the pre-audit baseline and stating the actual release
commit's SHA was "not known until it is made." The tag has since been
created (see `docs/v0.1.0-research-release-plan.md`,
`docs/public-release-audit.md` for how it was audited and produced) —
this section is updated to name it directly, disclosed here rather than
left stale.

## What "freeze" means

**The freeze applies to the `v0.1.0-research` tag, not to repository
development.** Concretely:

- The tag `v0.1.0-research` and the commit it points to
  (`8298e953b792c78aa8604c7558ef701b2b862b28`) never move and are never
  edited in place — no rewritten history, no force-pushed tag, no edited
  release notes that add a new number or claim.
- Repository development does **not** stop. New work continues in the
  open, in `experiment/*`, `feature/*`, `fix/*`, and `docs/*` branches of
  this same public repository — see
  [docs/repository-boundary.md](https://github.com/kadireren7/membrane/blob/main/docs/repository-boundary.md) and
  [docs/open-development-policy.md](open-development-policy.md).
- Results produced on those branches are not verified public claims of
  the `v0.1.0-research` release. Nothing merged or developed after this
  freeze retroactively changes what `v0.1.0-research` claims.
- Verified new results — once they pass the same bar this release was
  held to (real measurement, sourced, claim-audited, negative results
  included, reproducible) — are folded into a **future, separate**
  release (`v0.2.0-research` or later): a new tag, never a mutation of
  this one.

## Public release scope (`v0.1.0-research`)

Everything in the public `kadireren7/membrane` repository at commit
`8298e953b792c78aa8604c7558ef701b2b862b28`: the C11/C++17/SystemVerilog
implementation, the discrete-event simulators, the bit-exact quantization
verification, the unified 128K×512 sweep results, the FPGA cosimulation,
the academic manuscript (`paper/`), the reproduction/verification tooling
(`scripts/`, `paper/scripts/`), the hardware-validation plan and outreach
package (`hardware/`, `outreach/`), and the full phase-by-phase
documentation (`docs/`).

## Verified components (as of this freeze)

- **Software/simulation**: 462/462 unified-sweep scenarios, bit-exact
  CPU/ggml quantization parity (100,000+ blocks), FPGA/CPU Verilator
  cosimulation (520,000 transactions) — all re-verifiable via
  `scripts/verify-results.py` (13/13) and `scripts/demo.sh`.
- **Academic manuscript**: claim-audited (`paper/claim-audit.md`),
  14 independently-verified citations, `paper/scripts/verify-paper.py`
  (11/11), and `paper/main.pdf` builds successfully as a real GitHub
  Actions artifact (workflow: `Paper Build`).
- **CI**: `CI` (Debug, ASan+UBSan, TSan) and `Paper Build` both pass on
  the real GitHub Actions runner for this exact commit.
- **Outreach package**: `scripts/verify-outreach.py` (17/17), claim-gate
  enforcement (`outreach/hardware-claim-gates.md`), authorship/
  AI-assistance wording corrected and consistent (`bb4df95`).

## Open technical limitations (unchanged by this freeze)

- No real FPGA board, place-and-route result, or board bring-up exists.
- No real CXL hardware or CXL platform access exists.
- No real GPU serving-stack integration exists.
- Model scale is 135M/360M — below production LLM sizes.
- Every claim gated in `outreach/hardware-claim-gates.md` past Gate 1
  remains prohibited until its required real-hardware evidence exists.

This freeze does not change any of the above — it freezes the
`v0.1.0-research` *tag* as reproducible and internally consistent, not a
claim that physical validation has happened, and not a claim that
development itself has stopped.

## What changes are acceptable in the public repo after this freeze

- Fixes to reproducibility, CI, documentation accuracy, or internal
  consistency (the same category of change this freeze itself makes),
  developed on `fix/*` or `docs/*` branches and merged to `main`.
- Corrections to a claim found to be inaccurate, overstated, or
  inconsistent — always disclosed, never silently softened.
- New, real hardware-validation results that pass their corresponding
  gate in `outreach/hardware-claim-gates.md`, developed on an
  `experiment/*` branch and promoted to `main` through the controlled
  process in `docs/repository-boundary.md`.
- Genuinely new experiments, run with the same rigor as existing ones
  (real measurement, sourced, claim-audited, negative results included) —
  this is the expected, ongoing use of `experiment/*` branches, not an
  exception to the freeze.

None of the above ever edits the `v0.1.0-research` tag itself; verified
outcomes accumulate toward a future tagged release instead.

## What is not acceptable without an explicit, disclosed decision

- Adding a new headline claim not backed by a committed artifact.
- Removing or softening a negative/null finding.
- Silently changing a verified number already published under
  `v0.1.0-research`.
- Claiming physical hardware validation before its gate is actually
  passed.
- Moving or force-pushing the `v0.1.0-research` tag, or editing its
  release notes to add new numbers or claims.

## Where new work happens

Per `docs/repository-boundary.md`: unreleased experiments, new
predictors/codecs, simulator variants, and hardware-adjacent modeling all
develop on `experiment/*` branches of this same public repository — not
in a private companion repository. No private repository has been
created. (An earlier version of this document described a possible
private `membrane-labs` repository for this purpose; that plan was
explicitly abandoned in favor of full open development — see
`docs/open-development-policy.md`.) Vendor-specific FPGA/Vivado/Vitis
project files and raw hardware logs, if they arise from real hardware
access, are still handled per their own license/size constraints (see
`docs/open-development-policy.md`'s "vendor-specific files" section) but
land in this repository's branches, not a separate one.

## Conditions under which specific claims become unlocked

The `v0.1.0-research` tag itself is never "unfrozen" — it is a permanent,
immutable snapshot. What *does* change over time is which claims the
*current* `main` branch (and future releases) are allowed to carry:

- **A real hardware-validation result arrives** (Level A/B/C in
  `docs/phase8-hardware-validation-plan.md`) — exactly the claims that
  result's corresponding gate unlocks become allowed, per
  `outreach/hardware-claim-gates.md`, not a blanket reopening.
- **A real, disclosed error is found** in a currently-frozen claim — the
  freeze does not protect an inaccurate statement; it gets corrected
  immediately on `main`, following the same disclosure discipline as
  every prior correction in this project's history
  (`docs/phase7-hardware-outreach.md`, this document's own history),
  while `v0.1.0-research`'s own tagged state remains as originally
  published (the correction is a new, disclosed commit, not a rewrite of
  the old one).
