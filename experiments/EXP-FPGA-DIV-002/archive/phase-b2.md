# Experiment record: EXP-FPGA-DIV-002 Phase B2

Filled from [EXPERIMENT_TEMPLATE.md](https://github.com/kadireren7/membrane/blob/main/EXPERIMENT_TEMPLATE.md), same
convention as this experiment's own [experiment.md](experiment.md) (Phase
A) and [phase-b1.md](phase-b1.md) (Phase B1). Branch:
`experiment/q8-divider-pipeline`.

## Experiment ID

`EXP-FPGA-DIV-002` Phase B2

## Hypothesis

Phase B1's own measured collateral slowdown (Q8_0 decode +48.3%, Q4_0
decode +47.5%, Q4_0 encode +8.2%, all vs. unmodified production baseline)
is caused by a blanket scheduling choice (`!q8enc_inflight &&
!q4enc_inflight` gating ALL other issuance), not by any real hardware
resource conflict -- Q8_0 decode, Q4_0 decode, and Q4_0 encode each use
hardware entirely disjoint from `q8_scale_dual_radix4`'s two dividers. A
scheduler that lets those resource-independent transactions issue and
execute concurrently with an in-flight Q8_0/Q4_0 encode, while preserving
strict output ordering via a small, bounded number of result-holding
registers (not a general reorder buffer), should eliminate most of that
collateral cost while preserving `q8_scale_dual_radix4`'s own exactness
and area advantage unchanged.

## Preflight (task item 0)

Two shell processes were reported still running at the end of Phase B1.
Both were `until ! pgrep -f "run-exp-q8-divider-002.sh" ...; do sleep N;
done; ...` watcher loops launched by the prior session to wait for Phase
B1's own background pipeline. **Root cause of why they never terminated**:
each watcher's own shell command line (visible via `/proc/[pid]/cmdline`)
contained the literal string `run-exp-q8-divider-002.sh` as part of its
own `eval`'d script text -- `pgrep -f` matches against full command lines
of ALL processes (excluding only the invoking `pgrep` call itself, not
ancestor shells), so each watcher's own `pgrep -f "run-exp-q8-
divider-002.sh"` matched its own wrapper shell forever, even though the
real Phase B1 pipeline had already finished (confirmed independently: the
Phase B1 git commit and push had already completed). Neither watcher was
writing to any result artifact -- both were pure read-only polling loops
(`sleep`, `pgrep`, occasionally `tail`) -- so no concurrent-write risk
existed. Both were killed (`kill <pid>`, confirmed terminated). Branch
HEAD confirmed at `5f05b4f` before any change was made. Working tree
confirmed clean except the pre-existing dirty `third_party/llama.cpp`
submodule (untouched throughout this phase, per its own explicit scope).
This same self-matching `pgrep -f` hazard was then deliberately avoided
for Phase B2's own background monitoring, using PID-based `kill -0`
checks instead of command-line string matching.

## Baseline / prior commit

Phase B1 result, commit `5f05b4f` (`research: evaluate dual exact
radix-4 Q8 dividers`), pushed to `experiment/q8-divider-pipeline`. See
`phase-b1.md` for that phase's own full record (unchanged, historical).

## Method

1. Traced the exact ready/valid and scheduler behavior through
   `q8_scale_dual_radix4`, Phase B1's own experimental top, and the
   production top's own header/assertions to (a) confirm the ordering
   contract (task item 3) directly from source rather than assuming it,
   and (b) classify every stall cycle into a taxonomy, quantifying Phase
   B1's own measured stall percentages by mode --
   `results/b2-stall-root-cause.md`.
2. Designed and implemented the smallest correct scheduler change meeting
   the task's own explicit constraints (no depth-4/8 ROB, no
   general-purpose reorder buffer, bounded bookkeeping only): a global
   sequence-number-tagged in-order-retirement mechanism, with one hold
   register each for Q8_0/Q4_0 encode (now independent of each other) and
   a shared, small (`SHADOW_DEPTH` = 1 or 2, parameterized) hold queue for
   the fixed-latency `tag_pipe` classes (Q8_0/Q4_0 decode) --
   `rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4_b2.sv`.
   `q8_scale_dual_radix4.sv` itself is reused byte-for-byte, unmodified,
   from Phase B1.
3. Wrote a single C++ correctness+performance tool, compiled three times
   (baseline / Phase B1 / Phase B2) via the same compile-time-DUT-
   selection technique Phase B1 established, that (a) drives every
   required correctness scenario (task item 6: balanced per-mode traffic,
   mixed-mode interleave, long Q8_0-encode bursts, Q8_0-encode-then-long-
   Q4_0-decode-burst, three alternating patterns, dense random-mode
   heavy-backpressure, a queue-full boundary stage, reset in every
   externally-observable scheduler state) against golden vectors with
   strict FIFO-order checks, and (b) measures a 10-profile performance
   matrix (task item 7) with full percentile latency statistics --
   `rtl/experimental/q8_div/tb_top_verilator_q8_b2_variant.cpp`. Two real
   bugs were found and fixed in this tool during its own development
   (disclosed, not hidden): a self-inflicted deadlock in the queue-full
   boundary stage (holding `out_ready=0` while looping until a count of
   retirements that could never happen under that condition), and a
   shared-RNG hazard that would have silently let the actual sequence of
   transaction modes diverge between differently-timed DUTs, undermining
   the "identical seeds and traffic" comparison the task itself requires
   -- fixed by giving mode selection its own RNG stream, advanced once per
   issued transaction rather than once per cycle.
4. Extended `scripts/run-exp-q8-divider-002.sh` with a `--phase b2` mode
   (`--quick`/`--full`/`--resume`/`--output-dir`), building all three
   correctness+performance binaries, running the synthesis matrix (task
   item 9), and reusing the existing local-verification-suite stage.
5. Ran `--phase b2 --quick` first to validate the whole pipeline (~8,675
   correctness transactions + a 10-profile matrix at N_PROFILE=2,000 per
   variant), then `--phase b2 --full` for real numbers (6,250,000
   correctness transactions + a 10-profile matrix at N_PROFILE=200,000,
   per variant).
6. Measured the `SHADOW_DEPTH=1` (default) result from the `--full` run
   directly. Since the depth=1 result did not meet the strict <=10%
   collateral-slowdown target at 20-25% Q8_0-encode traffic density (task
   item 8), per item 10's own instruction, parameterized the SAME RTL file
   with `SHADOW_DEPTH` (rather than duplicating it into a second file) and
   separately built and ran the `SHADOW_DEPTH=2` configuration at the same
   full scale (manual `-GSHADOW_DEPTH=2` Verilator elaboration + a direct
   binary run, reusing the same golden vectors -- not yet wired into the
   script's own `--phase b2` flag, see Limitations).
7. Compared all four variants (baseline, B1, B2 depth=1, B2 depth=2)
   against the task's own success thresholds (item 8) and documented the
   result honestly, including where the strict target is NOT met and why
   (item 8's own "report the lower bound and the exact architectural
   reason" fallback clause) -- `results/b2-comparison.md`,
   `results/b2-stall-breakdown.csv`.
8. Re-ran the synthesis matrix (component `q8_scale_dual_radix4`, full
   B2 top-level best-effort) and the full local verification suite
   (Debug/Release/ASan+UBSan/TSan ctest, ggml quant parity,
   `verify-results.py`/`verify-paper.py`/`verify-outreach.py`), all as
   part of `--phase b2 --full`.

## Why not just stall `tag_pipe`? (a real design hazard found and avoided,
not merely an alternative considered)

An earlier version of this design attempted to preserve ordering by
freezing `tag_pipe`'s own shift register whenever its tail held a
completed entry that was not yet its turn to retire. This is unsound: an
admitted `tag_pipe` entry's downstream data (`q8_dequantize`'s or
`q4_unpack`'s own output, captured into `q8dec_pad[0]`/`q4dec_pad[0]`) is
produced by a plain, always-running, one-cycle-fixed-latency pipeline with
**no ready/valid backpressure input of its own** -- if a freeze happens to
coincide with the exact cycle that pipeline's result becomes valid (a real
race: the freeze condition is driven by an unrelated, older, still-
outstanding Q8_0/Q4_0 encode transaction, not by anything about the entry
currently arriving), the fresh result is silently overwritten the next
cycle with nothing to blame, since nothing captured it. This is data loss,
not just suboptimal scheduling. The shipped design avoids this entirely:
`tag_pipe` (and its downstream data-alignment pipes) are NEVER frozen --
they always shift unconditionally, exactly as Phase B1/production do; the
only new behavior is capturing the (bounded, `SHADOW_DEPTH`-limited)
tail entries that arrive out of turn into a dedicated hold register the
one cycle they are ready, which has no backpressure hazard because the
capture condition and the tail's own arrival are the same event.

## Environment

Same project dev machine as every prior phase: 5.6 GiB RAM, shared with
other concurrent local sessions. Available RAM dropped as low as several
hundred MiB during the synthesis matrix stage (no OOM kill observed).
Same toolchain: `tools/.local-yosys` (Yosys 0.33), `tools/.local-verilator`.
No place-and-route tool, no Xilinx/Altera toolchain, no physical FPGA
board.

## Metrics

Same categories as Phase B1 plus: per-mode min/mean/p50/p95/p99/max
latency and throughput across 10 traffic profiles; per-mode collateral
slowdown vs. baseline at multiple Q8_0-encode traffic densities; overall
cycles/transaction improvement vs. Phase B1; stall-cycle taxonomy with
real quantified per-mode numbers (`results/b2-stall-breakdown.csv`);
`SHADOW_DEPTH=1` vs. `SHADOW_DEPTH=2` incremental comparison.

## Success criteria / Results against task item 8's thresholds

- **Correctness**: 0 payload mismatches, 0 ordering errors, 0
  drops/duplicates, 0 reset-recovery failures -- **MET**, across four full
  6,250,000-transaction runs (baseline, B1, B2 depth=1, B2 depth=2).
- **Area**: `q8_scale_dual_radix4` (unmodified, reused) remains
  -97.76% vs. the original baseline `q8_scale` -- **MET** (well over the
  90% bar) at the component level; no real synthesized number exists for
  the scheduler's own added logic at the full-top level (neither Phase
  B1's own full top nor Phase B2's own full top completed synthesis, both
  UNAVAILABLE) so no real full-top delta can be claimed either way,
  disclosed honestly rather than assumed negligible.
- **Q8_0 decode / Q4_0 decode / Q4_0 encode collateral slowdown <=10%**:
  **MET at light Q8_0-encode density (10%)** -- collateral is actually
  negative (B2 faster than baseline) at both evaluated depths. **NOT MET
  at 20-25% Q8_0-encode density** -- residual collateral is 16-26%
  (depth=1) / 16-21% (depth=2), though this represents eliminating
  67-77% of Phase B1's own collateral cost. See
  `results/b2-comparison.md` section 6 for the exact architectural reason
  (input-FIFO queueing behind Q8_0/Q4_0 encode's own single-in-flight
  service time, not shadow-queue depth, dominates the residual once
  `IN_FIFO_DEPTH=16`, an existing out-of-scope parameter, is the binding
  constraint).
- **Overall cycles/transaction improvement >=25% vs. B1**: **MET**, on
  every mixed-traffic profile measured, by both depths (28.5-36.6%).
- **No regression in pure non-Q8_ENC streams**: **MET** -- `100pct_Q8_DEC`
  and `100pct_Q4_DEC` profiles show B2 within measurement noise of
  baseline (both near their own structural II=1-ish floor, since neither
  mode ever contends with an encode-class transaction in a 100%-single-
  mode stream).

## Limitations

- The strict <=10% collateral-slowdown bound is not met at realistic-to-
  heavy Q8_0-encode traffic density with either evaluated `SHADOW_DEPTH`
  -- disclosed as a real, architecturally-grounded shortfall (section 6 of
  `results/b2-comparison.md`), not glossed over.
- No real full-top-level synthesized cell count exists for either Phase
  B1's or Phase B2's own full top-level (both UNAVAILABLE, best-effort
  `synth_ecp5` timed out at the same 1500s bound for the same reason as
  every prior phase's own whole-top attempt) -- the scheduler's own real
  added-area cost is therefore ESTIMATED from register-bit counts, not
  MEASURED_BY_TOOL.
- `SHADOW_DEPTH=2` was evaluated with a manual Verilator parameter
  override, not yet wired into `scripts/run-exp-q8-divider-002.sh`'s own
  `--phase b2` flag as a selectable option -- a natural, small follow-up
  if this line of work continues, not done here to keep this phase's own
  script change minimal and because `SHADOW_DEPTH=1` remains the shipped
  default regardless (section 8 of `results/b2-comparison.md`).
- The "realistic" traffic mix is a disclosed SIMULATED/reconstructed
  approximation (20% Q8_0 encode / 40% Q8_0 decode / 10% Q4_0 encode / 30%
  Q4_0 decode), not a captured real workload trace, same convention as
  this project's own prior "Q8 runtime amax distribution" precedent.
- No real Fmax/timing-closure number exists for any configuration -- no
  P&R tool in this environment, unchanged from every prior phase.
- No real FPGA hardware, board, or vendor toolchain was used anywhere in
  this experiment.

## Decision

**CONTINUE.**

- Exact: **YES** -- 0 mismatches, 0 ordering errors, 0 drops/duplicates,
  0 reset-recovery failures, across four full 6,250,000-transaction runs
  (2 more than Phase B1's own scope, since this phase evaluates 2 shadow
  depths on top of baseline+B1).
- B2 performance targets met: **PARTIALLY** -- the >=25%-vs-B1 overall
  improvement target is met cleanly on every profile; the strict
  <=10%-vs-baseline collateral bound is met (and exceeded) at light
  Q8_0-encode density but not at 20-25% density, for a real, disclosed,
  architecturally-grounded reason (section 6 of `results/b2-comparison.md`)
  that this phase's own scope does not authorize resolving further
  (deeper input buffering or a larger reorder structure than `SHADOW_DEPTH
  <= 2` are both out of scope).
- Area remains strongly favorable: **YES** at the component level (the
  one place with a real, apples-to-apples measured number).
  Full-top-level area impact of the scheduler itself is UNAVAILABLE
  (disclosed, not assumed).
- Bounded/simple scheduler: **YES** -- explicitly smaller than every
  configuration EXP-FPGA-DIV-001 Phase B3 already rejected, no general
  reorder buffer, `SHADOW_DEPTH<=2` only.
- Reproducible: **YES** -- `scripts/run-exp-q8-divider-002.sh --phase b2
  --quick|--full`, both exercised this session (depth=2 sweep via a
  documented manual parameter override, see Limitations).

Per this task's own explicit instruction ("Do not call B1
production-ready merely because the divider itself is excellent"), the
symmetric statement applies here: Phase B2's scheduler is a real,
substantial, honestly-quantified improvement over Phase B1 -- it is not
being called a complete solution to collateral slowdown merely because
the improvement is large. This is a Phase-B2-internal, experiment-branch-
only decision. It does **not** authorize merging any experimental file
into production RTL.

## Promotion status

`not proposed` -- remains on `experiment/q8-divider-pipeline`, pushed to
the public repository, **not merged into `main`**, no pull request
opened, per this task's own explicit scope. Nothing here is a verified
public claim of the `v0.1.0-research` release.
