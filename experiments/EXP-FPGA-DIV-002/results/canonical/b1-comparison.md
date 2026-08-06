# EXP-FPGA-DIV-002 Phase B1 -- baseline vs. dual-radix4 `q8_scale`: comparison

Every number below is **MEASURED** this session (`scripts/run-exp-q8-divider-002.sh
--phase b1 --full`, 2026-08-03) unless explicitly marked **ESTIMATED** or
**UNAVAILABLE**. Raw data: `b1-differential.json`, `b1-full-datapath.json`,
`b1-synthesis.csv`.

## 1. Correctness

| | Baseline `q8_scale` | `q8_scale_dual_radix4` |
|---|---|---|
| Divider | `membrane_fp_divider` x2 (wide combinational) | `membrane_fp_divider_radix4` x2 (iterative, reused unmodified from production `q4_scale.sv`) |
| Differential cases | -- | 4,052,224 |
| d mismatches | -- | **0** |
| id mismatches | -- | **0** |
| Accepted == Retired == Total | -- | 4,052,224 == 4,052,224 == 4,052,224 (no drop/duplicate) |
| Reset-recovery fails | -- | 0 |
| All-zero (`amax`=+0) | d=+0.0, id=0.0 (masked) | identical, bit-exact |
| Negative-zero (`amax`=-0) | d=-0.0, id=-Infinity (baseline's own pre-existing quirk, NOT masked -- `amax_f32==32'h0` check doesn't catch -0.0) | identical, bit-exact reproduction of the SAME quirk (not fixed) |

## 2. Latency / throughput

| | Baseline `q8_scale` | `q8_scale_dual_radix4` |
|---|---|---|
| Latency (cycles) | 1 (fixed) | min=2, mean=14.888, max=34 (w/ backpressure); no-bp min=2, mean=14.599, max=15 |
| Initiation interval | 1 | **16** (measured, 2,000 back-to-back cases) |
| Max in-flight | 1 | 1 (single in-flight by construction; live-asserted, never violated) |
| ECP5 cells | 123,742 | **2,775** (**-97.76%**) |
| Generic cells | 21,800 | 4,442 (-79.62%) |

Two radix-4 instances measured together (2,775 cells) land *below* both the
naive 2x-single-instance estimate (2 x 1,509 = 3,018) and Phase A's own
extrapolated 3,000-4,000-cell range -- real integration synthesis shares
some cost between the two parallel instances, same qualitative effect
already seen in baseline `q8_scale`'s own 2-instance sharing.

## 3. Full-datapath impact (1,310,000 transactions/variant)

| Mode | Baseline mean latency (cycles) | Dual-radix4 mean latency (cycles) | Change |
|---|---|---|---|
| Q8_0 encode | 38.519 (focused) / 54.506 (mixed-run aggregate) | 333.474 | **+511.8%** (direct effect -- expected) |
| Q8_0 decode | 26.025 | 38.600 | **+48.3%** (collateral -- Q8_0 decode never touches the new divider) |
| Q4_0 encode | 267.859 | 289.920 | **+8.2%** (collateral, on top of Q4_0 encode's own pre-existing serialization cost) |
| Q4_0 decode | 26.113 | 38.507 | **+47.5%** (collateral) |
| Overall cycles/transaction | 6.732 | 12.151 | **+80.5%** |

Both variants: 1,310,000/1,310,000 transactions, **0 fails**, 0 internal
assertion firings (`--assert`-compiled Verilator builds).

**Why decode paths slow down too**: Q8_0/Q4_0 decode never touch
`q8_scale_dual_radix4`, but this phase's own scheduling choice (full
serialization of both single-in-flight encode classes, no reorder buffer --
the simplest CORRECT option, per this phase's own explicit scope) blocks
*all* issuance while a Q8_0 encode transaction is in flight. Since that
window is now ~330 cycles instead of ~54, everything queued behind it in
the input FIFO waits longer before being issued -- a real, measured,
disclosed cost of the scheduling choice, not the divider substitution
itself.

## 4. Performance estimates (ESTIMATED clock, MEASURED cycles; Fmax UNVERIFIED)

| | Baseline `q8_scale` | `q8_scale_dual_radix4` |
|---|---|---|
| Cycles/op (II) | 1 | 16 |
| Ops/s @100MHz | 100,000,000 | 6,250,000 |
| Ops/s @200MHz | 200,000,000 | 12,500,000 |
| Area-throughput proxy (ops/s per ECP5 cell) @100MHz | ~808 | ~2,252 |

Dual-radix4 is **~2.79x more area-efficient** (throughput per synthesized
cell) despite 16x lower raw per-instance throughput, because its footprint
shrank ~44.6x. Fmax: **UNAVAILABLE**. Timing closure: **UNVERIFIED**.
Structurally correct statement: *both single-cycle wide combinational Q8
divides were structurally removed; vendor timing closure remains
unverified.*

## 5. Synthesis matrix summary (see `b1-synthesis.csv` for full detail)

| Variant | Generic cells | ECP5 cells | vs. baseline |
|---|---|---|---|
| A. `membrane_fp_divider` standalone | 10,234 | 73,629 | -- |
| B. `membrane_fp_divider_radix4` standalone | 1,556 | 1,509 | -97.95% ECP5 vs A |
| C. `q8_scale` baseline | 21,800 | 123,742 | -- |
| D. `q8_scale_dual_radix4` | 4,442 | 2,775 | -97.76% ECP5 vs C |
| E. Full experimental top-level | UNAVAILABLE | UNAVAILABLE | best-effort synth_ecp5 timed out at 1500s, same disclosed limitation as Phase A's own baseline full-top attempt -- not a synthesizability failure (hierarchy check + Verilator elaboration both clean) |

## Decision rationale

**PROMOTE_CANDIDATE** for the experiment branch (see `phase-b1.md`'s own
Decision section for the full item-by-item justification). Weighed
explicitly: a **-97.76%** real, measured area reduction at the `q8_scale`
level against a real, measured **16x** initiation-interval cost and a
**48/47/8 percent** collateral slowdown on Q8_0 decode / Q4_0 encode+decode
under this phase's own deliberately-simplest scheduling choice. Exactness
is not in question (0 mismatches across 4,052,224 cases); the open
questions this phase leaves for a follow-up (CONTINUE-class, if pursued)
are (1) a smarter scheduler that lets Q8_0 decode / Q4_0 paths continue
issuing around an in-flight Q8_0 encode, and (2) a real (not best-effort/
timed-out) full-top synthesis number once more compute/memory budget is
available.

This is an experiment-branch-only decision. **Not** an authorization to
merge into `main` -- no pull request has been opened, per this phase's own
explicit scope.
