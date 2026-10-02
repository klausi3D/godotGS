#!/usr/bin/env python3
"""Fail when the Doxygen C++ API output does not document the module's classes.

Doxygen still writes html/index.html when its INPUT is missing or matches no
file (that only produces warnings), so "the output exists" proves nothing. This
check derives the expected classes from the module itself: every class that
`modules/gaussian_splatting/register_types.cpp` registers with
GDREGISTER_*CLASS(...) must have a Doxygen class page, identified by the page
title "<Name> Class Reference" (independent of Doxygen's file-naming scheme).

With --newer-than EPOCH, a page only counts if it was written at or after that
time, so pages left over from an earlier run cannot satisfy the check.

Run by scripts/build_documentation.py right after Doxygen succeeds.
"""
from __future__ import annotations

import argparse
import html
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
REGISTER_TYPES = REPO_ROOT / "modules" / "gaussian_splatting" / "register_types.cpp"
HTML_DIR = REPO_ROOT / "docs" / "api" / "cpp" / "html"

REGISTER_RE = re.compile(r"\bGDREGISTER_(?:[A-Z]+_)*CLASS\(\s*([A-Za-z_][A-Za-z0-9_:]*)\s*\)")
TITLE_RE = re.compile(r"<title>(.*?)</title>", re.S | re.I)
CLASS_TITLE_RE = re.compile(r"(?:.*?:\s+)?(.+?)\s+Class Reference$")


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def expected_classes(register_types_text: str) -> list[str]:
    return sorted(set(REGISTER_RE.findall(_strip_comments(register_types_text))))


def documented_classes(html_dir: Path, newer_than: float | None = None) -> set[str]:
    names: set[str] = set()
    for page in html_dir.glob("class*.html"):
        if newer_than is not None and page.stat().st_mtime < newer_than:
            continue
        head = page.read_text(encoding="utf-8", errors="replace")[:4096]
        title = TITLE_RE.search(head)
        if not title:
            continue
        match = CLASS_TITLE_RE.match(html.unescape(title.group(1)).strip())
        if match:
            names.add(match.group(1).strip())
    return names


def check(expected: list[str], documented: set[str]) -> list[str]:
    if not expected:
        return [f"no GDREGISTER_*CLASS(...) found in {REGISTER_TYPES.relative_to(REPO_ROOT).as_posix()} (refusing to pass vacuously)"]
    return [f"{name}: registered in the module but has no Doxygen class page" for name in expected if name not in documented]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--html-dir", type=Path, default=HTML_DIR)
    parser.add_argument("--newer-than", type=float, default=None, help="Only count pages written at or after this epoch time.")
    args = parser.parse_args(argv)

    try:
        expected = expected_classes(REGISTER_TYPES.read_text(encoding="utf-8"))
    except OSError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    if not args.html_dir.is_dir():
        print(f"ERROR: Doxygen HTML output not found at {args.html_dir}", file=sys.stderr)
        return 1
    documented = documented_classes(args.html_dir, args.newer_than)
    failures = check(expected, documented)
    if failures:
        print(
            f"Doxygen output does not document the module ({len(failures)} of {len(expected)} registered classes missing; "
            "check INPUT/FILE_PATTERNS in docs/Doxyfile and the Doxygen warnings):",
            file=sys.stderr,
        )
        for failure in failures[:20]:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print(f"Doxygen output documents all {len(expected)} registered module classes ({len(documented)} class pages).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
