#!/usr/bin/env python3
"""Check every relative Markdown link in this repository resolves to a
real file (or a real anchor-free directory). Does not check http(s)
links (no network access assumed in CI) -- see verify-outreach.py for
the narrower internal-link check that package already does for
outreach/ and hardware/; this script covers the whole repository.

Exit code: 0 if every relative link resolves, 1 otherwise.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
SKIP_DIRS = {".git", "node_modules"}

FAILURES = []
CHECKED = 0


def iter_markdown_files():
    for p in REPO_ROOT.rglob("*.md"):
        if any(part in SKIP_DIRS for part in p.parts):
            continue
        yield p


def main():
    global CHECKED
    for md_file in iter_markdown_files():
        text = md_file.read_text(errors="replace")
        for match in LINK_RE.finditer(text):
            target = match.group(1).strip()
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            target = target.split("#", 1)[0].strip()
            if not target:
                continue
            CHECKED += 1
            resolved = (md_file.parent / target).resolve()
            if not resolved.exists():
                FAILURES.append(f"{md_file.relative_to(REPO_ROOT)}: broken link -> {target}")

    for f in FAILURES:
        print(f"[FAIL] {f}")
    print(f"\n{CHECKED - len(FAILURES)}/{CHECKED} relative links resolve")
    sys.exit(1 if FAILURES else 0)


if __name__ == "__main__":
    main()
