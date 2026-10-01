#!/usr/bin/env python3
"""Validate local Markdown links within the repository."""

from __future__ import annotations

import argparse
import re
import sys
import unicodedata
from pathlib import Path
from typing import Iterable, Iterator, List, Optional, Set, Tuple
from urllib.parse import urldefrag, unquote

MARKDOWN_LINK_PATTERN = re.compile(r"!?(\[[^\]]+\])\(([^)]+)\)")
HTML_ANCHOR_PATTERN = re.compile(r"""<a\b[^>]*?\shref=["']([^"']+)["']""", re.IGNORECASE)

# Generator output directories that are gitignored and built only in docs CI
# (repo-relative POSIX path -> how to build it). A link into one of them cannot
# be checked on a tree where the generator has not run, so while the directory
# is ABSENT such a link is reported as skipped, not as valid. Once the
# directory exists it is checked like any other link, and --require-generated
# turns an absent directory into a failure (the docs CI job passes it after
# running the generators).
GENERATED_OUTPUT_DIRS = {
    "docs/api/cpp": "python scripts/build_documentation.py --doxygen (needs Doxygen; config docs/Doxyfile)",
}


def absent_generated_root(resolved_path: Path, repo_root: Path) -> Optional[str]:
    """The declared generated-output directory containing resolved_path, if that directory does not exist."""
    for rel_root in GENERATED_OUTPUT_DIRS:
        root = (repo_root / rel_root).resolve()
        try:
            resolved_path.relative_to(root)
        except ValueError:
            continue
        if not root.exists():
            return rel_root
    return None
HEADING_PATTERN = re.compile(r"^(#+)\s+(.*)$")


def slugify(text: str) -> str:
    """Convert a heading into a GitHub-compatible anchor slug."""
    normalized = unicodedata.normalize("NFKD", text)
    stripped = "".join(ch for ch in normalized if not unicodedata.combining(ch))
    lowered = stripped.lower()
    cleaned = re.sub(r"[^0-9a-z\s-]", "", lowered)
    collapsed = re.sub(r"\s+", "-", cleaned).strip("-")
    return collapsed


def parse_headings(path: Path) -> Set[str]:
    headings: Set[str] = set()
    try:
        content = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return headings
    for line in content.splitlines():
        match = HEADING_PATTERN.match(line.strip())
        if not match:
            continue
        heading_text = match.group(2).strip()
        # Remove explicit anchor definitions such as `{#custom-anchor}`
        if "{" in heading_text and heading_text.endswith("}"):
            heading_text = heading_text[: heading_text.index("{")].strip()
        slug = slugify(heading_text)
        if slug:
            headings.add(slug)
    return headings


def iter_markdown_files(paths: Iterable[Path]) -> Iterator[Path]:
    for base in paths:
        if base.is_file() and base.suffix.lower() in {".md", ".markdown"}:
            yield base
        elif base.is_dir():
            for path in base.rglob("*.md"):
                yield path


def extract_links(content: str) -> Iterator[Tuple[str, str]]:
    in_code_block = False
    for line in content.splitlines():
        stripped = line.strip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_code_block = not in_code_block
            continue
        if in_code_block:
            continue
        for match in MARKDOWN_LINK_PATTERN.finditer(line):
            start_index = match.start()
            if start_index > 0 and line[start_index - 1] not in {" ", "\t", "(", "[", "{"}:
                continue
            label, target = match.groups()
            if label.startswith("!"):
                continue
            yield label, target
        # Raw-HTML anchors (the API index tables use them) are links too.
        for match in HTML_ANCHOR_PATTERN.finditer(line):
            yield "<a>", match.group(1)


def is_external(target: str) -> bool:
    return target.startswith("http://") or target.startswith("https://") or target.startswith("mailto:")


def validate_link(
    source: Path,
    target: str,
    repo_root: Path,
    require_generated: bool = False,
    skipped: Optional[List[str]] = None,
) -> Optional[str]:
    if not target or target.startswith("javascript:"):
        return None
    if is_external(target):
        return None

    raw_target = target.strip()
    raw_target = raw_target.split("?", 1)[0]
    fragment_target, frag = urldefrag(raw_target)
    fragment_target = unquote(fragment_target)
    frag = unquote(frag)

    if not fragment_target and frag:
        # Anchor within the same file
        headings = parse_headings(source)
        if slugify(frag) not in headings:
            return f"Missing anchor '#{frag}' in {source.relative_to(repo_root)}"
        return None

    resolved_path = (source.parent / fragment_target).resolve()
    try:
        resolved_path.relative_to(repo_root.resolve())
    except ValueError:
        return f"Link escapes repository boundary: {target}"

    if not resolved_path.exists():
        generated_root = absent_generated_root(resolved_path, repo_root)
        if generated_root is not None and not require_generated and skipped is not None:
            skipped.append(
                f"Skipped (generated output {generated_root}/ not built; build it with "
                f"{GENERATED_OUTPUT_DIRS[generated_root]}): {target} (from {source.relative_to(repo_root)})"
            )
            return None
        return f"Missing file: {target} (from {source.relative_to(repo_root)})"

    if frag and resolved_path.suffix.lower() in {".md", ".markdown"}:
        headings = parse_headings(resolved_path)
        if slugify(frag) not in headings:
            rel = resolved_path.relative_to(repo_root)
            return f"Missing anchor '#{frag}' in {rel}"

    return None


def validate_paths(paths: List[Path], repo_root: Path, require_generated: bool = False) -> int:
    failures = 0
    skipped: List[str] = []
    for path in iter_markdown_files(paths):
        try:
            content = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            print(f"Skipping non-UTF8 file: {path}")
            continue
        for _label, target in extract_links(content):
            error = validate_link(path, target, repo_root, require_generated, skipped)
            if error:
                failures += 1
                print(error)
    for note in skipped:
        print(note)
    return failures


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Validate repository Markdown links")
    parser.add_argument("paths", nargs="*", default=["docs", "wiki", "README.md"], help="Files or directories to scan")
    parser.add_argument(
        "--require-generated",
        action="store_true",
        help="Fail links into declared generated-output directories (e.g. docs/api/cpp) even when the directory was not built.",
    )
    args = parser.parse_args(argv)

    repo_root = Path(__file__).resolve().parents[2]
    targets = [(repo_root / Path(p)).resolve() for p in args.paths]
    failures = validate_paths(targets, repo_root, args.require_generated)
    if failures:
        print(f"Found {failures} broken link(s).", file=sys.stderr)
        return 1
    print("All checked links are valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
