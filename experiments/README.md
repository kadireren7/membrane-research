# Experiments

| Experiment | Question | Status | Decision | Maintained/promoted? | README |
|---|---|---|---|---|---|
| EXP-FPGA-DIV-001 | Can the Q4_0 datapath's general-purpose FP32 divider be replaced by something with meaningfully fewer synthesized cells, bit-exact? | Complete (4 phases) | `PROMOTE_CANDIDATE` → merged | **Yes** — [kadireren7/membrane#2](https://github.com/kadireren7/membrane/pull/2) | [README.md](EXP-FPGA-DIV-001/README.md) |
| EXP-FPGA-DIV-002 | Can the same idea (exact radix-4 division) work for Q8_0's *dual*-divider case, and can the resulting scheduler's collateral cost be bounded? | Complete (5 phases) | `RESEARCH_COMPLETE_NO_PROMOTION` | No — experimental only, nothing merged | [README.md](EXP-FPGA-DIV-002/README.md) |

See `ROADMAP.md` at this repository's root for what happens next
(including why `EXP-FPGA-DIV-002` is closed rather than continued as a
`B5`) and each experiment's own README for full phase-by-phase detail.
