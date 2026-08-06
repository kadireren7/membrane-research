# Source map

Human-readable companion to [`import-manifest.json`](import-manifest.json)
(this repository's 301-file import record) and
[`repository-split-inventory.json`](repository-split-inventory.json)
(the full 476-file cross-repository inventory, including the 175 files
that stayed in `kadireren7/membrane`). Read `repository-contract.md` in
this same directory first for the decisions this migration executed.

**Which of the two JSON files to trust for "where does this file live
now"**: `import-manifest.json`'s `destination_path`/`destination_sha256`
fields are kept current — re-verified by `scripts/verify-provenance.py`
in CI against the actual working tree, including after the Part 3
documentation-template restructuring moved many files into `archive/`
and `results/canonical/` post-import. `repository-split-inventory.json`
is preserved as originally generated at Gate B copy time (the moment
each `membrane-research`-destined file was first copied and hashed) —
its `destination_path`/`destination_sha256` for those same 301 entries
reflect that initial flat layout, not the current one, and are not
updated as files move afterward.

## Source refs

| Ref | Commit | Contribution |
|---|---|---|
| `origin/main` | `9dbbede255dccf025cc3ecad7f17cd9f52f384a8` | Primary source — most of both repositories' content |
| `origin/experiment/q8-divider-pipeline` | `61ce8adc4733512a78dcf5c04844e6b85da04b54` | `EXP-FPGA-DIV-002` (Phases A-B4): never merged to `main` by design, imported here whole |
| `origin/experiment/fp-divider-pipeline` | `4c22efa5bc29f42579f2ea641dd6c9458dec988c` | 16 files unique to this branch — `rtl/experimental/fp_div/*` and its testbench, orphaned since `EXP-FPGA-DIV-001` merged to `main` (PR #2, `f96c695`) without this branch itself being deleted or its RTL re-added elsewhere |

All other branches on `kadireren7/membrane` were checked during
inventory generation and contributed zero unique content (either merged
into one of the three refs above, or containing no files not already
accounted for).

## Deduplication

476 files total across the three refs, checked pairwise by **content**
SHA256 (via `git cat-file -p <ref>:<path>` piped through `hashlib
.sha256`, not git's own blob SHA1, so a file with the same bytes but a
different git object history still dedupes correctly). Result: 0
duplicates. No file needed a "which ref wins" decision.

## Where each classification landed

| Classification | Count | Destination |
|---|---|---|
| `maintained-product` | 172 | `membrane` |
| `vendored` | 1 | `membrane` (the `third_party/llama.cpp` submodule gitlink — untouched) |
| `active-research` | 195 | `membrane-research` |
| `historical-research` | 70 | `membrane-research`, under each experiment's own `archive/` |
| `generated-result` | 34 | `membrane-research`, under each experiment's own `results/canonical/` |
| `shared-policy` | 4 | Resolved per-file per `repository-contract.md` Decisions 1-2, not a blanket rule — some content stays in `membrane` (trimmed), some moves here (full version) |

301 files landed in `membrane-research`, 175 in `membrane` — see
`import-manifest.json`'s `counts` block for the exact figures this table
summarizes.

## Notable single-file provenance notes

- **`third_party/llama.cpp`** is a git submodule (gitlink, mode
  `160000`), not a regular blob — its "content hash" in the inventory is
  recorded as `SUBMODULE:<pinned-commit-sha>` rather than a SHA256, and
  it was never touched, read, or copied by this migration (explicit
  non-negotiable rule — see `repository-contract.md` section 8).
- **`patches/llama.cpp-membrane-kv-type-override.patch`** stayed in
  `membrane` (`maintained-product`) — it's a build-time patch the core
  quantization path applies to the submodule above, not research
  evidence.
- **`rtl/experimental/fp_div/*`** (16 files) came from
  `experiment/fp-divider-pipeline`, not `main` or
  `experiment/q8-divider-pipeline` — seeing this ref cited on any file in
  `experiments/EXP-FPGA-DIV-001/` is not a typo; that branch was never
  merged, only its documentation was (see that experiment's own
  `README.md` "Provenance" section for the full story of the reunion).

## Known gaps in this migration

- Neither source branch (`q8-divider-pipeline`, `fp-divider-pipeline`)
  was deleted by this migration, and both remain the authoritative git
  history for their own experiments — this repository's copy is a
  point-in-time, hash-verified snapshot, not a live mirror. A future
  commit on either branch in `kadireren7/membrane` will not
  automatically appear here.
- The `MEMBRANE_PRODUCTION_ROOT` reproduction-script gap disclosed in
  both `EXP-FPGA-DIV-001/reproduction/README.md` and
  `EXP-FPGA-DIV-002/reproduction/README.md` is a direct consequence of
  this split (the scripts were written when experimental and production
  RTL lived in the same working tree) and is tracked, not silently
  worked around, in `ROADMAP.md` Track 4.
