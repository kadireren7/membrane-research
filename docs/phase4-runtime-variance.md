# Phase 4.3 — Runtime Measurement Variance and Drift Investigation

## Purpose

Phase 4.2 found a real, disclosed gap it did not set out to look for
(§10 of `docs/phase4-runtime-calibration.md`): the SAME final policy,
re-measured through this project's own tools, produced different
cosine numbers — large enough to flip a zero-margin accept decision at
a threshold boundary. That section named two suspects (flash-attention
resolution, context reuse) without conclusively isolating either. This
phase investigates the actual root cause with controlled, real
measurements, and applies a minimal fix if one is found (item 8's
explicit instruction — no threshold or benchmark-scope change either
way).

New tool: `tools/membrane-kv-variance/`, built specifically for this
investigation, reusing (per this project's established
one-copy-per-tool convention) the same context/decode/metrics
primitives `membrane-kv-runtime-optimizer` uses, but with every
previously-implicit configuration knob (thread count, flash-attention
resolution, prompt-decode batching shape) made explicit and
independently controllable, so each can be tested in isolation.

## 1. Determinism audit (item 1)

A code-level review of `tools/membrane-kv-runtime-optimizer/main.cpp`
(the tool whose accept/reject decisions this whole project depends on)
before any new code was written:

- **Sampling / seed**: none. Every decode path is pure greedy argmax
  (`argmax()` over `llama_get_logits_ith`) — no `llama_sampler`, no
  temperature/top-k/top-p, no seed anywhere in the tool. Ruled out as a
  variance source immediately.
- **Thread count**: hardcoded `cp.n_threads = cp.n_threads_batch = 4`
  in `make_context()`, identical for every context the tool creates.
- **Context reuse**: none. `eval_live()` and `eval_offline()` each
  create fresh `llama_context`s via `make_context()` and `llama_free()`
  both before returning, every single call — no context, and therefore
  no KV cache, is ever reused across two different evaluations.
- **Model state reset**: not applicable — a fresh context has no prior
  state to reset; see context reuse above.
- **Prompt order**: `screening_order()` reorders WHICH prompt is
  checked first per candidate (recall-critical first, for cheap early
  exit), but every prompt still gets its own fresh
  `capture_baseline()`/`eval_live()` call regardless of order — no
  shared mutable state between prompts.
- **Floating-point reduction order / scheduler behavior**: this is
  where the audit found two real, concrete, measured asymmetries
  between how the reference (`capture_baseline()`) and every candidate
  (`eval_live()`) were actually computed:
  1. **flash-attention resolution.** `capture_baseline()`'s context
     never sets `flash_attn_type`, leaving it at llama.cpp's default
     `LLAMA_FLASH_ATTN_TYPE_AUTO`. Every `eval_live()` context passes a
     non-NULL `kv_type_override` callback, which the tool's own
     `make_context()` uses to unconditionally force
     `LLAMA_FLASH_ATTN_TYPE_ENABLED` — even for a policy that maps
     every layer back to F16 (§2's controlled test below shows this
     resolves to a no-op at real context sizes, but it was a real,
     un-measured asymmetry until tested).
  2. **prompt-decode batching shape.** `capture_baseline()` decodes the
     prompt as (prefix, last-token) across two separate `llama_decode`
     calls (needed so the offline blob-splicing backend has a
     prefix-only KV blob to splice into); `eval_live()` decoded the
     whole prompt in one `llama_decode` call. This is a real,
     deterministic difference in batch shape between the reference and
     the candidate, unrelated to quantization. §2 below shows this one
     fully explains the Phase 4.2 §10 gap.

## 2. Isolating the two suspects (items 1, 6)

Both were tested directly and independently with
`membrane-kv-variance --mode flashattn`, which captures the reference
and evaluates the candidate under explicit, controllable
`(n_threads, flash_attn_type, decode_shape)` triples instead of the
optimizer's fixed, asymmetric choices.

**Repeated in-process determinism (baseline check before testing
anything else):** the SAME policy+prompt, called through `eval_live()`
20 times in a row in the same process (SmolLM2-135M, `recall.txt`, the
real exported `135m-aggressive.mpol` policy, `n_tokens=1024
gen_tokens=24`) produced **bit-identical** cosine, top1, top5, KL,
first-divergence, KV-byte count, generated text, AND KV-state
SHA-256 checksum across all 20 runs. Repeating the same 20-run
experiment at `n_threads` in `{1, 2, 4}` also produced bit-identical
results within AND across thread counts. **In-process repeated
measurement of the identical configuration has zero variance on this
hardware.** Whatever caused Phase 4.2 §10's discrepancy, it is not
run-to-run jitter within a process.

**Flash-attention, symmetric sweep** (reference and candidate given
the SAME setting), all-F16 policy against its own baseline,
`gen_tokens=8`:

| setting | cosine |
|---|---|
| auto | 1.00000000 |
| disabled | 1.00000000 |
| enabled | 1.00000000 |

Every symmetric setting gives an exact 1.0 — no measurable effect on
its own when reference and candidate agree.

**Reproducing the optimizer's actual asymmetry** (reference:
flash_attn=auto + split-decode; candidate: flash_attn=enabled +
whole-decode), same all-F16 policy:

cosine **0.99999972** — not 1.0, despite zero quantization anywhere in
this test. Confirms the asymmetry is real and measurable even in the
simplest possible case.

**Isolating which half of the asymmetry causes this**, using the real
exported `135m-aggressive.mpol` policy against `recall.txt` at real
scale (`n_tokens=1024, gen_tokens=32`):

| configuration | cosine |
|---|---|
| flash_attn=auto, symmetric (whole-shape both sides) | 0.99654394 |
| flash_attn=enabled, symmetric (whole-shape both sides) | 0.99654394 |
| **optimizer's real asymmetry** (split ref / whole candidate) | **0.99656049** |
| reference shape=split, flash_attn=enabled BOTH sides | 0.99656049 |
| reference shape=whole, flash_attn=enabled BOTH sides | 0.99654394 |

This is conclusive: **flash-attention `auto` resolves identically to
`enabled` at real context sizes (zero contribution to the gap)**; the
entire measured difference comes from the **prompt-decode batching
shape**. Holding flash-attention fixed and varying only the reference's
decode shape reproduces the optimizer's exact asymmetric number
(0.99656049) when shape=split, and the symmetric number (0.99654394)
when shape=whole — the shape variable alone fully accounts for the
gap, matching both the direction and the order of magnitude of Phase
4.2 §10's original finding (0.998724 vs 0.998714 there; 0.0000166 here
vs ~0.00001 there, for shorter generation).

**A real, separate discovery from this sweep:** `flash_attn=disabled`
crashes `llama_state_seq_get_data` with a `GGML_ASSERT("tensor read
out of bounds")` on a real (`n_ctx=1024`) context — an interaction
between the MEMBRANE `kv_type_override` patch and non-flash-attention
KV cache layout. This is reported here as found, not fixed: neither
`membrane-kv-runtime` nor `membrane-kv-runtime-optimizer` ever actually
requests `DISABLED` in production (both only use `AUTO` for the
reference or force `ENABLED` for a candidate), so it does not affect
this project's actual measurements and is out of scope for this
phase's fix.

## 3. Repeated measurement, 20 runs (item 2)

SmolLM2-135M, `recall.txt`, the real exported `135m-aggressive.mpol`
policy, `n_tokens=1024 gen_tokens=24`, `n_threads=4`, `flash_attn=auto`:
**all 20 runs produced bit-identical cosine (0.99669393), top1 (100%),
top5 (100%), KL (0.00629647), first-divergence token (24, i.e. none),
KV byte count (3319644), generated text, AND KV-state SHA-256 checksum
(`9e398f5b...`).** Zero measurable variance across 20 independent
in-process evaluations of the identical configuration.

## 4. Thread comparison (item 3)

Same policy+prompt, 5 repeats at each of `n_threads` in `{1, 2, 4}`:
every one of the 15 runs produced the exact same checksum
(`9e398f5b...`, matching §3's) and cosine (0.99669393) — **thread count
has zero measurable effect on determinism for this model on this
hardware.** TTFT scales sensibly with thread count (2225ms at 1 thread
down to 685ms at 4 threads), confirming the threading is genuinely
active and doing real work — it just doesn't change the numerical
result.

## 5. Quantization timing: native vs post-hoc (item 6)

Same policy+prompt, symmetric `n_threads=4 flash_attn=auto`, 5 repeats
each of the native (write-time, ggml's own Q4_0/Q8_0 quantize kernels
via `kv_type_override`) and post-hoc (membrane's own `quant_roundtrip`
applied to an already-captured F16 blob) paths:

| path | cosine | top1 |
|---|---|---|
| native (write-time) | 0.99669393 | 100.0000% |
| post-hoc (after the fact) | 0.98985915 | 95.8333% |

Both fully deterministic (stddev 0.0 across all 5 repeats each). The
delta (cosine +0.00683, top1 +4.17 points) is **roughly two orders of
magnitude larger** than the flash-attention/shape asymmetry from §2
(0.0000166). **This is the dominant, real source of the
offline-vs-runtime prediction unreliability Phase 4.1 originally
found** — not measurement-configuration asymmetry, but membrane's own
quantize function (`quant_roundtrip_group`, a simple per-32-element
max-abs linear quantizer) rounding differently than ggml's native
Q4_0/Q8_0 block-quantization kernels for the identical source values.
This was always structurally expected (blob-splicing was designed as a
cheap pre-screen specifically because it isn't the real thing — Phase
4.2 §1), but this is the first time the "isn't the real thing" gap has
been measured in isolation from every other confound (same policy,
same prompt, same threads, same flash-attention setting, same
baseline).

## 6. Offline/runtime drift, token-by-token and per-slot (item 5)

Same policy+prompt: aggregate offline cosine 0.989859 vs runtime
cosine 0.996694 (delta +0.006835 — consistent with §5, since this is
the same native-vs-post-hoc comparison, now decomposed by position and
by slot). First per-step divergence beyond epsilon=0.001 is at **token
0** — the very first generated token, not something that builds up
over the sequence.

**Incremental per-slot attribution** (adding the policy's 12 Q4 slots
one at a time from an all-Q8 starting point, real offline+live
evaluation after each addition):

| slot added | offline cosine | runtime cosine | gap |
|---|---|---|---|
| layer 0 V | 0.990270 | 0.999635 | 0.009364 |
| layer 3 V | 0.990305 | 0.998986 | 0.008681 |
| layer 4 V | 0.989737 | 0.998902 | 0.009165 |
| layer 5 V | 0.990118 | 0.998243 | 0.008126 |
| layer 6 K | 0.991560 | 0.997826 | 0.006266 |
| layer 10 V | 0.991080 | 0.997328 | 0.006248 |
| layer 21 V | 0.990285 | 0.996957 | 0.006673 |
| layer 22 V | 0.990420 | 0.996757 | 0.006336 |
| layer 25 V | 0.990722 | 0.997246 | 0.006524 |
| layer 26 V | 0.990770 | 0.997019 | 0.006249 |
| layer 28 V | 0.990257 | 0.997120 | 0.006863 |
| layer 29 V | 0.989859 | 0.996694 | 0.006835 |

Unlike a hypothesis of one dominant layer driving the gap, **the
offline-vs-runtime gap is roughly constant (0.006-0.009) from the very
first slot addition onward** and does not grow or shrink noticeably as
more slots accumulate — consistent with §5's finding that the gap's
source (the quantization function itself) applies per-slot uniformly,
rather than being a compounding effect from accumulating many changes.
(A separate smoke-scale test with a much more aggressive all-Q4 policy
did show the gap shrinking sharply as more layers were added — that
pattern is scenario-dependent and not reported further here since it
was not re-verified at real scale; the moderate 12-V-slot policy above
is the real, at-scale measurement.)

## 7. The fix (item 8)

**Root cause, fully isolated and measured (§2):** the reference
(`capture_baseline()`) and every candidate (`eval_live()`) in
`tools/membrane-kv-runtime-optimizer/main.cpp` decoded the prompt with
different batching shapes — (prefix, last-token) as two separate
`llama_decode` calls for the reference, one whole-prompt call for the
candidate. This alone, with every other variable held identical
(threads, flash-attention, policy, baseline), fully reproduces Phase
4.2 §10's original discrepancy in both direction and order of
magnitude.

**Minimal fix applied:** `tools/membrane-kv-runtime-optimizer/main.cpp`
gained a `decode_prompt_matched()` helper that decodes the prompt in
the same (prefix, last-token) shape `capture_baseline()` already uses
(required there for the offline blob-splicing backend's prefix-only
blob), and `eval_live()`'s two `decode_prompt(...)` call sites (for
`free_ctx` and `forced_ctx`) now call it instead of decoding the whole
prompt in one call. No threshold, margin, search-budget, or benchmark
scope changed — this is purely a measurement-methodology correction.

**Verified fixed:** re-running the real, rebuilt optimizer binary
(`--search-budget 1`, `short.txt`, `SmolLM2-135M`) now shows `all-FP16
x short.txt: cosine 1.000000` in its own final comparison table —
exactly 1.0, where Phase 4.2's real runs showed 0.999998 (135M) /
0.999999 (360M) for the identical comparison before this fix.

`membrane-kv-runtime` (Phase 4.1's tool) was checked and found to
already be internally self-consistent: its own reference and every
candidate go through the identical `run_config()` function with the
identical (whole-prompt) decode shape, so it was never asymmetric with
itself — no fix needed there. It does use a *different* convention
than the (now-fixed) optimizer tool (whole-prompt vs
prefix-plus-last-token), which is why a cross-tool comparison between
the two (as Phase 4.2 §10 did) can still show a residual, tiny
(~0.00001-0.00002 cosine) difference even after this fix — both tools
are now internally self-consistent, but they still don't share one
convention. Unifying that convention was judged out of scope for a
"minimal fix": it would mean changing a tool (`membrane-kv-runtime`)
that was not itself broken, for a benefit (cross-tool number-for-number
matching) far smaller than either tool's own margin-tier buffers.

**What this fix does and does not explain:** it fully explains the
tiny (~0.00001-00.00002 cosine) gap between the optimizer's own
accept-time measurement and a later re-measurement of the identical
policy through the identical tool. It does **not** explain, and was
never expected to explain, the much larger (~0.005-0.01 cosine)
offline-vs-runtime gap Phase 4.1 originally found and Phase 4.2 relied
on LIVE_RUNTIME to correct for — §5/§6 above show that larger gap comes
from the quantization function difference, which is exactly why Phase
4.2's entire two-backend design (offline pre-screen only, real runtime
required to accept) was correct to insist on. This fix makes the real
backend's OWN self-consistency slightly better; it does not, and was
never meant to, replace the real backend with the offline one.

## 8. Verification (item 9)

- **Release**: `test_checkpoint` (16/16) and the existing unit suite
  (16/16) both rebuilt and pass under `build-rel`.
- **ASan+UBSan**: same, rebuilt and pass under `build-asan`, 0 sanitizer
  reports.
- **llama.cpp-integrated build**: `build-llama` rebuilds cleanly
  (50/50 targets, including the fixed `membrane-kv-runtime-optimizer`,
  the unchanged `membrane-kv-runtime`, and the new
  `membrane-kv-variance`).
- **Deterministic repeat test**: §3/§4 above (20 in-process repeats,
  15 more across 3 thread counts, all bit-identical) — this phase's own
  central empirical claim, verified directly rather than assumed.
- **Real interrupted/resumed run, on the FIXED binary**: launched a real
  search (`SmolLM2-135M`, `--search-budget 10`, two prompts,
  `n_tokens=512 gen_tokens=16`), waited for the checkpoint to accumulate
  real live-eval decisions, sent `SIGTERM` while the process was still
  running, then verified the checkpoint's 12 lines all parsed as valid
  JSON with no torn record. Resumed with `--resume`: the log reported
  `resuming interrupt-verify/conservative: 10 prior decisions
  fast-forwarded` (matching the conservative tier's real
  `search_complete` already in the checkpoint) followed by `resuming
  interrupt-verify/balanced: 0 prior decisions fast-forwarded` (the
  tier that had not started yet) — exactly the expected fast-forward
  behavior, confirming checkpoint/resume still works correctly after
  the `eval_live()` shape fix (which touches none of the checkpoint
  code, but this is verified rather than assumed).

## 9. Summary

The root cause of Phase 4.2 §10's measurement variance was found,
isolated with controlled experiments, and fixed with a small,
targeted change: a real batching-shape asymmetry between how the
reference and the candidate decoded the prompt, unrelated to
threading, sampling, or flash-attention resolution (all three were
directly tested and ruled out at real scale). The fix makes
`membrane-kv-runtime-optimizer`'s own accept-time measurement and any
later re-measurement of the identical policy agree exactly for a
non-quantized reference case (cosine 1.000000, was 0.999998).

A second, larger, and arguably more important finding came out of the
same investigation: the offline-vs-runtime gap Phase 4.1 and 4.2 relied
on real-runtime verification to catch is dominated by membrane's own
quantization function differing from ggml's native quantize kernels
(§5, §6) — roughly two orders of magnitude larger than the
configuration-asymmetry bug this phase fixed. That gap is not a bug;
it is the entire reason Phase 4.2's two-backend design exists, and this
phase's controlled measurement makes that design decision's
justification quantitative rather than anecdotal for the first time.

No quality threshold, margin, search budget, or benchmark scope was
changed anywhere in this phase.

