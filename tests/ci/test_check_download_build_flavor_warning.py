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
