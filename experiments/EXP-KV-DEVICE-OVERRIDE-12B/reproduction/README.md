# Reproduction — EXP-KV-DEVICE-OVERRIDE-12B

## What's needed

1. A pristine checkout of `third_party/llama.cpp` pinned at this
   phase's base commit lineage (see `MANIFEST.json` for the exact
   product base SHA this branch diverged from, `d6e6189d0417858bfcf3d51c766eaa4be5fe46e1`).
2. Apply, in order: the pre-existing `kv_type_override` patch, then
   `patches/llama.cpp-membrane-kv-device-override.patch` (this phase's
   patch, reproduced from `patches/phase12b-kv-device-override.patch`'s
   `patches/llama.cpp-membrane-kv-device-override.patch` hunk). Both
   apply cleanly with plain `git apply` (no `--reverse`, no `-C`), i.e.
   an exact context match, not fuzz-matched.
3. Build `tools/membrane-kv-device-override/` (wired into the top-level
   `CMakeLists.txt` by `patches/phase12b-kv-device-override.patch`) —
   requires a Vulkan-capable build (`build-vulkan`) to exercise the
   real-GPU probe; the llama-free `test_split_policy.c` unit tests run
   on any build.
4. Model weights: SmolLM2-135M-Instruct and SmolLM2-360M-Instruct
   (GGUF, Q8_0/Q5_1 quantized) — not included in this repository.
5. Hardware: an NVIDIA GPU with a working Vulkan driver was used
   (GTX 1650, 4096 MiB VRAM) for every VRAM/performance/pressure-test
   number in this phase's results. The llama-free unit tests and the
   default-regression CLI smoke test do not require a GPU.

## What is NOT preserved

- Model weight files themselves.
- The exact host OS/driver/Vulkan runtime versions beyond what is
  mentioned in `results/canonical/summary.json`'s prose (no separate
  environment-lock file was captured this phase).
- A CI harness — this phase's builds and probe runs were executed
  manually on a single shared research host.

## Classification

**PARTIALLY_REPRODUCIBLE** — the patch, source, and test code are
complete and exact; the numeric VRAM/throughput results depend on the
specific GPU (GTX 1650) and are disclosed as scale/hardware-specific in
`README.md`. Rerunning on different hardware would be expected to
reproduce the qualitative findings (independent placement works,
default behavior unchanged) but not the exact MiB/tok-s figures.
