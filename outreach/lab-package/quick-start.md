# Quick start (no hardware required)

Do this before considering any hardware commitment — it takes about a
minute and confirms the software side of this project is real and
working before you invest lab time.

```bash
git clone --recurse-submodules https://github.com/kadireren7/membrane
cd membrane
./scripts/demo.sh --quick
```

This builds the project, runs the bit-exact CPU/ggml quantization
parity test (100,000+ blocks), runs the full 520,000-transaction FPGA
Verilator cosimulation, and runs a small exact-retrieval scenario — all
from small, already-committed fixtures. No model download. Roughly
25–50 seconds on a modest machine, depending on whether the build cache
is warm. Writes `demo-output/demo-results.json` /
`demo-results.md`.

## What to read next, and why

| File | Why it matters to a lab |
|---|---|
| `README.md` | Project overview, what's real vs. simulated vs. assumed, headline results with sources. |
| `paper/main.md` | Full manuscript: methodology, evaluation, negative results, limitations. |
| `paper/related-work-matrix.md` | Honest comparison against 14 real prior-work sources — shows where this project is and isn't ahead. |
| `docs/phase8-hardware-validation-plan.md` | The 3-level plan this lab package supports (FPGA sim/impl, real board, CXL platform). |
| `outreach/hardware-claim-gates.md` | Exactly which hardware claims are currently allowed vs. prohibited — useful for calibrating how much is actually proven today. |
| `hardware/board-targets.md` | Candidate boards, real (yosys-measured) resource-utilization numbers, the known FP32-divider timing-closure risk. |
| `hardware/vendor-wrapper/` | The interface skeleton (AXI4-Stream/AXI-Lite/DMA) a real integration would wire into your board's shell — elaborates cleanly under yosys today, not yet synthesized on a real toolchain. |

## Verifying the claims yourself

```bash
python3 scripts/verify-results.py      # README/doc headline numbers vs. source artifacts
python3 paper/scripts/verify-paper.py  # manuscript claim/citation/label audit
```

Both should report all checks passing. If either doesn't, that's worth
raising before proceeding — it would mean the repository's own
verification tooling caught something.
