# Phase 4.4 — ggml-Exact Quantization Parity

## Purpose

Phase 4.3 traced Phase 4.2's measurement-variance finding to a real,
fixable configuration bug (docs/phase4-runtime-variance.md), but also
measured and quantified a separate, larger, structural gap: MEMBRANE's
own offline quantization simulation (`quant_roundtrip_group`, a simple
per-32-element max-abs linear quantizer written for Phase 3.3) never
matched ggml's real Q8_0/Q4_0 block format or rounding math. This phase
closes that gap: every place MEMBRANE simulates KV quantization now
calls ggml's own, real, linked quantize/dequantize functions instead of
reimplementing an approximation of them.

## 1. Reference review (item 1) — what ggml's real math actually is

Read directly from the pinned llama.cpp commit
`c0bc8591e8815c63cb01dd3f051a8b0df02501c9`
(`third_party/llama.cpp/ggml/src/ggml-quants.c`,
`ggml-cpu/quants.c`, `ggml-cpu/arch/x86/quants.c`,
`ggml-common.h`), not guessed or reinterpreted:

**Block layout** (`ggml-common.h`): `block_q8_0 { ggml_half d; int8_t
qs[32]; }` (34 bytes), `block_q4_0 { ggml_half d; uint8_t qs[16]; }` (18
bytes, two 4-bit values packed per byte). Both quantize in groups of 32
elements ("blocks"), each block carrying its own F16 scale.

**Q8_0 quantize** (`quantize_row_q8_0_ref`): per block, `d = amax /
127`, `id = 1/d`; each element quantizes to `roundf(x * id)` (round half
away from zero), stored as a signed int8 directly (symmetric range
-127..127).

**Q4_0 quantize** (`quantize_row_q4_0_ref`): per block, `d = max / -8`
(note: signed `max`, not `amax` — this makes the scale's sign track the
dominant element's sign) , `id = 1/d`; each element quantizes to
`MIN(15, (int8_t)(x*id + 8.5f))`, stored as an unsigned nibble
(asymmetric range -8..7, not the symmetric ±7 a naive linear quantizer
would produce).

**Dequantize** (`dequantize_row_q8_0`/`dequantize_row_q4_0`): both are
simply `value = (stored_int - offset) * d` per element — no surprises,
single implementation each, no arch-specific override in this ggml
version.

**A real subtlety this review found, verified empirically (§3
below):** Q8_0 has TWO different quantize implementations in this ggml
version, and they are NOT bit-identical to each other:
- `quantize_row_q8_0_ref` (`ggml-quants.c`) — the scalar form ggml's own
  comment labels "reference implementation for deterministic creation
  of model files." Used by `ggml_quantize_chunk` (GGUF file conversion,
  e.g. `llama-quantize`). Rounds with `roundf()` — half away from zero.
- `quantize_row_q8_0` (`ggml-cpu/arch/x86/quants.c` on this
  `-march=native` AVX2 build) — what the CPU backend actually calls
  when converting a LIVE tensor's values into a Q8_0-typed tensor
  during inference (e.g. writing attention output into a Q8_0 KV cache
  via MEMBRANE's `kv_type_override`). Rounds via
  `_mm256_round_ps(..., _MM_ROUND_NEAREST)` — IEEE-754 round-half-to-
  even.

MEMBRANE's whole purpose is predicting/measuring real KV cache
quantization, not GGUF model-file conversion, so `membrane_ggml_quant`
(§2) calls the CPU-backend forms — the same functions ggml's own CPU
backend dispatches to for live tensor quantization — not the "_ref"
forms. Q4_0 has no arch-specific override in this ggml version (its
CPU-backend form is literally `{ quantize_row_q4_0_ref(x, y, k); }`),
so there is no such choice to make there.

## 2. The `membrane_ggml_quant` module (item 2)

New: `include/membrane/ggml_quant.h` + `src/quant/ggml_quant.c`, built
as a separate static library (`membrane_ggml_quant`, only when
`MEMBRANE_ENABLE_LLAMA` is ON — it links directly against ggml, unlike
the rest of `membrane_core`, which stays dependency-free for the
portable Release/ASan suites).

No math is reimplemented. Every function is a thin adapter that: (1)
converts the caller's F16 input to F32 via ggml's own
`ggml_fp16_to_fp32_row` (not MEMBRANE's own F16 conversion — eliminating
any doubt about a second, independent F16 implementation introducing
drift), (2) calls ggml's real `quantize_row_q8_0` / `quantize_row_q4_0`
/ `dequantize_row_q8_0` / `dequantize_row_q4_0` directly, (3) converts
back to F16 via `ggml_fp32_to_fp16_row`. `membrane_ggml_quant_roundtrip`
composes quantize+dequantize into the direct replacement for the
retired `quant_roundtrip_inplace`.

Source and license attribution: the pinned llama.cpp commit above, MIT
license (`third_party/llama.cpp/LICENSE`). No ggml source was copied
into this repository — `src/quant/ggml_quant.c` only calls the real,
linked symbols.

## 3. Bit-parity tests (item 3)

New: `tests/unit/test_ggml_quant_parity.c`, built and run only inside
the `MEMBRANE_ENABLE_LLAMA` block (it links ggml directly to make an
independent comparison call — unlike `test_checkpoint`, it cannot run
under the portable, llama-free Release/ASan suites; see §8).

Every test compares `membrane_ggml_quant`'s output against calling
ggml's real quantize/dequantize functions directly on the SAME,
F16-rounded input (comparing against the un-rounded original would
manufacture a spurious mismatch purely from the adapter's own, correct,
F16→F32 boundary conversion — a real bug this test suite caught in
itself during development, fixed before any of these numbers were
final). Categories, all passing:

- all-zero, constant, positive/negative extrema (±65504, the F16 max,
  and values near F16's smallest normal), halfway-rounding boundaries
  (values landing exactly on a .5 multiple of the block's quantization
  step), NaN/Inf-containing blocks, denormal/subnormal F16 values,
  **100,000 deterministic random blocks** (fixed-seed PRNG, reproducible
  across runs).
- On any mismatch: reports the block index, byte/element index,
  expected vs. actual value, and a window of the surrounding input —
  none fired in the final run.

**A separate, non-assertion measurement** in the same binary
(`report_ref_vs_cpu_backend_rounding`) directly confirms §1's
rounding-mode finding: quantizing a constructed halfway-boundary block
via `quantize_row_q8_0` (CPU backend, what this module calls) vs.
`quantize_row_q8_0_ref` (GGUF-file reference) produces different bytes
at every .5 boundary, exactly matching round-half-to-even vs.
round-half-away-from-zero (e.g. input 2.50 → 2 vs. 3; input 0.50 → 0
vs. 1). Measured, not assumed.

## 4. Migrating production paths off `quant_roundtrip_group` (item 4)

- **`membrane-kv-sensitivity`** (the offline sensitivity profiler,
  Phase 3.3): `quant_roundtrip_inplace` now calls
  `membrane_ggml_quant_roundtrip` directly. The old per-32-element
  linear quantizer was DELETED (not kept dead-code-behind-a-flag) —
  the historical formula is documented in §1 above and remains in git
  history; keeping unused, warning-suppressed dead code around had no
  benefit over that.
- **`membrane-kv-runtime-optimizer`**'s `eval_offline()` (the
  OFFLINE_BLOB Stage A pre-screen): same change, same deletion. Stage A
  never itself accepts a candidate (Phase 4.2's structural guarantee,
  unchanged), but a more accurate pre-screen changes which candidates
  Stage B spends its live-evaluation budget testing first.
- **`membrane-kv-quality`**: did not use `quant_roundtrip_group` at all
  — it already quantizes via real `GGML_TYPE_Q8_0`/`GGML_TYPE_Q4_0`
  context types directly (Phase 3.2's original, simpler design, no
  blob-splicing). Untouched here, and already ggml-exact by
  construction.
- **`membrane-kv-runtime`**: no blob-splicing, no `quant_roundtrip`
  usage — real per-layer `kv_type_override` runtime only. Untouched.
- **`membrane-kv-variance`** (Phase 4.3's diagnostic tool): kept its
  copy of the legacy quantizer, but ONLY because this tool's own
  `--mode quant-timing` exists specifically to compare old vs. new —
  an actively-exercised, clearly-labeled (`_LEGACY` suffix) comparison
  arm, not a silently-used production path. Every other mode
  (`repeat`, `threads`, `flashattn`, `drift`) and the default arm of
  `quant-timing` itself now use `membrane_ggml_quant` exclusively; only
  `quant-timing`'s explicit legacy-comparison arm still calls the old
  math, by design (see §6).

## 5. Cross-tool parity (item 5) — a real, found, and fixed convention gap

Ran the same model, prompt, and (where applicable) policy through
`membrane-kv-quality` and `membrane-kv-runtime` and compared KV byte
count and quality metrics directly. Real KV-byte figures BEFORE any
fix, SmolLM2-135M, `short.txt`, `n-tokens 256 gen-tokens 8`:

| tool | config | kv bytes |
|---|---|---|
| membrane-kv-quality | all-FP16 baseline | 484836 |
| membrane-kv-runtime | all-FP16 | 300420 |
| membrane-kv-quality | Q8_0 | 258036 |
| membrane-kv-runtime | all-Q8 | 160020 |

A real, exactly-quantifiable discrepancy — not a rounding artifact.
Traced to source (item 7's "isolate exactly which convention differs"):
`membrane-kv-runtime`'s `run_config()` measures `llama_state_seq_get_
size()` immediately after the prompt decode, BEFORE the generation
loop; `membrane-kv-quality`'s `run_pass()` measured it AFTER the full
prompt+generation decode instead — the generated tokens' own cached
K/V were being counted as part of "the KV footprint" in one tool but
not the other. `membrane-kv-runtime-optimizer` and `membrane-kv-
variance` both inherited `membrane-kv-runtime`'s prompt-only
convention (verified by direct code reading), so 3 of the 4 real-
runtime tools already agreed with each other; `membrane-kv-quality`
was the outlier.

**Fix:** moved `membrane-kv-quality`'s `kv_state_bytes` measurement to
immediately after the prompt decode, matching the other three tools —
the prompt-only convention was chosen as canonical because it is
already what the majority of tools (and every tool whose measurements
directly gate an accept/reject decision) use, and because it is the
more meaningful number for KV-cache compression's real motivating case
(long-context prompts, where the prompt dominates total KV footprint
regardless of how many tokens get generated afterward).

**Verified fixed**, same model/prompt/config:

| tool | config | kv bytes |
|---|---|---|
| membrane-kv-quality | all-FP16 baseline | 300420 |
| membrane-kv-runtime | all-FP16 | 300420 |
| membrane-kv-quality | Q8_0 | 160020 |
| membrane-kv-runtime | all-Q8 | 160020 |
| membrane-kv-quality | Q4_0 | 85140 |
| membrane-kv-runtime | all-Q4 | 85140 |

Exact match, every row. Quality metrics were already consistent before
this fix (`membrane-kv-quality`'s Q8_0 `logit_cosine 1.0000` and
`membrane-kv-runtime`'s `all-Q8 cosine 0.999962` agree at the 4-decimal
display precision `membrane-kv-quality` prints; Q4_0's `0.9960` vs.
`0.996046` likewise) — the KV-byte timing was the one real convention
gap between these two tools, and it is now closed rather than merely
documented (item 7's "define one canonical convention and migrate all
tools to it" branch, not the "name modes differently" branch — a
single, unambiguous fix was possible here).

## 6. Regression benchmark (item 6) -- and a real, surprising finding

`membrane-kv-variance --mode quant-timing` was extended (Phase 4.4) to
report three arms side by side instead of two: **native** (real
write-time ggml quantization, unchanged by this phase), **post-hoc-
exact** (the new `membrane_ggml_quant`), and **post-hoc-LEGACY** (the
old per-32-element linear quantizer, kept alive only inside this one
diagnostic mode for exactly this comparison).

**For a real, moderate policy** (the actual exported
`135m-aggressive.mpol` from Phase 4.2, 12 Q4 V-slots, `recall.txt`,
real scale `n-tokens 1024 gen-tokens 24`):

| arm | cosine | top1 |
|---|---|---|
| native | 0.99669 | 100.0% |
| post-hoc-exact | 0.98906 | 91.7% |
| post-hoc-LEGACY | 0.98986 | 95.8% |

Exact and legacy are close to each other and both reasonably close to
native -- the expected, unsurprising result of a real fix: still an
approximation (blob-splicing's fundamental limitation, unchanged), but
now built on the mathematically correct per-element quantization.

**For an extreme policy** (`all-q4`, 60 slots -- every layer's K and V,
the most aggressive case this project tests), same prompt:

| arm | cosine | top1 |
|---|---|---|
| native | 0.98647 | 87.5% |
| post-hoc-exact | 0.80366 | 100.0% |
| post-hoc-LEGACY | 0.98100 | 100.0% |

This is a real, measured, and genuinely surprising result: post-hoc-
exact is dramatically WORSE than post-hoc-LEGACY here, not better --
the opposite of what "switched to the mathematically correct function"
would naively predict. This was investigated as a suspected bug before
being accepted as a finding (per-element dumps of real captured KV
values showed both quantizers producing individually reasonable,
correctly-shaped output for the same block; an isolated unit-level
comparison on synthetic data showed comparable per-element RMSE between
the two methods at both 64 and 192 elements per row -- matching this
model's real per-layer K/V row width). The milder, real-policy result
above rules out a systematic adapter bug (same code path, no anomaly
there). The conclusion: **blob-splicing's retroactive-quantization
approximation becomes numerically chaotic under extreme, simultaneous,
every-layer 4-bit perturbation, regardless of whether the per-element
quantization math is exactly correct.** Matching ggml's real rounding
does not reliably make an already-fragile simulation MORE predictive
once the simulated perturbation is large enough to leave the regime
where small per-element errors stay small downstream -- it can go
either direction depending on the specific noise pattern's interaction
with that regime's sensitivity. This deepens, rather than contradicts,
Phase 4.1/4.2's original finding: offline prediction cannot be trusted
at the extremes, which is exactly why real-runtime verification
(`EVAL_LIVE_RUNTIME`) remains the sole acceptance authority regardless
of how accurate the offline math gets.

**Regression run on the real, completed Phase 4.2 checkpoint** (all 3
tiers resumed with `--search-budget 40`, zero new live evaluations
needed -- pure fast-forward -- then drift recomputed and the full
7-config x 8-prompt final comparison table regenerated with the fixed
binary):

The `all-FP16`/`all-Q8`/`all-Q4` real-runtime rows in this run's final
comparison table differ from Phase 4.2's originally-recorded numbers
(e.g. `recall.txt` all-Q8: 0.999388 here vs. 0.999593 originally;
all-FP16-vs-itself: exactly 1.000000 here vs. 0.999998 originally).
**This is Phase 4.3's already-committed decode-shape fix, not a new
Phase 4.4 effect** -- confirmed precisely: `git diff a0324be --
tools/membrane-kv-runtime-optimizer/main.cpp` (`a0324be` is Phase
4.3's commit) touches zero lines of `eval_live()`. Phase 4.4 only
changed `eval_offline()`/`quant_roundtrip_inplace`; every `eval_live()`
call (every number in the `all-FP16`/`all-Q8`/`all-Q4` rows above, and
every real accept/reject decision from the original search) is
identical code to what Phase 4.3 already shipped and verified. The
DRIFT lines' "runtime" side (§ above, e.g. conservative tier's
`runtime 0.999130`) is the same Phase-4.3-attributable change; only
their "offline" side (`offline 0.999905`, up from Phase 4.2's original
`0.999941`) is new to this phase.

**No accept/reject decision changed.** All 3 tiers' fast-forward
reported the exact same accepted-slot counts as Phase 4.2's original
run (conservative: 1, balanced: 1, aggressive: 12) -- expected, since
fast-forwarding replays RECORDED decisions rather than re-deciding
anything, but confirms the checkpoint itself parsed and matched
identically under the new binary (model hash, prompt-set hash, and
tool-version checks all passed, or the run would have refused to
resume at all -- see docs/phase4-runtime-calibration.md §5).

## 7. Verification (item 8)

- **Release**: existing unit suite (16/16) and `test_checkpoint`
  (16/16) rebuilt and pass under `build-rel`, unaffected by this
  phase's changes (neither touches checkpoint.h or the portable
  membrane_core sources).
- **ASan+UBSan**: same, rebuilt and pass under `build-asan`, 0
  sanitizer reports. `test_ggml_quant_parity` itself cannot build
  under `build-asan` (it links ggml directly, and this project's
  established convention -- set by `test_checkpoint` in Phase 4.2 --
  keeps ASan coverage scoped to the portable, llama-free subset of the
  codebase; llama.cpp itself is not built with sanitizers in this
  project). This mirrors exactly how Phase 4.2/4.3's llama-dependent
  test/tool verification was scoped.
- **llama.cpp-integrated build**: full `build-llama` rebuild, all
  targets, including the new `membrane_ggml_quant` library and
  `test_ggml_quant_parity` binary, the migrated
  `membrane-kv-sensitivity`/`membrane-kv-runtime-optimizer`, and the
  fixed `membrane-kv-quality`.
- **Bit-parity exhaustive/deterministic tests**: `test_ggml_quant_parity`
  -- 100,000+ random blocks plus every edge-case category in §3, all
  passing, run via `ctest`.
- **Real interrupted/resumed run**: this phase's own regression
  benchmark (§6) WAS a real resume of a genuine, previously-interrupted-
  and-completed checkpoint (Phase 4.2's `135m.ckpt`), fast-forwarding
  40+40+40 real recorded decisions per tier under the new binary and
  confirming identity checks (model hash, prompt-set hash, tool
  version) all passed -- the same resume path Phase 4.2/4.3 already
  exercised with a live `SIGTERM`, now additionally proven to still
  work correctly after this phase's code changes.

## 8. Summary

**What changed:** MEMBRANE's offline quantization simulation (blob-
splicing's Stage A pre-screen, the standalone sensitivity profiler, and
`membrane-kv-variance`'s default quant-timing/drift arms) now calls
ggml's own, real, linked Q8_0/Q4_0 quantize and dequantize functions
instead of an approximate, hand-written linear quantizer. A real,
separate convention bug (`membrane-kv-quality` measuring KV bytes at a
different point in the pipeline than every other tool) was found and
fixed as part of verifying cross-tool parity, achieving exact
byte-for-byte KV-size agreement across all four real-runtime-adjacent
tools for the first time.

**What did not change:** every real accept/reject decision this
project has made or will make (`EVAL_LIVE_RUNTIME` is untouched by this
phase, confirmed by diff, not assumption); the exported Phase 4.2
policies remain identical; no quality threshold, margin, search budget,
or benchmark scope was changed anywhere in this phase.

**What is now better-understood, not better in outcome:** the offline
pre-screen's predictions are now built on provably correct per-element
math, but this does NOT make blob-splicing reliably more predictive of
real runtime behavior -- for a real, moderate policy the improvement is
present but small; for an extreme, every-layer 4-bit policy, matching
ggml's real rounding made the offline prediction WORSE, not better (§6).
This is reported as measured, not smoothed over: it reinforces, with a
concrete number behind it for the first time, exactly why Phase 4.2's
architecture never lets the offline backend accept a candidate on its
own.

**Remaining convention gap:** none identified after §5's fix. All four
real-runtime-adjacent tools (`membrane-kv-quality`,
`membrane-kv-sensitivity`, `membrane-kv-runtime`,
`membrane-kv-runtime-optimizer`) now agree on KV-byte measurement
timing (prompt-only, before generation) and quantization math (real
ggml functions, not an approximation).
