# Open development policy

MEMBRANE is developed **fully in the open**, in this one public
repository (`kadireren7/membrane`) — there is no private `membrane-labs`
companion repository.
No private repository has been created, and none is planned — see
[docs/repository-boundary.md](https://github.com/kadireren7/membrane/blob/main/docs/repository-boundary.md) for the branch
structure this enables and
[docs/research-release-freeze.md](research-release-freeze.md) for how
this coexists with `v0.1.0-research` remaining an immutable tag.

## Why full open source

- **Public scrutiny is the check this project relies on.** MEMBRANE's own
  standing discipline is that every claim cites a real, committed
  artifact and every negative result gets the same visibility as a
  positive one (`docs/results-summary.md` §4). That discipline is easiest
  to hold to, and easiest for anyone outside the project to verify,
  when the work that produced it is visible from the start — not
  assembled privately and published only once it looks good.
- **Reproducibility degrades the moment part of the history is
  private.** A reader who can see only the polished result has to trust
  that nothing favorable was cherry-picked from unseen private work. A
  reader who can see the actual `experiment/*` branches — including the
  ones that failed — can check that for themselves.
- **A private research repo was considered and explicitly rejected.**
  `docs/repository-boundary.md` originally described a possible private
  `membrane-labs` repository for unreleased experiments, vendor-specific
  hardware project files, and similar material. Kadir decided against
  creating it: the reproducibility and scrutiny benefits above outweigh
  whatever convenience a private staging area would have offered.

## What still never gets committed, anywhere in this repository

Full openness does not mean committing everything indiscriminately.
These stay out of every branch, not just `main`:

- **Secrets.** API keys, tokens, credentials, or anything else that
  grants access to a system — this project has none today and should
  never gain any through a careless commit.
- **Model weights.** `models/*.gguf` and equivalent are gitignored;
  models are downloaded or otherwise obtained by the person reproducing
  a result, per `docs/reproduction.md`, never vendored into the repo.
- **Private data.** Anything that isn't this project's own generated
  traces/benchmarks/captures — e.g. data a collaborating lab shares
  under a limited-distribution understanding — does not get committed
  to a public branch. If that ever happens, it is handled as a scoped
  exception, disclosed explicitly in the relevant experiment record, not
  committed silently.

## Unverified results vs. released results

An `experiment/*` branch is not a public claim. Only two things carry
MEMBRANE's actual claim discipline:

1. **`main`** — reviewed, tested, reproducible, claim-audited.
2. **A tagged release** (`v0.1.0-research`, and future
   `vX.Y.Z-research` tags) — an immutable snapshot of `main` at a point
   in time, per `docs/research-release-freeze.md`.

Anything on an active branch — including partial results, numbers from a
run that hasn't finished, or a hypothesis that turned out false — is
research-in-progress, documented per `EXPERIMENT_TEMPLATE.md`, and is
explicitly **not** a claim this project stands behind until it is merged
to `main` (and, for headline results, until a future release tags it).
Readers should treat anything outside a tagged release accordingly.

## Vendor-specific files and licensing

FPGA/CXL work in this project sometimes touches vendor toolchains
(Xilinx Vivado/Vitis, board vendor IP, etc.). Before committing any
vendor-originated file (project files, IP cores, constraint files,
generated netlists) to any branch:

- Check that vendor's redistribution terms actually permit committing
  the file to a public repository — many vendor toolchain outputs and
  IP cores are not freely redistributable even though the toolchain
  itself may be freely downloadable.
- If a file cannot be redistributed, commit a script or instructions to
  regenerate it locally instead of the file itself, and say so explicitly
  in the relevant `docs/` or experiment record.
- See [docs/licensing.md](https://github.com/kadireren7/membrane/blob/main/docs/licensing.md) for the existing boundary
  between MEMBRANE's own Apache-2.0 code and everything under its own
  separate terms (`third_party/llama.cpp`, captured traces, model
  artifacts).

## Evidence required for real hardware claims

Nothing in this repository — on any branch — claims a real hardware
measurement without the evidence to back it, per
`outreach/hardware-claim-gates.md`'s gate table. Concretely, before an
experiment record or a PR claims something like "runs on an FPGA board"
or "measured CXL latency," it must include:

- The actual board/device identification and a way to verify access to
  it existed (e.g. a dated log, a photo, a collaborating lab's
  acknowledgment).
- Raw output from the real run, not just a derived summary — reduced,
  reviewed data belongs in the repository per
  `hardware/results-schema.json`; the corresponding experiment record
  says where the raw log lives.
- Which specific gate in `outreach/hardware-claim-gates.md` the result
  satisfies, and which claims remain out of reach until further gates
  are cleared.

Absent that evidence, hardware-adjacent work stays labeled SIMULATED,
EXTRAPOLATED, or ASSUMED — never presented as a real measurement.
