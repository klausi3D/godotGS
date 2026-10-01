#!/usr/bin/env python3
"""Unit tests for the public-stage exclusions in scripts/stage_public_docs.py.

The staging script decides what reaches the public docs site and its search
index. Internal and historical material (agent memory, archives at any depth,
reports, programs, finished-refactor working docs) must never be staged, and a
link from a public page to such a file must be rewritten to a GitHub URL rather
than left dangling.

These tests live in tests/agentic because `python -m unittest discover -s
tests/agentic` runs in the required agentic-pr-gate job on every PR; a test under
tests/ci would only run if it were wired into tests/ci/run_module_tests.py.
"""

from __future__ import annotations

import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "stage_public_docs.py"
spec = importlib.util.spec_from_file_location("stage_public_docs", SCRIPT)
assert spec and spec.loader
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)

REPO_URL = "https://github.com/example-owner/example-repo"
REF = "0123456789abcdef0123456789abcdef01234567"


def _write(path: Path, text: str = "# page\n") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class ExclusionMatchingTest(unittest.TestCase):
    def _excluded(self, relative: str, patterns) -> bool:
        return stage.should_exclude(Path(relative), patterns)

    def test_any_depth_pattern_excludes_nested_directory(self):
        patterns = ("**/archive",)
        self.assertTrue(self._excluded("archive/old.md", patterns))
        self.assertTrue(self._excluded("reports/archive/old.md", patterns))
        self.assertTrue(self._excluded("a/b/archive/c/old.md", patterns))

    def test_any_depth_pattern_matches_whole_components_only(self):
        patterns = ("**/archive",)
        self.assertFalse(self._excluded("archived/page.md", patterns))
        self.assertFalse(self._excluded("reports/archive-notes.md", patterns))
        self.assertFalse(self._excluded("reports/my_archive/page.md", patterns))

    def test_directory_prefix_excludes_everything_beneath_it(self):
        patterns = ("reports",)
        self.assertTrue(self._excluded("reports", patterns))
        self.assertTrue(self._excluded("reports/index.md", patterns))
        self.assertTrue(self._excluded("reports/sub/deep/page.md", patterns))

    def test_prefix_is_anchored_and_component_bounded(self):
        patterns = ("reports",)
        self.assertFalse(self._excluded("reports-public/page.md", patterns))
        self.assertFalse(self._excluded("guide/reports/page.md", patterns))

    def test_file_pattern_excludes_exactly_that_file(self):
        patterns = ("architecture/spec.md",)
        self.assertTrue(self._excluded("architecture/spec.md", patterns))
        self.assertFalse(self._excluded("architecture/spec.md.bak", patterns))
        self.assertFalse(self._excluded("architecture/overview.md", patterns))
        self.assertFalse(self._excluded("other/architecture/spec.md", patterns))

    def test_malformed_patterns_are_rejected(self):
        for bad in ("/", "..", "reports/../api", "**", "a/**/b"):
            with self.subTest(pattern=bad):
                with self.assertRaises(ValueError):
                    stage.parse_exclusions([bad])

    def test_unmatched_pattern_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            docs = Path(tmp)
            _write(docs / "reports" / "archive" / "a.md")
            unmatched = stage.find_unmatched_exclusions(
                docs, ("reports", "**/archive", "architecture/gone.md")
            )
        self.assertEqual(unmatched, ["architecture/gone.md"])


class StagingTest(unittest.TestCase):
    """Stage a synthetic docs tree inside a temporary 'repository'."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name).resolve()
        self.docs = self.repo / "docs"
        self.out = self.repo / ".site" / "public-docs"
        patcher = mock.patch.object(stage, "REPO_ROOT", self.repo)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self._tmp.cleanup)

    def _stage(self, exclusions, *, repo_url=REPO_URL, ref=REF):
        return stage.copy_docs_tree(
            source_root=self.docs,
            output_root=self.out,
            exclusions=exclusions,
            repo_url=repo_url,
            ref=ref,
        )

    def _staged(self) -> set[str]:
        return {p.relative_to(self.out).as_posix() for p in self.out.rglob("*") if p.is_file()}

    def test_nested_and_prefix_exclusions_are_not_staged(self):
        _write(self.docs / "index.md")
        _write(self.docs / "reports" / "index.md")
        _write(self.docs / "reports" / "archive" / "old.md")
        _write(self.docs / "guide" / "archive" / "old.md")
        _write(self.docs / "architecture" / "overview.md")
        _write(self.docs / "architecture" / "spec.md")

        self._stage(("reports", "**/archive", "architecture/spec.md"))

        self.assertEqual(self._staged(), {"index.md", "architecture/overview.md"})

    def test_link_to_excluded_page_is_rewritten_to_github(self):
        _write(
            self.docs / "guide" / "page.md",
            "\n".join(
                [
                    "See [the audit](../reports/audit.md#findings).",
                    "See [the spec](../architecture/spec.md \"title\").",
                    "See [all reports](../reports/).",
                    '<a href="../reports/audit.md">html link</a>',
                    "![diagram](../reports/archive/plot.png)",
                    "Stay [local](other.md).",
                    "",
                ]
            ),
        )
        _write(self.docs / "guide" / "other.md")
        _write(self.docs / "reports" / "audit.md")
        _write(self.docs / "reports" / "archive" / "plot.png", "png")
        _write(self.docs / "architecture" / "spec.md")

        self._stage(("reports", "**/archive", "architecture/spec.md"))
        staged = (self.out / "guide" / "page.md").read_text(encoding="utf-8")

        blob = f"{REPO_URL}/blob/{REF}"
        self.assertIn(f"[the audit]({blob}/docs/reports/audit.md#findings)", staged)
        self.assertIn(f'[the spec]({blob}/docs/architecture/spec.md "title")', staged)
        self.assertIn(f"[all reports]({REPO_URL}/tree/{REF}/docs/reports)", staged)
        self.assertIn(f'<a href="{blob}/docs/reports/audit.md">', staged)
        self.assertIn(
            "![diagram](https://raw.githubusercontent.com/example-owner/example-repo/"
            f"{REF}/docs/reports/archive/plot.png)",
            staged,
        )
        self.assertIn("Stay [local](other.md).", staged)
        self.assertNotIn("../reports/", staged)
        self.assertNotIn("../architecture/spec.md", staged)

    def test_link_to_public_page_is_left_relative(self):
        _write(self.docs / "guide" / "page.md", "[api](../api/index.md)\n")
        _write(self.docs / "api" / "index.md")

        self._stage(("reports",))

        staged = (self.out / "guide" / "page.md").read_text(encoding="utf-8")
        self.assertEqual(staged, "[api](../api/index.md)\n")

    def _run_main(self, *extra_args: str) -> int:
        argv = ["stage_public_docs.py", "--source", "docs", "--output", ".site/public-docs", *extra_args]
        with mock.patch("sys.argv", argv), mock.patch("builtins.print"):
            return stage.main()

    def test_cli_aborts_on_a_stale_exclusion_before_staging(self):
        # A synthetic tree in which every DEFAULT_EXCLUSIONS entry matches, so the
        # only possible stale entry is the one passed on the command line.
        _write(self.docs / "index.md")
        for pattern in stage.DEFAULT_EXCLUSIONS:
            target = pattern[3:] if pattern.startswith("**/") else pattern
            _write(self.docs / target if target.endswith(".md") else self.docs / target / "page.md")

        self.assertEqual(self._run_main(), 0)  # control: the defaults alone stage
        shutil.rmtree(self.out)

        with self.assertRaises(SystemExit) as raised:
            self._run_main("--exclude", "architecture/does-not-exist.md")
        self.assertIn("'architecture/does-not-exist.md'", str(raised.exception.code))
        self.assertFalse(self.out.exists(), "a stale exclusion must abort before anything is staged")


class RepositoryPublicScopeTest(unittest.TestCase):
    """DEFAULT_EXCLUSIONS against the real docs/ tree."""

    DOCS = ROOT / "docs"

    def test_every_default_exclusion_matches_a_docs_path(self):
        self.assertEqual(stage.find_unmatched_exclusions(self.DOCS, stage.DEFAULT_EXCLUSIONS), [])

    def test_internal_and_historical_material_is_not_staged(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp).resolve() / "public-docs"
            stage.copy_docs_tree(
                source_root=self.DOCS,
                output_root=out,
                exclusions=stage.DEFAULT_EXCLUSIONS,
                repo_url=REPO_URL,
                ref=REF,
            )
            staged = [p.relative_to(out) for p in out.rglob("*") if p.is_file()]

        self.assertGreater(len(staged), 50, "staging produced almost nothing; the fixture drifted")
        leaked = sorted(
            p.as_posix()
            for p in staged
            if p.parts[0] in {"agent_memory", "reports", "programs"} or "archive" in p.parts[:-1]
        )
        self.assertEqual(leaked, [])
        for historical in (
            "architecture/gaussian-renderer-refactor-memory.md",
            "architecture/refactor-phase-runner.md",
            "architecture/gaussian-pipeline-unification-plan.md",
            "architecture/gaussian-pipeline-deprecation-deletion-plan.md",
            "architecture/resolve_lighting_redesign_spec.md",
            "architecture/tier2_cluster_culling_spec.md",
        ):
            with self.subTest(page=historical):
                self.assertNotIn(Path(historical), staged)

    def test_generated_architecture_pages_opt_out_of_search(self):
        pages = sorted((self.DOCS / "architecture" / "generated").glob("*.md"))
        self.assertTrue(pages, "no generated architecture pages found; the glob drifted")
        for page in pages:
            with self.subTest(page=page.name):
                text = page.read_text(encoding="utf-8")
                self.assertTrue(text.startswith("---\n"), "front matter must open the file")
                front_matter = text.split("\n---\n", 1)[0]
                self.assertRegex(front_matter, r"\nsearch:\n  exclude: true\n?")


if __name__ == "__main__":
    unittest.main()
