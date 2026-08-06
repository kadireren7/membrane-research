# Phase 4.1 — Runtime Mixed-Precision KV Policy Engine

## Purpose and scope

Phases 3.1-3.6 built and validated a risk-aware optimizer that searches for
a per-layer, per-K/V precision map ("policy") using **blob-splicing**: a
research technique that captures a real KV-cache state blob, numerically
perturbs specific byte ranges to simulate quantization, and reloads it.
The underlying ggml tensors stay F16 the whole time — blob-splicing never
actually stores anything at reduced precision, so it can measure
*simulated* quality but never real memory or real speed.

This phase builds the opposite: a runtime that loads a policy produced by
that optimizer and applies it to an **actual** llama.cpp inference context,
where each layer's K and V ggml tensors are genuinely allocated at the
policy's chosen precision. Every number this phase reports — KV bytes,
tokens/second, TTFT — comes from real execution, not simulation.

**The central question this phase asks, and the answer it measures:** does
a policy that blob-splicing validated as safe stay safe when it actually
runs? The measured answer, reported in full in §7, is **not entirely** —
memory reduction transfers from offline to real runtime almost perfectly,
but the offline-computed quality safety margins do not reliably hold in
real execution, even though exact-answer correctness held in every test
run. This is the phase's main finding, not a footnote.

## 1. Policy file format

A small, versioned binary format (not JSON — a binary format avoids
hand-rolling JSON parsing/escaping edge cases in C and lets every field be
checksum-verified as a single contiguous byte range, matching the
project's existing block/backend-file binary-header conventions).

Fixed 244-byte header, little-endian, followed by `layer_count * 2` bytes
(one K-precision byte and one V-precision byte per layer, values
restricted to `{16, 8, 4}` = FP16/Q8_0/Q4_0), followed by a 4-byte CRC32
(`membrane_block_checksum`, reused from the existing block module) over
everything before it:

| Field | Bytes | Notes |
|---|---|---|
| magic | 4 | `"MOL1"` as a u32 |
| format_version | 4 | `1` |
| model_sha256 | 32 | raw digest, not hex |
| llama_cpp_commit | 40 | raw ASCII git SHA-1, fixed width |
| layer_count | 4 | |
| model_name | 64 | NUL-padded label |
| tier_name | 32 | NUL-padded label (e.g. `"balanced"`) |
| cosine_min / top1_min / top5_min | 8 each | class threshold snapshot |
| cosine_margin / top1_margin / top5_margin | 8 each | tier margin snapshot |
| search_budget / evals_used | 4 each | optimizer metadata |
| created_unix_time | 8 | |
| *(trailing)* k_prec[i], v_prec[i] | 2 x layer_count | one byte each, `{16,8,4}` |
| checksum | 4 | CRC32 over everything above |

The model hash is real SHA-256 (`src/hash/sha256.c`, a from-scratch
implementation since the project had none — verified against `hashlib`
for `""`, `"abc"`, the NIST 448-bit test vector, and a longer string,
all matching exactly). CRC32 (not SHA-256) is used for the *file*
checksum since that only needs to catch corruption/truncation, not resist
tampering, and the project already had a CRC32 primitive.

## 2. Policy loader module (`membrane_policy`)

`include/membrane/policy.h`, `src/policy/policy.c`. No global mutable
state: `membrane_policy_t` is an opaque, heap-allocated handle returned by
`membrane_policy_load`, and every function takes it explicitly.

- `membrane_policy_save(path, membrane_policy_build_t*)` — writes a policy
  from a plain field bundle. Rejects (before touching disk) a NULL/zero
  layer count, an out-of-range precision value anywhere in `k_prec`/
  `v_prec`, or a `model_name`/`tier_name`/`llama_cpp_commit` string too
  long for its fixed field.
- `membrane_policy_load(path, membrane_policy_t **out)` — parses and
  validates the FILE FORMAT (magic, version, exact byte length for the
  claimed `layer_count`, checksum, every precision byte in range).
  Returns `MEMBRANE_ERR_IO` (missing/unreadable file),
  `MEMBRANE_ERR_CORRUPT_DATA` (bad magic/version/size/checksum/precision),
  or `MEMBRANE_ERR_ALLOC_FAILED`. `*out` is left NULL on any failure.
- `membrane_policy_validate(policy, membrane_policy_context_t *ctx, char
  *reason_buf, size_t reason_cap)` — a *separate* step that checks a
  successfully-loaded policy against the model/runtime it is about to be
  applied to (real model SHA-256, compiled-in llama.cpp commit, actual
  layer count). Returns `MEMBRANE_ERR_MISMATCH` and a human-readable
  reason (e.g. `"model hash mismatch: policy=... actual=..."`) on any
  field mismatch. Splitting load from validate is deliberate: a policy
  can be syntactically well-formed and still be the *wrong* policy for
  this model, and the two failure modes should be distinguishable.
- `membrane_policy_query(policy, layer, is_v, membrane_precision_t *out)`
  — O(1) array lookup, `MEMBRANE_ERR_INVALID_ARG` for an out-of-range
  layer or a NULL policy/out pointer.
- `membrane_policy_destroy(policy)` — NULL-safe no-op.

## 3. Runtime integration (the llama.cpp patch)

**Investigation finding, before any code was written:** llama.cpp's KV
cache already stores the type of every layer's K/V tensor independently
(`layers[il].k->type`) — `cpy_k`/`cpy_v` (writes), `get_k`/`get_v`
(reads), and `state_write_data`/`state_read_data` (save/restore, which
already serializes a per-layer type byte into the blob) are all
per-layer-type-agnostic already. The **only** place that assumes a single
type for the whole cache is tensor *allocation*, at construction time —
two lines in `llama_kv_cache`'s constructor
(`third_party/llama.cpp/src/llama-kv-cache.cpp`) that call
`ggml_new_tensor_3d(ctx, type_k, ...)` with a flat scalar for every
layer. This meant the necessary patch was much smaller than initially
expected.

**The patch** (`patches/llama.cpp-membrane-kv-type-override.patch`, 163
lines across 6 files, against the pinned commit
`c0bc8591e8815c63cb01dd3f051a8b0df02501c9`) is applied to the vendored
submodule's working tree — never committed inside the submodule's own
git history, and never pushed to its upstream remote. `CMakeLists.txt`
applies it automatically and idempotently whenever
`MEMBRANE_ENABLE_LLAMA=ON` is configured: it first checks with `git
apply --reverse --check` whether the patch is already present (so a
second `cmake` configure is a no-op, verified directly — reconfiguring
twice in a row applies the patch once, not twice, and leaves the
submodule's diff byte-identical to the patch file), and only runs `git
apply` if it is not. This means a fresh `git submodule update --init`
followed by a normal MEMBRANE build needs no manual patch step. The
submodule's tracked commit pin itself is never changed by this — only
its working tree, locally, at configure time.

- `include/llama.h`: two new optional fields on `llama_context_params` —
  `kv_type_override` (a `ggml_type (*)(int32_t il, bool is_v, void *ud)`
  callback) and `kv_type_override_ud`. Both default to `nullptr`.
- `src/llama-memory.h`, `src/llama-context.cpp`: the same two fields
  threaded through `llama_memory_params`, populated from
  `llama_context_params` when a context is built.
- `src/llama-model.cpp`: threaded into the **one** call site that
  constructs the standard (non-SWA, non-hybrid, non-MLA) `llama_kv_cache`
  — the path SmolLM2 and Qwen2 both use. The other 6 call sites (iSWA,
  hybrid, DSA, DSv4 architectures) are untouched; they keep working
  exactly as before because the new constructor parameters are
  defaulted, and this phase never claimed to support those architectures.
- `src/llama-kv-cache.h/.cpp`: the constructor gains the same two
  optional trailing parameters. At allocation time, each layer resolves
  `layer_type_k = kv_type_override ? kv_type_override(il, false, ud) :
  type_k` (and the V equivalent) instead of the flat scalar.

**A real correctness issue found and fixed while writing the patch:**
llama.cpp has an existing accuracy feature — a Hadamard rotation applied
to K and/or V before quantization, active whenever the *whole* cache is a
quantized type and `head_dim % 64 == 0` (true for both SmolLM2 and
Qwen2). This is computed as **two single booleans for the entire cache**
(`attn_rot_k`, `attn_rot_v`), not per layer. With a per-layer override
active, applying this uniformly would be wrong for any layer that stays
F16 (the rotation would corrupt otherwise-untouched F16 K/V), and the
existing scalar `ggml_is_quantized(type_k)` check can't express "some
layers are quantized, others aren't." The patch force-disables this
feature whenever `kv_type_override` is non-NULL (reusing the existing
`LLAMA_ATTN_ROT_DISABLE` env var's code path), logging why. **This is a
disclosed, real scope limitation, not a hidden one**: policies applied
through this runtime do not benefit from Hadamard rotation, even on
layers that stay quantized. (Whether this materially explains the gap in
§7 was investigated; see there for the reasoning.)

**Bit-identical baseline verified, not assumed.** `kv_type_override`
defaults to `nullptr`; every existing call site that doesn't pass it
compiles and behaves identically to upstream. This was checked, not just
argued: the entire `llama.cpp` target (226 objects) and every downstream
MEMBRANE tool rebuilt clean after the patch, `membrane-kv-sensitivity`'s
`self_test()` (no perturbation must reproduce the true baseline exactly)
still passed, and its `all-Q8` reference metrics were unchanged from
before the patch on the same model/prompt.

## 4. First application

Two policies exported from Phase 3.6's real search results via the new
`membrane-policy-export` tool (which computes the model's real SHA-256 and
embeds the compiled-in llama.cpp commit, so the exported file is
self-describing and checkable):

- `SmolLM2-135M`, tiers `balanced` and `aggressive` (30 layers each,
  exact `kbits`/`vbits` taken from Phase 3.6's `pareto_policy` JSONL
  records, not re-derived).
- `SmolLM2-360M`, tiers `balanced` and `aggressive` (32 layers each, same
  sourcing).

Each export's SHA-256 was cross-checked against the value already
documented in `docs/phase3-cross-model.md` for that model — both matched
exactly, confirming the hashing is consistent across the two phases'
independent tooling.

## 5. Runtime adapter tool (`membrane-kv-runtime`)

New tool, `tools/membrane-kv-runtime/main.cpp`. Given `--model` and
`--prompt`, it always runs three real, native-type baselines
(`all-FP16`, `all-Q8`, `all-Q4`, uniform across every layer) through
genuine `llama_context`s. If `--policy PATH` is also given, it loads and
**validates** the policy (§2) before doing anything else — a
model/commit/layer-count mismatch aborts the run with exit code 1 before
any inference happens (verified directly, not just unit-tested: running
the 360M policy against the 135M model prints `policy REJECTED: model
hash mismatch: policy=7d23...ccab6 actual=f535...6ef57` and exits 1).
Only if validation passes does it run a fourth real context with
`kv_type_override` wired to `membrane_policy_query`.

**When `--policy` is omitted, the tool never touches `kv_type_override`
at all** — this is the "MEMBRANE disabled" path item 8 asked for, and it
is the *default*, not a special mode: every context creation without a
policy is byte-for-byte the same call as the pre-Phase-4.1 code.

For every configuration, quality is measured against a **real** FP16
reference decoded the same way (prompt decode, one free-running pass,
one teacher-forced pass) — the same top1/top5/cosine/KL/first-divergence/
exact-recall metrics Phase 3 used, reimplemented directly against real
`llama_context`s instead of spliced blobs.

## 6. Real hardware performance (item 7)

Averaged over the full valid prompt set (5 prompts for 135M, 8 for
360M), both tiers, at `n_tokens=1024, gen_tokens=128` (matching Phase
3.6's own methodology exactly, after an initial run at `gen_tokens=32`
was caught and discarded — see §7 for why that distinction mattered):

| Model | Config | mean TTFT | mean tok/s | max peak RSS |
|---|---|---|---|---|
| 135M | all-FP16 | 177-181ms | 49.2-51.6 | 394 MB |
| 135M | all-Q8 | 225-230ms | 48.4-51.1 | 399 MB |
| 135M | all-Q4 | 234-244ms | 50.9-51.4 | 412 MB |
| 135M | MEMBRANE-policy | 227-237ms | 49.0-50.9 | 441-443 MB |
| 360M | all-FP16 | 571-580ms | 19.0-19.3 | 872 MB |
| 360M | all-Q8 | 682-691ms | 19.3-19.5 | 872 MB |
| 360M | all-Q4 | 700-757ms | 19.5-19.8 | 872 MB |
| 360M | MEMBRANE-policy | 688-694ms | 19.0-19.5 | 905-907 MB |

**No real speed win from quantized KV at these model sizes.** TTFT is
consistently *higher*, not lower, for every quantized config (native or
policy-driven) — the extra quantize/dequantize work per step outweighs
any memory-bandwidth benefit at 135M-360M scale on this CPU host; tok/s
differences are within run-to-run noise. This matches Phase 3's
native-type finding exactly (same pattern, same conclusion, now measured
through a real per-layer-mixed context rather than only uniform native
types). **This is a disclosed negative result, not a claim of speedup.**

Peak RSS for `MEMBRANE-policy` is measurably higher (441-443 MB vs
394-412 MB at 135M) than every native config, including all-Q4 — this is
consistent with the policy-driven context allocating tensors of multiple
distinct types simultaneously (F16 and Q8_0 and Q4_0 tensors) rather than
one uniform type, which has different (and here, apparently less
buffer-reuse-friendly) allocation characteristics in ggml's backend
allocator. Not investigated further; reported as measured.

**KV-cache memory reduction is highly accurate versus the offline
projection.** Real, measured `llama_state_seq_get_size` ratios track
Phase 3.6's analytically-projected ratios closely:

| Model / tier | Real runtime (this phase) | Offline projection (Phase 3.6) |
|---|---|---|
| 135M balanced | 2.332x-2.340x | 2.341x |
| 135M aggressive | 2.610x-2.620x | 2.623x |
| 360M balanced | 1.936x-1.939x | 1.939x |
| 360M aggressive | 2.094x-2.098x | 2.098x |

This is the part of the offline-to-online transfer that works exactly as
intended: the memory-cost model built in Phase 3 (`bytes_per_row()`,
ggml's real Q8_0/Q4_0 block formulas) predicts real, physically measured
KV-cache bytes to within a fraction of a percent, across both models and
both tiers.

**Policy lookup overhead is negligible, as expected from the design.**
Since the callback fires only once per layer at context-construction
time (not per token — see §3's investigation finding for why the public
per-write API needed no changes), it is inherently cheap: median
25-36ns/call across 6 of 8 runs. One 360M `balanced` run showed a
1024-call-batch outlier (15,679ns/call vs the surrounding runs' 25-36ns)
— the callback body is a pure array index with no I/O or locking, so a
500x difference in one run out of eight almost certainly reflects
scheduler/system noise during that specific process's context
construction, not a real cost of the lookup mechanism; reported here
rather than silently excluded.

## 7. Quality validation: runtime vs. offline (item 6) — the main finding

Every policy-driven run was compared against Phase 3.6's blob-splicing
result for the *same* model, tier, and prompt, checked against the exact
per-prompt-class threshold (recall-critical: cosine >= 0.9975, top1/top5
>= 99%; general/code/natural/repeated: cosine >= 0.995, top1 >= 98%,
top5 >= 99%; balanced tier adds +0.001/+0.5/+0.1, aggressive adds
nothing) that Phase 3.5/3.6's optimizer actually used to accept the
policy in the first place.

**A methodology bug was caught and fixed before trusting these numbers.**
The first full run used `--gen-tokens 32` (an old habit from earlier,
smaller smoke tests), not Phase 3.6's actual `--gen-tokens 128` — an
unfair comparison, since fewer generated tokens make each disagreement
cost more percentage points (Phase 3.5 documented this exact granularity
effect). A single-prompt check at the corrected `gen_tokens=128` showed
the gap barely moved (top1 96.88% -> 97.66%, cosine 0.98877 -> 0.99106,
both still far under the offline-validated bar) — proving the gap is not
primarily a gen_tokens artifact — after which the **entire** batch (26
prompt/tier/model runs) was redone at the correct `gen_tokens=128`. All
numbers below are from that corrected run.

| Model / tier | Real runtime clears its own offline-validated bar | Offline blob-splicing (for reference) | min runtime cosine | recall_ok |
|---|---|---|---|---|
| 135M balanced | 0/5 | 5/5 | 0.98525 | 5/5 |
| 135M aggressive | 0/5 | 5/5 | 0.96527 | 5/5 |
| 360M balanced | 2/8 | 8/8 | 0.99955 | 8/8 |
| 360M aggressive | 5/8 | 8/8 | 0.99849 | 8/8 |

**The offline-validated quality margin does not reliably transfer to the
real runtime.** Across all 26 (model, tier, prompt) combinations tested,
the real per-layer-quantized context's cosine similarity and top1
agreement against a real FP16 reference are consistently *lower* than
what blob-splicing predicted for the identical policy — sometimes by a
wide margin (135M aggressive's `repeat.txt`: offline cosine 0.99882,
real runtime cosine 0.96527). 12 of 26 clear the bar; 14 do not, all 10
of 135M's fail outright.

**Exact-answer correctness held in every single test.** `recall_ok`
(the free-running generation actually contains the documented correct
answer) was `true` in all 26 runs, on every recall-critical prompt,
across both models and both tiers, despite the metric-level shortfalls
above. This is an important, separate fact from the quality-margin
result: the policies did not produce a wrong factual answer in this test
set, even where they missed the finer-grained statistical bar the
optimizer used to accept them.

**A grounded hypothesis for the gap (not proven, but mechanistically
well-supported):** blob-splicing perturbs the *prompt prefix's* cached
K/V once (byte-level quantize-dequantize noise injected across the whole
captured blob, covering positions 0..P-2), then continues generation —
but every subsequent *newly generated* token's K/V is written into that
same blob's tensors, which stay F16-typed throughout blob-splicing's
approach, so those tokens are never themselves re-quantized as
generation proceeds. A real per-layer-quantized cache, by contrast,
quantizes **every** token's K/V as it is written, continuously,
including every token generated during the run — so real cumulative
quantization error compounds across the whole generation in a way
blob-splicing's one-time prefix perturbation cannot fully capture. The
evidence supporting this reading: the **native**, uniform-type
comparisons (`all-Q8`, `all-Q4`, which this tool measures through real
native contexts, and which Phase 3.6 also measured through real native
contexts via `run_kv_combo` rather than splicing) agree closely between
the two tools' independent measurements (e.g. 135M `all-Q4` on
`recall.txt`: 0.982255 here vs 0.982252 in Phase 3.6 — essentially
identical) — it is specifically the **spliced, per-layer-mixed** policy
comparisons that diverge from their real counterparts. This localizes
the gap to blob-splicing's simulation of mixed per-layer quantization
specifically, consistent with the mechanism above. This hypothesis is
not confirmed by a dedicated isolation experiment in this phase (that
would be reasonable future work); it is reported as the most
well-supported explanation available from the evidence gathered here,
not as a settled fact.

**What this means in practice:** a policy's blob-splicing validation is
useful for *searching* — cheaply exploring which layers can plausibly
tolerate reduced precision without needing a real per-layer runtime to
exist yet (which is exactly the role it played in Phases 3.4-3.6, before
this runtime existed) — but it should not be treated as a final safety
certificate for real deployment. This runtime tool is what closes that
gap: it is now possible, and should be standard practice, to re-validate
any offline-selected policy against a real per-layer context (exactly as
done here) before trusting its quality margins for anything beyond
"the exact answer still comes out right," which is the one property that
did hold throughout this test set.

## 8. Safety (item 8)

- **Wrong-model policy: rejected**, both by direct unit test (`test_
  model_hash_mismatch`, `test_llama_commit_mismatch`,
  `test_layer_count_mismatch`) and end-to-end through the real tool
  (§5's 360M-vs-135M example, exit code 1, no inference attempted).
- **Missing layer records: rejected** (`test_missing_layer_records`) —
  `membrane_policy_load` checks the file's exact byte length against
  what the header's own `layer_count` implies before trusting any
  per-layer byte.
- **Corrupted policy: rejected** (`test_checksum_corruption`) — a single
  flipped byte anywhere in the body is caught by the trailing CRC32.
- **Unsupported precision: explicit error**, both at save time
  (`test_unsupported_precision_rejected`, before any byte reaches disk)
  and at load time for a hand-corrupted file whose checksum was
  recomputed to isolate the precision-range check specifically from
  checksum corruption (`test_unsupported_precision_on_disk_rejected`).
- **MEMBRANE disabled -> safe baseline**: not a special code path to
  audit separately — it is the absence of the `--policy` flag, which
  means `kv_type_override` is never set, which means every context this
  tool creates without a policy is identical to a context created by any
  other, unpatched llama.cpp caller. Verified via the self-test and
  `all-Q8` reference-metric comparison in §3.

## 9. Tests

`tests/unit/test_policy.c`, 13 scenarios, all passing under both Release
and ASan+UBSan (15/15 total suite, up from 14/14 before this phase —
this phase added no code to any previously-tested module, so the
existing 14 are an unchanged regression check):

parse/round-trip · deterministic loading (two independent loads of the
same file agree on every layer) · precision query (in-range and
out-of-range) · unsupported precision rejected (both at save and,
separately with a recomputed checksum, at load) · model hash mismatch ·
llama.cpp commit mismatch · layer count mismatch · checksum corruption ·
truncated policy (three truncation points: mid-header, missing
checksum, empty file) · missing layer records (short file for the
claimed layer count) · wrong format entirely (non-policy file) · NULL/
missing-path safety (NULL path, nonexistent path, NULL build args, NULL
policy to destroy).

## 10. Verification (item 11)

- `cmake --build build-rel` and `cmake --build build-asan`: both clean,
  `ctest` 15/15 in each.
- `cmake --build build-llama` (the llama.cpp-integrated targets,
  including the new `membrane-kv-runtime` and `membrane-policy-export`):
  clean, 226/226 objects for the patched `llama` target plus every
  downstream MEMBRANE tool.
- `membrane-kv-sensitivity`'s `self_test()` re-run after the patch:
  still PASS, confirming the patch does not disturb the existing
  blob-splicing research path it was built for.
- `cmake --build build-llama-asan --target membrane-kv-runtime
  membrane-policy-export`: clean. `membrane-kv-runtime` run under this
  build with a real policy loaded and applied (135M `balanced`,
  `recall.txt`) to completion, through all four configurations
  (`all-FP16`/`all-Q8`/`all-Q4`/`MEMBRANE-policy`) — no ASan or UBSan
  reports.

## 11. Honest scope limitations

- **Architecture coverage**: only the standard, non-SWA, non-hybrid,
  non-MLA `llama_kv_cache` construction path honors `kv_type_override`
  (the one call site patched, matching SmolLM2/Qwen2's architecture).
  Sliding-window, hybrid-recurrent, and MLA models are not covered by
  this phase's patch at all.
- **Hadamard rotation disabled under override** (§3) — a real, measured
  accuracy feature is unavailable whenever a policy is active, on every
  layer including ones that stay quantized-native-equivalent.
- **The core finding of §7**: offline blob-splicing validation does not
  reliably predict real per-layer-quantized quality margins. Any policy
  accepted purely by blob-splicing should be re-validated through this
  runtime (or a future one) before being trusted beyond "the exact
  answer still comes out right."
- **Only 135M and 360M were exercised end-to-end** through the real
  runtime in this phase (matching the two models Phase 3.6 completed;
  Qwen2.5-1.5B's offline search never completed in Phase 3.6, so there
  is no policy to load for it here either).
- **CPU-only, single host.** No GPU/accelerator path was touched or
  tested.
