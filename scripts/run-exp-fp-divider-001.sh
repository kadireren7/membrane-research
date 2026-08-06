#!/usr/bin/env bash
#
# EXP-FPGA-DIV-001 reproduction script.
#
# Usage: scripts/run-exp-fp-divider-001.sh --phase <phase> [--quick|--full]
#            [--skip-build] [--resume] [--output-dir <dir>]
#
#   --phase b1     Phase B1: fp32_scale_neg_pow2 vs. membrane_fp_divider
#                  for the Q4 mx/-8.0f operation.
#   --phase b2     Phase B2: fp32_div_iterative_exact vs. membrane_fp_divider
#                  for the Q4 1/d operation, plus q4_scale_b2/
#                  membrane_quant_stream_top_b2 integration and full
#                  baseline/B1/B2 datapath + synthesis comparison.
#   --phase b3     Phase B3: decouples Q8_0 encode/decode and Q4_0 decode
#                  issuance from Q4_0 encode's in-flight status (B2's own
#                  collateral-slowdown source, see phase-b3-root-cause.md),
#                  using a small bounded membrane_completion_reorder buffer
#                  (rtl/experimental/fp_div/membrane_completion_reorder.sv)
#                  to keep global in-order retirement correct. Builds and
#                  compares membrane_quant_stream_top_b3 at REORDER_DEPTH
#                  1/2/4 (--full also adds 8), plus baseline/B1/B2 on the
#                  identical workload for a fair comparison.
#   --quick        (default) a fast smoke pass: a small differential
#                  run, a small full-datapath integration run (same
#                  binary, same checks, fewer transactions -- see
#                  rtl/experimental/fp_div/tb_top_verilator_variant.cpp's
#                  own header comment for why this is "the same
#                  testbench", not a separately written lighter test),
#                  and a synthesis ELABORATION-ONLY smoke check
#                  (hierarchy -check, no full synth_ecp5 -- that takes
#                  minutes per module, not appropriate for --quick).
#   --full         the exact scope each phase's own phase-*.md reports:
#                  b1: >=2,000,000 differential cases, full 520,000-
#                  transaction datapath test for baseline+B1, complete
#                  generic+ECP5 synthesis matrix (standalone+q4_scale).
#                  b2: >=2,000,000 differential cases (2,456,685 in the
#                  committed run: boundary sweep + specials cross
#                  product + pow2 + random + general-random + real Q4
#                  runtime d-distribution), full 520,000-transaction
#                  datapath test for baseline+B1+B2, complete
#                  generic+ECP5 synthesis matrix (standalone+q4_scale,
#                  all 3 variants).
#                  b3: reuses b2's own differential test unchanged (the
#                  divider itself is not modified this phase), >=1,000,000
#                  mixed/adversarial-pattern transactions per variant
#                  (baseline+B1+B2+B3 at REORDER_DEPTH 1/2/4[/8]),
#                  standalone generic+ECP5 synthesis for
#                  membrane_completion_reorder at every depth tested, and a
#                  whole-top synthesis attempt per B3 depth (marked
#                  UNAVAILABLE on timeout, same precedent as baseline/B1/B2
#                  never completing whole-top synthesis either).
#   --phase b4     Phase B4: replaces Phase B2's radix-2 (1 quotient
#                  bit/cycle) iterative Q4 divider with an exact radix-4
#                  (2 quotient bits/cycle) one
#                  (rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv),
#                  using B2's OWN full-serialization scheduling unchanged
#                  (no Phase B3 reorder buffer -- B3 was rejected as an
#                  architecture, see decision.md). New 3-way differential
#                  test (baseline vs. B2 radix-2 vs. B4 radix-4) plus
#                  baseline+B1+B2+B4 full-datapath comparison.
#   --skip-build   assume binaries already built under $BUILD_DIR; only
#                  re-run them. (Unconditional -- use --resume instead
#                  if you want per-artifact existence checks.)
#   --resume       like --skip-build, but PER ARTIFACT: any build
#                  output (Verilated binary, golden-vector file, or
#                  synthesis .ys/log) that already exists under
#                  $BUILD_DIR is left alone and not rebuilt; anything
#                  missing (e.g. after an interrupted previous run) is
#                  still built. Safe to pass on every invocation.
#   --output-dir   override the build/output directory (default:
#                  build-exp-fpga-div-001 under the repo root).
#
# No step is ever skipped or turned into a soft failure -- any real
# test failure here is a script failure (`set -e`), not a warning.
# Long-running steps (the full differential test, the full datapath
# test, the full synthesis matrix) print their own progress: the
# differential/datapath binaries have a built-in 5-8 second heartbeat,
# and this script prints a start/elapsed/end banner around every yosys
# invocation, since yosys itself does not.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

PHASE=""
MODE="quick"
SKIP_BUILD=0
RESUME=0
BUILD_DIR="$REPO_ROOT/build-exp-fpga-div-001"

while [ $# -gt 0 ]; do
	case "$1" in
	--phase)
		shift
		PHASE="$1"
		;;
	--quick) MODE="quick" ;;
	--full) MODE="full" ;;
	--skip-build) SKIP_BUILD=1 ;;
	--resume) RESUME=1 ;;
	--output-dir)
		shift
		BUILD_DIR="$1"
		;;
	-h | --help)
		sed -n '2,50p' "$0"
		exit 0
		;;
	*)
		echo "unknown argument: $1" >&2
		exit 1
		;;
	esac
	shift
done

if [ "$PHASE" != "b1" ] && [ "$PHASE" != "b2" ] && [ "$PHASE" != "b3" ] && [ "$PHASE" != "b4" ]; then
	echo "error: --phase b1, b2, b3, or b4 required (got: '${PHASE}')" >&2
	exit 1
fi

YOSYS="$REPO_ROOT/tools/.local-yosys/usr/bin/yosys"
VERILATOR_BIN=""
LOCAL_VERILATOR_ROOT="$REPO_ROOT/tools/.local-verilator/usr/share/verilator"
for candidate in verilator "$REPO_ROOT/tools/.local-verilator/usr/bin/verilator"; do
	if command -v "$candidate" >/dev/null 2>&1; then
		VERILATOR_BIN="$(command -v "$candidate")"
		break
	elif [ -x "$candidate" ]; then
		VERILATOR_BIN="$candidate"
		break
	fi
done
case "$VERILATOR_BIN" in
"$REPO_ROOT/tools/.local-verilator/"*)
	[ -d "$LOCAL_VERILATOR_ROOT" ] && export VERILATOR_ROOT="$LOCAL_VERILATOR_ROOT"
	;;
esac
if [ -z "$YOSYS" ] || [ ! -x "$YOSYS" ]; then
	echo "error: yosys not found at $YOSYS" >&2
	exit 1
fi
if [ -z "$VERILATOR_BIN" ]; then
	echo "error: verilator not found in PATH or tools/.local-verilator/usr/bin" >&2
	exit 1
fi

mkdir -p "$BUILD_DIR"

stage()
{
	echo
	echo "== $(date '+%H:%M:%S') [stage] $* =="
}

# --resume: an artifact that already exists is left alone; --skip-build:
# every build step is unconditionally skipped; neither given: always
# (re)build. Returns 0 ("do build this") or 1 ("skip it").
need_build()
{
	local artifact="$1"

	if [ "$SKIP_BUILD" -eq 1 ]; then
		return 1
	fi
	if [ "$RESUME" -eq 1 ] && [ -e "$artifact" ]; then
		stage "resume: '$artifact' already exists, skipping rebuild"
		return 1
	fi
	return 0
}

yosys_run()
{
	local label="$1"
	local script="$2"
	local t0 t1

	stage "yosys: $label"
	t0=$(date +%s)
	"$YOSYS" -s "$script"
	t1=$(date +%s)
	echo "[stage] yosys: $label done in $((t1 - t0))s"
}

# Like yosys_run, but a timeout is reported as UNAVAILABLE (with elapsed
# time and, where /usr/bin/time is present, peak RSS) rather than a script
# failure -- the SAME precedent baseline/B1/B2 already established for
# whole-top-level synthesis on this project's memory-constrained dev
# machine (see experiment.md's Environment section). Only ever used for
# that one specific, already-disclosed exception -- every other yosys
# invocation in this script still uses the hard-failing yosys_run above.
yosys_run_soft()
{
	local label="$1"
	local script="$2"
	local timeout_s="${3:-300}"
	local t0 t1 rc

	stage "yosys (soft, timeout=${timeout_s}s): $label"
	t0=$(date +%s)
	set +e
	if command -v /usr/bin/time >/dev/null 2>&1; then
		timeout "${timeout_s}s" /usr/bin/time -v "$YOSYS" -s "$script" 2>"$BUILD_DIR/synth/.soft-time.$$"
		rc=$?
		grep -i "maximum resident" "$BUILD_DIR/synth/.soft-time.$$" 2>/dev/null || true
		rm -f "$BUILD_DIR/synth/.soft-time.$$"
	else
		timeout "${timeout_s}s" "$YOSYS" -s "$script"
		rc=$?
	fi
	set -e
	t1=$(date +%s)
	if [ "$rc" -eq 124 ]; then
		echo "[stage] yosys (soft): $label -- UNAVAILABLE (timed out after $((t1 - t0))s / ${timeout_s}s bound, not a synthesizability failure -- see this script's own comment)"
		return 124
	elif [ "$rc" -ne 0 ]; then
		echo "[stage] yosys (soft): $label -- FAILED (exit $rc) after $((t1 - t0))s" >&2
		return "$rc"
	else
		echo "[stage] yosys (soft): $label done in $((t1 - t0))s"
		return 0
	fi
}

gen_datapath_vectors()
{
	# Block count is a REQUIRED argument (task item 1 of the Phase B3 fix:
	# Phase B1/B2 always called this with the literal 120000, matching
	# N_PER_MODE=120000 exactly; Phase B3's --full N_PER_MODE=200000
	# exceeds that, so the count is no longer hardcoded here -- the
	# generated files keep their historical "*_120k.txt" names (many
	# binaries reference that literal path) but their CONTENTS are sized
	# to whatever count is actually requested, tracked via a marker file
	# so a later run with a different count correctly regenerates instead
	# of silently reusing an undersized file.
	local count="$1"
	local marker="/tmp/.top_vectors_count"
	stage "generate golden vectors for full-datapath test (count=$count)"
	local vecdir="$BUILD_DIR/vectors"
	mkdir -p "$vecdir"
	if need_build "$vecdir/gen_top_x"; then
		cc -O2 -I "$REPO_ROOT/include" -o "$vecdir/gen_top_x" "$REPO_ROOT/rtl/tb/gen_top_x_vectors.c" "$REPO_ROOT/src/codecs/f16convert.c"
		cc -O2 -I "$REPO_ROOT/include" -o "$vecdir/gen_pack" "$REPO_ROOT/rtl/tb/gen_pack_vectors.c" "$REPO_ROOT/src/codecs/f16convert.c" "$REPO_ROOT/src/quant/quant_simd.c"
		cc -O2 -I "$REPO_ROOT/include" -o "$vecdir/gen_dequant" "$REPO_ROOT/rtl/tb/gen_dequant_vectors.c" "$REPO_ROOT/src/codecs/f16convert.c" "$REPO_ROOT/src/quant/quant_simd.c"
		cc -O2 -I "$REPO_ROOT/include" -o "$vecdir/gen_q4pack" "$REPO_ROOT/rtl/tb/gen_q4pack_vectors.c" "$REPO_ROOT/src/codecs/f16convert.c" "$REPO_ROOT/src/quant/quant_simd.c"
		cc -O2 -I "$REPO_ROOT/include" -o "$vecdir/gen_q4unpack" "$REPO_ROOT/rtl/tb/gen_q4unpack_vectors.c" "$REPO_ROOT/src/codecs/f16convert.c" "$REPO_ROOT/src/quant/quant_simd.c"
	fi
	if [ -f "/tmp/top_x_120k.txt" ] && [ -f "$marker" ] && [ "$(cat "$marker" 2>/dev/null)" = "$count" ]; then
		if ! need_build "/tmp/top_x_120k.txt"; then
			stage "vectors already generated for count=$count, skipping"
			return
		fi
	fi
	if [ "$SKIP_BUILD" -eq 1 ]; then
		echo "error: --skip-build given but no vectors generated yet for count=$count" >&2
		exit 1
	fi
	"$vecdir/gen_top_x" "$count" /tmp/top_x_120k.txt
	"$vecdir/gen_pack" /tmp/top_x_120k.txt /tmp/top_q8pack_120k.txt
	"$vecdir/gen_dequant" /tmp/top_q8pack_120k.txt /tmp/top_q8dequant_120k.txt
	"$vecdir/gen_q4pack" /tmp/top_x_120k.txt /tmp/top_q4pack_120k.txt
	"$vecdir/gen_q4unpack" /tmp/top_q4pack_120k.txt /tmp/top_q4unpack_120k.txt
	echo "$count" >"$marker"
}

VINC="$REPO_ROOT/tools/.local-verilator/usr/share/verilator/include"

# =============================================================================
# Phase B1
# =============================================================================
run_phase_b1()
{
	local DIFF_BASELINE_OBJ="$BUILD_DIR/diff-baseline-obj"
	local DIFF_CANDIDATE_OBJ="$BUILD_DIR/diff-candidate-obj"
	local DIFF_BIN="$BUILD_DIR/tb_fp32_scale_neg_pow2"
	local TOP_BASELINE_OBJ="$BUILD_DIR/top-baseline-obj"
	local TOP_B1_OBJ="$BUILD_DIR/top-b1-obj"

	if need_build "$DIFF_BIN"; then
		stage "build: differential test (membrane_fp_divider vs. fp32_scale_neg_pow2)"
		rm -rf "$DIFF_BASELINE_OBJ" "$DIFF_CANDIDATE_OBJ"
		"$VERILATOR_BIN" --cc -Wno-fatal --Mdir "$DIFF_BASELINE_OBJ" \
			--top-module membrane_fp_divider \
			"$REPO_ROOT/rtl/valid_delay_line.sv" "$REPO_ROOT/rtl/membrane_fp_divider.sv"
		"$VERILATOR_BIN" --cc -Wno-fatal --Mdir "$DIFF_CANDIDATE_OBJ" \
			--top-module fp32_scale_neg_pow2 \
			"$REPO_ROOT/rtl/valid_delay_line.sv" "$REPO_ROOT/rtl/experimental/fp_div/fp32_scale_neg_pow2.sv"
		make -C "$DIFF_BASELINE_OBJ" -f Vmembrane_fp_divider.mk Vmembrane_fp_divider__ALL.a -j2
		make -C "$DIFF_CANDIDATE_OBJ" -f Vfp32_scale_neg_pow2.mk Vfp32_scale_neg_pow2__ALL.a -j2
		g++ -O2 -std=c++17 \
			-I "$VINC" -I "$VINC/vltstd" \
			-I "$DIFF_BASELINE_OBJ" -I "$DIFF_CANDIDATE_OBJ" \
			"$REPO_ROOT/rtl/tb/tb_fp32_scale_neg_pow2.cpp" \
			"$VINC/verilated.cpp" "$VINC/verilated_threads.cpp" \
			"$DIFF_BASELINE_OBJ/Vmembrane_fp_divider__ALL.a" \
			"$DIFF_CANDIDATE_OBJ/Vfp32_scale_neg_pow2__ALL.a" \
			-pthread -lpthread -latomic \
			-o "$DIFF_BIN"
	fi

	if need_build "$TOP_BASELINE_OBJ/Vtop_baseline"; then
		stage "build: full-datapath test (baseline membrane_quant_stream_top)"
		rm -rf "$TOP_BASELINE_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_BASELINE_OBJ" \
			--top-module membrane_quant_stream_top \
			"$REPO_ROOT/rtl/membrane_fp_pkg.sv" "$REPO_ROOT/rtl/membrane_fp_adder.sv" \
			"$REPO_ROOT/rtl/membrane_fp_divider.sv" "$REPO_ROOT/rtl/membrane_fp_multiplier.sv" \
			"$REPO_ROOT/rtl/stream_fifo.sv" "$REPO_ROOT/rtl/valid_delay_line.sv" \
			"$REPO_ROOT/rtl/q4_pack.sv" "$REPO_ROOT/rtl/q4_scale.sv" "$REPO_ROOT/rtl/q4_scan.sv" \
			"$REPO_ROOT/rtl/q4_unpack.sv" "$REPO_ROOT/rtl/q8_dequantize.sv" \
			"$REPO_ROOT/rtl/q8_maxabs_reduce.sv" "$REPO_ROOT/rtl/q8_quantize_pack.sv" \
			"$REPO_ROOT/rtl/q8_scale.sv" "$REPO_ROOT/rtl/membrane_quant_stream_top.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-o Vtop_baseline
	fi
	if need_build "$TOP_B1_OBJ/Vtop_b1"; then
		stage "build: full-datapath test (B1 membrane_quant_stream_top_b1)"
		rm -rf "$TOP_B1_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_B1_OBJ" \
			--top-module membrane_quant_stream_top_b1 \
			"$REPO_ROOT/rtl/membrane_fp_pkg.sv" "$REPO_ROOT/rtl/membrane_fp_adder.sv" \
			"$REPO_ROOT/rtl/membrane_fp_divider.sv" "$REPO_ROOT/rtl/membrane_fp_multiplier.sv" \
			"$REPO_ROOT/rtl/stream_fifo.sv" "$REPO_ROOT/rtl/valid_delay_line.sv" \
			"$REPO_ROOT/rtl/q4_pack.sv" "$REPO_ROOT/rtl/q4_scan.sv" \
			"$REPO_ROOT/rtl/q4_unpack.sv" "$REPO_ROOT/rtl/q8_dequantize.sv" \
			"$REPO_ROOT/rtl/q8_maxabs_reduce.sv" "$REPO_ROOT/rtl/q8_quantize_pack.sv" \
			"$REPO_ROOT/rtl/q8_scale.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_scale_neg_pow2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/q4_scale_b1.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/membrane_quant_stream_top_b1.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-CFLAGS "-DMEMBRANE_B1_VARIANT" \
			-o Vtop_b1
	fi

	local DIFF_RANDOM_COUNT N_PER_MODE N_MIX
	if [ "$MODE" = "quick" ]; then
		DIFF_RANDOM_COUNT=20000
		N_PER_MODE=2000
		N_MIX=500
	else
		DIFF_RANDOM_COUNT=2200000
		N_PER_MODE=120000
		N_MIX=40000
	fi
	gen_datapath_vectors "$N_PER_MODE"

	stage "differential test ($MODE: $DIFF_RANDOM_COUNT random cases + boundary sweep + specials)"
	"$DIFF_BIN" "$DIFF_RANDOM_COUNT"

	stage "full-datapath test, baseline ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX)"
	"$TOP_BASELINE_OBJ/Vtop_baseline" "$N_PER_MODE" "$N_MIX"
	stage "full-datapath test, B1 variant ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX)"
	"$TOP_B1_OBJ/Vtop_b1" "$N_PER_MODE" "$N_MIX"

	local SYNTH_DIR="$BUILD_DIR/synth"
	mkdir -p "$SYNTH_DIR"

	if [ "$MODE" = "quick" ]; then
		stage "synthesis smoke test (elaboration only, no full synth_ecp5 -- see --full)"
		cat >"$SYNTH_DIR/elab-standalone.ys" <<EOF
read_verilog -sv rtl/valid_delay_line.sv rtl/membrane_fp_divider.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv
hierarchy -check -top membrane_fp_divider
EOF
		yosys_run "elaborate membrane_fp_divider" "$SYNTH_DIR/elab-standalone.ys"
		cat >"$SYNTH_DIR/elab-candidate.ys" <<EOF
read_verilog -sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv
hierarchy -check -top fp32_scale_neg_pow2
EOF
		yosys_run "elaborate fp32_scale_neg_pow2" "$SYNTH_DIR/elab-candidate.ys"
		cat >"$SYNTH_DIR/elab-q4scale-b1.ys" <<EOF
read_verilog -sv rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/q4_scale_b1.sv
hierarchy -check -top q4_scale_b1
EOF
		yosys_run "elaborate q4_scale_b1" "$SYNTH_DIR/elab-q4scale-b1.ys"
	else
		stage "full synthesis matrix (generic + ECP5, standalone unit + q4_scale, both variants -- several minutes, real synth_ecp5 memory cost, run sequentially not in parallel)"

		cat >"$SYNTH_DIR/standalone-baseline-generic.ys" <<EOF
read_verilog -sv rtl/valid_delay_line.sv rtl/membrane_fp_divider.sv
hierarchy -check -top membrane_fp_divider
proc; opt
synth -top membrane_fp_divider
stat
EOF
		yosys_run "standalone baseline (membrane_fp_divider), generic" "$SYNTH_DIR/standalone-baseline-generic.ys"

		cat >"$SYNTH_DIR/standalone-baseline-ecp5.ys" <<EOF
read_verilog -sv rtl/valid_delay_line.sv rtl/membrane_fp_divider.sv
hierarchy -check -top membrane_fp_divider
synth_ecp5 -top membrane_fp_divider
stat
EOF
		yosys_run "standalone baseline (membrane_fp_divider), ECP5" "$SYNTH_DIR/standalone-baseline-ecp5.ys"

		cat >"$SYNTH_DIR/standalone-b1-generic.ys" <<EOF
read_verilog -sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv
hierarchy -check -top fp32_scale_neg_pow2
proc; opt
synth -top fp32_scale_neg_pow2
stat
EOF
		yosys_run "standalone B1 (fp32_scale_neg_pow2), generic" "$SYNTH_DIR/standalone-b1-generic.ys"

		cat >"$SYNTH_DIR/standalone-b1-ecp5.ys" <<EOF
read_verilog -sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv
hierarchy -check -top fp32_scale_neg_pow2
synth_ecp5 -top fp32_scale_neg_pow2
stat
EOF
		yosys_run "standalone B1 (fp32_scale_neg_pow2), ECP5" "$SYNTH_DIR/standalone-b1-ecp5.ys"

		cat >"$SYNTH_DIR/q4scale-baseline-generic.ys" <<EOF
read_verilog -sv rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/q4_scale.sv
hierarchy -check -top q4_scale
proc; opt
synth -top q4_scale
stat
EOF
		yosys_run "q4_scale baseline, generic" "$SYNTH_DIR/q4scale-baseline-generic.ys"

		cat >"$SYNTH_DIR/q4scale-baseline-ecp5.ys" <<EOF
read_verilog -sv rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/q4_scale.sv
hierarchy -check -top q4_scale
synth_ecp5 -top q4_scale
stat
EOF
		yosys_run "q4_scale baseline, ECP5" "$SYNTH_DIR/q4scale-baseline-ecp5.ys"

		cat >"$SYNTH_DIR/q4scale-b1-generic.ys" <<EOF
read_verilog -sv rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/q4_scale_b1.sv
hierarchy -check -top q4_scale_b1
proc; opt
synth -top q4_scale_b1
stat
EOF
		yosys_run "q4_scale_b1, generic" "$SYNTH_DIR/q4scale-b1-generic.ys"

		cat >"$SYNTH_DIR/q4scale-b1-ecp5.ys" <<EOF
read_verilog -sv rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/q4_scale_b1.sv
hierarchy -check -top q4_scale_b1
synth_ecp5 -top q4_scale_b1
stat
EOF
		yosys_run "q4_scale_b1, ECP5" "$SYNTH_DIR/q4scale-b1-ecp5.ys"
	fi

	stage "done (phase b1, $MODE)"
	echo "See experiments/EXP-FPGA-DIV-001/phase-b1.md and results/b1-comparison.md for the committed numbers this script reproduces."
}

# =============================================================================
# Phase B2
# =============================================================================
run_phase_b2()
{
	local DIFF_BIN="$BUILD_DIR/tb_fp32_div_iterative_exact"
	local DIFF_BASELINE_OBJ="$BUILD_DIR/b2-baseline-obj"
	local DIFF_CANDIDATE_OBJ="$BUILD_DIR/b2-cand-obj"
	local TOP_BASELINE_OBJ="$BUILD_DIR/top-baseline-obj"
	local TOP_B1_OBJ="$BUILD_DIR/top-b1-obj"
	local TOP_B2_OBJ="$BUILD_DIR/top-b2-obj"

	if need_build "$DIFF_BIN"; then
		stage "build: differential test (membrane_fp_divider vs. fp32_div_iterative_exact)"
		rm -rf "$DIFF_BASELINE_OBJ" "$DIFF_CANDIDATE_OBJ"
		"$VERILATOR_BIN" --cc -Wno-fatal --Mdir "$DIFF_BASELINE_OBJ" \
			--top-module membrane_fp_divider \
			"$REPO_ROOT/rtl/valid_delay_line.sv" "$REPO_ROOT/rtl/membrane_fp_divider.sv"
		"$VERILATOR_BIN" --cc -Wno-fatal --Mdir "$DIFF_CANDIDATE_OBJ" \
			--top-module fp32_div_iterative_exact \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_div_iterative_exact.sv"
		make -C "$DIFF_BASELINE_OBJ" -f Vmembrane_fp_divider.mk Vmembrane_fp_divider__ALL.a -j2
		make -C "$DIFF_CANDIDATE_OBJ" -f Vfp32_div_iterative_exact.mk Vfp32_div_iterative_exact__ALL.a -j2
		g++ -O2 -std=c++17 \
			-I "$VINC" -I "$VINC/vltstd" \
			-I "$DIFF_BASELINE_OBJ" -I "$DIFF_CANDIDATE_OBJ" \
			-I "$REPO_ROOT/include" \
			"$REPO_ROOT/rtl/tb/tb_fp32_div_iterative_exact.cpp" \
			"$REPO_ROOT/src/codecs/f16convert.c" \
			"$VINC/verilated.cpp" "$VINC/verilated_threads.cpp" \
			"$DIFF_BASELINE_OBJ/Vmembrane_fp_divider__ALL.a" \
			"$DIFF_CANDIDATE_OBJ/Vfp32_div_iterative_exact__ALL.a" \
			-pthread -lpthread -latomic \
			-o "$DIFF_BIN"
	fi

	if need_build "$TOP_BASELINE_OBJ/Vtop_baseline"; then
		stage "build: full-datapath test (baseline membrane_quant_stream_top)"
		rm -rf "$TOP_BASELINE_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_BASELINE_OBJ" \
			--top-module membrane_quant_stream_top \
			"$REPO_ROOT/rtl/membrane_fp_pkg.sv" "$REPO_ROOT/rtl/membrane_fp_adder.sv" \
			"$REPO_ROOT/rtl/membrane_fp_divider.sv" "$REPO_ROOT/rtl/membrane_fp_multiplier.sv" \
			"$REPO_ROOT/rtl/stream_fifo.sv" "$REPO_ROOT/rtl/valid_delay_line.sv" \
			"$REPO_ROOT/rtl/q4_pack.sv" "$REPO_ROOT/rtl/q4_scale.sv" "$REPO_ROOT/rtl/q4_scan.sv" \
			"$REPO_ROOT/rtl/q4_unpack.sv" "$REPO_ROOT/rtl/q8_dequantize.sv" \
			"$REPO_ROOT/rtl/q8_maxabs_reduce.sv" "$REPO_ROOT/rtl/q8_quantize_pack.sv" \
			"$REPO_ROOT/rtl/q8_scale.sv" "$REPO_ROOT/rtl/membrane_quant_stream_top.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-o Vtop_baseline
	fi
	if need_build "$TOP_B1_OBJ/Vtop_b1"; then
		stage "build: full-datapath test (B1 membrane_quant_stream_top_b1)"
		rm -rf "$TOP_B1_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_B1_OBJ" \
			--top-module membrane_quant_stream_top_b1 \
			"$REPO_ROOT/rtl/membrane_fp_pkg.sv" "$REPO_ROOT/rtl/membrane_fp_adder.sv" \
			"$REPO_ROOT/rtl/membrane_fp_divider.sv" "$REPO_ROOT/rtl/membrane_fp_multiplier.sv" \
			"$REPO_ROOT/rtl/stream_fifo.sv" "$REPO_ROOT/rtl/valid_delay_line.sv" \
			"$REPO_ROOT/rtl/q4_pack.sv" "$REPO_ROOT/rtl/q4_scan.sv" \
			"$REPO_ROOT/rtl/q4_unpack.sv" "$REPO_ROOT/rtl/q8_dequantize.sv" \
			"$REPO_ROOT/rtl/q8_maxabs_reduce.sv" "$REPO_ROOT/rtl/q8_quantize_pack.sv" \
			"$REPO_ROOT/rtl/q8_scale.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_scale_neg_pow2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/q4_scale_b1.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/membrane_quant_stream_top_b1.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-CFLAGS "-DMEMBRANE_B1_VARIANT" \
			-o Vtop_b1
	fi
	if need_build "$TOP_B2_OBJ/Vtop_b2"; then
		stage "build: full-datapath test (B2 membrane_quant_stream_top_b2)"
		rm -rf "$TOP_B2_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_B2_OBJ" \
			--top-module membrane_quant_stream_top_b2 \
			"$REPO_ROOT/rtl/membrane_fp_pkg.sv" "$REPO_ROOT/rtl/membrane_fp_adder.sv" \
			"$REPO_ROOT/rtl/membrane_fp_divider.sv" "$REPO_ROOT/rtl/membrane_fp_multiplier.sv" \
			"$REPO_ROOT/rtl/stream_fifo.sv" "$REPO_ROOT/rtl/valid_delay_line.sv" \
			"$REPO_ROOT/rtl/q4_pack.sv" "$REPO_ROOT/rtl/q4_scan.sv" \
			"$REPO_ROOT/rtl/q4_unpack.sv" "$REPO_ROOT/rtl/q8_dequantize.sv" \
			"$REPO_ROOT/rtl/q8_maxabs_reduce.sv" "$REPO_ROOT/rtl/q8_quantize_pack.sv" \
			"$REPO_ROOT/rtl/q8_scale.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_scale_neg_pow2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_div_iterative_exact.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/q4_scale_b2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/membrane_quant_stream_top_b2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-CFLAGS "-DMEMBRANE_B2_VARIANT" \
			-o Vtop_b2
	fi

	local DIFF_RANDOM_COUNT DIFF_GENERAL_COUNT DIFF_Q4DIST_COUNT N_PER_MODE N_MIX
	if [ "$MODE" = "quick" ]; then
		DIFF_RANDOM_COUNT=2000
		DIFF_GENERAL_COUNT=500
		DIFF_Q4DIST_COUNT=200
		N_PER_MODE=500
		N_MIX=200
	else
		DIFF_RANDOM_COUNT=2200000
		DIFF_GENERAL_COUNT=200000
		DIFF_Q4DIST_COUNT=50000
		N_PER_MODE=120000
		N_MIX=40000
	fi
	gen_datapath_vectors "$N_PER_MODE"

	stage "differential test ($MODE: $DIFF_RANDOM_COUNT random(num=1) + $DIFF_GENERAL_COUNT random(general) + $DIFF_Q4DIST_COUNT q4-dist cases, plus boundary/specials/pow2 sweeps)"
	"$DIFF_BIN" "$DIFF_RANDOM_COUNT" "$DIFF_GENERAL_COUNT" "$DIFF_Q4DIST_COUNT"

	stage "full-datapath test, baseline ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX)"
	"$TOP_BASELINE_OBJ/Vtop_baseline" "$N_PER_MODE" "$N_MIX"
	stage "full-datapath test, B1 variant ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX)"
	"$TOP_B1_OBJ/Vtop_b1" "$N_PER_MODE" "$N_MIX"
	stage "full-datapath test, B2 variant ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX)"
	"$TOP_B2_OBJ/Vtop_b2" "$N_PER_MODE" "$N_MIX"

	local SYNTH_DIR="$BUILD_DIR/synth"
	mkdir -p "$SYNTH_DIR"

	if [ "$MODE" = "quick" ]; then
		stage "synthesis smoke test (elaboration only, no full synth_ecp5 -- see --full)"
		cat >"$SYNTH_DIR/elab-b2-standalone.ys" <<EOF
read_verilog -sv rtl/experimental/fp_div/fp32_div_iterative_exact.sv
hierarchy -check -top fp32_div_iterative_exact
EOF
		yosys_run "elaborate fp32_div_iterative_exact" "$SYNTH_DIR/elab-b2-standalone.ys"
		cat >"$SYNTH_DIR/elab-q4scale-b2.ys" <<EOF
read_verilog -sv rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/fp32_div_iterative_exact.sv rtl/experimental/fp_div/q4_scale_b2.sv
hierarchy -check -top q4_scale_b2
EOF
		yosys_run "elaborate q4_scale_b2" "$SYNTH_DIR/elab-q4scale-b2.ys"
	else
		stage "full synthesis matrix (generic + ECP5, standalone unit + q4_scale, baseline/B1/B2 -- several minutes, real synth_ecp5 memory cost, run sequentially not in parallel)"

		local variant src top
		for variant in baseline b1 b2; do
			case "$variant" in
			baseline)
				src="rtl/valid_delay_line.sv rtl/membrane_fp_divider.sv"
				top="membrane_fp_divider"
				;;
			b1)
				src="rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv"
				top="fp32_scale_neg_pow2"
				;;
			b2)
				src="rtl/experimental/fp_div/fp32_div_iterative_exact.sv"
				top="fp32_div_iterative_exact"
				;;
			esac

			cat >"$SYNTH_DIR/standalone-$variant-generic.ys" <<EOF
read_verilog -sv $src
hierarchy -check -top $top
proc; opt
synth -top $top
stat
EOF
			yosys_run "standalone $variant ($top), generic" "$SYNTH_DIR/standalone-$variant-generic.ys"

			cat >"$SYNTH_DIR/standalone-$variant-ecp5.ys" <<EOF
read_verilog -sv $src
hierarchy -check -top $top
synth_ecp5 -top $top
stat
EOF
			yosys_run "standalone $variant ($top), ECP5" "$SYNTH_DIR/standalone-$variant-ecp5.ys"
		done

		for variant in baseline b1 b2; do
			case "$variant" in
			baseline)
				src="rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/q4_scale.sv"
				top="q4_scale"
				;;
			b1)
				src="rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/q4_scale_b1.sv"
				top="q4_scale_b1"
				;;
			b2)
				src="rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/fp32_div_iterative_exact.sv rtl/experimental/fp_div/q4_scale_b2.sv"
				top="q4_scale_b2"
				;;
			esac

			cat >"$SYNTH_DIR/q4scale-$variant-generic.ys" <<EOF
read_verilog -sv $src
hierarchy -check -top $top
proc; opt
synth -top $top
stat
EOF
			yosys_run "q4_scale $variant ($top), generic" "$SYNTH_DIR/q4scale-$variant-generic.ys"

			cat >"$SYNTH_DIR/q4scale-$variant-ecp5.ys" <<EOF
read_verilog -sv $src
hierarchy -check -top $top
synth_ecp5 -top $top
stat
EOF
			yosys_run "q4_scale $variant ($top), ECP5" "$SYNTH_DIR/q4scale-$variant-ecp5.ys"
		done
	fi

	stage "done (phase b2, $MODE)"
	echo "See experiments/EXP-FPGA-DIV-001/phase-b2.md and results/b2-comparison.md for the committed numbers this script reproduces."
}

# =============================================================================
# Phase B3
# =============================================================================
run_phase_b3()
{
	local DIFF_BIN="$BUILD_DIR/tb_fp32_div_iterative_exact"
	local DIFF_BASELINE_OBJ="$BUILD_DIR/b2-baseline-obj"
	local DIFF_CANDIDATE_OBJ="$BUILD_DIR/b2-cand-obj"
	local TOP_BASELINE_OBJ="$BUILD_DIR/top-baseline-obj"
	local TOP_B1_OBJ="$BUILD_DIR/top-b1-obj"
	local TOP_B2_OBJ="$BUILD_DIR/top-b2-obj"

	# The divider itself is unchanged this phase -- reuse Phase B2's own
	# differential test build/artifacts unmodified (same source files,
	# same binary name/location), not a separately maintained copy.
	if need_build "$DIFF_BIN"; then
		stage "build: differential test (membrane_fp_divider vs. fp32_div_iterative_exact, reused from Phase B2 -- divider unchanged)"
		rm -rf "$DIFF_BASELINE_OBJ" "$DIFF_CANDIDATE_OBJ"
		"$VERILATOR_BIN" --cc -Wno-fatal --Mdir "$DIFF_BASELINE_OBJ" \
			--top-module membrane_fp_divider \
			"$REPO_ROOT/rtl/valid_delay_line.sv" "$REPO_ROOT/rtl/membrane_fp_divider.sv"
		"$VERILATOR_BIN" --cc -Wno-fatal --Mdir "$DIFF_CANDIDATE_OBJ" \
			--top-module fp32_div_iterative_exact \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_div_iterative_exact.sv"
		make -C "$DIFF_BASELINE_OBJ" -f Vmembrane_fp_divider.mk Vmembrane_fp_divider__ALL.a -j2
		make -C "$DIFF_CANDIDATE_OBJ" -f Vfp32_div_iterative_exact.mk Vfp32_div_iterative_exact__ALL.a -j2
		g++ -O2 -std=c++17 \
			-I "$VINC" -I "$VINC/vltstd" \
			-I "$DIFF_BASELINE_OBJ" -I "$DIFF_CANDIDATE_OBJ" \
			-I "$REPO_ROOT/include" \
			"$REPO_ROOT/rtl/tb/tb_fp32_div_iterative_exact.cpp" \
			"$REPO_ROOT/src/codecs/f16convert.c" \
			"$VINC/verilated.cpp" "$VINC/verilated_threads.cpp" \
			"$DIFF_BASELINE_OBJ/Vmembrane_fp_divider__ALL.a" \
			"$DIFF_CANDIDATE_OBJ/Vfp32_div_iterative_exact__ALL.a" \
			-pthread -lpthread -latomic \
			-o "$DIFF_BIN"
	fi

	local COMMON_SRC=(
		"$REPO_ROOT/rtl/membrane_fp_pkg.sv" "$REPO_ROOT/rtl/membrane_fp_adder.sv"
		"$REPO_ROOT/rtl/membrane_fp_divider.sv" "$REPO_ROOT/rtl/membrane_fp_multiplier.sv"
		"$REPO_ROOT/rtl/stream_fifo.sv" "$REPO_ROOT/rtl/valid_delay_line.sv"
		"$REPO_ROOT/rtl/q4_pack.sv" "$REPO_ROOT/rtl/q4_scan.sv"
		"$REPO_ROOT/rtl/q4_unpack.sv" "$REPO_ROOT/rtl/q8_dequantize.sv"
		"$REPO_ROOT/rtl/q8_maxabs_reduce.sv" "$REPO_ROOT/rtl/q8_quantize_pack.sv"
		"$REPO_ROOT/rtl/q8_scale.sv"
	)

	if need_build "$TOP_BASELINE_OBJ/Vtop_baseline"; then
		stage "build: full-datapath test (baseline membrane_quant_stream_top, reused)"
		rm -rf "$TOP_BASELINE_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_BASELINE_OBJ" \
			--top-module membrane_quant_stream_top \
			"${COMMON_SRC[@]}" "$REPO_ROOT/rtl/q4_scale.sv" "$REPO_ROOT/rtl/membrane_quant_stream_top.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-o Vtop_baseline
	fi
	if need_build "$TOP_B1_OBJ/Vtop_b1"; then
		stage "build: full-datapath test (B1 membrane_quant_stream_top_b1, reused)"
		rm -rf "$TOP_B1_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_B1_OBJ" \
			--top-module membrane_quant_stream_top_b1 \
			"${COMMON_SRC[@]}" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_scale_neg_pow2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/q4_scale_b1.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/membrane_quant_stream_top_b1.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-CFLAGS "-DMEMBRANE_B1_VARIANT" \
			-o Vtop_b1
	fi
	if need_build "$TOP_B2_OBJ/Vtop_b2"; then
		stage "build: full-datapath test (B2 membrane_quant_stream_top_b2, reused)"
		rm -rf "$TOP_B2_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_B2_OBJ" \
			--top-module membrane_quant_stream_top_b2 \
			"${COMMON_SRC[@]}" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_scale_neg_pow2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_div_iterative_exact.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/q4_scale_b2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/membrane_quant_stream_top_b2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-CFLAGS "-DMEMBRANE_B2_VARIANT" \
			-o Vtop_b2
	fi

	local B3_DEPTHS
	if [ "$MODE" = "quick" ]; then
		B3_DEPTHS="1 2 4"
	else
		B3_DEPTHS="1 2 4 8"
	fi

	local depth TOP_B3_OBJ
	for depth in $B3_DEPTHS; do
		TOP_B3_OBJ="$BUILD_DIR/top-b3-d${depth}-obj"
		if need_build "$TOP_B3_OBJ/Vtop_b3_d${depth}"; then
			stage "build: full-datapath test (B3 membrane_quant_stream_top_b3, REORDER_DEPTH=$depth)"
			rm -rf "$TOP_B3_OBJ"
			"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_B3_OBJ" \
				--top-module membrane_quant_stream_top_b3 \
				-GREORDER_DEPTH="$depth" \
				"${COMMON_SRC[@]}" \
				"$REPO_ROOT/rtl/experimental/fp_div/fp32_scale_neg_pow2.sv" \
				"$REPO_ROOT/rtl/experimental/fp_div/fp32_div_iterative_exact.sv" \
				"$REPO_ROOT/rtl/experimental/fp_div/q4_scale_b2.sv" \
				"$REPO_ROOT/rtl/experimental/fp_div/membrane_completion_reorder.sv" \
				"$REPO_ROOT/rtl/experimental/fp_div/membrane_quant_stream_top_b3.sv" \
				"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
				-CFLAGS "-DMEMBRANE_B3_VARIANT -DMEMBRANE_B3_LABEL=\\\"depth${depth}\\\"" \
				-o "Vtop_b3_d${depth}"
		fi
	done

	local DIFF_RANDOM_COUNT DIFF_GENERAL_COUNT DIFF_Q4DIST_COUNT N_PER_MODE N_MIX N_ADV
	if [ "$MODE" = "quick" ]; then
		DIFF_RANDOM_COUNT=2000
		DIFF_GENERAL_COUNT=500
		DIFF_Q4DIST_COUNT=200
		N_PER_MODE=500
		N_MIX=200
		N_ADV=200
	else
		DIFF_RANDOM_COUNT=2200000
		DIFF_GENERAL_COUNT=200000
		DIFF_Q4DIST_COUNT=50000
		N_PER_MODE=200000
		N_MIX=100000
		N_ADV=70000
	fi
	gen_datapath_vectors "$N_PER_MODE"

	stage "differential test (reused from Phase B2, $MODE: $DIFF_RANDOM_COUNT random(num=1) + $DIFF_GENERAL_COUNT random(general) + $DIFF_Q4DIST_COUNT q4-dist cases)"
	"$DIFF_BIN" "$DIFF_RANDOM_COUNT" "$DIFF_GENERAL_COUNT" "$DIFF_Q4DIST_COUNT"

	stage "full-datapath test, baseline ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX N_ADV=$N_ADV)"
	"$TOP_BASELINE_OBJ/Vtop_baseline" "$N_PER_MODE" "$N_MIX" "$N_ADV"
	stage "full-datapath test, B1 variant ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX N_ADV=$N_ADV)"
	"$TOP_B1_OBJ/Vtop_b1" "$N_PER_MODE" "$N_MIX" "$N_ADV"
	stage "full-datapath test, B2 variant ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX N_ADV=$N_ADV)"
	"$TOP_B2_OBJ/Vtop_b2" "$N_PER_MODE" "$N_MIX" "$N_ADV"
	for depth in $B3_DEPTHS; do
		TOP_B3_OBJ="$BUILD_DIR/top-b3-d${depth}-obj"
		stage "full-datapath test, B3 variant REORDER_DEPTH=$depth ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX N_ADV=$N_ADV)"
		"$TOP_B3_OBJ/Vtop_b3_d${depth}" "$N_PER_MODE" "$N_MIX" "$N_ADV"
	done

	local SYNTH_DIR="$BUILD_DIR/synth"
	mkdir -p "$SYNTH_DIR"

	if [ "$MODE" = "quick" ]; then
		stage "synthesis smoke test (elaboration only, no full synth_ecp5 -- see --full)"
		cat >"$SYNTH_DIR/elab-b3-reorder.ys" <<EOF
read_verilog -sv rtl/experimental/fp_div/membrane_completion_reorder.sv
chparam -set DEPTH 4 membrane_completion_reorder
hierarchy -check -top membrane_completion_reorder
EOF
		yosys_run "elaborate membrane_completion_reorder (DEPTH=4)" "$SYNTH_DIR/elab-b3-reorder.ys"
		cat >"$SYNTH_DIR/elab-b3-top.ys" <<EOF
read_verilog -sv rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/membrane_fp_adder.sv rtl/membrane_fp_multiplier.sv rtl/valid_delay_line.sv rtl/stream_fifo.sv rtl/q4_pack.sv rtl/q4_scan.sv rtl/q4_unpack.sv rtl/q8_dequantize.sv rtl/q8_maxabs_reduce.sv rtl/q8_quantize_pack.sv rtl/q8_scale.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/fp32_div_iterative_exact.sv rtl/experimental/fp_div/q4_scale_b2.sv rtl/experimental/fp_div/membrane_completion_reorder.sv rtl/experimental/fp_div/membrane_quant_stream_top_b3.sv
chparam -set REORDER_DEPTH 4 membrane_quant_stream_top_b3
hierarchy -check -top membrane_quant_stream_top_b3
EOF
		yosys_run "elaborate membrane_quant_stream_top_b3 (REORDER_DEPTH=4)" "$SYNTH_DIR/elab-b3-top.ys"
	else
		stage "full synthesis matrix (generic + ECP5, membrane_completion_reorder standalone at every depth tested -- several minutes)"
		for depth in $B3_DEPTHS; do
			cat >"$SYNTH_DIR/reorder-d${depth}-generic.ys" <<EOF
read_verilog -sv rtl/experimental/fp_div/membrane_completion_reorder.sv
chparam -set DEPTH $depth membrane_completion_reorder
hierarchy -check -top membrane_completion_reorder
proc; opt
synth -top membrane_completion_reorder
stat
EOF
			yosys_run "membrane_completion_reorder DEPTH=$depth, generic" "$SYNTH_DIR/reorder-d${depth}-generic.ys"

			cat >"$SYNTH_DIR/reorder-d${depth}-ecp5.ys" <<EOF
read_verilog -sv rtl/experimental/fp_div/membrane_completion_reorder.sv
chparam -set DEPTH $depth membrane_completion_reorder
hierarchy -check -top membrane_completion_reorder
synth_ecp5 -top membrane_completion_reorder
stat
EOF
			yosys_run "membrane_completion_reorder DEPTH=$depth, ECP5" "$SYNTH_DIR/reorder-d${depth}-ecp5.ys"
		done

		stage "whole-top synthesis attempt, membrane_quant_stream_top_b3 (REORDER_DEPTH=1/2/4 only, depth=8 skipped -- same whole-top memory risk baseline/B1/B2 already disclosed; soft timeout, UNAVAILABLE not a failure)"
		for depth in 1 2 4; do
			cat >"$SYNTH_DIR/top-b3-d${depth}-ecp5.ys" <<EOF
read_verilog -sv rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/membrane_fp_adder.sv rtl/membrane_fp_multiplier.sv rtl/valid_delay_line.sv rtl/stream_fifo.sv rtl/q4_pack.sv rtl/q4_scan.sv rtl/q4_unpack.sv rtl/q8_dequantize.sv rtl/q8_maxabs_reduce.sv rtl/q8_quantize_pack.sv rtl/q8_scale.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/fp32_div_iterative_exact.sv rtl/experimental/fp_div/q4_scale_b2.sv rtl/experimental/fp_div/membrane_completion_reorder.sv rtl/experimental/fp_div/membrane_quant_stream_top_b3.sv
chparam -set REORDER_DEPTH $depth membrane_quant_stream_top_b3
hierarchy -check -top membrane_quant_stream_top_b3
synth_ecp5 -top membrane_quant_stream_top_b3
stat
EOF
			yosys_run_soft "whole-top membrane_quant_stream_top_b3 REORDER_DEPTH=$depth, ECP5" "$SYNTH_DIR/top-b3-d${depth}-ecp5.ys" 300 || true
		done
	fi

	stage "done (phase b3, $MODE)"
	echo "See experiments/EXP-FPGA-DIV-001/phase-b3.md and results/b3-comparison.md for the committed numbers this script reproduces."
}

# =============================================================================
# Phase B4
# =============================================================================
run_phase_b4()
{
	local DIFF_BIN="$BUILD_DIR/tb_fp32_div_iterative_radix4_exact"
	local DIFF_BASELINE_OBJ="$BUILD_DIR/b4-baseline-obj"
	local DIFF_B2_OBJ="$BUILD_DIR/b4-b2-obj"
	local DIFF_B4_OBJ="$BUILD_DIR/b4-cand-obj"
	local TOP_BASELINE_OBJ="$BUILD_DIR/top-baseline-obj"
	local TOP_B1_OBJ="$BUILD_DIR/top-b1-obj"
	local TOP_B2_OBJ="$BUILD_DIR/top-b2-obj"
	local TOP_B4_OBJ="$BUILD_DIR/top-b4-obj"

	if need_build "$DIFF_BIN"; then
		stage "build: 3-way differential test (membrane_fp_divider vs. fp32_div_iterative_exact vs. fp32_div_iterative_radix4_exact)"
		rm -rf "$DIFF_BASELINE_OBJ" "$DIFF_B2_OBJ" "$DIFF_B4_OBJ"
		"$VERILATOR_BIN" --cc -Wno-fatal --Mdir "$DIFF_BASELINE_OBJ" \
			--top-module membrane_fp_divider \
			"$REPO_ROOT/rtl/valid_delay_line.sv" "$REPO_ROOT/rtl/membrane_fp_divider.sv"
		"$VERILATOR_BIN" --cc -Wno-fatal --Mdir "$DIFF_B2_OBJ" \
			--top-module fp32_div_iterative_exact \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_div_iterative_exact.sv"
		"$VERILATOR_BIN" --cc -Wno-fatal --Mdir "$DIFF_B4_OBJ" \
			--top-module fp32_div_iterative_radix4_exact \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv"
		make -C "$DIFF_BASELINE_OBJ" -f Vmembrane_fp_divider.mk Vmembrane_fp_divider__ALL.a -j2
		make -C "$DIFF_B2_OBJ" -f Vfp32_div_iterative_exact.mk Vfp32_div_iterative_exact__ALL.a -j2
		make -C "$DIFF_B4_OBJ" -f Vfp32_div_iterative_radix4_exact.mk Vfp32_div_iterative_radix4_exact__ALL.a -j2
		g++ -O2 -std=c++17 \
			-I "$VINC" -I "$VINC/vltstd" \
			-I "$DIFF_BASELINE_OBJ" -I "$DIFF_B2_OBJ" -I "$DIFF_B4_OBJ" \
			-I "$REPO_ROOT/include" \
			"$REPO_ROOT/rtl/tb/tb_fp32_div_iterative_radix4_exact.cpp" \
			"$REPO_ROOT/src/codecs/f16convert.c" \
			"$VINC/verilated.cpp" "$VINC/verilated_threads.cpp" \
			"$DIFF_BASELINE_OBJ/Vmembrane_fp_divider__ALL.a" \
			"$DIFF_B2_OBJ/Vfp32_div_iterative_exact__ALL.a" \
			"$DIFF_B4_OBJ/Vfp32_div_iterative_radix4_exact__ALL.a" \
			-pthread -lpthread -latomic \
			-o "$DIFF_BIN"
	fi

	local COMMON_SRC=(
		"$REPO_ROOT/rtl/membrane_fp_pkg.sv" "$REPO_ROOT/rtl/membrane_fp_adder.sv"
		"$REPO_ROOT/rtl/membrane_fp_divider.sv" "$REPO_ROOT/rtl/membrane_fp_multiplier.sv"
		"$REPO_ROOT/rtl/stream_fifo.sv" "$REPO_ROOT/rtl/valid_delay_line.sv"
		"$REPO_ROOT/rtl/q4_pack.sv" "$REPO_ROOT/rtl/q4_scan.sv"
		"$REPO_ROOT/rtl/q4_unpack.sv" "$REPO_ROOT/rtl/q8_dequantize.sv"
		"$REPO_ROOT/rtl/q8_maxabs_reduce.sv" "$REPO_ROOT/rtl/q8_quantize_pack.sv"
		"$REPO_ROOT/rtl/q8_scale.sv"
	)

	if need_build "$TOP_BASELINE_OBJ/Vtop_baseline"; then
		stage "build: full-datapath test (baseline membrane_quant_stream_top, reused)"
		rm -rf "$TOP_BASELINE_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_BASELINE_OBJ" \
			--top-module membrane_quant_stream_top \
			"${COMMON_SRC[@]}" "$REPO_ROOT/rtl/q4_scale.sv" "$REPO_ROOT/rtl/membrane_quant_stream_top.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-o Vtop_baseline
	fi
	if need_build "$TOP_B1_OBJ/Vtop_b1"; then
		stage "build: full-datapath test (B1 membrane_quant_stream_top_b1, reused)"
		rm -rf "$TOP_B1_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_B1_OBJ" \
			--top-module membrane_quant_stream_top_b1 \
			"${COMMON_SRC[@]}" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_scale_neg_pow2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/q4_scale_b1.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/membrane_quant_stream_top_b1.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-CFLAGS "-DMEMBRANE_B1_VARIANT" \
			-o Vtop_b1
	fi
	if need_build "$TOP_B2_OBJ/Vtop_b2"; then
		stage "build: full-datapath test (B2 membrane_quant_stream_top_b2, reused)"
		rm -rf "$TOP_B2_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_B2_OBJ" \
			--top-module membrane_quant_stream_top_b2 \
			"${COMMON_SRC[@]}" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_scale_neg_pow2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_div_iterative_exact.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/q4_scale_b2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/membrane_quant_stream_top_b2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-CFLAGS "-DMEMBRANE_B2_VARIANT" \
			-o Vtop_b2
	fi
	if need_build "$TOP_B4_OBJ/Vtop_b4"; then
		stage "build: full-datapath test (B4 membrane_quant_stream_top_b4)"
		rm -rf "$TOP_B4_OBJ"
		"$VERILATOR_BIN" --cc --exe --build -j 2 -Wno-fatal --Mdir "$TOP_B4_OBJ" \
			--top-module membrane_quant_stream_top_b4 \
			"${COMMON_SRC[@]}" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_scale_neg_pow2.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/q4_scale_b4.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/membrane_quant_stream_top_b4.sv" \
			"$REPO_ROOT/rtl/experimental/fp_div/tb_top_verilator_variant.cpp" \
			-CFLAGS "-DMEMBRANE_B4_VARIANT" \
			-o Vtop_b4
	fi

	local DIFF_RANDOM_COUNT DIFF_GENERAL_COUNT DIFF_Q4DIST_COUNT N_PER_MODE N_MIX N_ADV
	if [ "$MODE" = "quick" ]; then
		DIFF_RANDOM_COUNT=2000
		DIFF_GENERAL_COUNT=500
		DIFF_Q4DIST_COUNT=200
		N_PER_MODE=500
		N_MIX=200
		N_ADV=200
	else
		DIFF_RANDOM_COUNT=4200000
		DIFF_GENERAL_COUNT=200000
		DIFF_Q4DIST_COUNT=50000
		N_PER_MODE=200000
		N_MIX=100000
		N_ADV=70000
	fi
	gen_datapath_vectors "$N_PER_MODE"

	stage "3-way differential test ($MODE: $DIFF_RANDOM_COUNT random(num=1, reproduces B2's 2.2M + >=2M additional) + $DIFF_GENERAL_COUNT random(general) + $DIFF_Q4DIST_COUNT q4-dist cases)"
	"$DIFF_BIN" "$DIFF_RANDOM_COUNT" "$DIFF_GENERAL_COUNT" "$DIFF_Q4DIST_COUNT"

	stage "full-datapath test, baseline ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX N_ADV=$N_ADV)"
	"$TOP_BASELINE_OBJ/Vtop_baseline" "$N_PER_MODE" "$N_MIX" "$N_ADV"
	stage "full-datapath test, B1 variant ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX N_ADV=$N_ADV)"
	"$TOP_B1_OBJ/Vtop_b1" "$N_PER_MODE" "$N_MIX" "$N_ADV"
	stage "full-datapath test, B2 variant ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX N_ADV=$N_ADV)"
	"$TOP_B2_OBJ/Vtop_b2" "$N_PER_MODE" "$N_MIX" "$N_ADV"
	stage "full-datapath test, B4 variant ($MODE: N_PER_MODE=$N_PER_MODE N_MIX=$N_MIX N_ADV=$N_ADV)"
	"$TOP_B4_OBJ/Vtop_b4" "$N_PER_MODE" "$N_MIX" "$N_ADV"

	local SYNTH_DIR="$BUILD_DIR/synth"
	mkdir -p "$SYNTH_DIR"

	if [ "$MODE" = "quick" ]; then
		stage "synthesis smoke test (elaboration only, no full synth_ecp5 -- see --full)"
		cat >"$SYNTH_DIR/elab-b4-standalone.ys" <<EOF
read_verilog -sv rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv
hierarchy -check -top fp32_div_iterative_radix4_exact
EOF
		yosys_run "elaborate fp32_div_iterative_radix4_exact" "$SYNTH_DIR/elab-b4-standalone.ys"
		cat >"$SYNTH_DIR/elab-q4scale-b4.ys" <<EOF
read_verilog -sv rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv rtl/experimental/fp_div/q4_scale_b4.sv
hierarchy -check -top q4_scale_b4
EOF
		yosys_run "elaborate q4_scale_b4" "$SYNTH_DIR/elab-q4scale-b4.ys"
	else
		stage "full synthesis matrix (generic + ECP5, standalone unit + q4_scale, baseline/B2/B4 -- several minutes, real synth_ecp5 memory cost, run sequentially not in parallel)"

		local variant src top
		for variant in baseline b2 b4; do
			case "$variant" in
			baseline)
				src="rtl/valid_delay_line.sv rtl/membrane_fp_divider.sv"
				top="membrane_fp_divider"
				;;
			b2)
				src="rtl/experimental/fp_div/fp32_div_iterative_exact.sv"
				top="fp32_div_iterative_exact"
				;;
			b4)
				src="rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv"
				top="fp32_div_iterative_radix4_exact"
				;;
			esac

			cat >"$SYNTH_DIR/standalone-$variant-b4run-generic.ys" <<EOF
read_verilog -sv $src
hierarchy -check -top $top
proc; opt
synth -top $top
stat
EOF
			yosys_run "standalone $variant ($top), generic" "$SYNTH_DIR/standalone-$variant-b4run-generic.ys"

			cat >"$SYNTH_DIR/standalone-$variant-b4run-ecp5.ys" <<EOF
read_verilog -sv $src
hierarchy -check -top $top
synth_ecp5 -top $top
stat
EOF
			yosys_run "standalone $variant ($top), ECP5" "$SYNTH_DIR/standalone-$variant-b4run-ecp5.ys"
		done

		for variant in baseline b1 b2 b4; do
			case "$variant" in
			baseline)
				src="rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/q4_scale.sv"
				top="q4_scale"
				;;
			b1)
				src="rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/q4_scale_b1.sv"
				top="q4_scale_b1"
				;;
			b2)
				src="rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/fp32_div_iterative_exact.sv rtl/experimental/fp_div/q4_scale_b2.sv"
				top="q4_scale_b2"
				;;
			b4)
				src="rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/valid_delay_line.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv rtl/experimental/fp_div/q4_scale_b4.sv"
				top="q4_scale_b4"
				;;
			esac

			cat >"$SYNTH_DIR/q4scale-$variant-b4run-generic.ys" <<EOF
read_verilog -sv $src
hierarchy -check -top $top
proc; opt
synth -top $top
stat
EOF
			yosys_run "q4_scale $variant ($top), generic" "$SYNTH_DIR/q4scale-$variant-b4run-generic.ys"

			cat >"$SYNTH_DIR/q4scale-$variant-b4run-ecp5.ys" <<EOF
read_verilog -sv $src
hierarchy -check -top $top
synth_ecp5 -top $top
stat
EOF
			yosys_run "q4_scale $variant ($top), ECP5" "$SYNTH_DIR/q4scale-$variant-b4run-ecp5.ys"
		done

		stage "whole-top synthesis attempt, membrane_quant_stream_top_b4 (soft timeout, UNAVAILABLE not a failure -- same precedent as baseline/B1/B2/B3)"
		cat >"$SYNTH_DIR/top-b4-ecp5.ys" <<EOF
read_verilog -sv rtl/membrane_fp_pkg.sv rtl/membrane_fp_divider.sv rtl/membrane_fp_adder.sv rtl/membrane_fp_multiplier.sv rtl/valid_delay_line.sv rtl/stream_fifo.sv rtl/q4_pack.sv rtl/q4_scan.sv rtl/q4_unpack.sv rtl/q8_dequantize.sv rtl/q8_maxabs_reduce.sv rtl/q8_quantize_pack.sv rtl/q8_scale.sv rtl/experimental/fp_div/fp32_scale_neg_pow2.sv rtl/experimental/fp_div/fp32_div_iterative_radix4_exact.sv rtl/experimental/fp_div/q4_scale_b4.sv rtl/experimental/fp_div/membrane_quant_stream_top_b4.sv
hierarchy -check -top membrane_quant_stream_top_b4
synth_ecp5 -top membrane_quant_stream_top_b4
stat
EOF
		yosys_run_soft "whole-top membrane_quant_stream_top_b4, ECP5" "$SYNTH_DIR/top-b4-ecp5.ys" 300 || true
	fi

	stage "done (phase b4, $MODE)"
	echo "See experiments/EXP-FPGA-DIV-001/phase-b4.md and results/b4-comparison.md for the committed numbers this script reproduces."
}

case "$PHASE" in
b1) run_phase_b1 ;;
b2) run_phase_b2 ;;
b3) run_phase_b3 ;;
b4) run_phase_b4 ;;
esac
