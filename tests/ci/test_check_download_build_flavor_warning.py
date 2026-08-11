#!/usr/bin/env python3
"""Discrimination tests for `check_download_build_flavor_warning.py`.

A guard that passes on the committed tree proves the tree is clean today. It does
not prove the guard can fail -- and a guard that cannot fail is the defect this
repository keeps finding, not a check. Every case below therefore either drives a
synthetic tree the guard must REJECT, or is an explicit non-vacuity control on the
real tree.

The two mutations named in the milestone plan are `test_deleting_the_warning_is_red`
(delete the admonition from an existing download page) and
`test_new_page_linking_to_releases_without_a_warning_is_red` (add a page that hands
out the download and says nothing). Both are here, and both are asserted on the
guard's real exit code, not on an internal helper.

Two further families were added after review (Codex, PR #872), each pinning a way
the first version of the guard printed PASS without having checked anything:

* `LinkFormTests` -- the guard keyed on the inline `](URL)` form alone, so a
  reference-style link, a raw HTML anchor, an autolink or a bare URL handed out an
  unwarned binary invisibly. Measured before the fix: each of those pages left the
  guard at exit 0 on the real tree, because the four inline-link surfaces kept the
  subject non-empty and the exit-2 non-vacuity control satisfied.
* `UninspectableInputTests` -- an unreadable directory, an undecodable file and an
  unterminated code fence were all silently `continue`d. Measured before the fix:
  a tracked latin-1 page containing an unguarded Releases link left the guard at
  exit 0. "Could not inspect" must not read as "inspected and compliant"; it is
  now exit 3.

Round three added two more families, both instances of one property -- the guard
must ask each question over the text the reader is actually shown:

* `RenderedProseTests` -- `has_warning_paragraph()` read the raw file, so a page
  whose only `-O0` / `dev_build` mention sat inside a fenced snippet, an indented
  code block or an HTML comment was reported compliant while rendering no warning
  at all. Measured before the fix: each of those pages was exit 0, alone in its
  tree, with the guard printing "1 download surface(s) all carry the ... warning".
* `RenderedLinkTests` -- an orphan `[dashboard]: ../performance/index.md`
  definition that no reference link uses renders nothing, and was accepted as the
  dashboard cross-reference. Measured before the fix: exit 0.

Both families also assert the opposite direction, because a fix that passes by
rejecting everything is not a fix: a warning written with ordinary inline code,
one in a four-space-indented admonition body (which is how all three MkDocs pages
in this repository write theirs), and a genuinely referenced `[dashboard]` label
must all still be GREEN.

Round four found three more, all in the credit view and all false PASSes, which
is why round three's claim that an unmodelled form could only cause a false FAIL
was too strong:

* `NestedIndentedCodeTests` -- the mask asked *whether* a container was open, not
  where its content started, so once inside a list item no indentation however
  deep was read as code. Measured before the fix: a page whose only warning was
  an eight-space snippet under `- Example only:` was exit 0. The blockquote
  variant (`>` then a four-space snippet) was invisible for the same reason,
  because indentation inside a quote was measured before the `>` markers.
* `ImageTests` -- `![Performance Dashboard](../performance/index.md)` matched the
  inline-link pattern, and `![dashboard]` matched the shortcut-reference pattern.
  Measured before the fix: exit 0 for all three image forms, on pages that render
  a picture and no link at all.
* `BlockBoundaryTests` -- a blank line inside a blockquote is a bare `>`, whose
  `.strip()` is non-empty, so two quoted paragraphs merged and satisfied the
  same-paragraph contract between them. Measured before the fix: exit 0.

Each family again asserts both directions. The GREEN controls that pin the
over-correction boundary are the four-space list continuation (which must NOT
become an indented code block now that the mask reads indentation inside
containers), the six-space continuation of a *nested* item, the multi-line
`> [!WARNING]` blockquote and its lazy continuation (which must NOT be split now
that the splitter reads quote markers), and a linked image
`[![Chart](chart.png)](../performance/index.md)`, whose destination is a real
link a reader can follow.
"""

from __future__ import annotations

import io
import contextlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_download_build_flavor_warning as guard  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

RELEASES_URL = "https://github.com/klausi3D/godotGS/releases"
RELEASES_LINK = f"[GitHub Releases]({RELEASES_URL})"

WARNING_BLOCK = (
    "!!! warning \"Nightly binaries are unoptimized `-O0` builds\"\n"
    "    Published nightlies are compiled with `dev_build=yes`, i.e. `-O0`.\n"
    "    See the [Performance Dashboard](../performance/index.md#measurement-environment).\n"
)


def _run_guard(root: Path, *extra: str) -> tuple[int, str]:
    """Invoke the guard exactly as CI does, capturing its exit code and output."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = guard.main(["--root", str(root), *extra])
    return code, out.getvalue() + err.getvalue()


class _SyntheticTree(unittest.TestCase):
    """A minimal repo shape: a performance dashboard plus a pages directory."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "docs" / "performance").mkdir(parents=True)
        (self.root / "docs" / "performance" / "index.md").write_text(
            "# Performance Dashboard\n\n### Measurement environment\n", encoding="utf-8"
        )
        self.pages = self.root / "docs" / "getting-started"
        self.pages.mkdir(parents=True)
        self.addCleanup(self._tmp.cleanup)

    def _write_page(self, name: str, body: str) -> Path:
        path = self.pages / name
        path.write_text(body, encoding="utf-8")
        return path

    def _compliant_page(self, name: str = "downloads.md") -> Path:
        return self._write_page(
            name, f"# Downloads\n\n{RELEASES_LINK}\n\n{WARNING_BLOCK}"
        )


class SyntheticTreeTests(_SyntheticTree):
    """Each case builds a minimal repo shape and asserts the guard's verdict."""

    # --- the control: the fixture the other cases mutate must itself be GREEN ---

    def test_compliant_page_is_green(self) -> None:
        self._compliant_page()
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)
        self.assertIn("1 download surface", output)

    # --- mutation 1: delete the admonition ---

    def test_deleting_the_warning_is_red(self) -> None:
        page = self._compliant_page()
        page.write_text(f"# Downloads\n\n{RELEASES_LINK}\n", encoding="utf-8")
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("downloads.md", output)
        self.assertIn("-O0", output)

    # --- mutation 2: a NEW page that links to Releases and says nothing ---

    def test_new_page_linking_to_releases_without_a_warning_is_red(self) -> None:
        self._compliant_page()
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)

        self._write_page("shiny-new-download-page.md", f"# Get It\n\n{RELEASES_LINK}\n")
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("shiny-new-download-page.md", output)
        self.assertNotIn("downloads.md:", output)

    # --- the co-occurrence rule is what makes the match evidence of a warning ---

    def test_tokens_split_across_paragraphs_is_red(self) -> None:
        self._write_page(
            "downloads.md",
            "# Downloads\n\n"
            f"{RELEASES_LINK}\n\n"
            "The binary name carries a `dev_build` segment.\n\n"
            "Unrelated aside about `-O0` somewhere else entirely.\n\n"
            "[Performance Dashboard](../performance/index.md)\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("no single paragraph", output)

    # --- the actionability half of "carries the warning" ---

    def test_warning_without_a_performance_link_is_red(self) -> None:
        self._write_page(
            "downloads.md",
            "# Downloads\n\n"
            f"{RELEASES_LINK}\n\n"
            "Nightlies are `dev_build=yes`, i.e. `-O0`.\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("docs/performance/index.md", output)

    def test_absolute_url_to_the_dashboard_does_not_satisfy_the_link_rule(self) -> None:
        """Only a repo-relative link is checkable by `check_links.py`."""
        self._write_page(
            "downloads.md",
            "# Downloads\n\n"
            f"{RELEASES_LINK}\n\n"
            "Nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "[Dashboard](https://example.invalid/docs/performance/index.md)\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)

    # --- the shape rule: a mention is not an offer ---

    def test_backticked_url_is_not_a_download_surface(self) -> None:
        self._compliant_page()
        self._write_page(
            "archived-audit.md",
            f"# Audit\n\nAdd a link to `{RELEASES_URL}/latest`.\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)
        self.assertIn("1 download surface", output)

    def test_url_inside_a_fenced_block_is_not_a_download_surface(self) -> None:
        """A printed example is not an offer either -- but see the fence tests."""
        self._compliant_page()
        self._write_page(
            "example.md",
            "# Example\n\n```console\n"
            f"$ curl -L {RELEASES_URL}/latest\n"
            "```\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)
        self.assertIn("1 download surface", output)

    def test_the_mkdocs_staging_copy_is_not_counted_as_a_second_surface(self) -> None:
        """`.site/` is a generated copy of `docs/`, already covered at its source.

        Without this the derived count doubles on any checkout where the docs site
        has been staged locally, and a stale copy could fail the guard against text
        nobody can edit. The authored original is still scanned -- see the second
        half, which is what makes this a de-duplication and not an exemption.
        """
        self._compliant_page()
        staged = self.root / ".site" / "public-docs" / "getting-started"
        staged.mkdir(parents=True)
        (staged / "downloads.md").write_text(
            f"# Downloads\n\n{RELEASES_LINK}\n", encoding="utf-8"
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)
        self.assertIn("1 download surface", output)

        # ... and pruning the copy did not prune the source it was copied from.
        (self.pages / "downloads.md").write_text(
            f"# Downloads\n\n{RELEASES_LINK}\n", encoding="utf-8"
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("docs/getting-started/downloads.md", output)

    def test_a_different_repos_releases_page_is_not_our_download_surface(self) -> None:
        self._compliant_page()
        self._write_page(
            "upstream.md",
            "# Upstream\n\n[Godot Releases](https://github.com/godotengine/godot/releases)\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)
        self.assertIn("1 download surface", output)

    # --- non-vacuity, fail-closed ---

    def test_empty_subject_is_a_failure_not_a_pass(self) -> None:
        """If nothing matches, the guard must not report success.

        This is the failure mode the guard would otherwise have: misspell the link
        pattern and it protects nothing while staying green forever.
        """
        self._write_page("downloads.md", "# Downloads\n\nNo link here.\n")
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_EMPTY_SUBJECT, output)
        self.assertIn("no Markdown file offers", output)

    def test_misspelling_the_url_pattern_is_a_failure_not_a_pass(self) -> None:
        """The non-vacuity control stated as the mutation it exists to catch.

        A compliant tree plus a pattern that no longer matches how the link is
        written must be exit 2, not exit 0.
        """
        self._compliant_page()
        broken = guard.re.compile(r"https?://github\.com/klausi3D/godotGS/releeses")
        with mock.patch.object(guard, "RELEASES_URL_PATTERN", broken):
            code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_EMPTY_SUBJECT, output)

    def test_list_mode_on_an_empty_subject_also_fails(self) -> None:
        self._write_page("downloads.md", "# Downloads\n\nNo link here.\n")
        code, output = _run_guard(self.root, "--list")
        self.assertEqual(code, guard.EXIT_EMPTY_SUBJECT, output)


class LinkFormTests(_SyntheticTree):
    """Coverage must follow the property, not one authoring mechanism (Codex #872).

    Every case here writes ONE unwarned download page in a form the original
    inline-only pattern could not see, alongside a compliant page that keeps the
    subject non-empty -- which is precisely why the old guard stayed green: the
    non-vacuity control was satisfied by somebody else's page.
    """

    def _assert_unwarned_page_is_red(self, body: str, expected_form: str) -> None:
        self._compliant_page()
        self.assertEqual(_run_guard(self.root)[0], guard.EXIT_OK)
        self._write_page("new-download-page.md", body)
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("new-download-page.md", output)
        self.assertIn(expected_form, output)

    def test_reference_style_link_without_a_warning_is_red(self) -> None:
        self._assert_unwarned_page_is_red(
            "# Get It\n\nGrab it from [Get nightly][releases].\n\n"
            f"[releases]: {RELEASES_URL}\n",
            "reference definition",
        )

    def test_reference_style_link_with_a_title_is_still_seen(self) -> None:
        self._assert_unwarned_page_is_red(
            "# Get It\n\nGrab it from [Get nightly][releases].\n\n"
            f'[releases]: {RELEASES_URL} "Nightly builds"\n',
            "reference definition",
        )

    def test_raw_html_anchor_without_a_warning_is_red(self) -> None:
        self._assert_unwarned_page_is_red(
            f'# Get It\n\n<a href="{RELEASES_URL}">Download the nightly</a>\n',
            "raw HTML href",
        )

    def test_single_quoted_raw_html_anchor_without_a_warning_is_red(self) -> None:
        self._assert_unwarned_page_is_red(
            f"# Get It\n\n<a href='{RELEASES_URL}' class='btn'>Download</a>\n",
            "raw HTML href",
        )

    def test_autolink_without_a_warning_is_red(self) -> None:
        self._assert_unwarned_page_is_red(
            f"# Get It\n\nOpen <{RELEASES_URL}> and pick the newest entry.\n",
            "autolink",
        )

    def test_bare_url_without_a_warning_is_red(self) -> None:
        """GitHub's renderer turns a bare URL into a live link whatever was meant."""
        self._assert_unwarned_page_is_red(
            f"# Get It\n\nOpen {RELEASES_URL} and pick the newest entry.\n",
            "bare URL",
        )

    def test_a_compliant_reference_style_page_is_green(self) -> None:
        """The rule must accept the new forms, not merely reject them.

        Both halves are written reference-style here -- the Releases link and the
        dashboard cross-reference -- so this fails if the destination extractor
        still understands only inline links.
        """
        self._write_page(
            "refstyle.md",
            "# Get It\n\nGrab it from [Get nightly][releases].\n\n"
            "Published nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "See the [Performance Dashboard][dashboard].\n\n"
            f"[releases]: {RELEASES_URL}\n"
            "[dashboard]: ../performance/index.md\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)
        self.assertIn("1 download surface", output)

    def test_a_raw_html_dashboard_link_satisfies_the_cross_reference(self) -> None:
        self._write_page(
            "htmlpage.md",
            f'# Get It\n\n<a href="{RELEASES_URL}">Download</a>\n\n'
            "Published nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            '<a href="../performance/index.md">Performance Dashboard</a>\n',
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)


class RenderedProseTests(_SyntheticTree):
    """Credit is asked over what renders, not over the file (Codex #872, round 3).

    Every RED case here writes a page whose ONLY mention of the two tokens is in
    text a reader never sees. Every GREEN case writes a warning in a form this
    repository actually uses, so the rule cannot pass by rejecting everything.
    """

    def _assert_red(self, body: str) -> None:
        self._write_page("downloads.md", body)
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("no single paragraph", output)

    def _assert_green(self, body: str) -> None:
        self._write_page("downloads.md", body)
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)
        self.assertIn("1 download surface", output)

    # --- RED: the tokens exist, but only where nothing is rendered ---

    def test_tokens_only_inside_a_fenced_block_are_not_a_warning(self) -> None:
        self._assert_red(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "[Performance Dashboard](../performance/index.md)\n\n"
            "```ini\ndev_build=yes   # -O0\n```\n"
        )

    def test_tokens_only_inside_an_indented_fence_are_not_a_warning(self) -> None:
        """A fence inside a list item is indented past the top-level fence rule.

        The obligation view deliberately keeps the CommonMark top-level rule --
        failing to see a fence there only ever adds a download surface. The credit
        view cannot afford that error, so it reads fences at any indentation.
        """
        self._assert_red(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "[Performance Dashboard](../performance/index.md)\n\n"
            "- The nightly lane is configured like this:\n\n"
            "      ```ini\n      dev_build=yes   # -O0\n      ```\n"
        )

    def test_tokens_only_inside_an_indented_code_block_are_not_a_warning(self) -> None:
        self._assert_red(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "[Performance Dashboard](../performance/index.md)\n\n"
            "Release lane configuration:\n\n"
            "    dev_build=yes   # -O0\n"
        )

    def test_tokens_only_inside_an_html_comment_are_not_a_warning(self) -> None:
        self._assert_red(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "[Performance Dashboard](../performance/index.md)\n\n"
            "<!-- TODO: say that dev_build=yes means -O0 -->\n"
        )

    def test_tokens_only_inside_an_unterminated_html_comment_are_not_a_warning(self) -> None:
        """An unterminated comment swallows the rest of the page for the reader.

        Masked, not escalated: in the credit view over-masking only withholds
        credit, which is already fail-closed. Contrast the fence case, which is
        exit 3 because there ambiguity would hide an offered link.
        """
        self._assert_red(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "[Performance Dashboard](../performance/index.md)\n\n"
            "<!-- draft\n\nNightlies are `dev_build=yes`, i.e. `-O0`.\n"
        )

    def test_a_dashboard_link_only_inside_a_fenced_block_does_not_count(self) -> None:
        self._write_page(
            "downloads.md",
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "Nightlies are `dev_build=yes`, i.e. `-O0`.\n\n"
            "```markdown\n[Performance Dashboard](../performance/index.md)\n```\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("docs/performance/index.md", output)

    def test_a_dashboard_link_inside_inline_code_does_not_count(self) -> None:
        """Inline code is prose for TOKENS and not a link for DESTINATIONS."""
        self._write_page(
            "downloads.md",
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "Nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "Write it as `[Performance Dashboard](../performance/index.md)`.\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("never links to docs/performance/index.md", output)

    # --- GREEN: the forms this repository's warnings are actually written in ---

    def test_a_warning_written_with_inline_code_is_green(self) -> None:
        self._assert_green(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "Published nightlies are compiled with `dev_build=yes`, i.e. `-O0`.\n"
            "See the [Performance Dashboard](../performance/index.md).\n"
        )

    def test_a_warning_in_a_four_space_admonition_body_is_green(self) -> None:
        """The exact shape of all three MkDocs download pages in this repository.

        An admonition body is indented four spaces; reading that as an indented
        code block would reject every real warning the project has written.
        """
        self._assert_green(f"# Downloads\n\n{RELEASES_LINK}\n\n{WARNING_BLOCK}")

    def test_a_warning_in_a_list_item_continuation_is_green(self) -> None:
        self._assert_green(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "- Before you download:\n\n"
            "    Published nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "    See the [Performance Dashboard](../performance/index.md).\n"
        )

    def test_a_warning_in_a_blockquote_alert_is_green(self) -> None:
        """README uses GitHub's `> [!WARNING]` form rather than an admonition."""
        self._assert_green(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "> [!WARNING]\n"
            "> Nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "> See the [Performance Dashboard](../performance/index.md).\n"
        )

    def test_an_indented_continuation_of_a_paragraph_is_still_prose(self) -> None:
        """CommonMark: an indented code block cannot interrupt a paragraph."""
        self._assert_green(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "Published nightlies are compiled with\n"
            "    `dev_build=yes`, i.e. `-O0`.\n"
            "    See the [Performance Dashboard](../performance/index.md).\n"
        )

    # --- non-vacuity of the credit view itself ---

    def test_the_prose_view_removes_code_and_keeps_prose(self) -> None:
        """A mask that blanks everything, or nothing, would pass the cases above.

        Both halves are asserted on one document so neither degenerate mask can
        satisfy this: the fenced token must be gone AND the prose token must
        survive.
        """
        prose = guard.rendered_prose(
            "Visible `dev_build` prose.\n\n```\nhidden_token_in_a_fence\n```\n\n"
            "    hidden_token_in_an_indent\n\n<!-- hidden_token_in_a_comment -->\n"
        )
        self.assertIn("Visible `dev_build` prose.", prose)
        for hidden in (
            "hidden_token_in_a_fence",
            "hidden_token_in_an_indent",
            "hidden_token_in_a_comment",
        ):
            self.assertNotIn(hidden, prose)

    def test_the_prose_view_preserves_offsets(self) -> None:
        """Masks are offset-preserving so reported line numbers stay truthful."""
        text = "# T\n\n```\nx\n```\n\n    y\n\nz\n"
        prose = guard.rendered_prose(text)
        self.assertEqual(len(prose), len(text))
        self.assertEqual(prose.count("\n"), text.count("\n"))


class NestedIndentedCodeTests(_SyntheticTree):
    """Indented code is four columns past the CONTAINER, not past column zero.

    The first version of this mask tracked a boolean -- "is a container open?" --
    and left every indented line alone while one was. That is the wrong quantity:
    a list item whose content starts at column 2 turns column 6 into code, and a
    snippet at column 8 under `- Example only:` is a code block the reader is
    shown as code. Crediting it as a warning was Codex, PR #872, round 4.
    """

    def _assert_red(self, body: str) -> None:
        self._write_page("downloads.md", body)
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("no single paragraph", output)

    def _assert_green(self, body: str) -> None:
        self._write_page("downloads.md", body)
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)
        self.assertIn("1 download surface", output)

    # --- RED: a code block nested inside a container is still a code block ---

    def test_an_eight_space_snippet_under_a_list_item_is_not_a_warning(self) -> None:
        """The reported case: measured at exit 0 before the fix.

        The dashboard link is inside the snippet too, so this pins both halves of
        the credit view at once -- neither the tokens nor the cross-reference may
        be taken from text rendered as code.
        """
        self._write_page(
            "downloads.md",
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "- Example only:\n\n"
            "        dev_build=yes   # -O0\n"
            "        [Performance Dashboard](../performance/index.md)\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("no single paragraph", output)
        self.assertIn("never links to docs/performance/index.md", output)

    def test_a_twelve_space_snippet_under_a_nested_list_item_is_not_a_warning(self) -> None:
        self._assert_red(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "[Performance Dashboard](../performance/index.md)\n\n"
            "- Outer:\n"
            "  - Inner example:\n\n"
            "        dev_build=yes   # -O0\n"
        )

    def test_a_snippet_indented_inside_a_blockquote_is_not_a_warning(self) -> None:
        """Indentation inside a quote is measured after the `>` markers.

        Measuring raw leading whitespace made `>     x` look like column zero, so
        an indented code block inside a blockquote was never masked at all.
        """
        self._assert_red(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "[Performance Dashboard](../performance/index.md)\n\n"
            "> Release lane configuration:\n"
            ">\n"
            ">     dev_build=yes   # -O0\n"
        )

    def test_an_eight_space_snippet_in_an_admonition_body_is_not_a_warning(self) -> None:
        self._assert_red(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "[Performance Dashboard](../performance/index.md)\n\n"
            '!!! note "Lane configuration"\n'
            "    The nightly lane is configured like this:\n\n"
            "        dev_build=yes   # -O0\n"
        )

    def test_an_ordered_list_measures_its_own_marker_width(self) -> None:
        """`10. ` indents content to column 4, so code starts at column 8.

        A rule that assumed "two columns" for every list marker would read the
        continuation below as code and reject a real warning.
        """
        self._assert_red(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "[Performance Dashboard](../performance/index.md)\n\n"
            "10. Example only:\n\n"
            "        dev_build=yes   # -O0\n"
        )

    # --- GREEN: container content at any depth is still prose ---

    def test_a_four_space_continuation_of_a_list_item_is_green(self) -> None:
        """The boundary the RED cases must not cross. Content indent 2, so 4 < 6."""
        self._assert_green(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "- Before you download:\n\n"
            "    Published nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "    See the [Performance Dashboard](../performance/index.md).\n"
        )

    def test_a_six_space_continuation_of_a_nested_list_item_is_green(self) -> None:
        """Content indent 4 inside the inner item, so 6 is prose and 8 is code."""
        self._assert_green(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "- Outer:\n"
            "  - Before you download:\n\n"
            "      Published nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "      See the [Performance Dashboard](../performance/index.md).\n"
        )

    def test_an_ordered_list_continuation_is_green(self) -> None:
        self._assert_green(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "1. Before you download:\n\n"
            "   Published nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "   See the [Performance Dashboard](../performance/index.md).\n"
        )

    def test_a_warning_in_a_blockquote_after_a_blank_marker_is_green(self) -> None:
        """A quoted paragraph at column zero is prose however many precede it."""
        self._assert_green(
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "> Read this first.\n"
            ">\n"
            "> Nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "> See the [Performance Dashboard](../performance/index.md).\n"
        )

    # --- non-vacuity of the mask itself ---

    def test_the_mask_removes_nested_code_and_keeps_container_prose(self) -> None:
        """Both halves on one document, so neither degenerate mask satisfies it."""
        prose = guard.rendered_prose(
            "- Item:\n\n"
            "    visible_continuation_text\n\n"
            "        hidden_nested_snippet\n\n"
            "> Quoted:\n"
            ">\n"
            ">     hidden_quoted_snippet\n"
        )
        self.assertIn("visible_continuation_text", prose)
        self.assertIn("Quoted:", prose)
        for hidden in ("hidden_nested_snippet", "hidden_quoted_snippet"):
            self.assertNotIn(hidden, prose)

    def test_container_content_indent_is_measured_not_assumed(self) -> None:
        """The quantity the boolean version never had, pinned directly."""
        self.assertEqual(guard.container_content_indent("- item"), 2)
        self.assertEqual(guard.container_content_indent("  - item"), 4)
        self.assertEqual(guard.container_content_indent("10. item"), 4)
        self.assertEqual(guard.container_content_indent("1.  item"), 4)
        self.assertEqual(guard.container_content_indent('!!! warning "t"'), 4)
        self.assertEqual(guard.container_content_indent('    !!! note "t"'), 8)
        self.assertEqual(guard.container_content_indent('=== "Tab"'), 4)
        # A marker with no space after it is not a list; a thematic break is not
        # a container. Both previously raised the code threshold for free.
        self.assertIsNone(guard.container_content_indent("-not-a-list"))
        self.assertIsNone(guard.container_content_indent("* * *"))
        self.assertIsNone(guard.container_content_indent("---"))
        self.assertIsNone(guard.container_content_indent("ordinary prose"))

    def test_split_quote_prefix_reports_depth_and_content(self) -> None:
        self.assertEqual(guard.split_quote_prefix("> text"), (1, "text"))
        self.assertEqual(guard.split_quote_prefix("> > text"), (2, "text"))
        self.assertEqual(guard.split_quote_prefix(">"), (1, ""))
        self.assertEqual(guard.split_quote_prefix(">     x"), (1, "    x"))
        self.assertEqual(guard.split_quote_prefix("plain"), (0, "plain"))


class ImageTests(_SyntheticTree):
    """An image is looked at, not followed (Codex, PR #872, round 4).

    `![Performance Dashboard](../performance/index.md)` renders a broken picture
    and no link whatsoever, and satisfied the cross-reference rule in all three
    reference forms as well.
    """

    def _page_with_dashboard_written_as(self, markup: str) -> tuple[int, str]:
        self._write_page(
            "downloads.md",
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "Nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            f"{markup}\n",
        )
        return _run_guard(self.root)

    def _assert_not_a_link(self, markup: str) -> None:
        code, output = self._page_with_dashboard_written_as(markup)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("never links to docs/performance/index.md", output)

    def test_an_inline_image_is_not_the_dashboard_link(self) -> None:
        self._assert_not_a_link("![Performance Dashboard](../performance/index.md)")

    def test_a_shortcut_reference_image_is_not_the_dashboard_link(self) -> None:
        self._assert_not_a_link("![dashboard]\n\n[dashboard]: ../performance/index.md")

    def test_a_full_reference_image_is_not_the_dashboard_link(self) -> None:
        self._assert_not_a_link(
            "![Performance Dashboard][dashboard]\n\n[dashboard]: ../performance/index.md"
        )

    def test_a_collapsed_reference_image_is_not_the_dashboard_link(self) -> None:
        self._assert_not_a_link("![dashboard][]\n\n[dashboard]: ../performance/index.md")

    def test_an_image_with_an_attribute_list_is_not_the_dashboard_link(self) -> None:
        """MkDocs `attr_list` suffixes are how this repository writes its images."""
        self._assert_not_a_link(
            "![Performance Dashboard](../performance/index.md){ .gs-diagram }"
        )

    # --- GREEN: masking images must not swallow the links around them ---

    def test_an_image_wrapped_in_a_link_counts_as_the_link(self) -> None:
        """`[![alt](icon)](dest)` is a link to `dest`, and a reader can follow it.

        Masking the image is what makes this resolve to `dest`; matching `](`
        alone would have answered `icon.png`.
        """
        code, output = self._page_with_dashboard_written_as(
            "[![Chart](chart.png)](../performance/index.md)"
        )
        self.assertEqual(code, guard.EXIT_OK, output)

    def test_an_image_elsewhere_does_not_suppress_a_real_link(self) -> None:
        code, output = self._page_with_dashboard_written_as(
            "![Screenshot](shot.png)\n\n[Performance Dashboard](../performance/index.md)"
        )
        self.assertEqual(code, guard.EXIT_OK, output)

    def test_an_image_does_not_consume_the_reference_link_beside_it(self) -> None:
        code, output = self._page_with_dashboard_written_as(
            "![Screenshot][shot] and the [Performance Dashboard][dashboard].\n\n"
            "[shot]: shot.png\n[dashboard]: ../performance/index.md"
        )
        self.assertEqual(code, guard.EXIT_OK, output)


class BlockBoundaryTests(_SyntheticTree):
    """Two rendered blocks are not one paragraph (Codex, PR #872, round 4).

    A blank line inside a blockquote is a bare `>`, which `.strip()` reports as
    non-empty, so the splitter ran two quoted paragraphs together and let
    `dev_build` in one satisfy the co-occurrence rule with `-O0` in the other.
    """

    def _assert_split(self, body: str) -> None:
        self._write_page(
            "downloads.md",
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "[Performance Dashboard](../performance/index.md)\n\n" + body,
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("no single paragraph", output)

    def test_a_blank_quote_marker_splits_two_quoted_paragraphs(self) -> None:
        self._assert_split(
            "> Built with `dev_build=yes`.\n"
            ">\n"
            "> A separate thought about `-O0` in some other project.\n"
        )

    def test_a_blank_marker_in_a_nested_quote_splits_too(self) -> None:
        self._assert_split(
            "> > Built with `dev_build=yes`.\n"
            "> >\n"
            "> > A separate thought about `-O0`.\n"
        )

    def test_a_quote_opening_after_a_paragraph_is_a_new_block(self) -> None:
        """A blockquote interrupts a paragraph, so the two are not one block."""
        self._assert_split(
            "The binary name carries a `dev_build` segment.\n"
            "> Unrelated aside about `-O0`.\n"
        )

    def test_a_heading_between_two_paragraphs_splits_them(self) -> None:
        self._assert_split(
            "The binary name carries a `dev_build` segment.\n"
            "## Something else\n"
            "Unrelated aside about `-O0`.\n"
        )

    def test_a_rule_between_two_paragraphs_splits_them(self) -> None:
        """Written without blank lines, so only the rule itself can split them.

        With blank lines around it this case would pass on a splitter that only
        knows blank lines, and prove nothing.
        """
        self._assert_split(
            "The binary name carries a `dev_build` segment.\n"
            "---\n"
            "Unrelated aside about `-O0`.\n"
        )

    # --- GREEN: the boundary in the other direction ---

    def test_a_multi_line_blockquote_alert_stays_one_paragraph(self) -> None:
        """README's form: consecutive quoted lines are one block."""
        self._write_page(
            "downloads.md",
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "> [!WARNING]\n"
            "> Nightlies are `dev_build=yes`,\n"
            "> which means `-O0`.\n"
            "> See the [Performance Dashboard](../performance/index.md).\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)

    def test_a_lazy_continuation_of_a_blockquote_stays_one_paragraph(self) -> None:
        """A quote depth that DECREASES is a lazy continuation, not a new block."""
        self._write_page(
            "downloads.md",
            f"# Downloads\n\n{RELEASES_LINK}\n\n"
            "> Nightlies are `dev_build=yes`,\n"
            "which means `-O0`.\n"
            "See the [Performance Dashboard](../performance/index.md).\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_OK, output)

    def test_paragraph_splitting_is_non_vacuous(self) -> None:
        """A splitter that split every line, or none, would pass the cases above."""
        blocks = guard.paragraphs(
            "> one `dev_build`\n>\n> two `-O0`\n\nthree\n## four\nfive\n"
        )
        self.assertEqual(
            blocks,
            [
                "> one `dev_build`",
                "> two `-O0`",
                "three",
                "## four",
                "five",
            ],
        )


class RenderedLinkTests(_SyntheticTree):
    """A reference definition is not a link (Codex #872, round 3).

    `[dashboard]: ../performance/index.md` with nothing using `[dashboard]`
    renders nothing. Crediting it handed the guard a cross-reference the reader
    was never shown.
    """

    def _page(self, warning_and_links: str) -> None:
        self._write_page(
            "downloads.md", f"# Downloads\n\n{RELEASES_LINK}\n\n{warning_and_links}"
        )

    def test_an_orphan_dashboard_definition_is_red(self) -> None:
        self._page(
            "Nightlies are `dev_build=yes`, i.e. `-O0`.\n\n"
            "[dashboard]: ../performance/index.md\n"
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("renders nothing and does not count", output)

    def test_a_full_reference_dashboard_link_is_green(self) -> None:
        self._page(
            "Nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "See the [Performance Dashboard][dashboard].\n\n"
            "[dashboard]: ../performance/index.md\n"
        )
        self.assertEqual(_run_guard(self.root)[0], guard.EXIT_OK)

    def test_a_collapsed_reference_dashboard_link_is_green(self) -> None:
        self._page(
            "Nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "See the [dashboard][].\n\n"
            "[dashboard]: ../performance/index.md\n"
        )
        self.assertEqual(_run_guard(self.root)[0], guard.EXIT_OK)

    def test_a_shortcut_reference_dashboard_link_is_green(self) -> None:
        self._page(
            "Nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "See the [dashboard].\n\n"
            "[dashboard]: ../performance/index.md\n"
        )
        self.assertEqual(_run_guard(self.root)[0], guard.EXIT_OK)

    def test_label_matching_is_case_and_whitespace_insensitive(self) -> None:
        """CommonMark label matching; a stricter rule would reject a real link."""
        self._page(
            "Nightlies are `dev_build=yes`, i.e. `-O0`.\n"
            "See the [Performance   Dashboard].\n\n"
            "[performance dashboard]: ../performance/index.md\n"
        )
        self.assertEqual(_run_guard(self.root)[0], guard.EXIT_OK)

    def test_a_definition_used_only_inside_a_fenced_block_is_red(self) -> None:
        """The use has to render too, not merely appear in the file."""
        self._page(
            "Nightlies are `dev_build=yes`, i.e. `-O0`.\n\n"
            "```markdown\nSee the [dashboard].\n```\n\n"
            "[dashboard]: ../performance/index.md\n"
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("never links to docs/performance/index.md", output)

    def test_an_orphan_releases_definition_is_still_a_download_surface(self) -> None:
        """The asymmetry, pinned: obligation is a superset, credit is a subset.

        An unused `[releases]:` definition renders no link either -- but assuming
        it does costs a warning on a page that should carry one anyway, while
        assuming it does not costs an unwarned binary. The two views are supposed
        to disagree here, so the disagreement is asserted rather than left to be
        "fixed" later for symmetry.
        """
        self._compliant_page()
        self._write_page(
            "orphan.md", f"# Get It\n\nNothing links this.\n\n[releases]: {RELEASES_URL}\n"
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_VIOLATIONS, output)
        self.assertIn("orphan.md", output)


class UninspectableInputTests(_SyntheticTree):
    """"Could not inspect" must not read as "inspected and compliant" (Codex #872).

    Each case pairs the uninspectable input with a fully compliant page, because
    that is the situation the guard has to get right: the readable surfaces keep
    the subject non-empty, so nothing else notices.
    """

    def test_an_undecodable_markdown_file_is_a_failure(self) -> None:
        self._compliant_page()
        self.assertEqual(_run_guard(self.root)[0], guard.EXIT_OK)
        (self.pages / "legacy.md").write_bytes(
            f"# T\xe9l\xe9charger\n\n[Nightly]({RELEASES_URL})\n".encode("latin-1")
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_UNINSPECTABLE, output)
        self.assertIn("legacy.md", output)
        self.assertIn("could not be read as UTF-8", output)

    def test_an_unreadable_file_is_a_failure(self) -> None:
        self._compliant_page()
        (self.pages / "locked.md").write_text("# Locked\n", encoding="utf-8")
        real_read_text = Path.read_text

        def _raise_for_locked(self_path, *args, **kwargs):
            if self_path.name == "locked.md":
                raise PermissionError(13, "Permission denied")
            return real_read_text(self_path, *args, **kwargs)

        with mock.patch.object(Path, "read_text", _raise_for_locked):
            code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_UNINSPECTABLE, output)
        self.assertIn("locked.md", output)

    def test_a_directory_that_will_not_enumerate_is_a_failure(self) -> None:
        self._compliant_page()
        (self.root / "docs" / "restricted").mkdir()
        real_iterdir = Path.iterdir

        def _raise_for_restricted(self_path):
            if self_path.name == "restricted":
                raise PermissionError(13, "Permission denied")
            return real_iterdir(self_path)

        with mock.patch.object(Path, "iterdir", _raise_for_restricted):
            code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_UNINSPECTABLE, output)
        self.assertIn("restricted", output)
        self.assertIn("could not be enumerated", output)

    def test_an_unterminated_code_fence_is_a_failure(self) -> None:
        """Past an unclosed fence the guard cannot tell an example from an offer."""
        self._compliant_page()
        self._write_page(
            "truncated.md",
            "# Example\n\n```console\n"
            f"$ open {RELEASES_URL}\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_UNINSPECTABLE, output)
        self.assertIn("truncated.md", output)
        self.assertIn("never closed", output)

    def test_a_bomless_utf16_page_is_a_failure(self) -> None:
        """Decoding without raising is not the same as being read as authored.

        UTF-16LE without a BOM is valid UTF-8 -- its NUL bytes are -- so
        `read_text` succeeds and hands back `[\\x00N\\x00i...`, which no pattern
        can match. The page silently stops being a download surface. Measured
        before the fix: exit 0 with the unwarned link present.
        """
        self._compliant_page()
        self.assertEqual(_run_guard(self.root)[0], guard.EXIT_OK)
        (self.pages / "utf16.md").write_bytes(
            f"# Get It\n\n[Nightly]({RELEASES_URL})\n".encode("utf-16-le")
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_UNINSPECTABLE, output)
        self.assertIn("utf16.md", output)
        self.assertIn("NUL", output)

    def test_a_nul_byte_in_an_otherwise_decodable_page_is_a_failure(self) -> None:
        self._compliant_page()
        (self.pages / "corrupt.md").write_bytes(b"# Get It\n\n[Nightly](htt\x00ps://x)\n")
        code, output = _run_guard(self.root)
        self.assertEqual(code, guard.EXIT_UNINSPECTABLE, output)
        self.assertIn("corrupt.md", output)

    def test_list_mode_also_reports_uninspectable_inputs(self) -> None:
        """`--list` is what a maintainer runs to trust a green run; it must not lie."""
        self._compliant_page()
        (self.pages / "legacy.md").write_bytes(b"\xff\xfe# not utf-8\n")
        code, output = _run_guard(self.root, "--list")
        self.assertEqual(code, guard.EXIT_UNINSPECTABLE, output)

    def test_the_four_exit_codes_are_distinct(self) -> None:
        """A distinct status is the whole point; collapsing two defeats the fix."""
        codes = (
            guard.EXIT_OK,
            guard.EXIT_VIOLATIONS,
            guard.EXIT_EMPTY_SUBJECT,
            guard.EXIT_UNINSPECTABLE,
        )
        self.assertEqual(len(set(codes)), 4, codes)


class RealTreeControlTests(unittest.TestCase):
    """Grounding: the derived rule must actually find this repository's pages."""

    def test_the_real_tree_has_a_non_empty_subject(self) -> None:
        surfaces, _failures, _uninspectable = guard.check(ROOT)
        self.assertGreaterEqual(
            len(surfaces),
            2,
            "the derived download-surface set collapsed; the URL pattern no longer "
            "matches how this repository writes its Releases link, so the guard would "
            "pass while protecting nothing",
        )

    def test_the_real_tree_is_clean(self) -> None:
        surfaces, failures, uninspectable = guard.check(ROOT)
        self.assertEqual(failures, [], f"discovered surfaces: {surfaces}")
        self.assertEqual(uninspectable, [])

    def test_the_real_tree_has_no_uninspectable_authored_input(self) -> None:
        """Stated separately: a green run must mean every page was actually read."""
        _surfaces, _failures, uninspectable = guard.check(ROOT)
        self.assertEqual(uninspectable, [])

    def test_the_code_exclusion_fires_on_real_content(self) -> None:
        """The archived audit mentions the URL in backticks twice and is NOT a surface.

        Without a real-tree anchor the code exclusion could be silently broken (or
        silently over-broad) and every synthetic case would still pass.
        """
        audit = ROOT / "docs" / "reports" / "ci_release_audit_2026-04-16.md"
        self.assertTrue(audit.is_file(), f"missing fixture page: {audit}")
        text = audit.read_text(encoding="utf-8")
        self.assertIn(
            "github.com/klausi3D/godotGS/releases",
            text,
            "this control is only meaningful while the page still mentions the URL",
        )
        masked, problem = guard.mask_code(text)
        self.assertIsNone(problem, f"{audit}: {problem}")
        self.assertEqual(guard.releases_occurrences(masked), [])
        surfaces, _failures, _uninspectable = guard.check(ROOT)
        self.assertNotIn(audit, surfaces)

    def test_the_prose_view_fires_on_the_real_download_pages(self) -> None:
        """Both halves of the credit view, anchored to real authored content.

        `docs/getting-started/downloads.md` has fenced code blocks AND a
        four-space-indented admonition body carrying the warning. A mask that
        stopped removing code, or one that started eating admonition bodies,
        would still satisfy every synthetic case; it cannot satisfy this.
        """
        page = ROOT / "docs" / "getting-started" / "downloads.md"
        self.assertTrue(page.is_file(), f"missing fixture page: {page}")
        text = page.read_text(encoding="utf-8")
        self.assertIn("```", text, "this control needs a page that has fences")
        prose = guard.rendered_prose(text)
        # Masks blank in place, so compare visible characters, not length.
        self.assertLess(
            len("".join(prose.split())),
            len("".join(text.split())),
            "the prose view removed nothing from a page that contains fenced code",
        )
        self.assertNotIn("```", prose, "fence delimiters survived the prose view")
        self.assertTrue(
            guard.has_warning_paragraph(prose),
            "the real warning, written in a four-space-indented admonition body, "
            "no longer survives the prose view -- the indented-code rule is eating "
            "container content",
        )
        self.assertTrue(guard.links_to_performance_page(page, prose, ROOT))

    def test_a_real_pages_image_is_not_counted_as_a_link(self) -> None:
        """Anchored to authored content, not to a synthetic image.

        `docs/getting-started/quick-start.md` is a real download surface that
        embeds a diagram. Its `![...](../assets/images/...)` destination must not
        appear among the page's rendered link destinations, while the dashboard
        link on the same page must -- so neither an image rule that stopped
        firing nor one that ate every link can satisfy this.
        """
        page = ROOT / "docs" / "getting-started" / "quick-start.md"
        self.assertTrue(page.is_file(), f"missing fixture page: {page}")
        text = page.read_text(encoding="utf-8")
        self.assertIn("![", text, "this control needs a page that embeds an image")
        prose = guard.rendered_prose(text)
        destinations = guard.rendered_link_destinations(guard.mask_inline_code(prose))
        self.assertTrue(destinations, "the destination extractor returned nothing")
        self.assertFalse(
            [d for d in destinations if d.endswith(".svg") or d.endswith(".png")],
            f"an image source was counted as a link destination: {destinations}",
        )
        self.assertTrue(guard.links_to_performance_page(page, prose, ROOT))

    def test_the_real_download_pages_survive_the_block_splitter(self) -> None:
        """Every real warning is one block, in each of the two forms in use.

        The MkDocs pages write theirs as a four-space admonition body and README
        as a multi-line `> [!WARNING]` blockquote. A splitter that broke either
        into per-line blocks would reject all four pages, and a mask that read
        container content as code would empty them; both are asserted here on the
        authored text rather than only on synthetic fixtures.
        """
        surfaces, _failures, _uninspectable = guard.check(ROOT)
        self.assertGreaterEqual(len(surfaces), 4, surfaces)
        for page in surfaces:
            prose = guard.rendered_prose(page.read_text(encoding="utf-8"))
            blocks = [
                block
                for block in guard.paragraphs(prose)
                if all(token in block for token in guard.REQUIRED_TOKENS)
            ]
            self.assertTrue(
                blocks,
                f"{guard._display(page, ROOT)}: the warning no longer survives as a "
                "single rendered block",
            )

    def test_the_documented_warning_source_still_exists(self) -> None:
        """The prose this guard propagates has a single origin; keep it findable."""
        text = (ROOT / guard.PERFORMANCE_PAGE_RELPATH).read_text(encoding="utf-8")
        self.assertTrue(
            any(
                all(token in block for token in guard.REQUIRED_TOKENS)
                for block in guard.paragraphs(text)
            ),
            "docs/performance/index.md no longer carries the -O0 / dev_build warning "
            "the download pages point at",
        )


if __name__ == "__main__":
    unittest.main()
