#!/usr/bin/env python3
"""Parse Phase B3's yosys synthesis logs into results/b3-synthesis.csv.

Reads $SYNTH_DIR/{ref-q8scale-dual-radix4,top-b3l2,top-b3l4,top-b3split}-
{generic,ecp5,elab}.log (produced by run-exp-q8-divider-002.sh's own
run_synth/run_top_synth), extracts yosys's own "stat" cell-count breakdown,
and writes one CSV row per (target, flow). A missing/UNAVAILABLE ECP5 log
(full top-level attempts can legitimately time out, per phase-b2.md's own
precedent) is recorded as such, never silently dropped or treated as 0.
"""
import csv
import re
import sys
from pathlib import Path

TARGETS = [
	("ref-q8scale-dual-radix4", "q8_scale_dual_radix4 (component reference, unmodified since Phase B1)"),
	("top-b3l2", "membrane_quant_stream_top_q8_dual_radix4_b3_l2 (lookahead=2)"),
	("top-b3l4", "membrane_quant_stream_top_q8_dual_radix4_b3_l4 (lookahead=4)"),
	("top-b3split", "membrane_quant_stream_top_q8_dual_radix4_b3_split (split queues)"),
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
	if "TIMEOUT" in text or not text.strip():
		pass
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
		print("usage: gen-b3-synthesis-csv.py <synth_dir> <out_csv>", file=sys.stderr)
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
				"target": key, "label": label, "flow": "ecp5",
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
