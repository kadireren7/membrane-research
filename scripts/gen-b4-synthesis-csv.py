#!/usr/bin/env python3
"""Parse Phase B4's isolated-wrapper yosys synthesis logs into
results/b4-synthesis.csv (task item 10).

Reads $SYNTH_DIR/{ref-q8scale-dual-radix4,wrap-b3split,wrap-r1,wrap-r2,
wrap-r3}-{generic,ecp5}.log (produced by run-exp-q8-divider-002.sh's own
run_synth), extracts yosys's own "stat" cell-count breakdown, and writes
one CSV row per (target, flow). A missing wrapper log (the wrapper .sv
file not yet written, or synthesis genuinely unavailable) is recorded as
UNAVAILABLE, never silently dropped or treated as 0 -- see this project's
own established convention from gen-b3-synthesis-csv.py.
"""
import csv
import re
import sys
from pathlib import Path

TARGETS = [
	("ref-q8scale-dual-radix4", "q8_scale_dual_radix4 (component reference, unmodified since Phase B1)"),
	("wrap-b3split", "membrane_quant_stream_top_q8_dual_radix4_b4_b3split_synth_wrapper (B3-split scheduler, isolated)"),
	("wrap-r1", "membrane_quant_stream_top_q8_dual_radix4_b4_r1_synth_wrapper (R1 scheduler, isolated)"),
	("wrap-r2", "membrane_quant_stream_top_q8_dual_radix4_b4_r2_synth_wrapper (R2 scheduler, isolated)"),
	("wrap-r3", "membrane_quant_stream_top_q8_dual_radix4_b4_r3_synth_wrapper (R3 scheduler, isolated)"),
]

CELL_TYPES_OF_INTEREST = [
	"LUT4", "CCU2C", "PFUMX", "L6MUX21", "TRELLIS_FF", "TRELLIS_IO",
	"$_DFF_P_", "$_DFF_PP0_", "$_MUX_", "$_NOT_", "$_AND_", "$_OR_", "$_XOR_",
]

NUM_CELLS_RE = re.compile(r"Number of cells:\s*(\d+)")
CELL_LINE_RE = re.compile(r"^\s*(\S+)\s+(\d+)\s*$")


def parse_stat_log(path):
	if not path.exists():
		return None
	text = path.read_text()
	total = None
	m = NUM_CELLS_RE.search(text)
	if m:
		total = int(m.group(1))
	cells = {}
	in_cell_block = False
	for line in text.splitlines():
		if "Number of cells:" in line:
			in_cell_block = True
			continue
		if in_cell_block:
			m2 = CELL_LINE_RE.match(line)
			if m2:
				cells[m2.group(1)] = int(m2.group(2))
			elif line.strip() == "" or not line.startswith(" "):
				if cells:
					in_cell_block = False
	return {"total_cells": total, "cells": cells}


def main():
	if len(sys.argv) != 3:
		print("usage: gen-b4-synthesis-csv.py <synth_dir> <out_csv>", file=sys.stderr)
		sys.exit(1)
	synth_dir = Path(sys.argv[1])
	out_csv = Path(sys.argv[2])
	out_csv.parent.mkdir(parents=True, exist_ok=True)

	rows = []
	for key, label in TARGETS:
		for flow in ("generic", "ecp5", "elab"):
			log = synth_dir / f"{key}-{flow}.log"
			if not log.exists():
				continue
			parsed = parse_stat_log(log)
			if parsed is None:
				continue
			classification = "MEASURED_BY_TOOL" if parsed["total_cells"] is not None else "UNAVAILABLE"
			row = {
				"target": key, "label": label, "flow": flow,
				"classification": classification,
				"total_cells": parsed["total_cells"] if parsed["total_cells"] is not None else "",
			}
			for ct in CELL_TYPES_OF_INTEREST:
				row[ct] = parsed["cells"].get(ct, "")
			rows.append(row)
		if not any(r["target"] == key for r in rows):
			rows.append({
				"target": key, "label": label, "flow": "generic+ecp5",
				"classification": "UNAVAILABLE",
				"total_cells": "",
				**{ct: "" for ct in CELL_TYPES_OF_INTEREST},
			})

	fieldnames = ["target", "label", "flow", "classification", "total_cells"] + CELL_TYPES_OF_INTEREST
	with open(out_csv, "w", newline="") as f:
		w = csv.DictWriter(f, fieldnames=fieldnames)
		w.writeheader()
		w.writerows(rows)
	print(f"wrote {out_csv} ({len(rows)} rows)")


if __name__ == "__main__":
	main()
