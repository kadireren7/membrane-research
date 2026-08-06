# EXP-FPGA-DIV-002 Phase B2 -- stall root-cause analysis

Traces the exact ready/valid and scheduler behavior through
`q8_scale_dual_radix4`, the Phase B1 experimental top
(`rtl/experimental/q8_div/membrane_quant_stream_top_q8_dual_radix4.sv`),
and the production top (`rtl/membrane_quant_stream_top.sv`), classifies
every stall cycle, and quantifies Phase B1's own measured stall
percentages by mode. Written **before** any B2 RTL change (per task
ordering: root-cause first).

## 0. Ordering contract (task item 3): resolved from source, not assumed

`rtl/membrane_quant_stream_top.sv`'s own header, "---- ordering guarantee
----" section (lines 35-84), states explicitly and unconditionally:

> "Because issue order is preserved by the input FIFO and every
> transaction retires in the order it was issued ... output ordering is
> preserved by construction across ALL four modes."

This is not a design suggestion -- it is enforced by two live `` `ifndef
SYNTHESIS `` assertions in the same file (`in_flight` range check, and
`!(retire_fire && q4enc_direct_retire)` mutual exclusion), and it is what
every existing differential testbench in this project checks at retire
time (`ID MISMATCH at retire` / `MODE MISMATCH at retire` fatal checks in
`rtl/tb/tb_top_verilator.cpp` and `rtl/experimental/q8_div/tb_top_verilator_q8_variant.cpp`,
both of which track a single FIFO-ordered `inflight` deque and compare the
retiring transaction against `inflight[head]`, never searching for a
matching tag out of order). Phase B1's own experimental top
(`membrane_quant_stream_top_q8_dual_radix4.sv`) preserves the exact same
guarantee, using the exact same two mechanisms (shared `tag_pipe` for
fixed-latency modes, direct-retire-with-full-serialization for the two
now-variable-latency single-in-flight classes, Q4_0 encode and Q8_0
encode).

**Conclusion: option A -- strict output order matching accepted input
order -- is the confirmed, documented, assertion-enforced contract of this
datapath.** Option B (per-mode ordering only) and option C (tagged
out-of-order completion) are **not** what the existing RTL, its own
comments, or its own differential tests implement or check; adopting
either would be a silent contract change, not a scheduler optimization.
Phase B2 must therefore preserve strict global in-order retirement while
still allowing independent work to execute concurrently -- exactly the
"hold completed younger results until the older result completes" mode
the task's own item 3 describes for the strict-order case.

## 1. Why Phase B1 serializes globally (mechanism, not just symptom)

Phase B1's experimental top has **two** single-in-flight, variable-latency,
direct-retire classes: `q4enc_inflight` (unchanged from production,
`q4_scale`'s own radix-4 divider) and `q8enc_inflight` (new,
`q8_scale_dual_radix4`'s two radix-4 dividers). Its issue gate
(`membrane_quant_stream_top_q8_dual_radix4.sv` lines 160-174):

```
if (enc_mode_pop)
    issue_fire = in_fifo_out_valid && slot_ok && !q4enc_inflight
        && !q8enc_inflight && (flight_i == 0);
else
    issue_fire = in_fifo_out_valid && slot_ok && !q4enc_inflight
        && !q8enc_inflight;
```

The `else` branch -- covering Q8_0 decode AND Q4_0 decode, i.e. **every**
mode that does not touch either divider -- is gated by `!q8enc_inflight`
and `!q4enc_inflight` anyway. This is the **entire** mechanism, and it is
a blanket rule, not a resource-conflict check: Q8_0 decode
(`q8_dequantize`, one multiplier, no divider) and Q4_0 decode
(`q4_unpack`, one multiplier, no divider) share **zero** hardware with
either `q8_scale_dual_radix4` or `q4_scale`'s own divider, yet issuance of
either is blocked for the **entire** duration any Q8_0/Q4_0 encode
transaction is in flight. This blanket rule exists purely because it is
the simplest construction that keeps `tag_pipe` provably empty of "real"
entries whenever a direct-retire class is active (the file's own header,
and its own live assertion, prove this emptiness -- but the emptiness
itself is a **self-imposed** consequence of the blanket gate, not a
requirement of the ordering contract in section 0).

## 2. Stall taxonomy

Every cycle in the datapath, from a fixed external observer's point of
view, falls into exactly one category:

| Category | Definition | Present in this datapath? |
|---|---|---|
| **Divider busy** | `q8_scale_dual_radix4`'s own two radix-4 dividers are actively iterating on the currently-accepted operand (`busy=1`, `out_valid=0`). Unavoidable given the exact algorithm -- this is the real, disclosed latency cost Phase B1 already measured (mean 14.888 cycles, max 34). | Yes -- Q8_0 encode only. |
| **Input blocked by global serialization** | A transaction of a DIFFERENT, resource-independent mode sits at the head of the input FIFO, ready to issue, but is held back solely by `!q8enc_inflight`/`!q4enc_inflight` (section 1) even though its own engine is completely idle. **This is the avoidable category B2 targets.** | Yes -- Q8_0 decode, Q4_0 decode (100% of their B1 collateral cost, see section 3); Q4_0 encode partially. |
| **Output ordering wait** | An engine has *finished* its own computation but must hold its result because an OLDER (earlier-issued) transaction has not yet retired -- the real, bounded, disclosed cost B2 *introduces* in place of category above. Zero in B1 (B1 never lets a younger transaction get this far while an older one is outstanding) and bounded-by-construction in B2 (see `phase-b2.md`). | No in B1 (folded into "input blocked" instead); new, small, bounded category in B2. |
| **Downstream backpressure** | `out_ready` deasserted by the consumer, or `out_fifo` at its reserved-capacity limit. Present identically in baseline/B1/B2 -- not something the B2 scheduler changes. | Yes, all variants. |
| **Mode-switch bubble** | A shared physical pipeline stage that must drain/refill when consecutive transactions differ in mode. **Does not apply to this datapath**: every mode (Q8 encode, Q8 decode, Q4 encode, Q4 decode) has its own fully dedicated hardware chain (`q8_maxabs_reduce`/`q8_scale_*`/`q8_quantize_pack`; `q8_dequantize`; `q4_scan`/`q4_scale`/`q4_pack`; `q4_unpack`) -- nothing is time-multiplexed across modes, so there is no shared-stage refill cost to bubble on. Disclosed as genuinely not applicable, not force-fit into a number. | N/A (structurally absent). |
| **Reset recovery** | Fixed few cycles after `rst_n` deasserts before steady issuance resumes. Same small constant in every variant (baseline/B1/B2), already exercised by every full-datapath testbench's reset-mid-stream stage. | Yes, all variants, ~constant. |
| **Unavoidable Q8_ENC dependency** | A transaction genuinely CANNOT retire before an older, still-computing Q8_0 encode, because the ordering contract (section 0) requires it -- this is "divider busy" as *observed from a younger transaction's own vantage point*, not a separate physical cost. | Yes, in B2 only (B1 does not distinguish this from "input blocked" since it blocks issuance outright). |
| **Avoidable collateral stall** | Exactly the "input blocked by global serialization" category above, renamed to make the success/failure framing explicit: cycles where SOME transaction was ready and its own engine was idle, purely waiting on an unrelated engine's blanket gate. | Yes -- this is what Phase B2 is built to eliminate. |

## 3. Quantified Phase B1 stall percentages by mode (measured baseline)

Source: `results/b1-full-datapath.json` (1,310,000 transactions/variant,
identical golden vectors/traffic both runs). Since Q8_0 decode and Q4_0
decode share **zero** hardware with either divider engine, their entire
measured mean-latency delta vs. the unmodified production baseline is, by
the taxonomy above, 100% "avoidable collateral stall" (queueing behind
`!q8enc_inflight`/`!q4enc_inflight` in the input FIFO) -- none of it is
"divider busy" or "unavoidable Q8_ENC dependency," because those transactions
never touch either divider.

| Mode | Baseline mean latency (cyc) | B1 mean latency (cyc) | Delta (cyc) | Delta classified as |
|---|---|---|---|---|
| Q8_0 encode | 54.506 | 333.474 | +278.968 | Divider-busy (own real cost, ~15 cyc mean structurally) + input-blocked-on-itself (single-in-flight queueing against other Q8_0 encodes in burst/adversarial stages) |
| Q8_0 decode | 26.025 | 38.600 | +12.575 | **100% avoidable collateral stall** (zero shared hardware with either divider) |
| Q4_0 encode | 267.859 | 289.920 | +22.061 | Mostly pre-existing production-baseline serialization cost (Q4_0 encode was ALREADY fully serialized against everything in the unmodified baseline, per `membrane_quant_stream_top.sv`'s own header, `docs`/EXP-FPGA-DIV-001); the +22.061-cycle (+8.2%) delta specifically attributable to Q8_0 encode's presence is **avoidable collateral stall** (Q4_0 encode's own divider hardware is fully independent of `q8_scale_dual_radix4`) |
| Q4_0 decode | 26.113 | 38.507 | +12.394 | **100% avoidable collateral stall** (zero shared hardware with either divider) |

**Percentage breakdown** (delta as % of baseline mean, i.e. Phase B1's own
reported "collateral slowdown" numbers): Q8_0 decode +48.3%, Q4_0 encode
+8.2%, Q4_0 decode +47.5% -- and, per the analysis above, effectively ALL
of the Q8_0/Q4_0 decode numbers, and the majority of the Q4_0 encode
number, is avoidable (removable without touching the divider itself or
the ordering contract), because none of those three modes shares any
functional unit with `q8_scale_dual_radix4`.

## 4. What must NOT change

- `q8_scale_dual_radix4`'s own internal FSM, handshake, and rendezvous
  logic (untouched, reused verbatim in B2 -- same divider, same
  bit-exactness, same 4,052,224-case Phase B1 differential result still
  applies unchanged).
- The external port list, mode encoding, and 512-bit data format
  (`docs/phase5-synthesizable-fpga.md`'s CPU/FPGA partition contract).
- Strict global in-order retirement (section 0).
- No silent drop, duplicate, or reorder of any accepted transaction.

## 5. What Phase B2 targets

Replace the blanket `!q8enc_inflight && !q4enc_inflight` gate (section 1)
on Q8_0 decode / Q4_0 decode / Q4_0 encode issuance with a **resource-based**
gate: each engine may issue independently of the others' busy state
(since none share hardware), and a small, bounded (see `phase-b2.md`
section "Architecture") sequence-number-based hold mechanism defers only
the *retirement* (not the *execution*) of a younger, already-completed
result until an older, still-outstanding transaction retires first --
turning most of section 3's "avoidable collateral stall" into ordinary
pipelined execution, at the cost of a new, small, bounded "output ordering
wait" category that only appears when a younger transaction's own
computation finishes strictly before an older Q8_0/Q4_0 encode's does.
