#!/usr/bin/env python3
"""Parse Phase B3's 6-way Verilator cosim logs into results/b3-*.

Reads $BUILD_DIR/b3-<variant>.log (baseline, b1, b2, b3l2, b3l4, b3split),
each produced by tb_top_verilator_q8_b3_variant.cpp's own stdout (PROFILE
lines, a "=== correctness summary ===" block, and a final PASS/FAIL line).
Writes results/b3-correctness.json, results/b3-performance.csv, and
results/b3-candidate-comparison.md. All numbers here are SIMULATED
(Verilator cosim against the golden C reference), never real FPGA timing.
"""
import json
import re
import sys
from pathlib import Path

VARIANTS = [
	("baseline", "membrane_quant_stream_top (baseline)"),
	("b1", "Phase B1 (full serialization)"),
	("b2", "Phase B2 (scheduler-improved)"),
	("b3l2", "Phase B3 (lookahead=2)"),
	("b3l4", "Phase B3 (lookahead=4)"),
	("b3split", "Phase B3 (split queues)"),
]

PROFILE_RE = re.compile(
	r"^PROFILE (\S+) total_cycles=(\d+) total_txn=(\d+) cycles_per_txn=([\d.]+) accepted_per_cycle=([\d.]+)")
MODE_RE = re.compile(
	r"^\s+MODE (\S+)\s+count=(\d+)\s+min=(\d+)\s+mean=([\d.]+)\s+p50=(\d+)\s+p95=(\d+)\s+p99=(\d+)\s+max=(\d+)\s+throughput_txn_per_cycle=([\d.]+)")
SUMMARY_RE = re.compile(
	r"^total cycles this run: (\d+), total transactions checked: (\d+), overall cycles/transaction: ([\d.]+)")
PASS_RE = re.compile(
	r"^(PASS|FAIL).*?(\d+) transactions, (\d+) fails, ([\d.]+)s")


def parse_log(path):
	profiles = {}
	cur_profile = None
	summary = None
	pass_line = None
	with open(path) as f:
		for line in f:
			m = PROFILE_RE.match(line)
			if m:
				cur_profile = m.group(1)
				profiles[cur_profile] = {
					"total_cycles": int(m.group(2)),
					"total_txn": int(m.group(3)),
					"cycles_per_txn": float(m.group(4)),
					"accepted_per_cycle": float(m.group(5)),
					"modes": {},
				}
				continue
			m = MODE_RE.match(line)
			if m and cur_profile:
				profiles[cur_profile]["modes"][m.group(1)] = {
					"count": int(m.group(2)), "min": int(m.group(3)),
					"mean": float(m.group(4)), "p50": int(m.group(5)),
					"p95": int(m.group(6)), "p99": int(m.group(7)),
					"max": int(m.group(8)), "throughput_txn_per_cycle": float(m.group(9)),
				}
				continue
			m = SUMMARY_RE.match(line)
			if m:
				summary = {
					"total_cycles": int(m.group(1)),
					"total_transactions": int(m.group(2)),
					"overall_cycles_per_txn": float(m.group(3)),
				}
				continue
			m = PASS_RE.match(line)
			if m:
				pass_line = {
					"result": m.group(1), "transactions": int(m.group(2)),
					"fails": int(m.group(3)), "seconds": float(m.group(4)),
				}
	return profiles, summary, pass_line


def main():
	if len(sys.argv) != 3:
		print("usage: gen-b3-artifacts.py <build_dir> <results_dir>", file=sys.stderr)
		sys.exit(1)
	build_dir = Path(sys.argv[1])
	results_dir = Path(sys.argv[2])
	results_dir.mkdir(parents=True, exist_ok=True)

	data = {}
	for key, label in VARIANTS:
		log = build_dir / f"b3-{key}.log"
		if not log.exists():
			print(f"error: missing log {log}", file=sys.stderr)
			sys.exit(1)
		profiles, summary, pass_line = parse_log(log)
		data[key] = {"label": label, "profiles": profiles, "summary": summary, "pass": pass_line}

	# results/b3-correctness.json
	correctness = {}
	fail_count = 0
	for key, label in VARIANTS:
		d = data[key]
		ok = bool(d["pass"]) and d["pass"]["result"] == "PASS" and d["pass"]["fails"] == 0
		if not ok:
			fail_count += 1
		correctness[key] = {
			"label": label,
			"classification": "MEASURED_BY_TOOL",
			"transactions_checked": d["summary"]["total_transactions"] if d["summary"] else None,
			"fails": d["pass"]["fails"] if d["pass"] else None,
			"result": d["pass"]["result"] if d["pass"] else "MISSING",
			"overall_cycles_per_txn": d["summary"]["overall_cycles_per_txn"] if d["summary"] else None,
		}
	correctness["_meta"] = {
		"tool": "Verilator 5.x cosim, tb_top_verilator_q8_b3_variant.cpp",
		"classification": "SIMULATED (Verilator cosim against golden C reference); not a real FPGA measurement",
		"total_variants_failed": fail_count,
	}
	(results_dir / "b3-correctness.json").write_text(json.dumps(correctness, indent=2) + "\n")

	# results/b3-performance.csv
	all_profiles = []
	for key, _ in VARIANTS:
		all_profiles.extend(data[key]["profiles"].keys())
	seen = []
	for p in all_profiles:
		if p not in seen:
			seen.append(p)

	rows = ["variant,profile,mode,count,cycles_per_txn,accepted_per_cycle,min,mean,p50,p95,p99,max,throughput_txn_per_cycle"]
	for key, _ in VARIANTS:
		for profile in seen:
			pdata = data[key]["profiles"].get(profile)
			if not pdata:
				continue
			for mode, m in pdata["modes"].items():
				rows.append(
					f"{key},{profile},{mode},{m['count']},{pdata['cycles_per_txn']:.4f},"
					f"{pdata['accepted_per_cycle']:.6f},{m['min']},{m['mean']:.3f},{m['p50']},"
					f"{m['p95']},{m['p99']},{m['max']},{m['throughput_txn_per_cycle']:.6f}")
	(results_dir / "b3-performance.csv").write_text("\n".join(rows) + "\n")

	# results/b3-candidate-comparison.md
	def cyc(key, profile, mode=None):
		p = data[key]["profiles"].get(profile)
		if not p:
			return None
		if mode is None:
			return p["cycles_per_txn"]
		m = p["modes"].get(mode)
		return m["mean"] if m else None

	lines = []
	lines.append("# Phase B3 candidate comparison (SIMULATED -- Verilator cosim, not real FPGA timing)\n")
	lines.append("All cycles/transaction figures below are classified MEASURED_BY_TOOL: real Verilator")
	lines.append("cycle counts from tb_top_verilator_q8_b3_variant.cpp, cosimulated against the golden C")
	lines.append("reference. No real Fmax, timing closure, or power figure is implied.\n")

	lines.append("## Overall cycles/transaction (full correctness run, all modes+stages combined)\n")
	lines.append("| variant | overall cycles/txn | transactions | fails |")
	lines.append("|---|---|---|---|")
	for key, label in VARIANTS:
		d = data[key]
		cpt = d["summary"]["overall_cycles_per_txn"] if d["summary"] else None
		txn = d["summary"]["total_transactions"] if d["summary"] else None
		fails = d["pass"]["fails"] if d["pass"] else None
		lines.append(f"| {key} ({label}) | {cpt} | {txn} | {fails} |")
	lines.append("")

	lines.append("## Density-sweep profiles: cycles/transaction (lower is better)\n")
	density_profiles = [p for p in seen if "pct_Q8ENC" in p or p == "realistic_mix" or p == "uniform_random"]
	lines.append("| profile | baseline | b1 | b2 | b3_l2 | b3_l4 | b3_split |")
	lines.append("|---|---|---|---|---|---|---|")
	for p in density_profiles:
		vals = [cyc(k, p) for k, _ in VARIANTS]
		vals_s = [f"{v:.3f}" if v is not None else "n/a" for v in vals]
		lines.append(f"| {p} | " + " | ".join(vals_s) + " |")
	lines.append("")

	lines.append("## Adversarial HOL pattern: cycles/transaction (lower is better)\n")
	lines.append("| variant | cycles/txn | vs B2 |")
	lines.append("|---|---|---|")
	b2_adv = cyc("b2", "adversarial_HOL_pattern")
	for key, label in VARIANTS:
		v = cyc(key, "adversarial_HOL_pattern")
		if v is None:
			continue
		delta = f"{(1 - v / b2_adv) * 100:+.1f}%" if b2_adv else "n/a"
		lines.append(f"| {key} ({label}) | {v:.3f} | {delta if key != 'b2' else '(reference)'} |")
	lines.append("")

	lines.append("## Pure-stream profiles: per-mode mean latency (cycles)\n")
	pure_map = {
		"100pct_Q8_DEC": "Q8_DEC", "100pct_Q4_ENC": "Q4_ENC", "100pct_Q4_DEC": "Q4_DEC",
	}
	for profile, mode in pure_map.items():
		if profile not in seen:
			continue
		lines.append(f"### {profile} ({mode})\n")
		lines.append("| variant | mean latency | vs B2 |")
		lines.append("|---|---|---|")
		b2_v = cyc("b2", profile, mode)
		for key, label in VARIANTS:
			v = cyc(key, profile, mode)
			if v is None:
				continue
			delta = f"{(1 - v / b2_v) * 100:+.1f}%" if b2_v else "n/a"
			lines.append(f"| {key} ({label}) | {v:.3f} | {delta if key != 'b2' else '(reference)'} |")
		lines.append("")

	(results_dir / "b3-candidate-comparison.md").write_text("\n".join(lines) + "\n")
	print(f"wrote {results_dir / 'b3-correctness.json'}")
	print(f"wrote {results_dir / 'b3-performance.csv'}")
	print(f"wrote {results_dir / 'b3-candidate-comparison.md'}")


if __name__ == "__main__":
	main()
