#!/usr/bin/env python3
"""Completeness guard: docs/reference/project-settings.md covers the manifest.

The keys listed in the page's generated tables must equal the keys in
`modules/gaussian_splatting/config/project_settings_manifest.json`, both ways,
each exactly once:

- a manifest key missing from the page fails (a key was added without
  regenerating, or a row was deleted by hand);
- a page key missing from the manifest fails (a key was removed or renamed
  without regenerating, or a row was invented by hand);
- a key listed twice fails.

Only the first cell of each table row in the generated region (after the
hand-maintained block's marker line) counts, so keys mentioned in prose or in
the hand-maintained annotations neither satisfy nor break the check.

Pure Python, no Godot binary, no generator run: it is cheap enough for the
required guard-only lane. Whether the page is byte-identical to the
generator's output is checked separately by the docs CI freshness step.
"""
from __future__ import annotations

import json
import re
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PAGE_PATH = REPO_ROOT / "docs" / "reference" / "project-settings.md"
MANIFEST_PATH = REPO_ROOT / "modules" / "gaussian_splatting" / "config" / "project_settings_manifest.json"
MARKER_PREFIX = "> This block is hand-maintained."

KEY = r"rendering/gaussian_splatting/[A-Za-z0-9_/]+"
HTML_ROW_KEY_RE = re.compile(rf"<tr>\s*<td>\s*<pre>\s*<code>({KEY})</code>")
MD_ROW_KEY_RE = re.compile(rf"^\|\s*`({KEY})`\s*\|", re.M)


def page_keys(page_text: str) -> list[str]:
    """Keys in the first cell of every table row of the generated region."""
    lines = page_text.splitlines()
    start = 0
    for index, line in enumerate(lines):
        if line.startswith(MARKER_PREFIX):
            start = index + 1
            break
    generated = "\n".join(lines[start:])
    return HTML_ROW_KEY_RE.findall(generated) + MD_ROW_KEY_RE.findall(generated)


def manifest_keys(manifest: dict) -> list[str]:
    return [entry["key"] for entry in manifest.get("settings", []) if isinstance(entry, dict) and "key" in entry]


def compare(page: list[str], manifest: list[str]) -> list[str]:
    failures: list[str] = []
    for key, count in sorted(Counter(page).items()):
        if count > 1:
            failures.append(f"{key}: listed {count} times on the page")
    for key, count in sorted(Counter(manifest).items()):
        if count > 1:
            failures.append(f"{key}: listed {count} times in the manifest")
    page_set, manifest_set = set(page), set(manifest)
    for key in sorted(manifest_set - page_set):
        failures.append(f"{key}: in the manifest but missing from the page")
    for key in sorted(page_set - manifest_set):
        failures.append(f"{key}: on the page but not in the manifest")
    if not manifest_set:
        failures.append("manifest lists no keys (refusing to pass vacuously)")
    return failures


def main() -> int:
    try:
        page_text = PAGE_PATH.read_text(encoding="utf-8")
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    page = page_keys(page_text)
    manifest_list = manifest_keys(manifest)
    failures = compare(page, manifest_list)
    if failures:
        print(
            "Project settings reference does not cover the manifest "
            "(fix: python scripts/generate_project_settings_reference.py):",
            file=sys.stderr,
        )
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print(f"Project settings reference covers the manifest ({len(set(manifest_list))} keys).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
