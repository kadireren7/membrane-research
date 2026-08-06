#!/usr/bin/env bash
# Phase 5.4: standalone build script for membrane-fpga-runtime, kept
# separate from the main CMake build the same way Phase 5.2/5.3 kept
# their Verilator/yosys tooling out of CMake -- Verilator is a locally
# apt-extracted, non-system tool in this environment (see
# tools/.local-verilator/), and wiring conditional Verilator discovery
# into the main CMakeLists.txt was judged not worth the added
# complexity/breakage risk for users who don't have it, given every
# prior phase's RTL tooling has followed the same standalone-script
# convention. Run from anywhere; paths are resolved relative to this
# script's own location.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RTL_DIR="${REPO_ROOT}/rtl"
BUILD_DIR="${1:-/tmp/membrane-fpga-runtime-build}"

export VERILATOR_ROOT="${REPO_ROOT}/tools/.local-verilator/usr/share/verilator"
export PATH="${REPO_ROOT}/tools/.local-verilator/usr/bin:${PATH}"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

# membrane_core is plain C (implicit void* conversions etc.) --
# Verilator's generated Makefile compiles every extra source it's given
# with g++, which rejects those as errors under C++ rules. Build the
# real membrane_core static library with the project's own CMake/gcc
# flow instead of feeding .c files to Verilator directly, then link
# against it.
CORE_BUILD_DIR="${REPO_ROOT}/build-rel"
echo "[build.sh] building membrane_core (gcc, via CMake)..."
cmake --build "${CORE_BUILD_DIR}" --target membrane_core -j4 >/dev/null

echo "[build.sh] generating Verilator model for membrane_dma_bridge..."
verilator --cc --exe --build -j 4 \
  -Wno-fatal --timing --assert \
  -CFLAGS "-O2 -std=c++17 -I${REPO_ROOT}/include" \
  -LDFLAGS "${CORE_BUILD_DIR}/libmembrane_core.a -lpthread -lm" \
  -Mdir "${BUILD_DIR}" \
  --top-module membrane_dma_bridge \
  "${RTL_DIR}/membrane_fp_pkg.sv" \
  "${RTL_DIR}/valid_delay_line.sv" \
  "${RTL_DIR}/stream_fifo.sv" \
  "${RTL_DIR}/membrane_fp_divider.sv" \
  "${RTL_DIR}/membrane_fp_multiplier.sv" \
  "${RTL_DIR}/membrane_fp_adder.sv" \
  "${RTL_DIR}/q8_maxabs_reduce.sv" \
  "${RTL_DIR}/q8_scale.sv" \
  "${RTL_DIR}/q8_quantize_pack.sv" \
  "${RTL_DIR}/q8_dequantize.sv" \
  "${RTL_DIR}/q4_scan.sv" \
  "${RTL_DIR}/q4_scale.sv" \
  "${RTL_DIR}/q4_pack.sv" \
  "${RTL_DIR}/q4_unpack.sv" \
  "${RTL_DIR}/membrane_quant_stream_top.sv" \
  "${RTL_DIR}/membrane_dma_bridge.sv" \
  "${SCRIPT_DIR}/fpga_emu_device.cpp" \
  "${SCRIPT_DIR}/fpga_runtime.cpp" \
  "${SCRIPT_DIR}/main.cpp"

mv "${BUILD_DIR}/Vmembrane_dma_bridge" "${BUILD_DIR}/membrane-fpga-runtime"
echo "[build.sh] binary at ${BUILD_DIR}/membrane-fpga-runtime"
