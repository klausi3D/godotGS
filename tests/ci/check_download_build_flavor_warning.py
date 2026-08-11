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

## Two views of one document, and why they are deliberately different

Rounds 1-3 of review on this guard all found the same thing in a new costume:
**the guard passing on something the reader never sees.** An inline-only link
pattern missed reference-style links; a raw-text token search counted a warning
that only existed inside a fenced snippet; a definition-counting link extractor
credited an orphan `[dashboard]: ...` line that renders nothing at all. Patching
one authoring form per round is not a terminating process, so the rule is stated
once, as a property of *which* text each question is asked over:

* **Obligation -- "must this page carry the warning?" -- is asked over a
  superset of what the reader might see.** Every occurrence of the Releases URL
  outside certain code counts, in any syntax, recognised or not. When the guard
  is unsure whether something renders, it assumes it does, because the cost of
  being wrong is a page that hands out an unwarned binary.
* **Credit -- "does this page carry the warning?" -- is asked over a subset of
  what the reader certainly sees.** Fenced blocks (at any indentation), indented
  code blocks (measured relative to the enclosing container, not to column zero),
  HTML comments and images are removed; a reference definition counts as a link
  only when some reference link actually uses its label, and blocks are split
  where a reader sees a break, including a blank line written inside a blockquote
  as a bare `>`. When the guard is unsure whether something renders, it assumes
  it does not, because the cost of being wrong is a PASS the reader cannot see.

Both directions are meant to fail closed. Round three claimed more than that --
that an authoring form nobody anticipated could therefore only ever produce a
false FAIL. **That claim was wrong, and round four produced three
counter-examples, all false PASSes and all in the credit view:**

* an eight-space snippet inside a list item, because the indented-code mask
  asked *whether* a container was open rather than where its content began, so
  once inside one no indentation however deep was read as code;
* `![Performance Dashboard](../performance/index.md)`, because every link
  pattern matched the image syntax as a link;
* two blockquote paragraphs separated by the ordinary blank marker `>`, because
  `line.strip()` is non-empty there, so the two merged into one paragraph.

The asymmetry above is real, but it only holds for text the credit view
*classifies*. It says nothing about text the credit view classifies **wrongly**,
and each of those three was a misclassification: nested block structure it did
not model, and one inline syntax class it had never heard of. Each is fixed, and
the three fixes are structural rather than per-form -- indentation is measured
relative to the enclosing container instead of column zero, blockquote markers
are stripped before either question is asked, and images are masked before any
link pattern runs -- but the honest statement of the property is now:

**The credit view is a hand-written approximation of a Markdown renderer. Where
the approximation is right it fails closed. Where it is wrong it can fail either
way, and four rounds of review have found a new way each round.** The PR that
last touched this file records the recommendation for what to do about that; the
short version is that this shape does not terminate, and the terminating fixes
are to run the checks over rendered HTML or to constrain the warning to one
exactly-verifiable authoring form.

The one place ambiguity is escalated rather than resolved is the obligation
view, where treating an unterminated fence as code would *hide* a link: that is
exit 3. In the credit view the same ambiguity only ever withholds credit, which
is already the safe direction, so it is masked rather than escalated.

Inline code is the deliberate exception to "code is not prose". A warning
written as ``compiled with `dev_build=yes`, i.e. `-O0` `` renders, and is how
every page in this repository actually writes it, so the credit view keeps
inline-code *content* when looking for the warning tokens. It masks inline code
only when extracting links, where a `` `[x](y)` `` span renders as text rather
than as a link.

## What "carries the warning" means

Two things, both checkable:

1. **One paragraph containing both `-O0` and `dev_build`.** Co-occurrence within a
   single block, not merely somewhere in the file: `dev_build` appears in build
   docs for unrelated reasons (filename segments, flag tables), and `-O0` could
   drift into an unrelated aside. Requiring them in the same paragraph is what
   makes the match evidence of an actual warning rather than of two coincidences
   -- which only holds while "paragraph" means what a reader sees as one block,
   so `paragraphs()` splits on a blockquote's blank `>` marker, on a blockquote
   that opens mid-paragraph, and on a heading or rule that interrupts one.
2. **A link to the performance dashboard.** A warning that says "this is slow" and
   stops there tells a reader nothing they can act on. Requiring the cross-
   reference also puts these pages under `scripts/docs/check_links.py`, so a
   renamed heading or moved page fails a second, independent check.

The dashboard link is read through the same multi-form extractor, so a page that
writes it reference-style satisfies the rule the same way an inline one does --
but an *image* of the dashboard does not. `![Performance Dashboard](...)` renders
a picture and nothing a reader can follow, in every reference form as well, so
images are masked before any link pattern runs.

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

"Decoded without raising" is not the same as "was read as authored". A BOM-less
UTF-16 file is the counter-example that survived the first version of this rule:
its NUL bytes are valid UTF-8, so `read_text` succeeds and returns
`[\x00N\x00i\x00g...`, in which no URL pattern can match. The page is then not a
download surface, and the guard passes -- the exact swallow the exit-3 path
exists to close, reached through a decode that never failed. A decoded document
containing NUL is therefore uninspectable too.

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
    r"^\s{0,3}\[(?P<label>[^\]\n]+)\]:\s*<?(?P<dest>[^\s>]+)>?", re.MULTILINE
)
AUTOLINK_PATTERN = re.compile(r"<(?P<dest>[a-zA-Z][a-zA-Z0-9+.\-]*://[^>\s]+)>")
HTML_HREF_PATTERN = re.compile(
    r"href\s*=\s*(?:\"(?P<dq>[^\"]*)\"|'(?P<sq>[^']*)'|(?P<bare>[^\s\"'`=<>]+))",
    re.IGNORECASE,
)

# A reference DEFINITION renders nothing on its own; a reference LINK is what
# renders, and only when a definition matches its label. `[text][label]` and the
# collapsed `[label][]` are the first form; a bare `[label]` not followed by `(`,
# `[` or `:` is the shortcut form (the `:` exclusion is what stops a definition
# line from being read as a use of itself).
FULL_REFERENCE_PATTERN = re.compile(r"\[(?P<text>[^\]\n]*)\]\[(?P<label>[^\]\n]*)\]")
SHORTCUT_REFERENCE_PATTERN = re.compile(r"\[(?P<label>[^\]\n]+)\](?![\(\[:])")

# `![alt](dest)` is an image, not a link: a reader is shown a picture and given
# nothing to follow. Every link pattern above matches it anyway -- the inline
# one because it keys on `](`, the reference ones because `![label]` contains
# `[label]` -- so a page whose only dashboard reference was an image was
# credited with a cross-reference that does not exist (Codex, PR #872, round 4).
# The credit view therefore blanks image spans BEFORE any link pattern runs.
# Doing it as a mask rather than as a negative look-behind on each pattern is
# what also gets the linked-image case right: `[![alt](icon.png)](dest)` masks to
# `[        ](dest)`, whose destination is `dest` -- the one a reader follows --
# instead of `icon.png`.
IMAGE_PATTERN = re.compile(r"!\[[^\]\n]*\](?:\([^)\n]*\)|\[[^\]\n]*\])?")

# The named groups above that hold a link destination, whichever form matched.
DESTINATION_GROUPS = ("dest", "dq", "sq", "bare")

LINK_FORMS = (
    ("inline link", INLINE_LINK_PATTERN),
    ("reference definition", REFERENCE_DEFINITION_PATTERN),
    ("autolink", AUTOLINK_PATTERN),
    ("raw HTML href", HTML_HREF_PATTERN),
)

# Link forms that render on their own, with no second half elsewhere on the page.
DIRECT_LINK_FORMS = (INLINE_LINK_PATTERN, AUTOLINK_PATTERN, HTML_HREF_PATTERN)

# CommonMark allows a fence to be indented up to three spaces relative to its
# container, so at top level `FENCE_PATTERN` is the rule. Inside a list item or an
# admonition the same fence is indented further, which the top-level pattern
# cannot see -- fine for the obligation view (missing a fence only ever adds a
# surface) and wrong for the credit view, which uses `FENCE_ANY_INDENT_PATTERN`.
FENCE_PATTERN = re.compile(r"^(?P<indent>\s{0,3})(?P<fence>`{3,}|~{3,})(?P<info>.*)$")
FENCE_ANY_INDENT_PATTERN = re.compile(
    r"^(?P<indent>\s*)(?P<fence>`{3,}|~{3,})(?P<info>.*)$"
)
INLINE_CODE_PATTERN = re.compile(r"(?P<ticks>`+)(?!`)(?P<body>.*?)(?<!`)(?P=ticks)(?!`)")
HTML_COMMENT_PATTERN = re.compile(r"<!--.*?(?:-->|\Z)", re.DOTALL)

# A four-space indent is an indented code block only at top level. Inside a
# container it is that container's content and renders as ordinary prose -- which
# is exactly how all three MkDocs download pages in this repository write their
# warning. The first version of this rule tracked only *whether* a container was
# open, so once one was, no indentation however deep was ever read as code: an
# eight-space snippet inside a list item was credited as a warning (Codex,
# PR #872, round 4). Whether a container is open is the wrong quantity. The right
# one is the column its content starts at, because "indented code" means four
# columns past *that*, so these patterns yield a content indent rather than a
# boolean.
#
# `>` and `<` are deliberately absent. A blockquote is handled by stripping its
# markers (`BLOCKQUOTE_PREFIX_PATTERN`) so indentation inside a quote is measured
# where the reader sees it -- `>     x` is a four-space indent in the quote, and
# reading it as column zero hid indented code inside quotes entirely. An HTML
# block ends at the first blank line, and an indented code block needs a blank
# line before it, so treating `<div>` as an open container only suppressed the
# mask on lines where CommonMark says the block has already closed.
FOUR_SPACE_CONTAINER_PATTERN = re.compile(r'^[ \t]*(?:!!!|\?\?\?\+?|===[ \t]+")')
LIST_MARKER_PATTERN = re.compile(
    r"^(?P<indent>[ \t]*)(?P<marker>[-*+]|\d{1,9}[.)])(?P<space>[ \t]*)(?P<rest>.*)$"
)
THEMATIC_BREAK_PATTERN = re.compile(
    r"^[ \t]{0,3}(?:(?:\*[ \t]*){3,}|(?:-[ \t]*){3,}|(?:_[ \t]*){3,})$"
)
ATX_HEADING_PATTERN = re.compile(r"^[ \t]{0,3}#{1,6}(?:[ \t]|$)")

# A blockquote's content begins after its `>` markers, so both the credit view's
# indentation arithmetic and its paragraph splitting have to be done on what
# follows them. Matching `>` as an opaque "container is open" flag meant `>` on
# its own -- the ordinary way to put a blank line inside a quote -- was neither
# blank nor a container boundary, so two quoted paragraphs merged into one and
# satisfied the same-paragraph rule between them (Codex, PR #872, round 4).
BLOCKQUOTE_PREFIX_PATTERN = re.compile(r"^(?:[ \t]{0,3}>[ \t]?)+")

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


def _rejoin(text: str, lines: list[str]) -> str:
    joined = "\n".join(lines)
    return joined + "\n" if text.endswith("\n") else joined


def _mask_fenced_blocks(text: str, *, any_indent: bool) -> tuple[str, int]:
    """Blank fenced code blocks. Returns `(masked, unclosed_fence_line)`.

    `unclosed_fence_line` is 0 when every fence closed. `any_indent` selects the
    credit view's rule (a fence indented into a list item or an admonition is
    still a fence) over the obligation view's top-level CommonMark rule.
    """
    pattern = FENCE_ANY_INDENT_PATTERN if any_indent else FENCE_PATTERN
    masked_lines: list[str] = []
    open_fence: tuple[str, int] | None = None
    open_line = 0
    for number, line in enumerate(text.splitlines(), start=1):
        if open_fence is not None:
            char, length = open_fence
            stripped = line.strip()
            if (
                stripped
                and set(stripped) == {char}
                and len(stripped) >= length
                and (any_indent or len(line) - len(line.lstrip()) <= 3)
            ):
                open_fence = None
                open_line = 0
            masked_lines.append(" " * len(line))
            continue
        match = pattern.match(line)
        # CommonMark: a backtick fence's info string may not contain a backtick,
        # which is what keeps an inline code span from being read as a fence. The
        # rule is specific to backtick fences; a tilde fence's info string may.
        if match is not None and not (
            match.group("fence")[0] == "`" and "`" in match.group("info")
        ):
            fence = match.group("fence")
            open_fence = (fence[0], len(fence))
            open_line = number
            masked_lines.append(" " * len(line))
            continue
        masked_lines.append(line)
    return _rejoin(text, masked_lines), open_line if open_fence is not None else 0


def mask_inline_code(text: str) -> str:
    """Blank inline code spans, line by line, preserving every offset."""
    return _rejoin(
        text,
        [
            INLINE_CODE_PATTERN.sub(lambda m: " " * len(m.group(0)), line)
            for line in text.splitlines()
        ],
    )


def mask_code(text: str) -> tuple[str, str | None]:
    """The OBLIGATION view: blank code spans and fenced blocks, keeping offsets.

    Masked characters become spaces and newlines are kept, so line numbers and
    match offsets computed on the result still address the original document.

    Returns `(masked, problem)`. `problem` is non-None when the document's fences
    do not close, in which case the caller must treat the file as uninspectable
    rather than trusting the mask: past an unterminated fence the guard cannot
    tell an offered link from a printed example, and guessing "code" would hide
    the exact thing it is looking for.
    """
    masked, unclosed = _mask_fenced_blocks(text, any_indent=False)
    masked = mask_inline_code(masked)
    if unclosed:
        return masked, (
            f"a code fence opened on line {unclosed} is never closed, so this guard "
            "cannot tell which of the text below it is code and which is an offered "
            "link. Close the fence."
        )
    return masked, None


def split_quote_prefix(line: str) -> tuple[int, str]:
    """`(blockquote depth, the line as it reads inside that quote)`.

    `> > text` is depth 2 and `text`; a bare `>` is depth 1 and the empty
    string, which is how a blank line inside a blockquote is written.
    """
    match = BLOCKQUOTE_PREFIX_PATTERN.match(line)
    if match is None:
        return 0, line
    return match.group(0).count(">"), line[match.end() :]


def container_content_indent(line: str) -> int | None:
    """The column a container opened on `line` indents its content to, or None.

    An admonition, a collapsible admonition and a content tab (all enabled in
    `mkdocs.yml`) indent their bodies four columns from the marker. A list item's
    content starts after its marker and the spaces following it -- except that
    CommonMark caps that run at one column when five or more spaces follow,
    because everything past the first is then an indented code block *inside* the
    item, and except that a marker with no space after it (`-foo`) is not a list
    at all.
    """
    if THEMATIC_BREAK_PATTERN.match(line):
        return None
    if FOUR_SPACE_CONTAINER_PATTERN.match(line):
        return len(line) - len(line.lstrip()) + 4
    match = LIST_MARKER_PATTERN.match(line)
    if match is None:
        return None
    spaces = len(match.group("space"))
    if not match.group("rest").strip():
        spaces = 1
    elif spaces == 0:
        return None
    elif spaces > 4:
        spaces = 1
    return len(match.group("indent")) + len(match.group("marker")) + spaces


def _mask_indented_code(text: str) -> str:
    """Blank indented code blocks, at top level and inside containers alike.

    A run of lines is an indented code block when it starts after a blank line
    (CommonMark: it cannot interrupt a paragraph) and is indented four or more
    columns past where the innermost open container's content begins. At top
    level that reduces to the familiar four spaces; inside a list item whose
    content starts at column 2 it is six, and inside a blockquote it is four
    columns past the `>` markers.

    The container stack holds `(blockquote depth, content indent)` and is popped
    by any line that steps back out, so a snippet indented eight spaces under a
    list item is masked while the item's own four-space continuation is not.
    """
    masked_lines: list[str] = []
    stack: list[tuple[int, int]] = []
    previous_blank = True
    in_code = False
    for line in text.splitlines():
        depth, rest = split_quote_prefix(line)
        if not rest.strip():
            masked_lines.append(line)
            previous_blank = True
            in_code = False
            continue
        indent = len(rest) - len(rest.lstrip())
        while stack and (
            stack[-1][0] > depth or (stack[-1][0] == depth and indent < stack[-1][1])
        ):
            stack.pop()
        base = stack[-1][1] if stack and stack[-1][0] == depth else 0
        if indent >= base + 4 and (in_code or previous_blank):
            in_code = True
            masked_lines.append(" " * len(line))
        else:
            in_code = False
            content_indent = container_content_indent(rest)
            if content_indent is not None:
                stack.append((depth, content_indent))
            masked_lines.append(line)
        previous_blank = False
    return _rejoin(text, masked_lines)


def _mask_images(text: str) -> str:
    """Blank `![...]`, `![...](...)` and `![...][...]`, preserving every offset."""
    return IMAGE_PATTERN.sub(lambda m: " " * len(m.group(0)), text)


def _mask_html_comments(text: str) -> str:
    """Blank `<!-- ... -->`, including an unterminated one, preserving offsets.

    A comment renders nothing, so a warning written inside one is a warning the
    reader never sees. An unterminated comment is masked to the end of the file
    rather than escalated: in the credit view, over-masking only withholds
    credit, which is already the fail-closed direction.
    """
    return HTML_COMMENT_PATTERN.sub(
        lambda m: "".join("\n" if c == "\n" else " " for c in m.group(0)), text
    )


def rendered_prose(text: str) -> str:
    """The CREDIT view: what a reader actually sees, minus every code form.

    Inline code is deliberately kept -- see the module docstring. Use
    `mask_inline_code()` on the result when extracting links from it.
    """
    masked, _unclosed = _mask_fenced_blocks(text, any_indent=True)
    return _mask_html_comments(_mask_indented_code(masked))


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


def _normalise_label(label: str) -> str:
    """CommonMark link-label matching: case-insensitive, whitespace-collapsed."""
    return " ".join(label.split()).casefold()


def referenced_labels(text: str) -> set[str]:
    """Every link label the page actually *uses*, in any reference-link form."""
    labels: set[str] = set()
    for match in FULL_REFERENCE_PATTERN.finditer(text):
        # `[text][]` is the collapsed form: the label is the link text.
        labels.add(_normalise_label(match.group("label") or match.group("text")))
    for match in SHORTCUT_REFERENCE_PATTERN.finditer(text):
        labels.add(_normalise_label(match.group("label")))
    return labels


def rendered_link_destinations(text: str) -> list[str]:
    """Every destination that RENDERS as a link, across all supported forms.

    A reference definition is not a link. `[dashboard]: ../performance/index.md`
    with no `[dashboard]` anywhere else on the page renders nothing at all, so
    counting the definition credited the author with a cross-reference the reader
    was never given (Codex, PR #872). Definitions are therefore resolved through
    the labels the page actually uses, and an orphan definition contributes
    nothing.

    Images are masked out first, in every form, because an image is something a
    reader looks at rather than something they can follow -- and both
    `![Dashboard](../performance/index.md)` and `![dashboard]` with a matching
    definition were being counted as the cross-reference (Codex, PR #872,
    round 4).
    """
    text = _mask_images(text)
    destinations: list[str] = []
    for pattern in DIRECT_LINK_FORMS:
        for match in pattern.finditer(text):
            groups = match.groupdict()
            for key in DESTINATION_GROUPS:
                value = groups.get(key)
                if value:
                    destinations.append(value)

    definitions: dict[str, str] = {}
    for match in REFERENCE_DEFINITION_PATTERN.finditer(text):
        definitions.setdefault(_normalise_label(match.group("label")), match.group("dest"))
    if definitions:
        for label in referenced_labels(text):
            destination = definitions.get(label)
            if destination:
                destinations.append(destination)
    return destinations


def paragraphs(text: str) -> list[str]:
    """Split rendered text into blocks a reader sees as separate.

    A blank line ends a block -- and so does a blank line *inside* a blockquote,
    which is written as a bare `>` marker. Testing `line.strip()` missed that, so
    two quoted paragraphs merged and `dev_build` in one plus `-O0` in the other
    satisfied the same-paragraph contract between them (Codex, PR #872, round 4).

    Two more block boundaries fall out of the same reading, and are taken here
    because they are exact: a blockquote that *opens* on the line after a
    paragraph interrupts it, and an ATX heading or a thematic break is its own
    block. A quote depth that decreases does NOT split -- that is a lazy
    continuation, still the same paragraph.

    An admonition body, a list continuation and a multi-line blockquote all stay
    one block, which is how every warning in this repository is written.
    """
    blocks: list[str] = []
    current: list[str] = []
    previous_depth = 0

    def flush() -> None:
        if current:
            blocks.append("\n".join(current))
            current.clear()

    for line in text.splitlines():
        depth, rest = split_quote_prefix(line)
        if not rest.strip():
            flush()
            previous_depth = 0
            continue
        own_block = (
            ATX_HEADING_PATTERN.match(rest) is not None
            or THEMATIC_BREAK_PATTERN.match(rest) is not None
        )
        if own_block or depth > previous_depth:
            flush()
        current.append(line)
        previous_depth = depth
        if own_block:
            flush()
    flush()
    return blocks


def has_warning_paragraph(prose: str) -> bool:
    """True when one rendered paragraph carries both tokens.

    Takes the CREDIT view (`rendered_prose()`), not the raw file: tokens that
    only exist inside a fenced snippet, an indented code block or an HTML comment
    are text the reader is never shown, and crediting them was a reported defect
    (Codex, PR #872).
    """
    return any(
        all(token in block for token in REQUIRED_TOKENS) for block in paragraphs(prose)
    )


def links_to_performance_page(path: Path, prose: str, root: Path) -> bool:
    """True when some RENDERED link on the page resolves to the dashboard."""
    performance_page = (root / PERFORMANCE_PAGE_RELPATH).resolve()
    for destination in rendered_link_destinations(mask_inline_code(prose)):
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

        if "\x00" in text:
            # Decoded without raising, but not read as authored: a BOM-less
            # UTF-16 page is valid UTF-8 (its NULs are), and comes back as
            # `[\x00N\x00i...` in which no pattern can match. Silently not a
            # download surface is the exact swallow exit 3 exists to close.
            uninspectable.append(
                f"{rel}: decoded as UTF-8 but contains NUL bytes, so it is not the "
                "text an author wrote (a BOM-less UTF-16 file decodes this way). "
                "No pattern can match it, which would make an unwarned download "
                "surface invisible; re-save the file as UTF-8."
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

        # Obligation was decided above, on the permissive view. Credit below is
        # decided on the strict one: only what the reader is actually shown.
        prose = rendered_prose(text)

        forms = ", ".join(sorted({form for _line, _url, form in occurrences}))
        lines = ", ".join(str(line) for line, _url, _form in occurrences)
        if not has_warning_paragraph(prose):
            failures.append(
                f"{rel}: offers the GitHub Releases page ({forms}, line(s) {lines}) but "
                f"no single paragraph mentions both {REQUIRED_TOKENS[0]} and "
                f"{REQUIRED_TOKENS[1]}. Every published binary is an -O0 dev_build; a "
                "page that hands one out has to say so. Note that a code block, an "
                "indented snippet and an HTML comment do not count: the reader has to "
                "be able to see it."
            )
        if not links_to_performance_page(path, prose, root):
            failures.append(
                f"{rel}: offers the GitHub Releases page ({forms}, line(s) {lines}) but "
                "never links to docs/performance/index.md, so a reader is told the "
                "download is slow and given nowhere to see what an optimized build "
                "measures. A reference definition with no matching reference link "
                "renders nothing and does not count."
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
