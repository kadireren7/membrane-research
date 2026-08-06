# Outreach email templates

Four versions, one per target category (see `outreach/target-selection.md`
for how to pick a specific recipient). Each is roughly 120–180 words,
written in first person as Kadir Eren Altıntaş. **`[bracketed fields]`
must be filled in with real, verified specifics about the actual
recipient before sending — never send with a placeholder still in it.**
No version claims an existing relationship, a prior conversation, or a
result that hasn't happened. None of these have been sent; this file is
template source only.

For a single, general-purpose version instead of these four
category-specific ones, see `outreach/primary-email-template.md`.

---

## 1. University systems/architecture professor

**Subject:** KV-cache memory research — looking for FPGA/CXL access

Dear Professor [Last name],

I'm Kadir Eren Altıntaş, a systems programming student (42 İstanbul)
and the creator of MEMBRANE, an open-source project on LLM KV-cache
memory: mixed-precision tiering verified bit-exact against ggml's
reference quantizer, and an exact (non-approximate) sparse retrieval
design, evaluated at 128K context × 512 concurrency, with an FPGA
datapath cosimulated against the same reference math (520,000
transactions, zero mismatches).

I'm reaching out because [your group's specific published work on
memory systems / KV-cache / near-memory computing] is close to what I'm
trying to validate next: real place-and-route, and ideally a real board
result — everything hardware-adjacent in this project today is
simulation or a synthesis cell-count check.

Would your lab have FPGA toolchain access (Vivado/Quartus + a board)
for a short, scoped experiment — possibly a student project rather than
your own time? Details: `docs/phase8-hardware-validation-plan.md`.

Repository: github.com/kadireren7/membrane

Best,
Kadir

---

## 2. FPGA / reconfigurable computing laboratory

**Subject:** Synthesizable, bit-exact-verified KV-quantization RTL — seeking board access

Hello [Lab name] team,

I'm Kadir Eren Altıntaş, creator of MEMBRANE, an open-source LLM
KV-cache research project. The relevant piece for your group: a fully
synthesizable, purely-integer fixed-point Q8/Q4 quantization datapath,
cosimulated in Verilator against a real CPU reference (520,000
transactions, zero mismatches), confirmed to elaborate under yosys
0.33. Real cell counts exist per module — the FP32 divider is the
dominant cost (~73,600 LUT-class cells, un-pipelined), a disclosed
timing-closure risk I haven't resolved yet. No Fmax, place-and-route,
or board result exists — this environment has no P&R tool or hardware.

Given [your lab's specific FPGA/board infrastructure], would a scoped
place-and-route attempt — and, if timing closes, a loopback-DMA
bring-up — on hardware you already have be of any interest? The RTL is
vendor-IP-free by design (`hardware/README.md`), so it shouldn't
require redistributing anything proprietary.

Repository: github.com/kadireren7/membrane · Plan:
`docs/phase8-hardware-validation-plan.md`

Kadir

---

## 3. CXL / memory-systems research team

**Subject:** Simulated CXL near-memory KV-cache design — checking it against real hardware

Hello [Team/group name],

I'm Kadir Eren Altıntaş, creator of MEMBRANE, an open-source project
modeling a near-memory/CXL appliance for LLM KV-cache memory: a
discrete-event simulator calibrated from real captured attention
traces, evaluated at 128K context × 512 concurrency (462/462 scenarios
complete). Every CXL link-latency/bandwidth figure is a cited,
industry-typical assumption — no real CXL hardware has been used
anywhere in this project, and that's what I'd like to change.

I came across [your team's specific CXL/memory-tiering publication or
platform] and wanted to ask directly: does your team have access to a
CXL Type-3 device or emulation platform that could run a small,
scoped integration test (`docs/phase8-hardware-validation-plan.md`
Level C)? I want to find out whether the simulator's queueing model
resembles real CXL behavior at all — including if the answer is no.

Repository: github.com/kadireren7/membrane · Paper: `paper/main.md`

Kadir

---

## 4. Company research or hardware-prototyping team

**Subject:** Open-source KV-cache/FPGA research project — requesting hardware access

Hello [Team name],

I'm Kadir Eren Altıntaş, creator of MEMBRANE
(github.com/kadireren7/membrane), an open-source project on LLM
KV-cache memory: mixed-precision tiering verified bit-exact against
ggml's reference quantizer, exact sparse retrieval, and an FPGA
quantization datapath cosimulated against the same CPU math (520,000
transactions, zero mismatches). This is a research-access request, not
a sales pitch — every number is sourced and audited
(`paper/claim-audit.md`).

I'm reaching out because [your team's specific relevant hardware
prototyping capability or publication] looks like it could help answer
what this project can't answer alone: does the RTL actually synthesize,
close timing, and run correctly on real silicon? Right now every
hardware number here is a cosimulation, not a board result.

Could your team offer time-boxed or remote access to an FPGA board +
toolchain (or a CXL platform, if relevant)? Plan and criteria:
`docs/phase8-hardware-validation-plan.md`.

Kadir
