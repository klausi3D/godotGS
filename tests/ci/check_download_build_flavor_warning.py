#!/usr/bin/env python3
"""Guard: every page that sends a reader to the Releases page says the binaries are `-O0`.

## The failure this guards against

Every binary godotGS publishes is built with `dev_build=yes`, which resolves to
`optimize=none` (`SConstruct`, the `optimize == "auto"` branch) and therefore
compiles at `-O0`. CPU-side frame cost on such a build is inflated by roughly an
order of magnitude. The project already knew this and had already written it down
-- `docs/performance/index.md` carries the sentence verbatim -- but it was written
on the page a reader only reaches *after* deciding godotGS is slow, and on none of
the pages that hand out the download.

So the observable defect was not "nobody knew". It was "the warning existed
somewhere nobody downloading would look". A prose fix alone rots back the first
time somebody adds a fifth download surface, because nothing connects "this page
links to Releases" to "this page must say what it is handing out".

## Why the page set is derived rather than listed

This repository's recurring finding is that an invariant guarded by a hand-written
list is already broken: the list is only as complete as the last person to edit it,
and the page that gets forgotten is exactly the page that needed the guard. That
happened here before the guard was even written: the plan this implements listed
the download surfaces by hand and missed `docs/getting-started/quick-start.md`,
which hands out the same nightly link. The derived scan found it immediately.

So the subject is derived from the property itself: **any Markdown file in this
repository that contains a Markdown *link* whose destination is the GitHub Releases
page** is a download surface, and must carry the warning. Add a page tomorrow, and
it is covered tomorrow; delete one, and the guard shrinks with it.

## The shape rule, and what it deliberately excludes

The trigger is a Markdown *link* (`](<url>)`), not any occurrence of the URL. A
document that mentions the Releases URL inside inline code -- as the archived audit
in `docs/reports/` does, twice -- is describing the link, not offering it, and is
not a download surface. That exclusion falls out of the shape; it is not a
filename exemption that would have to be maintained.

## What "carries the warning" means

Two things, both checkable:

1. **One paragraph containing both `-O0` and `dev_build`.** Co-occurrence within a
   single block, not merely somewhere in the file: `dev_build` appears in build
   docs for unrelated reasons (filename segments, flag tables), and `-O0` could
   drift into an unrelated aside. Requiring them in the same paragraph is what
   makes the match evidence of an actual warning rather than of two coincidences.
2. **A link to the performance dashboard.** A warning that says "this is slow" and
   stops there tells a reader nothing they can act on. Requiring the cross-
   reference also puts these pages under `scripts/docs/check_links.py`, so a
   renamed heading or moved page fails a second, independent check.

## What this guard does NOT check

Whether the wording is *clear to a first-time downloader*. No automated check
substitutes for a person who has never built the project reading the page and
saying what performance they expect. That is stated in the PR that added this
guard and is deliberately not proxied by a readability metric here.

Exit codes: 0 pass, 1 violations found, 2 the guard could not run meaningfully
(no download surface discovered at all -- see `--list`).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The published download surface. Matching the org/repo rather than a generic
# "github.com/*/releases" keeps a quoted upstream Godot release link from being
# mistaken for ours.
RELEASES_LINK_PATTERN = re.compile(
    r"\]\(\s*<?(?P<url>https?://github\.com/klausi3D/godotGS/releases[^)\s>]*)",
    re.IGNORECASE,
)

# Any Markdown link destination, used to find the performance cross-reference.
MARKDOWN_LINK_PATTERN = re.compile(r"\]\(\s*<?(?P<target>[^)\s>]+)")

# Both must appear in the SAME paragraph. See module docstring.
REQUIRED_TOKENS = ("-O0", "dev_build")

PERFORMANCE_PAGE_RELPATH = Path("docs") / "performance" / "index.md"

# Pruned for speed and because nothing here is authored documentation of this
# project: VCS internals, build/tooling output, and the vendored upstream tree.
# Deliberately NOT pruned: `.github/`, `modules/`, `docs/reports/` and every other
# authored directory -- a download surface that appears in one of them must be
# caught, and an exclusion list is the artifact this guard exists to avoid.
PRUNED_DIRS = frozenset(
    {".git", "node_modules", "site", "bin", ".venv", "venv", "__pycache__"}
)
PRUNED_TOP_LEVEL = frozenset({"thirdparty"})


def iter_markdown_files(root: Path) -> list[Path]:
    """Every Markdown file in the project, minus vendored and generated trees."""
    found: list[Path] = []
    stack = [root]
    while stack:
        directory = stack.pop()
        try:
            entries = list(directory.iterdir())
        except OSError:
            continue
        for entry in entries:
            if entry.is_dir():
                if entry.name in PRUNED_DIRS:
                    continue
                if entry.parent == root and entry.name in PRUNED_TOP_LEVEL:
                    continue
                stack.append(entry)
            elif entry.suffix.lower() == ".md":
                found.append(entry)
    return sorted(found)


def links_to_releases(text: str) -> bool:
    return RELEASES_LINK_PATTERN.search(text) is not None


def paragraphs(text: str) -> list[str]:
    """Split on blank lines. An admonition body or a blockquote stays one block."""
    blocks: list[str] = []
    current: list[str] = []
    for line in text.splitlines():
        if line.strip():
            current.append(line)
        elif current:
            blocks.append("\n".join(current))
            current = []
    if current:
        blocks.append("\n".join(current))
    return blocks


def has_warning_paragraph(text: str) -> bool:
    return any(
        all(token in block for token in REQUIRED_TOKENS) for block in paragraphs(text)
    )


def links_to_performance_page(path: Path, text: str, root: Path) -> bool:
    """True when some Markdown link on the page resolves to the performance dashboard."""
    performance_page = (root / PERFORMANCE_PAGE_RELPATH).resolve()
    for match in MARKDOWN_LINK_PATTERN.finditer(text):
        target = match.group("target").split("#", 1)[0].strip()
        if not target or "://" in target:
            continue
        try:
            resolved = (path.parent / target).resolve()
        except OSError:
            continue
        if resolved == performance_page:
            return True
    return False


def discover_download_surfaces(root: Path) -> list[Path]:
    surfaces: list[Path] = []
    for path in iter_markdown_files(root):
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if links_to_releases(text):
            surfaces.append(path)
    return surfaces


def check(root: Path) -> tuple[list[Path], list[str]]:
    """Returns (discovered surfaces, failure messages)."""
    surfaces = discover_download_surfaces(root)
    failures: list[str] = []
    for path in surfaces:
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(root).as_posix()
        if not has_warning_paragraph(text):
            failures.append(
                f"{rel}: links to the GitHub Releases page but no single paragraph "
                f"mentions both {REQUIRED_TOKENS[0]} and {REQUIRED_TOKENS[1]}. Every "
                "published binary is an -O0 dev_build; a page that hands one out has "
                "to say so."
            )
        if not links_to_performance_page(path, text, root):
            failures.append(
                f"{rel}: links to the GitHub Releases page but never links to "
                "docs/performance/index.md, so a reader is told the download is slow "
                "and given nowhere to see what an optimized build measures."
            )
    return surfaces, failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root",
        type=Path,
        default=ROOT,
        help="Repository root to scan (default: this checkout).",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="Print the derived download-surface set and exit. Use this to confirm "
        "the guard has a non-empty subject before trusting a green run.",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()

    surfaces, failures = check(root)

    if args.list:
        for path in surfaces:
            print(path.relative_to(root).as_posix())
        print(f"[download-flavor-guard] {len(surfaces)} download surface(s) discovered.")
        return 0 if surfaces else 2

    if not surfaces:
        # Non-vacuity, fail-closed. A guard whose subject has silently become empty
        # passes forever while protecting nothing -- which is the defect class this
        # repository keeps finding, not one it should add.
        print(
            "[download-flavor-guard] FAIL: no Markdown file links to the GitHub "
            "Releases page. Either the download surfaces were removed, or the link "
            "pattern in this guard no longer matches how they are written. Run with "
            "--list and fix the pattern; do not let this pass silently.",
            file=sys.stderr,
        )
        return 2

    if failures:
        print("[download-flavor-guard] FAIL:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        f"[download-flavor-guard] PASS: {len(surfaces)} download surface(s) all carry "
        "the -O0 / dev_build warning and link to the performance dashboard."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
