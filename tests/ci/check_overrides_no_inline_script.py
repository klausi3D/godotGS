#!/usr/bin/env python3
"""Guard: the MkDocs theme overrides (`overrides/**`) carry no executable script.

## Why this guard exists

`overrides/**` is the Material `theme.custom_dir`. Its templates are rendered into
every page of the public docs site, and any static file in it is copied into the
site. The risk policy classifies `overrides/**` as R1 docs-site tooling, not R3
(maintainer decision, #1212 and PR #1207). That is only sound while nothing under
`overrides/` runs in a reader's browser: a strict MkDocs build renders a template,
it never executes or inspects script, so a redirect or data-exfiltrating script
would pass `docs-build` unnoticed.

Script for the site therefore lives in `docs/assets/javascripts/`, which is R3
and is loaded through `extra_javascript` in `mkdocs.yml`. This guard enforces the
split on the source of every file under `overrides/`.

## The rules

1. **Only template and style files.** Every file must have one of
   `ALLOWED_SUFFIXES`. A `.js` file under `overrides/` would be copied into the
   site and could be loaded by `extra_javascript` without any of the tokens below
   ever appearing.
2. **No `<script`**, in any case, with or without whitespace after `<`. An HTML
   or Jinja comment does not exempt it: the guard does not try to tell comments
   from markup, because that parsing is exactly what an attacker would target.
3. **No inline event-handler attribute** (`onclick=`, `ONLOAD =`, `/onerror=` ...):
   an `on<letters>` name followed by `=` that is not the tail of a longer
   identifier or a hyphenated name (`data-onboarding=` and `button=` are fine). A
   Jinja expression such as `x.onboarding == 1` is reported too; rename it. A
   false positive costs a rename, a false negative ships script.
4. **No `javascript:` URL.** Checked after decoding HTML entities and removing
   whitespace and control characters, because browsers do the same:
   `java&#x09;script:`, `&#106;avascript:` and `JavaScript :` are all caught.
5. **No nested document or plugin.** No `<iframe`, `<frame`, `<frameset`,
   `<object` or `<embed` element, and no `srcdoc` attribute on anything. A
   browser entity-decodes `srcdoc="&lt;script&gt;..."` and runs the result as a
   nested document, and the other elements load a document or plugin this guard
   cannot see.
6. **No `<meta http-equiv="refresh">` that carries a URL.** Only a bare delay
   (`content="30"`) passes; a URL, an unparsable value or a Jinja expression in
   `content` fails.

Rules 2, 3, 5 and 6 run twice: on the raw source and on the source after
`html.unescape()`, so an entity-encoded payload in any attribute value
(`title="&lt;script&gt;"`, `srcdoc="&#60;img onerror=...&#62;"`) is caught. A
literal `&lt;script&gt;` in text content is reported too; write it another way.

## Exit codes

- 0: every file passed.
- 1: at least one violation.
- 2: empty subject. No `overrides/` directory or no file in it, so the guard
  checked nothing (a vacuous pass is a failure).
- 3: uninspectable input. A file that is not valid UTF-8 cannot be scanned and is
  not skipped.

## Out of scope

This is a static tripwire on the authored source, not a sandbox. An adversarial
template that assembles a tag at render time (`{{ '<scr' ~ 'ipt>' }}`) is the
review's problem, which R1 still requires. Raw HTML in Markdown pages under
`docs/**` and `extra_javascript` entries in `mkdocs.yml` are outside this guard.
"""

from __future__ import annotations

import argparse
import html
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OVERRIDES_RELPATH = "overrides"

ALLOWED_SUFFIXES = (".html", ".css")

EXIT_OK = 0
EXIT_VIOLATIONS = 1
EXIT_EMPTY_SUBJECT = 2
EXIT_UNINSPECTABLE = 3

SCRIPT_TAG_RE = re.compile(r"<\s*script", re.IGNORECASE)
EVENT_HANDLER_RE = re.compile(r"(?<![A-Za-z0-9_\-])on[a-z]+\s*=", re.IGNORECASE)
# Browsers strip ASCII whitespace and C0 controls from a URL before reading its
# scheme, so the normalised text drops them before the match.
_URL_IGNORED_RE = re.compile(r"[\x00-\x20\x7f]+")
JAVASCRIPT_URL_TOKEN = "javascript:"
EMBED_ELEMENT_RE = re.compile(
    r"<\s*(?:iframe|object|embed|frameset|frame)(?![A-Za-z0-9_\-])", re.IGNORECASE
)
SRCDOC_ATTR_RE = re.compile(r"(?<![A-Za-z0-9_\-])srcdoc(?![A-Za-z0-9_\-])", re.IGNORECASE)
# A whole <meta ...> tag; a quoted value may contain '>'.
META_TAG_RE = re.compile(r"<\s*meta(?![A-Za-z0-9_\-])(?:\"[^\"]*\"|'[^']*'|[^'\">])*>?", re.IGNORECASE)
META_REFRESH_RE = re.compile(
    r"(?<![A-Za-z0-9_\-])http-equiv\s*=\s*[\"']?\s*refresh", re.IGNORECASE
)
META_CONTENT_RE = re.compile(
    r"(?<![A-Za-z0-9_\-])content\s*=\s*(?:\"([^\"]*)\"|'([^']*)'|([^\s>]+))", re.IGNORECASE
)
# A refresh `content` that is only a delay reloads the same page; anything after
# the delay is a URL to navigate to.
META_DELAY_ONLY_RE = re.compile(r"\s*\d+(?:\.\d*)?\s*")


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _meta_refresh_with_url(tag: str) -> bool:
    if not META_REFRESH_RE.search(tag):
        return False
    content = META_CONTENT_RE.search(tag)
    if content is None:
        return False
    value = next(group for group in content.groups() if group is not None)
    return META_DELAY_ONLY_RE.fullmatch(value) is None


def _scan_markup(text: str) -> list[str]:
    """Rules 2, 3, 5 and 6 on one rendering of the source."""
    found: list[str] = []
    for m in SCRIPT_TAG_RE.finditer(text):
        found.append(f"line {_line_of(text, m.start())}: script tag {m.group(0)!r}")
    for m in EVENT_HANDLER_RE.finditer(text):
        found.append(
            f"line {_line_of(text, m.start())}: inline event-handler attribute {m.group(0)!r}"
        )
    for m in EMBED_ELEMENT_RE.finditer(text):
        found.append(
            f"line {_line_of(text, m.start())}: nested-document/plugin element {m.group(0)!r}"
        )
    for m in SRCDOC_ATTR_RE.finditer(text):
        found.append(f"line {_line_of(text, m.start())}: srcdoc attribute {m.group(0)!r}")
    for m in META_TAG_RE.finditer(text):
        if _meta_refresh_with_url(m.group(0)):
            found.append(f"line {_line_of(text, m.start())}: meta refresh with a URL")
    return found


def scan_text(text: str) -> list[str]:
    """Return one message per violation found in a file's text."""
    raw = _scan_markup(text)
    problems = list(raw)
    decoded = html.unescape(text)
    if decoded != text:
        # Report only what entity decoding adds, so a raw hit is not listed twice.
        already_reported = Counter(raw)
        for message in _scan_markup(decoded):
            if already_reported[message] > 0:
                already_reported[message] -= 1
            else:
                problems.append(f"{message} (after HTML entity decoding)")
    normalised = _URL_IGNORED_RE.sub("", decoded).lower()
    count = normalised.count(JAVASCRIPT_URL_TOKEN)
    if count:
        problems.append(
            f"{count} javascript: URL(s) (matched after entity decoding and "
            "whitespace removal)"
        )
    return problems


def iter_override_files(root: Path) -> list[Path]:
    base = root / OVERRIDES_RELPATH
    if not base.is_dir():
        return []
    return sorted(path for path in base.rglob("*") if path.is_file())


def check(root: Path) -> tuple[list[Path], list[str], list[str]]:
    """Return (files scanned, violations, uninspectable files)."""
    files = iter_override_files(root)
    violations: list[str] = []
    uninspectable: list[str] = []
    for path in files:
        rel = path.relative_to(root).as_posix()
        if path.suffix.lower() not in ALLOWED_SUFFIXES:
            violations.append(
                f"{rel}: file type {path.suffix or '(none)'!r} is not allowed under "
                f"{OVERRIDES_RELPATH}/ (allowed: {', '.join(ALLOWED_SUFFIXES)}); "
                "put script in docs/assets/javascripts/"
            )
        try:
            text = path.read_bytes().decode("utf-8")
        except UnicodeDecodeError as exc:
            uninspectable.append(f"{rel}: not valid UTF-8 ({exc.reason})")
            continue
        violations.extend(f"{rel}: {problem}" for problem in scan_text(text))
    return files, violations, uninspectable


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root")
    args = parser.parse_args(argv)

    files, violations, uninspectable = check(args.root)
    if not files:
        print(
            f"[overrides-script] FAIL: no files under {OVERRIDES_RELPATH}/ in "
            f"{args.root}; the guard checked nothing",
            file=sys.stderr,
        )
        return EXIT_EMPTY_SUBJECT
    for line in violations:
        print(f"[overrides-script] {line}", file=sys.stderr)
    for line in uninspectable:
        print(f"[overrides-script] uninspectable: {line}", file=sys.stderr)
    if violations:
        print(
            f"[overrides-script] FAIL: {len(violations)} violation(s). Script for the "
            "docs site belongs in docs/assets/javascripts/ (R3), loaded through "
            "extra_javascript in mkdocs.yml.",
            file=sys.stderr,
        )
        return EXIT_VIOLATIONS
    if uninspectable:
        print(
            f"[overrides-script] FAIL: {len(uninspectable)} file(s) could not be scanned",
            file=sys.stderr,
        )
        return EXIT_UNINSPECTABLE
    print(f"[overrides-script] OK: {len(files)} file(s) under {OVERRIDES_RELPATH}/ carry no script.")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
