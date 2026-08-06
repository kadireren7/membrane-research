# EXP-FPGA-DIV-001 -- decision log

A single, running summary of this experiment's per-phase decisions and
why, so the current state is readable without opening every phase
document. Each phase's own document (`baseline.md`, `phase-b1.md`,
`phase-b2.md`, `phase-b3.md`, `phase-b4.md`) is the authoritative, detailed record; this
file is a pointer/summary, not a replacement. Nothing in this file
authorizes or implies a merge into `main` -- see `experiment.md`'s own
"Promotion status" section, which is the authoritative statement on that.

| Phase | Scope | Decision | Why (one line) |
|---|---|---|---|
| A | Characterize the baseline `membrane_fp_divider` and its 4 call sites | Accepted as complete | Pure characterization, no design work; real synthesis + 520,000-txn cosim confirmed the baseline unchanged and identified 4 candidate directions for Phase B |
| B1 | Replace `q4_scale`'s constant-divisor `mx/-8.0f` with an exact power-of-two shortcut | CONTINUE | Exact and clean (2.2M+ differential cases, 0 mismatches), but the real `q4_scale`-level area win was small (-2.2%) because ABC was already sharing most of the two divider instances' cost |
| B2 | Replace `q4_scale`'s remaining variable-divisor `1/d` with an exact iterative divider | CONTINUE | Exact and clean (2.45M+ differential cases, 0 mismatches; 520,000/520,000 full-datapath), large real area win (-96.9% at `q4_scale`), but full-serialization scheduling collaterally slowed 3 untouched chains ~1.9-2.6x -- a queueing cost, not a divider-speed cost |
| B3 | Decouple issuance of the 3 unaffected chains from Q4_0 encode's in-flight status, via a small bounded completion reorder buffer | **REJECT_ARCHITECTURE** (revised from this phase's own original CONTINUE call -- see note below) | Correct and bounded at every depth tested (0 fails/drops/duplicates/deadlocks across 7.77M+ real transaction-checks) -- NOT a correctness failure. Rejected as an ARCHITECTURE because the throughput gain is only ~4-5% (depth=4: 11.395 -> 10.936 cycles/txn) while the depth=4 reorder buffer alone (14,959 ECP5 cells) is far larger than the entire `q4_scale_b2` unit it's protecting (2,268 cells) -- the complexity/area this architecture adds is not justified by the throughput it returns |
| B4 | Replace B2's radix-2 iterative Q4 divider with an exact radix-4 (2 quotient bits/cycle) iterative divider, WITHOUT B3's reorder buffer | **PROMOTE_CANDIDATE** | Exact parity vs. BOTH `membrane_fp_divider` and B2 simultaneously (4,456,685 cases, 0 mismatches); 1,110,000/1,110,000 full-datapath, 0 fails; overall cycles/transaction -32.1% vs. B2 (7-8x the size of B3's own best result), for only +25.0% ECP5 cells at `q4_scale` (2,836 vs. B2's 2,268) -- B2's own -96.95% area win vs. baseline is barely eroded (-96.19%). No new scheduling complexity. The strongest result of any Phase B sub-phase to date |

## Note on the B3 decision revision

Phase B3's own document (`phase-b3.md` section 9) originally recorded
**CONTINUE**, reasoning that the throughput win was "real but modest" and
the area cost "measurably erodes but does not destroy" B2's advantage.
Revisiting this with the Phase B4 task's explicit framing: the reorder
buffer is not a small addition being tuned further -- at the only depth
that helps at all (4), it costs MORE area than the entire unit it exists
to protect, for a ~4-5% throughput gain. That is a genuine architecture-
level mismatch between cost and benefit, not a "just needs more tuning"
situation. This decision log now records **REJECT_ARCHITECTURE** for B3,
superseding `phase-b3.md`'s own CONTINUE call -- per this project's
disclosed-not-rewritten convention, `phase-b3.md` itself is NOT edited to
match; both documents remain on record, and this note explains the
discrepancy rather than hiding it. REJECT_ARCHITECTURE here means: the
correctness result stands (0 fails, real and reusable as a research
artifact, code not deleted), but the `membrane_completion_reorder` +
`membrane_quant_stream_top_b3` SCHEDULING approach is not carried forward
as the direction for reducing Q4_0-encode's collateral cost. See Phase B4
below for the alternative actually pursued (speed up the divider itself
instead of adding a scheduler).

## Current recommendation if this experiment continues

- **Phase B4 (radix-4 exact iterative divider, B2-style scheduling,
  no reorder buffer) is the strongest candidate on record.** If a future
  session is authorized to act on a promotion, this is the direction to
  bring to `main` -- not Phase B3's reorder buffer (rejected above), and
  not B2 alone (superseded by B4's own, larger, cheaper improvement).
  This decision log does not itself authorize that step; see "Promotion
  status" below.
- **`q8_scale.sv`'s two divider instances remain completely untouched**
  by every phase so far (A through B4) -- the same structural timing risk
  (wide combinational divide, no real Fmax data in this environment) Phase
  A first disclosed for them still applies, unchanged, unquantified. A
  natural, not-yet-started follow-on would apply Phase B4's own radix-4
  divider to `q8_scale.sv`'s two call sites too, using the same
  provably-exact construction.

## Promotion status

Not proposed for any phase (A, B1, B2, B3, or B4). See `experiment.md`'s
own "Promotion status" section for the authoritative statement -- this
remains disclosed, research-in-progress work on
`experiment/fp-divider-pipeline`, never merged into `main`, no pull
request opened, regardless of any individual phase's own PROMOTE_CANDIDATE/
CONTINUE/REJECT_ARCHITECTURE call recorded above.

**Post-promotion update (main context, this copy only)**: as recommended
above, Phase B4 (radix-4 exact iterative divider, B2-style scheduling, no
reorder buffer) was the direction brought to `main` -- **B1+B4 production
integration was merged through PR #2**, via a clean re-implementation, not
a merge of this experiment branch. See `experiment.md`'s own matching
update note for the full statement; not re-argued here per this file's
own disclosed-not-rewritten convention (the same convention that already
governs the Phase B3 decision-revision note above).
