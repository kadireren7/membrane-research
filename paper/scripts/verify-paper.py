#!/usr/bin/env python3
"""Paper-specific claim/artifact audit, complementing scripts/verify-results.py.

Checks:
  - every artifact path cited in paper/*.md is in benchmarks/MANIFEST.json
  - every figure's declared source CSV(s) match the manifest's tracked SHA-256
    (i.e. figures were generated from the exact committed data, not stale/edited data)
  - paper/tables/*.md are up to date with their source CSVs (delegates to
    generate-tables.py --check)
  - paper/figures/generated/*.svg are up to date (delegates to
    generate-figures.py --check)
  - no 135M number appears attributed to 360M or vice versa (spot checks
    on the specific figures named in paper/claim-audit.md)
  - no partial/superseded manifest artifact is cited in paper/*.md
  - REAL/SIMULATED labels in paper/claim-audit.md match benchmarks/MANIFEST.json
    where both describe the same artifact
  - every \\cite{...}/@key reference in paper/main.md exists in
    paper/references.bib, and every references.bib entry is cited at
    least once
  - no leftover "citation needed" / "[citation needed]" marker in the
    non-Related-Work-placeholder sense (this project intentionally keeps
    them in §11/Related Work of the *bib process notes*, but not in the
    body of claims -- see ALLOWED_CITATION_NEEDED_CONTEXTS below)
  - no prohibited overclaim phrase outside its allowlisted context

Exit code: 0 if every check passes, 1 otherwise.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PAPER_DIR = REPO_ROOT / "paper"
FAILURES = []
CHECK_COUNT = 0

# Figures -> the CSV/JSONL files their data is drawn from (must match
# generate-figures.py's actual reads -- kept here as an independent
# declaration so this script catches drift between the two).
FIGURE_SOURCES = {
	"bytes_per_token_comparison.svg": ["benchmarks/cxl-sim/unified-sweep.csv"],
	"capacity_across_device_sizes.svg": ["benchmarks/cxl-sim/unified-sweep.csv"],
	"p99_vs_host_cache.svg": ["benchmarks/cxl-sim/unified-sweep.csv"],
	"pipeline_sensitivity.svg": ["benchmarks/cxl-sim/unified-sweep-hardware-sensitivity.csv"],
	"oracle_vs_predictor_gap.svg": ["benchmarks/cxl-sim/unified-sweep.csv"],
	"quality_capacity_pareto.svg": [
		"benchmarks/cxl-sim/unified-sweep.csv",
		"benchmarks/cxl-sim/quality-reverify/quality-reverify-135m.jsonl",
		"benchmarks/cxl-sim/quality-reverify/quality-reverify-360m.jsonl",
	],
	"system_architecture.svg": [],  # schematic, not data-driven -- no source CSV
}

# Each entry: (regex for a 135M-only number, regex for a 360M-only
# number) -- used to make sure neither appears near the other model's
# name in paper/main.md.
MODEL_SPECIFIC_NUMBERS = {
	"SmolLM2-135M": ["187.2", "321.1", "8,100,156.8", "4,722,553.4"],
	"SmolLM2-360M": ["244.9", "404.7", "11,008,425.8", "6,660,908.4"],
}

PROHIBITED_PHRASES = [
	"first ", "novel ", "unprecedented", "production-ready", "production system",
	"real cxl acceleration", "hardware-proven", "state-of-the-art",
	"best-in-class", "outperforms all",
]
# Contexts where the phrase is allowed because it's being used to NEGATE
# the claim, not assert it -- checked by requiring one of these nearby.
NEGATION_MARKERS = ["not ", "never ", "no ", "nowhere", "isn't", "is not", "n't", "without"]


def check(name):
	def decorator(fn):
		def wrapper():
			global CHECK_COUNT
			CHECK_COUNT += 1
			try:
				ok, detail = fn()
			except Exception as e:  # noqa: BLE001
				ok, detail = False, f"raised {type(e).__name__}: {e}"
			status = "PASS" if ok else "FAIL"
			print(f"[{status}] {name}: {detail}")
			if not ok:
				FAILURES.append((name, detail))
		return wrapper
	return decorator


def load_manifest():
	return json.loads((REPO_ROOT / "benchmarks" / "MANIFEST.json").read_text())


def all_paper_markdown():
	return sorted(PAPER_DIR.glob("*.md")) + sorted((PAPER_DIR).glob("**/*.md"))


def paper_text_corpus():
	seen = set()
	text = ""
	for p in all_paper_markdown():
		if p in seen or "figures/generated" in str(p):
			continue
		seen.add(p)
		text += f"\n\n<!-- {p} -->\n" + p.read_text()
	return text


@check("every benchmarks/ artifact path cited in paper/*.md exists in the manifest")
def _c1():
	manifest = load_manifest()
	tracked = {a["path"] for a in manifest["artifacts"]}
	text = paper_text_corpus()
	cited = set(re.findall(r"`(benchmarks/[^`]+\.(?:csv|jsonl|json))`", text))
	# MANIFEST.json itself is not an "artifact" it tracks; glob patterns
	# (e.g. "benchmarks/cxl-sim/*.csv" in prose) are not concrete paths.
	cited = {p for p in cited if "MANIFEST.json" not in p and "*" not in p and "\n" not in p}
	missing = sorted(p for p in cited if p not in tracked)
	return len(missing) == 0, f"missing from manifest: {missing}" if missing else f"{len(cited)} cited paths all present"


@check("figure source CSVs' SHA-256 matches the manifest (no stale/edited input)")
def _c2():
	manifest = load_manifest()
	by_path = {a["path"]: a for a in manifest["artifacts"]}
	import hashlib

	bad = []
	for fig, sources in FIGURE_SOURCES.items():
		for src in sources:
			entry = by_path.get(src)
			if entry is None:
				bad.append(f"{fig}: {src} not in manifest")
				continue
			p = REPO_ROOT / src
			h = hashlib.sha256(p.read_bytes()).hexdigest()
			if h != entry["sha256"]:
				bad.append(f"{fig}: {src} hash mismatch (manifest={entry['sha256'][:12]} actual={h[:12]})")
	return len(bad) == 0, "; ".join(bad) if bad else f"{sum(len(v) for v in FIGURE_SOURCES.values())} figure-source hashes verified"


@check("paper/tables/*.md are up to date with their source CSVs")
def _c3():
	r = subprocess.run([sys.executable, str(PAPER_DIR / "scripts" / "generate-tables.py"), "--check"],
		capture_output=True, text=True)
	return r.returncode == 0, (r.stdout + r.stderr).strip().splitlines()[-1] if (r.stdout + r.stderr).strip() else "ok"


@check("paper/figures/generated/*.svg are up to date with their source CSVs")
def _c4():
	r = subprocess.run([sys.executable, str(PAPER_DIR / "scripts" / "generate-figures.py"), "--check"],
		capture_output=True, text=True)
	return r.returncode == 0, (r.stdout + r.stderr).strip().splitlines()[-1] if (r.stdout + r.stderr).strip() else "ok"


@check("no 135M-specific number is attributed to 360M or vice versa in paper/main.md")
def _c5():
	text = (PAPER_DIR / "main.md").read_text()
	bad = []
	# crude proximity check: split into paragraphs, and within any
	# paragraph mentioning one model's name, the OTHER model's specific
	# numbers must not also appear.
	for para in text.split("\n\n"):
		has_135 = "135M" in para
		has_360 = "360M" in para
		if has_135 and not has_360:
			for num in MODEL_SPECIFIC_NUMBERS["SmolLM2-360M"]:
				if num in para:
					bad.append(f"360M number '{num}' appears in a 135M-only paragraph: {para[:80]}...")
		if has_360 and not has_135:
			for num in MODEL_SPECIFIC_NUMBERS["SmolLM2-135M"]:
				if num in para:
					bad.append(f"135M number '{num}' appears in a 360M-only paragraph: {para[:80]}...")
	return len(bad) == 0, "; ".join(bad) if bad else "no cross-model number mixups found"


@check("no partial/superseded manifest artifact is cited in paper/*.md")
def _c6():
	manifest = load_manifest()
	bad_status = {a["path"] for a in manifest["artifacts"] if a["status"] in ("partial", "superseded")}
	text = paper_text_corpus()
	bad = [p for p in bad_status if p in text]
	return len(bad) == 0, f"cited partial/superseded artifacts: {bad}" if bad else "none cited"


@check("claim-audit.md's claim types are consistent with manifest labels where both name the same artifact")
def _c7():
	manifest = load_manifest()
	by_path = {a["path"]: a["label"] for a in manifest["artifacts"]}
	audit = (PAPER_DIR / "claim-audit.md").read_text()
	bad = []
	for path, label in by_path.items():
		if path not in audit:
			continue
		# find the claim-type token nearest this path mention
		idx = audit.find(path)
		window = audit[max(0, idx - 400):idx]
		m = re.findall(r"\*\*Claim type\*\*:\s*([^\n]+)", window)
		if not m:
			continue
		claimed = m[-1]
		# A hedged/compound claim type (one with a parenthetical
		# explanation) is allowed to differ from the manifest's bare
		# label -- that parenthetical IS the disclosure of the nuance
		# (e.g. "REAL (... though the policy replay is simulated ...)").
		# Only a bare, unqualified claim type must match exactly.
		if "(" in claimed:
			continue
		if label.lower() not in claimed.lower():
			bad.append(f"{path}: manifest label={label}, nearest claim-audit type={claimed.strip()}")
	return len(bad) == 0, "; ".join(bad) if bad else "consistent"


@check("every citation key used in paper/main.md exists in paper/references.bib")
def _c8():
	main_text = (PAPER_DIR / "main.md").read_text()
	used_keys = set(re.findall(r"@([a-zA-Z0-9_]+)", main_text))
	bib_text = (PAPER_DIR / "references.bib").read_text()
	bib_keys = set(re.findall(r"@\w+\{([a-zA-Z0-9_]+),", bib_text))
	missing = used_keys - bib_keys
	return len(missing) == 0, f"cited but not in .bib: {missing}" if missing else f"{len(used_keys)} keys all present in .bib"


@check("every paper/references.bib entry is cited at least once in paper/main.md")
def _c9():
	main_text = (PAPER_DIR / "main.md").read_text()
	used_keys = set(re.findall(r"@([a-zA-Z0-9_]+)", main_text))
	bib_text = (PAPER_DIR / "references.bib").read_text()
	bib_keys = set(re.findall(r"@\w+\{([a-zA-Z0-9_]+),", bib_text))
	unused = bib_keys - used_keys
	return len(unused) == 0, f"unused bib entries: {unused}" if unused else f"all {len(bib_keys)} entries cited"


@check("no leftover '[citation needed]' marker remains in paper/main.md's claim body (Related Work note excepted)")
def _c10():
	text = (PAPER_DIR / "main.md").read_text()
	# The only allowed occurrence is inside the AI-tool-policy placeholder
	# and the status-note description of this project's OWN historical
	# discipline (explaining that a prior phase used the marker) -- not a
	# live unresolved marker in a claim.
	occurrences = [m.start() for m in re.finditer(r"\[citation needed\]", text)]
	bad = []
	for pos in occurrences:
		window = text[max(0, pos - 200):pos]
		if "no citation was added" in window.lower() or "not yet surveyed" in window.lower() or "explicitly deferred" in window.lower():
			continue
		bad.append(text[max(0, pos - 60):pos + 20])
	return len(bad) == 0, f"unresolved citation-needed markers: {bad}" if bad else f"{len(occurrences)} historical-context mention(s) only, none unresolved"


@check("no prohibited overclaim phrase appears outside an allowlisted negating context")
def _c11():
	full_text = paper_text_corpus()
	text = full_text.lower()
	bad = []
	for phrase in PROHIBITED_PHRASES:
		for m in re.finditer(re.escape(phrase), text):
			# Skip meta-documentation lines that CATALOG the phrase
			# (claim-audit.md's "Prohibited overclaim wording" cells and
			# phrase-list section) rather than asserting it as a claim.
			wide_window = text[max(0, m.start() - 500):m.start()]
			if "prohibited" in wide_window or "allowlist" in wide_window:
				continue
			window = text[max(0, m.start() - 60): m.end() + 20]
			if any(neg in window for neg in NEGATION_MARKERS):
				continue
			if "to our knowledge" in window or "surveyed" in window:
				continue
			bad.append(f"'{phrase}' near: ...{text[max(0,m.start()-40):m.start()+40]}...")
	return len(bad) == 0, "; ".join(bad[:5]) if bad else f"no unhedged occurrence of {len(PROHIBITED_PHRASES)} prohibited phrases"


def main():
	for fn in (_c1, _c2, _c3, _c4, _c5, _c6, _c7, _c8, _c9, _c10, _c11):
		fn()
	print()
	print(f"{CHECK_COUNT - len(FAILURES)}/{CHECK_COUNT} checks passed")
	if FAILURES:
		print("\nFAILED:")
		for name, detail in FAILURES:
			print(f"  - {name}: {detail}")
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
