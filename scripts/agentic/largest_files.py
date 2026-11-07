#!/usr/bin/env python3
"""List the largest git-tracked text files under a path, by line count.

Usage: python scripts/agentic/largest_files.py [PATH ...] [--top N]

Derived from `git ls-files` on every run, so the ranking cannot drift the way a
hand-kept list would.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

TEXT_SUFFIXES = {
    ".h", ".hpp", ".cpp", ".cc", ".c", ".glsl", ".py", ".gd", ".md", ".json",
    ".yml", ".yaml", ".txt", ".cfg", ".tscn", ".tres", ".xml", ".inc",
}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*", default=["modules/gaussian_splatting"])
    parser.add_argument("--top", type=int, default=25)
    args = parser.parse_args(argv)

    root = Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True
        ).stdout.strip()
    )
    listed = subprocess.run(
        ["git", "ls-files", "-z", "--", *args.paths],
        capture_output=True,
        check=True,
        cwd=root,
    ).stdout.decode("utf-8", errors="replace")
    rows: list[tuple[int, int, str]] = []
    for rel in filter(None, listed.split("\0")):
        path = root / rel
        if path.suffix.lower() not in TEXT_SUFFIXES or not path.is_file():
            continue
        data = path.read_bytes()
        rows.append((data.count(b"\n"), len(data), rel))
    if not rows:
        print(f"no tracked text files under {args.paths}", file=sys.stderr)
        return 1
    rows.sort(reverse=True)
    print(f"{'lines':>7} {'KB':>7}  path")
    for lines, size, rel in rows[: args.top]:
        print(f"{lines:>7} {size // 1024:>7}  {rel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
