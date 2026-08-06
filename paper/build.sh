#!/usr/bin/env bash
#
# paper/build.sh -- build the MEMBRANE manuscript.
#
# Steps, in order:
#   1. Regenerate figures (paper/scripts/generate-figures.py) and tables
#      (paper/scripts/generate-tables.py).
#   2. Verify the bibliography: every \cite{...}/@key used in
#      paper/main.tex / paper/main.md has a corresponding entry (a
#      \bibitem in main.tex's inline thebibliography, and an @-entry in
#      references.bib), and vice versa.
#   3. Run paper/scripts/verify-paper.py.
#   4. If a LaTeX toolchain (pdflatex) is available: compile
#      paper/main.tex to PDF, treating any "Citation ... undefined" or
#      "Reference ... undefined" warning in the log as a build failure,
#      not just a warning.
#   5. If no LaTeX toolchain is available: report this clearly and stop
#      -- this script never fabricates a PDF.
#
# Usage: paper/build.sh
# Exit code: 0 only if every step (including PDF compilation, when a
# LaTeX toolchain is present) succeeded.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAPER_DIR="$SCRIPT_DIR"
REPO_ROOT="$(cd "$PAPER_DIR/.." && pwd)"
cd "$PAPER_DIR"

FAILED=0

echo "== 1/5: regenerate figures and tables =="
if python3 "$PAPER_DIR/scripts/generate-figures.py" && python3 "$PAPER_DIR/scripts/generate-tables.py"; then
	echo "[PASS] figures and tables regenerated"
else
	echo "[FAIL] figure/table generation failed" >&2
	FAILED=1
fi

echo
echo "== 2/5: bibliography verification =="
python3 - <<'PYEOF'
import re
import sys
from pathlib import Path

main_md = Path("main.md").read_text()
main_tex = Path("main.tex").read_text()
bib = Path("references.bib").read_text()

md_keys = set(re.findall(r"@([a-zA-Z0-9_]+)", main_md))
tex_cite_keys = set()
for m in re.finditer(r"\\cite\{([^}]+)\}", main_tex):
	tex_cite_keys.update(k.strip() for k in m.group(1).split(","))
tex_bibitem_keys = set(re.findall(r"\\bibitem\{([a-zA-Z0-9_]+)\}", main_tex))
bib_keys = set(re.findall(r"@\w+\{([a-zA-Z0-9_]+),", bib))

problems = []
missing_bib_for_md = md_keys - bib_keys
if missing_bib_for_md:
	problems.append(f"main.md cites keys missing from references.bib: {missing_bib_for_md}")
missing_bibitem_for_tex_cite = tex_cite_keys - tex_bibitem_keys
if missing_bibitem_for_tex_cite:
	problems.append(f"main.tex \\cite{{}} keys missing a \\bibitem: {missing_bibitem_for_tex_cite}")
missing_bib_for_tex = tex_bibitem_keys - bib_keys
if missing_bib_for_tex:
	problems.append(f"main.tex \\bibitem keys missing from references.bib: {missing_bib_for_tex}")
unused_bib = bib_keys - md_keys - tex_cite_keys
if unused_bib:
	problems.append(f"references.bib entries never cited in main.md or main.tex: {unused_bib}")

if problems:
	print("[FAIL] bibliography verification:")
	for p in problems:
		print(f"  - {p}")
	sys.exit(1)
print(f"[PASS] bibliography verification: {len(bib_keys)} entries, all cited and cross-consistent")
PYEOF
if [ $? -ne 0 ]; then
	FAILED=1
fi

echo
echo "== 3/5: verify-paper.py =="
if python3 "$PAPER_DIR/scripts/verify-paper.py"; then
	echo "[PASS] verify-paper.py"
else
	echo "[FAIL] verify-paper.py reported problems (see above)" >&2
	FAILED=1
fi

echo
echo "== 4/5: LaTeX toolchain check =="
LATEX_BIN=""
for candidate in pdflatex xelatex lualatex; do
	if command -v "$candidate" >/dev/null 2>&1; then
		LATEX_BIN="$candidate"
		break
	fi
done

if [ -z "$LATEX_BIN" ]; then
	echo "[FAIL] no LaTeX toolchain found (checked: pdflatex, xelatex, lualatex)." >&2
	echo "       This is a genuine, disclosed limitation of this build" >&2
	echo "       environment -- paper/main.tex exists and is structurally" >&2
	echo "       valid (balanced \\begin/\\end pairs, balanced braces)," >&2
	echo "       but this script will NOT fabricate a PDF. Install a" >&2
	echo "       LaTeX distribution (e.g. texlive) and re-run this script" >&2
	echo "       to actually produce paper/main.pdf." >&2
	FAILED=1
else
	echo "== 5/5: compiling paper/main.tex with $LATEX_BIN =="
	LOG="$PAPER_DIR/build.log"
	: >"$LOG"
	compile_ok=1
	# Run 3 passes (standard practice for a document with \cite/\ref
	# cross-references and an inline thebibliography, see the header
	# comment): the FIRST pass necessarily reports every \cite/\ref as
	# "undefined" -- that's how LaTeX bootstraps its .aux file, not a
	# real error -- and only resolves them once a later pass reads that
	# .aux back in. The undefined-citation/reference check below MUST
	# only inspect the LAST pass's own output, not the cumulative log
	# across all passes, or it will misreport pass 1's expected,
	# transient warnings as a real failure even when the bibliography
	# is completely correct (a real bug this project's own CI caught).
	LAST_PASS_LOG="$PAPER_DIR/build-last-pass.log"
	for pass in 1 2 3; do
		: >"$LAST_PASS_LOG"
		if ! "$LATEX_BIN" -interaction=nonstopmode -halt-on-error main.tex >"$LAST_PASS_LOG" 2>&1; then
			compile_ok=0
		fi
		cat "$LAST_PASS_LOG" >>"$LOG"
		if [ "$compile_ok" -eq 0 ]; then
			break
		fi
	done
	if [ "$compile_ok" -eq 0 ]; then
		echo "[FAIL] $LATEX_BIN compilation failed, see $LOG" >&2
		FAILED=1
	elif grep -qE "Citation.*undefined|Reference.*undefined" "$LAST_PASS_LOG"; then
		echo "[FAIL] undefined citation/reference warning(s) found in the FINAL pass (see $LOG for the full 3-pass log):" >&2
		grep -E "Citation.*undefined|Reference.*undefined" "$LAST_PASS_LOG" >&2
		FAILED=1
	elif [ ! -f "$PAPER_DIR/main.pdf" ]; then
		echo "[FAIL] $LATEX_BIN reported success but main.pdf was not produced" >&2
		FAILED=1
	else
		echo "[PASS] main.pdf built, no undefined citations/references"
	fi
	rm -f "$LAST_PASS_LOG"
fi

echo
if [ "$FAILED" -eq 1 ]; then
	echo "paper/build.sh: one or more steps failed -- see output above" >&2
	exit 1
fi
echo "paper/build.sh: all steps passed."
exit 0
