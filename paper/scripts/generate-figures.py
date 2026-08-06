#!/usr/bin/env python3
"""Generate every figure paper/main.md/main.tex references, from real
committed CSV/JSONL artifacts only -- no hand-authored chart data.
Dependency-free (no matplotlib/pandas; see svg_chart.py's docstring for
why). Output: paper/figures/generated/*.svg.

Usage: paper/scripts/generate-figures.py [--check]
--check: regenerate to a temp dir and diff against the committed
figures; exit 1 on any difference (used by paper/build.sh).
"""
import csv
import filecmp
import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import svg_chart as sc  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
BENCH = REPO_ROOT / "benchmarks" / "cxl-sim"
OUT_DIR = REPO_ROOT / "paper" / "figures" / "generated"

HOST_8GIB = "8589934592"
DEV_2TIB = "2199023255552"
DEV_1TIB = "1099511627776"
DEV_512GIB = "549755813888"
MODELS = ["SmolLM2-135M", "SmolLM2-360M"]


def read_rows(path):
	with open(path, newline="") as f:
		return list(csv.DictReader(f))


def fig_system_architecture(out_dir):
	"""Real system diagram (schematic), mirroring docs/architecture.md
	diagram A -- not data-driven (no numbers to fabricate), hand-laid-out
	boxes matching the actual module names in this repository."""
	c = sc.SvgCanvas(width=860, height=520)
	c.text(430, 30, "MEMBRANE end-to-end system", size=17, anchor="middle", weight="bold")

	boxes = [
		(60, 60, 260, 45, "LLM runtime (real)\nllama.cpp inference"),
		(60, 130, 260, 45, "Trace capture (real)\nmembrane-kv-{capture,attn-trace-capture}"),
		(60, 200, 260, 45, "Policy engine\nhot/warm/cold decision"),
		(60, 270, 120, 45, "Hot KV store\nFP16"),
		(200, 270, 120, 45, "Quant (bit-exact)\nQ8/Q4 vs. ggml"),
		(60, 340, 260, 45, "Exact sparse retrieval\npredictor + prefetch + compulsory fetch"),
		(440, 60, 260, 45, "CXL / near-memory appliance\n(simulated, no real hardware)"),
		(440, 130, 260, 45, "Compressed / cold store\n(file-backed backend)"),
		(440, 200, 260, 45, "FPGA datapath\n(Verilator-cosimulated, not on silicon)"),
		(440, 340, 260, 90, "Out-of-core simulation +\nartifact verification\n(checkpoint/resume, SHA-256 manifest)"),
	]
	for x, y, w, h, label in boxes:
		c.rect(x, y, w, h, fill="#EDF2F7", stroke="#4C78A8")
		lines = label.split("\n")
		for i, ln in enumerate(lines):
			c.text(x + w / 2, y + 20 + i * 16, ln, size=11, anchor="middle")

	arrows = [
		(190, 105, 190, 130), (190, 175, 190, 200),
		(190, 245, 120, 270), (190, 245, 260, 270),
		(190, 315, 190, 340),
		(320, 220, 440, 220),
		(320, 152, 440, 152),
		(320, 82, 440, 82),
		(320, 362, 440, 385),
	]
	for x1, y1, x2, y2 in arrows:
		c.line(x1, y1, x2, y2, stroke="#666", width=1.5)
		c.add(f'<polygon points="{x2-4},{y2-4} {x2+4},{y2-4} {x2},{y2+5}" fill="#666"/>')

	c.text(430, 500, "See docs/architecture.md for the full Mermaid diagram set (A-D).",
		size=10, anchor="middle", fill="#666")
	c.save(out_dir / "system_architecture.svg")


def fig_bytes_per_token(out_dir):
	rows = read_rows(BENCH / "unified-sweep.csv")
	baseline_full, baseline_compressed = {}, {}
	for r in rows:
		if r["comparison"] == "full-scan-cxl":
			baseline_full[r["model"]] = float(r["mean_bytes_per_token"])
		elif r["comparison"] == "compressed-full-scan-cxl":
			baseline_compressed[r["model"]] = float(r["mean_bytes_per_token"])

	comparisons = ["exact-no-prefetch", "exact-predictor", "exact-predictor-prefetch",
		"exact-predictor-coalescing", "oracle"]
	groups = ["full-scan\n(analytical)", "compressed\n(analytical)"] + comparisons
	values = {m: [] for m in MODELS}
	for m in MODELS:
		values[m].append(baseline_full[m])
		values[m].append(baseline_compressed[m])
	for comp in comparisons:
		for m in MODELS:
			match = [r for r in rows if r["model"] == m and r["comparison"] == comp
				and r["precision"] == "all-q8" and r["host_cache_total_bytes"] == HOST_8GIB
				and r["device_total_bytes"] == DEV_2TIB]
			values[m].append(float(match[0]["mean_bytes_per_token"]) if match else None)

	sc.grouped_bar_chart(
		out_dir / "bytes_per_token_comparison.svg",
		"Mean bytes/token by comparison (8GiB host / 2TiB device / all-Q8)",
		groups, MODELS, values,
		y_label="mean bytes/token (log scale)", log_scale=True,
		unit="Source: benchmarks/cxl-sim/unified-sweep.csv",
		width=900,
	)


def fig_capacity_across_device_sizes(out_dir):
	rows = read_rows(BENCH / "unified-sweep.csv")
	device_sizes = [DEV_512GIB, DEV_1TIB, DEV_2TIB]
	device_labels = ["512GiB", "1TiB", "2TiB"]
	series = {}
	for m in MODELS:
		for prec in ["fp16", "all-q8"]:
			key = f"{m} ({prec})"
			pts = []
			for dev, label in zip(device_sizes, device_labels):
				match = [r for r in rows if r["model"] == m and r["precision"] == prec
					and r["comparison"] == "exact-predictor"
					and r["host_cache_total_bytes"] == HOST_8GIB and r["device_total_bytes"] == dev]
				if match:
					pts.append((device_labels.index(label) + 1, float(match[0]["cap_effective_capacity_ratio"])))
			series[key] = pts

	sc.line_chart(
		out_dir / "capacity_across_device_sizes.svg",
		"Effective capacity ratio vs. device size (8GiB host, exact-predictor)",
		series, y_label="cap_effective_capacity_ratio", x_label="device size (1=512GiB, 2=1TiB, 3=2TiB)",
		unit="Source: benchmarks/cxl-sim/unified-sweep.csv",
	)


def fig_p99_vs_host_cache(out_dir):
	rows = read_rows(BENCH / "unified-sweep.csv")
	host_sizes = sorted(set(int(r["host_cache_total_bytes"]) for r in rows if r["host_cache_total_bytes"]))
	series = {}
	for m in MODELS:
		pts = []
		for hc in host_sizes:
			match = [r for r in rows if r["model"] == m and r["precision"] == "all-q8"
				and r["comparison"] == "exact-predictor-prefetch"
				and int(r["host_cache_total_bytes"]) == hc and r["device_total_bytes"] == DEV_2TIB]
			if match:
				pts.append((hc / (1024 ** 2), float(match[0]["p99_latency_ns"]) / 1e6))
		series[m] = pts
	sc.line_chart(
		out_dir / "p99_vs_host_cache.svg",
		"p99 latency vs. host-cache size (2TiB device, all-Q8, exact-predictor-prefetch)",
		series, y_label="p99 latency (ms)", x_label="host cache size (MiB, log scale)",
		log_x=True, unit="Source: benchmarks/cxl-sim/unified-sweep.csv",
	)


def fig_pipeline_sensitivity(out_dir):
	rows = read_rows(BENCH / "unified-sweep-hardware-sensitivity.csv")
	pipeline_rows = {int(r["pipelines"]): r for r in rows if r["profile"].startswith("pipelines-")}
	pts = sorted((k, float(v["p99_latency_ns"]) / 1e6) for k, v in pipeline_rows.items())
	sc.line_chart(
		out_dir / "pipeline_sensitivity.svg",
		"p99 latency vs. quant-pipeline count (SmolLM2-135M only, §9 hardware-sensitivity matrix)",
		{"SmolLM2-135M": pts}, y_label="p99 latency (ms, log scale)", x_label="pipeline count",
		log_y=True, unit="Source: benchmarks/cxl-sim/unified-sweep-hardware-sensitivity.csv",
	)


def fig_oracle_vs_predictor_gap(out_dir):
	rows = read_rows(BENCH / "unified-sweep.csv")
	host_sizes = sorted(set(int(r["host_cache_total_bytes"]) for r in rows if r["host_cache_total_bytes"]))
	groups = [f"{h // (1024**2)}MiB" for h in host_sizes]
	values = {}
	for m in MODELS:
		for comp, label in [("oracle", f"{m} oracle"), ("exact-predictor", f"{m} predictor")]:
			pts = []
			for hc in host_sizes:
				match = [r for r in rows if r["model"] == m and r["precision"] == "all-q8"
					and r["comparison"] == comp and int(r["host_cache_total_bytes"]) == hc
					and r["device_total_bytes"] == DEV_2TIB]
				pts.append(float(match[0]["mean_bytes_per_token"]) if match else None)
			values[label] = pts
	sc.grouped_bar_chart(
		out_dir / "oracle_vs_predictor_gap.svg",
		"Oracle vs. real predictor bytes/token (2TiB device, all-Q8)",
		groups, list(values.keys()), values,
		y_label="mean bytes/token", x_label="host cache size",
		unit="Source: benchmarks/cxl-sim/unified-sweep.csv", width=900,
	)


def fig_quality_capacity_pareto(out_dir):
	sweep_rows = read_rows(BENCH / "unified-sweep.csv")
	points = []
	for m, qfile in [("SmolLM2-135M", "quality-reverify-135m.jsonl"),
			("SmolLM2-360M", "quality-reverify-360m.jsonl")]:
		q_by_type = {}
		for line in open(BENCH / "quality-reverify" / qfile):
			d = json.loads(line)
			q_by_type.setdefault(d["type"], []).append(d["top1_pct"]["mean"])

		fp16_cap = [r for r in sweep_rows if r["model"] == m and r["precision"] == "fp16"
			and r["comparison"] == "exact-predictor" and r["host_cache_total_bytes"] == HOST_8GIB
			and r["device_total_bytes"] == DEV_2TIB]
		q8_cap = [r for r in sweep_rows if r["model"] == m and r["precision"] == "all-q8"
			and r["comparison"] == "exact-predictor" and r["host_cache_total_bytes"] == HOST_8GIB
			and r["device_total_bytes"] == DEV_2TIB]
		mixed_cap = [r for r in sweep_rows if r["model"] == m and r["precision"] == "safe-mixed"
			and r["comparison"] == "exact-predictor" and r["host_cache_total_bytes"] == HOST_8GIB
			and r["device_total_bytes"] == DEV_2TIB]

		if fp16_cap:
			points.append((float(fp16_cap[0]["cap_effective_capacity_ratio"]), 100.0, "fp16", m))
		if q8_cap and "q8_0" in q_by_type:
			mean_q8 = sum(q_by_type["q8_0"]) / len(q_by_type["q8_0"])
			points.append((float(q8_cap[0]["cap_effective_capacity_ratio"]), mean_q8, "all-q8", m))
		if mixed_cap and "q4_0" in q_by_type:
			# NOTE (disclosed in the figure + caption): safe-mixed is a
			# tiered Q8/Q4 policy, not pure Q4 -- q4_0's quality number is
			# used here as the closest available "more compressed than
			# Q8" quality data point, an approximation, not an exact
			# per-tier quality measurement of safe-mixed itself.
			mean_q4 = sum(q_by_type["q4_0"]) / len(q_by_type["q4_0"])
			points.append((float(mixed_cap[0]["cap_effective_capacity_ratio"]), mean_q4, "safe-mixed (approx., see caption)", m))

	sc.scatter_chart(
		out_dir / "quality_capacity_pareto.svg",
		"Quality (top-1 match %) vs. capacity ratio, by precision tier",
		points, x_label="cap_effective_capacity_ratio (8GiB host / 2TiB device)",
		y_label="top-1 match rate (%, mean over recall/distractor/longcontext prompts)",
		unit="Sources: unified-sweep.csv + quality-reverify/*.jsonl",
	)


def main():
	check_only = "--check" in sys.argv
	if check_only:
		tmp = Path(tempfile.mkdtemp())
		target = tmp
	else:
		target = OUT_DIR
		target.mkdir(parents=True, exist_ok=True)

	fig_system_architecture(target)
	fig_bytes_per_token(target)
	fig_capacity_across_device_sizes(target)
	fig_p99_vs_host_cache(target)
	fig_pipeline_sensitivity(target)
	fig_oracle_vs_predictor_gap(target)
	fig_quality_capacity_pareto(target)

	if check_only:
		if not OUT_DIR.exists():
			print(f"generate-figures.py --check: {OUT_DIR} does not exist -- run without --check first", file=sys.stderr)
			return 1
		cmp = filecmp.dircmp(OUT_DIR, target)
		diffs = cmp.diff_files + cmp.left_only + cmp.right_only
		shutil.rmtree(tmp)
		if diffs:
			print(f"generate-figures.py --check: figures out of date: {diffs}", file=sys.stderr)
			return 1
		print("generate-figures.py --check: all figures up to date.")
		return 0

	print(f"wrote {len(list(target.glob('*.svg')))} figures to {target}")
	return 0


if __name__ == "__main__":
	sys.exit(main())
