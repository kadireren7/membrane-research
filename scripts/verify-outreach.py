#!/usr/bin/env python3
"""Verify the hardware-outreach package: internal links, lab-package
completeness, hardware-results schema conformance, prohibited-claim/
hype-word scanning across outreach/ and hardware/; (added in the
Phase 7.3 wording-correction pass) authorship/AI-assistance wording and
demo-duration consistency across outreach/, paper/, docs/, and
README.md; and (added in the public-release audit pass) staleness
checks -- no lingering "paper skeleton" phrasing, no stale "Phase 0
through N" range behind the real current phase, workflow badges only
pointing at real workflow files, no claim that a private/companion
repository currently exists, and no claim that paper/main.pdf is
committed/tracked (it never is -- verified against git directly).

This does NOT re-check paper/'s citation/claim-audit machinery (see
paper/scripts/verify-paper.py) or benchmarks/ headline numbers (see
scripts/verify-results.py) -- it is specific to the outreach/hardware
materials and the release-consistency claims layered on top of them.

Exit code: 0 if every check passes, 1 otherwise.
"""
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
OUTREACH_DIR = REPO_ROOT / "outreach"
HARDWARE_DIR = REPO_ROOT / "hardware"
FAILURES = []
CHECK_COUNT = 0

REQUIRED_LAB_PACKAGE_FILES = [
	"README.md", "quick-start.md", "required-hardware.md",
	"experiment-checklist.md", "expected-artifacts.md", "collaboration-scope.md",
]

# Hype words the one-page summary (and, by extension, all outreach
# material) must never use, per the Phase 7.3 spec.
HYPE_WORDS = ["revolutionary", "production-ready", "industry-leading"]

# Hardware claims that are gated -- see outreach/hardware-claim-gates.md.
# Each maps a prohibited phrase to the gate name that would need to pass
# before it's allowed.
GATED_CLAIMS = {
	"hardware bit-exact": "Gate 4 (hardware bit-exactness)",
	"fpga-deployed": "Gate 3 (real board bring-up)",
	"fpga-verified": "Gate 3 (real board bring-up)",
	"board-verified": "Gate 3 (real board bring-up)",
	"runs on an fpga": "Gate 3 (real board bring-up)",
	"real cxl acceleration": "Gate 7 (real CXL platform integration)",
	"cxl-accelerated": "Gate 7 (real CXL platform integration)",
	"hardware-validated": "Gate 3/6/7 depending on context",
}

NEGATION_MARKERS = [
	"not ", "never ", "no ", "nowhere", "isn't", "is not", "n't", "without",
	"prohibited", "gated", "gate for", "required for", "bar for",
	"minimum bar", "before any", "any claim", "for any",
]


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


def all_outreach_hardware_markdown():
	files = []
	for base in (OUTREACH_DIR, HARDWARE_DIR):
		if base.exists():
			files.extend(sorted(base.rglob("*.md")))
	return files


@check("lab-package has all 6 required files")
def _c1():
	missing = [f for f in REQUIRED_LAB_PACKAGE_FILES
		if not (OUTREACH_DIR / "lab-package" / f).exists()]
	return len(missing) == 0, f"missing: {missing}" if missing else "all 6 present"


@check("internal (relative, non-http) links in outreach/ and hardware/ resolve")
def _c2():
	errors = []
	for md_file in all_outreach_hardware_markdown():
		md_dir = md_file.parent
		for m in re.finditer(r"\]\(([^:)]+)\)", md_file.read_text()):
			target = m.group(1).split("#")[0]
			if not target:
				continue
			resolved = (md_dir / target).resolve()
			if not resolved.exists():
				errors.append(f"{md_file.relative_to(REPO_ROOT)} -> {target}")
	return len(errors) == 0, f"{len(errors)} broken link(s): {errors[:10]}" if errors else "all internal links resolve"


@check("hardware/results-schema.json is valid JSON with the required field list")
def _c3():
	schema = json.loads((HARDWARE_DIR / "results-schema.json").read_text())
	required_fields = {
		"board", "fpga", "toolchain", "bitstream_hash", "clock_mhz",
		"pcie_generation", "pcie_width", "host_cpu", "operation", "precision",
		"batch_size", "queue_depth", "bytes", "throughput_bytes_per_sec",
		"latency_ns", "cpu_utilization_pct", "board_power_watts",
		"temperature_celsius", "parity_failures", "timestamp_utc", "result_label",
	}
	declared = set(schema.get("required", []))
	missing = required_fields - declared
	return len(missing) == 0, f"schema missing required fields: {missing}" if missing else f"all {len(required_fields)} required fields present"


def _validate_against_schema(instance, schema):
	errors = []
	props = schema["properties"]
	for r in schema.get("required", []):
		if r not in instance:
			errors.append(f"missing required field: {r}")
	for k, v in instance.items():
		if k not in props:
			if schema.get("additionalProperties") is False:
				errors.append(f"unexpected field not in schema: {k}")
			continue
		spec = props[k]
		t = spec.get("type")
		if t == "string" and not isinstance(v, str):
			errors.append(f"{k}: expected string")
		if t == "number" and not isinstance(v, (int, float)):
			errors.append(f"{k}: expected number")
		if t == "integer" and not isinstance(v, int):
			errors.append(f"{k}: expected integer")
		if t == "object" and not isinstance(v, dict):
			errors.append(f"{k}: expected object")
		if "enum" in spec and v not in spec["enum"]:
			errors.append(f"{k}: {v!r} not in enum {spec['enum']}")
		if "pattern" in spec and isinstance(v, str) and not re.fullmatch(spec["pattern"], v):
			errors.append(f"{k}: {v!r} does not match pattern {spec['pattern']}")
		if t == "object" and "required" in spec:
			for rr in spec["required"]:
				if rr not in v:
					errors.append(f"{k}.{rr}: missing required nested field")
	return errors


@check("hardware/results-example.json validates against the schema and is labeled DOCUMENTED_EXAMPLE")
def _c4():
	schema = json.loads((HARDWARE_DIR / "results-schema.json").read_text())
	example_path = HARDWARE_DIR / "results-example.json"
	if not example_path.exists():
		return False, "hardware/results-example.json does not exist"
	example = json.loads(example_path.read_text())
	errors = _validate_against_schema(example, schema)
	if example.get("result_label") != "DOCUMENTED_EXAMPLE":
		errors.append(f"result_label is {example.get('result_label')!r}, expected DOCUMENTED_EXAMPLE")
	return len(errors) == 0, "; ".join(errors) if errors else "validates cleanly, correctly labeled DOCUMENTED_EXAMPLE"


@check("no REAL_HARDWARE-labeled result exists anywhere in the repository (none is disclosed as real yet)")
def _c5():
	bad = []
	for json_file in list(HARDWARE_DIR.rglob("*.json")):
		try:
			data = json.loads(json_file.read_text())
		except (json.JSONDecodeError, UnicodeDecodeError):
			continue
		records = data if isinstance(data, list) else [data]
		for rec in records:
			if isinstance(rec, dict) and rec.get("result_label") == "REAL_HARDWARE":
				bad.append(str(json_file.relative_to(REPO_ROOT)))
	return len(bad) == 0, f"REAL_HARDWARE record(s) found (not allowed yet): {bad}" if bad else "none found, as expected"


@check("no hype word (revolutionary/production-ready/industry-leading) in outreach/")
def _c6():
	bad = []
	for md_file in sorted(OUTREACH_DIR.rglob("*.md")):
		text = md_file.read_text().lower()
		for word in HYPE_WORDS:
			if word in text:
				bad.append(f"{md_file.relative_to(REPO_ROOT)}: '{word}'")
	return len(bad) == 0, "; ".join(bad) if bad else f"no occurrence of {HYPE_WORDS}"


@check("no gated hardware claim appears unhedged outside hardware-claim-gates.md's own documentation")
def _c7():
	bad = []
	gates_file = OUTREACH_DIR / "hardware-claim-gates.md"
	for md_file in all_outreach_hardware_markdown():
		if md_file == gates_file:
			continue  # the gates doc itself documents these phrases; that's its job
		text = md_file.read_text().lower()
		for phrase, gate in GATED_CLAIMS.items():
			for m in re.finditer(re.escape(phrase), text):
				window = text[max(0, m.start() - 80): m.end() + 40]
				if any(neg in window for neg in NEGATION_MARKERS):
					continue
				bad.append(f"{md_file.relative_to(REPO_ROOT)}: '{phrase}' (requires {gate}) near: ...{window.strip()[:100]}...")
	return len(bad) == 0, "; ".join(bad[:8]) if bad else f"no unhedged occurrence of {len(GATED_CLAIMS)} gated claims outside hardware-claim-gates.md"


@check("hardware-claim-gates.md's summary table matches its own per-gate sections")
def _c8():
	text = (OUTREACH_DIR / "hardware-claim-gates.md").read_text()
	gate_headers = re.findall(r"## Gate (\d+):.*?\((PASSED|NOT PASSED)\)", text)
	table_rows = re.findall(r"\| (\d+)\. .*? \| (\*\*PASSED\*\*|not passed) \|", text)
	header_status = {int(n): s for n, s in gate_headers}
	table_status = {int(n): ("PASSED" if "PASSED" in s else "NOT PASSED") for n, s in table_rows}
	mismatches = [n for n in header_status if header_status.get(n) != table_status.get(n)]
	return len(mismatches) == 0, f"gate(s) with mismatched status between section header and summary table: {mismatches}" if mismatches else f"{len(header_status)} gates consistent"


@check("email-templates.md has no unfilled bracket field left as a literal send-ready example")
def _c9():
	text = (OUTREACH_DIR / "email-templates.md").read_text()
	# Bracketed fields are EXPECTED (they're meant to be filled in) --
	# this check instead confirms the file's own header explicitly warns
	# never to send with one still present, so the safeguard is documented.
	has_bracket_fields = bool(re.search(r"\[Last name\]|\[Lab name\]|\[Team", text))
	has_warning = "never send with a placeholder" in text.lower()
	ok = has_bracket_fields and has_warning
	return ok, "bracketed fields present and the never-send-with-a-placeholder warning is present" if ok else "expected bracket fields or warning missing"


# Wider corpus for the authorship/dev-process wording checks below --
# these claims appear across outreach/, paper/, docs/, and README.md,
# not just outreach/hardware/.
def _wider_corpus():
	files = []
	for pattern in ("outreach/**/*.md", "paper/*.md", "paper/*.tex", "docs/*.md", "README.md"):
		files.extend(sorted(REPO_ROOT.glob(pattern)))
	return [f for f in files if f.is_file()]


# Phase-summary docs that legitimately QUOTE old, now-corrected wording
# as part of their own "here's what changed and why" disclosure (the
# same reasoning that excludes outreach/hardware-claim-gates.md from
# the GATED_CLAIMS scan -- documenting a phrase is not asserting it).
META_DOCUMENTATION_FILES = {
	"docs/phase7-hardware-outreach.md",
	# Historical phase-completion record: accurately describes what
	# Phase 7.1 built AT THE TIME (a paper skeleton) -- not a live
	# status claim about paper/'s current state.
	"docs/phase7-research-release.md",
	# This audit's own report: discusses the "paper skeleton" phrase
	# and the pre-audit "Phase 0 through 7.1" wording as findings it
	# corrected, the same documenting-not-asserting reasoning as above.
	"docs/public-release-audit.md",
}


def _wider_corpus_excluding_meta_docs():
	return [f for f in _wider_corpus()
		if str(f.relative_to(REPO_ROOT)) not in META_DOCUMENTATION_FILES]


# Phrases that would misrepresent Kadir's role (implying he hand-wrote
# every line without AI assistance, or that he's a credentialed/
# affiliated "independent researcher"). Each maps to a short reason,
# used only for the failure message.
PROHIBITED_AUTHORSHIP_PHRASES = {
	"i personally wrote every line": "implies no AI assistance was used",
	"wrote every line of code": "implies no AI assistance was used",
	"authored every line": "implies no AI assistance was used",
	"i hand-wrote": "implies no AI assistance was used",
}

# "sole author" alone is fine when qualified (e.g. "sole human author",
# "sole author of this work" immediately followed by an ownership/
# direction clarification) -- this set is phrases that are NEVER okay
# regardless of qualification, checked separately from GATED_CLAIMS.
BARE_SOLE_AUTHOR_PATTERN = re.compile(r"\bsole author\b(?!\s+is\b)")


@check("no phrase implies Kadir hand-wrote all code without AI assistance")
def _c10():
	bad = []
	for f in _wider_corpus_excluding_meta_docs():
		text = f.read_text().lower()
		for phrase, reason in PROHIBITED_AUTHORSHIP_PHRASES.items():
			if phrase in text:
				bad.append(f"{f.relative_to(REPO_ROOT)}: '{phrase}' ({reason})")
	return len(bad) == 0, "; ".join(bad) if bad else f"no occurrence of {len(PROHIBITED_AUTHORSHIP_PHRASES)} prohibited authorship phrases"


@check("'sole author'/'independent researcher' only appear in an accurate, qualified form")
def _c11():
	bad = []
	for f in _wider_corpus_excluding_meta_docs():
		text = f.read_text()
		lower = text.lower()
		for m in BARE_SOLE_AUTHOR_PATTERN.finditer(lower):
			window = lower[max(0, m.start() - 40): m.end() + 60]
			# Accurate forms: "sole human author", "sole author of this
			# work is <name>" (paper authorship credit -- a legitimate,
			# different claim from "wrote every line"), or immediately
			# followed by a clarifying "creator"/"human"/"owner" word.
			if "human" in window or "creator" in window or "owner" in window or re.search(r"sole author of (this work|)\s*is\b", window):
				continue
			bad.append(f"{f.relative_to(REPO_ROOT)}: unqualified 'sole author' near: ...{text[max(0,m.start()-40):m.start()+60]}...")
		for m in re.finditer(r"\bindependent researcher\b", lower):
			bad.append(f"{f.relative_to(REPO_ROOT)}: unqualified 'independent researcher'")
	return len(bad) == 0, "; ".join(bad[:8]) if bad else "no unqualified occurrence"


@check("every scripts/demo.sh duration mention is qualified (a range or explicit caveat, not a bare single number)")
def _c12():
	bad = []
	pattern = re.compile(r"~?25\s*(?:-|–|second)")
	for f in _wider_corpus_excluding_meta_docs():
		text = f.read_text()
		lower = text.lower()
		if "demo.sh" not in lower:
			continue
		for m in re.finditer(r"~?25\s*second", lower):
			window = lower[max(0, m.start() - 80): m.end() + 120]
			qualified = any(tok in window for tok in ("25–50", "25-50", "depending on", "in that run", "varies", "cache"))
			if not qualified:
				bad.append(f"{f.relative_to(REPO_ROOT)}: unqualified '~25 second(s)' near demo.sh mention")
	return len(bad) == 0, "; ".join(bad) if bad else "all demo.sh duration mentions are qualified"


@check("no live-status doc still describes paper/ as an incomplete 'skeleton'")
def _c13():
	bad = []
	for f in _wider_corpus_excluding_meta_docs():
		text = f.read_text().lower()
		if re.search(r"paper.{0,20}skeleton|academic.paper skeleton", text):
			bad.append(f"{f.relative_to(REPO_ROOT)}: still describes paper/ as a 'skeleton'")
	return len(bad) == 0, "; ".join(bad) if bad else "no stale 'paper skeleton' phrase in any live-status doc"


@check("no live-status doc cites a stale 'Phase 0 through N' range behind the real current phase")
def _c14():
	bad = []
	# The current phase range is at least 7.3 (hardware outreach + the
	# wording-correction/CI-repair/audit work layered on top of it) --
	# any doc still citing an earlier upper bound is out of date.
	for f in _wider_corpus_excluding_meta_docs():
		text = f.read_text()
		for m in re.finditer(r"Phase 0\s*(?:[–‐-]+|through)\s*7\.(\d+)", text):
			if int(m.group(1)) < 3:
				bad.append(f"{f.relative_to(REPO_ROOT)}: stale 'Phase 0 through 7.{m.group(1)}' (current: 7.3+)")
	return len(bad) == 0, "; ".join(bad) if bad else "no stale phase-range citation found"


@check("every README.md workflow badge points at a workflow file that actually exists")
def _c15():
	readme = (REPO_ROOT / "README.md").read_text()
	bad = []
	for m in re.finditer(r"actions/workflows/([a-zA-Z0-9_.-]+)/badge\.svg", readme):
		wf_name = m.group(1)
		wf_path = REPO_ROOT / ".github" / "workflows" / wf_name
		if not wf_path.exists():
			bad.append(f"badge references .github/workflows/{wf_name}, which does not exist")
	return len(bad) == 0, "; ".join(bad) if bad else "all workflow badges point at real, existing workflow files"


@check("no doc claims a private/companion repository (e.g. membrane-labs) currently exists")
def _c16():
	# A careful document establishes "membrane-labs does not exist yet"
	# clearly once (its first mention or opening paragraph), then refers
	# to it freely afterward without re-hedging every single sentence --
	# requiring every individual mention to repeat the caveat would make
	# these documents unreadable. So: at least ONE occurrence's own
	# nearby window (not the whole file, which risks a coincidental,
	# unrelated hedge word elsewhere in a long document passing this
	# check for the wrong reason) must carry a real hedge -- specific,
	# deliberately narrow phrases, not a generic word like "planned"
	# that could easily appear elsewhere in the same file by chance.
	hedge_words = (
		"does not exist yet", "not yet created", "not been created",
		"if one comes to exist", "if and when it is", "not created",
		"repository was created", "no private repository has been",
	)
	bad = []
	for f in _wider_corpus_excluding_meta_docs():
		text = f.read_text().lower()
		if "membrane-labs" not in text:
			continue
		positions = [m.start() for m in re.finditer("membrane-labs", text)]
		hedged_somewhere = any(
			any(h in text[max(0, p - 600): p + 600] for h in hedge_words)
			for p in positions
		)
		if not hedged_somewhere:
			bad.append(f"{f.relative_to(REPO_ROOT)}: mentions 'membrane-labs' but no occurrence has a not-yet-created hedge nearby")
	return len(bad) == 0, "; ".join(bad) if bad else "every file mentioning membrane-labs also hedges its non-existence somewhere"


@check("no doc claims paper/main.pdf is committed/tracked in the repository")
def _c17():
	bad = []
	tracked_git = set(
		__import__("subprocess").run(
			["git", "-C", str(REPO_ROOT), "ls-files", "paper/main.pdf"],
			capture_output=True, text=True,
		).stdout.split()
	)
	if "paper/main.pdf" in tracked_git:
		bad.append("paper/main.pdf IS actually tracked by git -- this would itself be a real problem (should be a CI artifact only)")
	claim_words = ("is committed", "is tracked", "committed to the repo", "committed to this repository", "tracked in the repo")
	for f in _wider_corpus_excluding_meta_docs():
		# Strip markdown emphasis markers before matching -- "is **not**
		# committed" would otherwise hide the negation from a plain
		# substring search for "not " (the space right after "not" is
		# consumed by the closing "**" instead).
		text = f.read_text().lower().replace("*", "").replace("_", "")
		for phrase in claim_words:
			# Check EVERY occurrence, not just the first -- a doc can
			# legitimately use a phrase like "is committed" elsewhere
			# (e.g. "before anything is committed to RTL") unrelated to
			# main.pdf; only checking the first match would silently
			# skip a later, real violation.
			for m in re.finditer(re.escape(phrase), text):
				idx = m.start()
				window = text[max(0, idx - 100):idx]
				if "main.pdf" in window or "main.pdf" in text[idx:idx + 60]:
					if "not " not in window and "never " not in window:
						bad.append(f"{f.relative_to(REPO_ROOT)}: claims main.pdf '{phrase}' without a negation nearby")
	return len(bad) == 0, "; ".join(bad) if bad else "no doc claims main.pdf is tracked; git confirms it truly isn't"


def main():
	for fn in (_c1, _c2, _c3, _c4, _c5, _c6, _c7, _c8, _c9, _c10, _c11, _c12,
			_c13, _c14, _c15, _c16, _c17):
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
