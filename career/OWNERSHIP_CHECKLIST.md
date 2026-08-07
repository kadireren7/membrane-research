# Ownership checklist

Practical tasks Kadir should be able to do himself, without relying on
an agent session's memory of having done them. Each item names the
exact file/command to use. Check an item off only after doing it
yourself, not after reading this document.

- [ ] **Clean clone and build.** `git clone --recurse-submodules
  https://github.com/kadireren7/membrane && cd membrane && cmake -S . -B
  build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest
  --test-dir build --output-on-failure`. Should finish with all tests
  passing, no manual intervention.
- [ ] **Explain the CMake flow out loud.** What does
  `-DMEMBRANE_ENABLE_LLAMA=ON` actually turn on, and why is it off by
  default? Where does the `third_party/llama.cpp` submodule get pulled
  in, and what target actually needs it?
- [ ] **Trace one KV block end to end.** Pick a single KV cache entry
  and follow it from capture (`tools/membrane-kv-capture` in
  `membrane-research`) through quantization (`src/quant/quant_simd.c`)
  to storage — be able to say, without looking, which function does
  what at each step.
- [ ] **Explain one quantization function in your own words.** Open
  `src/quant/quant_simd.c`, pick one Q8_0 or Q4_0 function, and explain
  what it computes and why it matches ggml's own math (not just "it
  passes the parity test").
- [ ] **Explain one ready/valid RTL path.** Open `rtl/stream_fifo.sv` or
  `rtl/membrane_quant_stream_top.sv`, pick one module boundary, and
  explain what has to be true on a given clock edge for a transfer to
  happen.
- [ ] **Reproduce one canonical experiment yourself.** Run
  `experiments/EXP-FPGA-DIV-001/reproduction/README.md` (or
  DIV-002's) end to end in `membrane-research`, `--quick` mode is
  enough — confirm you get a PASS line, not just that the doc says one
  exists. This is also `ROADMAP.md`'s R3 track.
- [ ] **Inspect one Yosys report yourself.** Open
  `experiments/EXP-FPGA-DIV-002/results/canonical/b1-synthesis.csv` and
  a raw `.ys` script (e.g. `q8scale-generic.ys`) — be able to say what
  each column means and what command actually produced the cell count.
- [ ] **Explain ASan/TSan/CodeQL's distinct roles.** What does each one
  actually catch that the others don't? (Memory errors at runtime vs.
  data races at runtime vs. static pattern analysis without running
  anything.) Why does the TSan CI job need the `setarch -R` workaround?
- [ ] **Explain PR protection on `main`.** What does the `main-
  protection` ruleset actually require before a merge is allowed? What
  would happen if you tried to push directly to `main`?
- [ ] **Independently review one small patch.** Pick any recent commit,
  read the diff cold (no prior context from an agent session), and
  write down what you think it does and whether you'd have approved it
  — before checking the commit message.
- [ ] **Make one small manual change and test it.** Not agent-assisted
  — pick something low-risk (a comment, a test case, a doc typo fix),
  edit it by hand, build, run the relevant tests, and confirm they still
  pass.
