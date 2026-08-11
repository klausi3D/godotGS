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
repository that offers the reader the GitHub Releases page** is a download
surface, and must carry the warning. Add a page tomorrow, and it is covered
tomorrow; delete one, and the guard shrinks with it.

## What counts as "offers the reader the Releases page"

The first version of this guard keyed on a single authoring mechanism -- the
inline `](URL)` form -- and that reintroduced the very defect the derivation was
meant to remove, one level down (Codex, PR #872). A reference-style link:

    Grab it from [Get nightly][releases].

    [releases]: https://github.com/klausi3D/godotGS/releases

and a raw HTML anchor:

    <a href="https://github.com/klausi3D/godotGS/releases">Download</a>

are both perfectly ordinary in this repository's Markdown, both render as the
download link, and both were invisible to that pattern. Worse, they were
invisible *silently*: the four inline-link surfaces kept the subject non-empty,
so the non-vacuity exit-2 control -- the guard's only defence against protecting
nothing -- stayed satisfied while a brand-new page handed out an unwarned binary.

The rule is therefore keyed on the URL itself rather than on one syntax:

**Any occurrence of the Releases URL anywhere in a Markdown file, except inside
code, makes that file a download surface.**

That covers inline links, reference-style links (via the definition line, which
is where the URL actually appears), autolinks `<https://...>`, raw HTML
`<a href="...">`, and a bare URL in prose -- which GitHub's own renderer turns
into a live link whether or not the author meant it to be one. `link_form_at()`
still names which of those forms was seen, so a failure message says what it
found, but no verdict depends on that classification: a form this guard has
never heard of still trips it. Recognising a *superset* of the link forms is
what makes the coverage property-shaped instead of mechanism-shaped.

## The shape rule, and what it deliberately excludes

The one exclusion is code. A document that mentions the Releases URL inside
inline code -- as the archived audit in `docs/reports/` does, twice -- is
describing the link, not offering it, and is not a download surface. That
exclusion falls out of the shape; it is not a filename exemption that would have
to be maintained. Fenced blocks are excluded for the same reason.

The exclusion is only sound while the guard can actually tell code from prose.
An unterminated code fence means it cannot: everything after it is ambiguous,
and silently treating it as code would hide exactly the link this guard exists to
find. That is reported as an uninspectable input (exit 3), not skipped.

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

The dashboard link is read through the same multi-form extractor, so a page that
writes it reference-style satisfies the rule the same way an inline one does.

## Why "could not inspect" is not "compliant"

`iter_markdown_files()` used to swallow `OSError` from an unreadable directory,
and the surface scan used to swallow `OSError`/`UnicodeDecodeError` from an
unreadable or undecodable file. Both `continue`d. With four readable compliant
surfaces keeping the subject non-empty, a tracked non-UTF-8 page containing an
unguarded Releases link -- or a whole subtree that failed to enumerate -- left
this guard printing PASS. That is the repository's named defect shape: absence of
a signal reported as a passing signal. Every such input is now collected and
returned as exit 3, distinct from both "clean" (0) and "violations found" (1),
and distinct from the existing empty-subject non-vacuity failure (2).

## What this guard does NOT check

Whether the wording is *clear to a first-time downloader*. No automated check
substitutes for a person who has never built the project reading the page and
saying what performance they expect. That is stated in the PR that added this
guard and is deliberately not proxied by a readability metric here.

Exit codes:

* 0 -- pass.
* 1 -- violations found: a download surface without the warning or without the
  dashboard link.
* 2 -- the guard could not run meaningfully: no download surface discovered at
  all (see `--list`).
* 3 -- the guard could not inspect an authored input: a directory that would not
  enumerate, a Markdown file that would not read or decode, or a file whose code
  fences do not close. Inconclusive is a failure, not a skip.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

EXIT_OK = 0
EXIT_VIOLATIONS = 1
EXIT_EMPTY_SUBJECT = 2
EXIT_UNINSPECTABLE = 3

# The published download surface. Matching the org/repo rather than a generic
# "github.com/*/releases" keeps a quoted upstream Godot release link from being
# mistaken for ours. Deliberately NOT anchored to any link syntax -- see the
# module docstring: the syntax-anchored version was the reported defect.
RELEASES_URL_PATTERN = re.compile(
    r"https?://github\.com/klausi3D/godotGS/releases[^\s)>\"'\]]*",
    re.IGNORECASE,
)

# The link forms this repository's Markdown actually uses. These do NOT gate
# whether a file is a download surface (the URL scan above does that, so an
# unrecognised form still trips the guard); they name the form in the failure
# message, and they are what the performance-dashboard cross-reference is read
# through, where a destination -- not a raw URL match -- is what is needed.
INLINE_LINK_PATTERN = re.compile(r"\]\(\s*<?(?P<dest>[^)\s>]+)")
REFERENCE_DEFINITION_PATTERN = re.compile(
    r"^\s{0,3}\[[^\]\n]+\]:\s*<?(?P<dest>[^\s>]+)>?", re.MULTILINE
)
AUTOLINK_PATTERN = re.compile(r"<(?P<dest>[a-zA-Z][a-zA-Z0-9+.\-]*://[^>\s]+)>")
HTML_HREF_PATTERN = re.compile(
    r"href\s*=\s*(?:\"(?P<dq>[^\"]*)\"|'(?P<sq>[^']*)'|(?P<bare>[^\s\"'`=<>]+))",
    re.IGNORECASE,
)

# The named groups above that hold a link destination, whichever form matched.
DESTINATION_GROUPS = ("dest", "dq", "sq", "bare")

LINK_FORMS = (
    ("inline link", INLINE_LINK_PATTERN),
    ("reference definition", REFERENCE_DEFINITION_PATTERN),
    ("autolink", AUTOLINK_PATTERN),
    ("raw HTML href", HTML_HREF_PATTERN),
)

FENCE_PATTERN = re.compile(r"^(?P<indent>\s{0,3})(?P<fence>`{3,}|~{3,})(?P<info>.*)$")
INLINE_CODE_PATTERN = re.compile(r"(?P<ticks>`+)(?!`)(?P<body>.*?)(?<!`)(?P=ticks)(?!`)")

# Both must appear in the SAME paragraph. See module docstring.
REQUIRED_TOKENS = ("-O0", "dev_build")

PERFORMANCE_PAGE_RELPATH = Path("docs") / "performance" / "index.md"

# Pruned for speed and because nothing here is authored documentation of this
# project: VCS internals, build/tooling output, and the vendored upstream tree.
# `.site/` is the gitignored MkDocs staging tree (`.site/public-docs` is a rewritten
# COPY of `docs/`, `.site/site` the rendered HTML): scanning it double-counts pages
# already covered at their authored source, and a stale copy left over from an
# earlier build would fail this guard against text nobody can edit. Pruning a
# generated duplicate of an already-scanned input removes no coverage.
# Deliberately NOT pruned: `.github/`, `modules/`, `docs/reports/` and every other
# authored directory -- a download surface that appears in one of them must be
# caught, and an exclusion list is the artifact this guard exists to avoid.
PRUNED_DIRS = frozenset(
    {".git", ".site", "node_modules", "site", "bin", ".venv", "venv", "__pycache__"}
)
PRUNED_TOP_LEVEL = frozenset({"thirdparty"})


def iter_markdown_files(root: Path) -> tuple[list[Path], list[str]]:
    """Every Markdown file in the project, minus vendored and generated trees.

    Returns `(files, uninspectable)`. A directory that will not enumerate is
    reported, never skipped: an unread subtree is not an empty subtree, and the
    caller must not be able to mistake one for the other.
    """
    found: list[Path] = []
    uninspectable: list[str] = []
    stack = [root]
    while stack:
        directory = stack.pop()
        try:
            entries = list(directory.iterdir())
        except OSError as exc:
            uninspectable.append(
                f"{_display(directory, root)}: directory could not be enumerated "
                f"({type(exc).__name__}: {exc}). A subtree this guard cannot read may "
                "contain an unwarned download surface, so this is a failure, not a skip."
            )
            continue
        for entry in entries:
            try:
                is_dir = entry.is_dir()
            except OSError as exc:
                uninspectable.append(
                    f"{_display(entry, root)}: could not be stat'ed "
                    f"({type(exc).__name__}: {exc})."
                )
                continue
            if is_dir:
                if entry.name in PRUNED_DIRS:
                    continue
                if entry.parent == root and entry.name in PRUNED_TOP_LEVEL:
                    continue
                stack.append(entry)
            elif entry.suffix.lower() == ".md":
                found.append(entry)
    return sorted(found), uninspectable


def _display(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def mask_code(text: str) -> tuple[str, str | None]:
    """Blank out code spans and fenced blocks, preserving every offset.

    Masked characters become spaces and newlines are kept, so line numbers and
    match offsets computed on the result still address the original document.

    Returns `(masked, problem)`. `problem` is non-None when the document's fences
    do not close, in which case the caller must treat the file as uninspectable
    rather than trusting the mask: past an unterminated fence the guard cannot
    tell an offered link from a printed example, and guessing "code" would hide
    the exact thing it is looking for.
    """
    masked_lines: list[str] = []
    open_fence: tuple[str, int, int] | None = None
    open_line = 0
    for number, line in enumerate(text.splitlines(), start=1):
        if open_fence is not None:
            char, length, _indent = open_fence
            stripped = line.strip()
            if (
                stripped
                and set(stripped) == {char}
                and len(stripped) >= length
                and len(line) - len(line.lstrip()) <= 3
            ):
                open_fence = None
            masked_lines.append(" " * len(line))
            continue
        match = FENCE_PATTERN.match(line)
        # CommonMark: a backtick fence's info string may not contain a backtick,
        # which is what keeps an inline code span from being read as a fence. The
        # rule is specific to backtick fences; a tilde fence's info string may.
        if match is not None and not (
            match.group("fence")[0] == "`" and "`" in match.group("info")
        ):
            fence = match.group("fence")
            open_fence = (fence[0], len(fence), len(match.group("indent")))
            open_line = number
            masked_lines.append(" " * len(line))
            continue
        masked_lines.append(
            INLINE_CODE_PATTERN.sub(lambda m: " " * len(m.group(0)), line)
        )
    masked = "\n".join(masked_lines)
    if text.endswith("\n"):
        masked += "\n"
    if open_fence is not None:
        return masked, (
            f"a code fence opened on line {open_line} is never closed, so this guard "
            "cannot tell which of the text below it is code and which is an offered "
            "link. Close the fence."
        )
    return masked, None


def link_form_at(masked: str, start: int) -> str:
    """Name the link form a Releases-URL occurrence at `start` was written in.

    Reporting only. The verdict never depends on this: an occurrence no form
    matches is still a download surface, which is what keeps the rule keyed on
    the property rather than on the mechanisms enumerated here.
    """
    for name, pattern in LINK_FORMS:
        for match in pattern.finditer(masked):
            for key, value in match.groupdict().items():
                if key not in DESTINATION_GROUPS or value is None:
                    continue
                begin, end = match.span(key)
                if begin <= start < end:
                    return name
    return "bare URL"


def releases_occurrences(masked: str) -> list[tuple[int, str, str]]:
    """`(line, url, form)` for every offered Releases link in a masked document."""
    occurrences: list[tuple[int, str, str]] = []
    for match in RELEASES_URL_PATTERN.finditer(masked):
        line = masked.count("\n", 0, match.start()) + 1
        occurrences.append((line, match.group(0), link_form_at(masked, match.start())))
    return occurrences


def link_destinations(masked: str) -> list[str]:
    """Every link destination on the page, across all supported forms."""
    destinations: list[str] = []
    for _name, pattern in LINK_FORMS:
        for match in pattern.finditer(masked):
            groups = match.groupdict()
            for key in DESTINATION_GROUPS:
                value = groups.get(key)
                if value:
                    destinations.append(value)
    return destinations


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


def links_to_performance_page(path: Path, masked: str, root: Path) -> bool:
    """True when some link on the page resolves to the performance dashboard."""
    performance_page = (root / PERFORMANCE_PAGE_RELPATH).resolve()
    for destination in link_destinations(masked):
        target = destination.split("#", 1)[0].strip()
        if not target or "://" in target:
            continue
        try:
            resolved = (path.parent / target).resolve()
        except OSError:
            continue
        if resolved == performance_page:
            return True
    return False


def check(root: Path) -> tuple[list[Path], list[str], list[str]]:
    """Returns (discovered surfaces, violations, uninspectable inputs)."""
    files, uninspectable = iter_markdown_files(root)
    surfaces: list[Path] = []
    failures: list[str] = []

    for path in files:
        rel = _display(path, root)
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            uninspectable.append(
                f"{rel}: could not be read as UTF-8 ({type(exc).__name__}: {exc}). "
                "An authored page this guard cannot read may hand out an unwarned "
                "binary, so it is a failure, not a skip."
            )
            continue

        masked, problem = mask_code(text)
        if problem is not None:
            uninspectable.append(f"{rel}: {problem}")
            continue

        occurrences = releases_occurrences(masked)
        if not occurrences:
            continue
        surfaces.append(path)

        forms = ", ".join(sorted({form for _line, _url, form in occurrences}))
        lines = ", ".join(str(line) for line, _url, _form in occurrences)
        if not has_warning_paragraph(text):
            failures.append(
                f"{rel}: offers the GitHub Releases page ({forms}, line(s) {lines}) but "
                f"no single paragraph mentions both {REQUIRED_TOKENS[0]} and "
                f"{REQUIRED_TOKENS[1]}. Every published binary is an -O0 dev_build; a "
                "page that hands one out has to say so."
            )
        if not links_to_performance_page(path, masked, root):
            failures.append(
                f"{rel}: offers the GitHub Releases page ({forms}, line(s) {lines}) but "
                "never links to docs/performance/index.md, so a reader is told the "
                "download is slow and given nowhere to see what an optimized build "
                "measures."
            )

    return surfaces, failures, uninspectable


def _report_uninspectable(uninspectable: list[str]) -> None:
    print("[download-flavor-guard] FAIL (uninspectable input):", file=sys.stderr)
    for item in uninspectable:
        print(f"  - {item}", file=sys.stderr)
    print(
        "  This guard cannot report compliance for text it never read. Fix the input "
        "or prune it deliberately; do not let 'could not inspect' pass as 'inspected "
        "and compliant'.",
        file=sys.stderr,
    )


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

    surfaces, failures, uninspectable = check(root)

    if args.list:
        for path in surfaces:
            print(_display(path, root))
        print(f"[download-flavor-guard] {len(surfaces)} download surface(s) discovered.")
        if uninspectable:
            _report_uninspectable(uninspectable)
            return EXIT_UNINSPECTABLE
        return EXIT_OK if surfaces else EXIT_EMPTY_SUBJECT

    if uninspectable:
        # Fail-closed, and with its own exit code. "Could not inspect" collapsing
        # into "inspected and compliant" is this repository's named defect shape;
        # the caller must be able to tell the two apart from the exit status alone.
        _report_uninspectable(uninspectable)
        return EXIT_UNINSPECTABLE

    if not surfaces:
        # Non-vacuity, fail-closed. A guard whose subject has silently become empty
        # passes forever while protecting nothing -- which is the defect class this
        # repository keeps finding, not one it should add.
        print(
            "[download-flavor-guard] FAIL: no Markdown file offers the GitHub "
            "Releases page. Either the download surfaces were removed, or the URL "
            "pattern in this guard no longer matches how they are written. Run with "
            "--list and fix the pattern; do not let this pass silently.",
            file=sys.stderr,
        )
        return EXIT_EMPTY_SUBJECT

    if failures:
        print("[download-flavor-guard] FAIL:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return EXIT_VIOLATIONS

    print(
        f"[download-flavor-guard] PASS: {len(surfaces)} download surface(s) all carry "
        "the -O0 / dev_build warning and link to the performance dashboard."
    )
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
