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
"""

from __future__ import annotations

import io
import contextlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_download_build_flavor_warning as guard  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

RELEASES_LINK = "[GitHub Releases](https://github.com/klausi3D/godotGS/releases)"

WARNING_BLOCK = (
    "!!! warning \"Nightly binaries are unoptimized `-O0` builds\"\n"
    "    Published nightlies are compiled with `dev_build=yes`, i.e. `-O0`.\n"
    "    See the [Performance Dashboard](../performance/index.md#measurement-environment).\n"
)


def _run_guard(root: Path) -> tuple[int, str]:
    """Invoke the guard exactly as CI does, capturing its exit code and output."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = guard.main(["--root", str(root)])
    return code, out.getvalue() + err.getvalue()


class SyntheticTreeTests(unittest.TestCase):
    """Each case builds a minimal repo shape and asserts the guard's verdict."""

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

    # --- the control: the fixture the other cases mutate must itself be GREEN ---

    def test_compliant_page_is_green(self) -> None:
        self._compliant_page()
        code, output = _run_guard(self.root)
        self.assertEqual(code, 0, output)
        self.assertIn("1 download surface", output)

    # --- mutation 1: delete the admonition ---

    def test_deleting_the_warning_is_red(self) -> None:
        page = self._compliant_page()
        page.write_text(f"# Downloads\n\n{RELEASES_LINK}\n", encoding="utf-8")
        code, output = _run_guard(self.root)
        self.assertEqual(code, 1, output)
        self.assertIn("downloads.md", output)
        self.assertIn("-O0", output)

    # --- mutation 2: a NEW page that links to Releases and says nothing ---

    def test_new_page_linking_to_releases_without_a_warning_is_red(self) -> None:
        self._compliant_page()
        code, output = _run_guard(self.root)
        self.assertEqual(code, 0, output)

        self._write_page("shiny-new-download-page.md", f"# Get It\n\n{RELEASES_LINK}\n")
        code, output = _run_guard(self.root)
        self.assertEqual(code, 1, output)
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
        self.assertEqual(code, 1, output)
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
        self.assertEqual(code, 1, output)
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
        self.assertEqual(code, 1, output)

    # --- the shape rule: a mention is not an offer ---

    def test_backticked_url_is_not_a_download_surface(self) -> None:
        self._compliant_page()
        self._write_page(
            "archived-audit.md",
            "# Audit\n\nAdd a link to `https://github.com/klausi3D/godotGS/releases/latest`.\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, 0, output)
        self.assertIn("1 download surface", output)

    def test_a_different_repos_releases_page_is_not_our_download_surface(self) -> None:
        self._compliant_page()
        self._write_page(
            "upstream.md",
            "# Upstream\n\n[Godot Releases](https://github.com/godotengine/godot/releases)\n",
        )
        code, output = _run_guard(self.root)
        self.assertEqual(code, 0, output)
        self.assertIn("1 download surface", output)

    # --- non-vacuity, fail-closed ---

    def test_empty_subject_is_a_failure_not_a_pass(self) -> None:
        """If nothing matches, the guard must not report success.

        This is the failure mode the guard would otherwise have: misspell the link
        pattern and it protects nothing while staying green forever.
        """
        self._write_page("downloads.md", "# Downloads\n\nNo link here.\n")
        code, output = _run_guard(self.root)
        self.assertEqual(code, 2, output)
        self.assertIn("no Markdown file links", output)

    def test_list_mode_on_an_empty_subject_also_fails(self) -> None:
        self._write_page("downloads.md", "# Downloads\n\nNo link here.\n")
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = guard.main(["--root", str(self.root), "--list"])
        self.assertEqual(code, 2, out.getvalue() + err.getvalue())


class RealTreeControlTests(unittest.TestCase):
    """Grounding: the derived rule must actually find this repository's pages."""

    def test_the_real_tree_has_a_non_empty_subject(self) -> None:
        surfaces, _ = guard.check(ROOT)
        self.assertGreaterEqual(
            len(surfaces),
            2,
            "the derived download-surface set collapsed; the link pattern no longer "
            "matches how this repository writes its Releases link, so the guard would "
            "pass while protecting nothing",
        )

    def test_the_real_tree_is_clean(self) -> None:
        surfaces, failures = guard.check(ROOT)
        self.assertEqual(failures, [], f"discovered surfaces: {surfaces}")

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
