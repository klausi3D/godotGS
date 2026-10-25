#!/usr/bin/env python3
"""Test public-stage exclusions and source dates in scripts/stage_public_docs.py.

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
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import yaml

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
        self.assertEqual(staged.split("\n---\n", 1)[1], "[api](../api/index.md)\n")

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


class SourceContentDatesTest(unittest.TestCase):
    """Exercise the staging pipeline against real, dated Git history."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name).resolve() / "source"
        self.repo.mkdir()
        self.docs = self.repo / "docs"
        self.out = self.repo / ".site" / "public-docs"
        self.page = self.docs / "guide" / "page.md"
        self._git("init", "-q")
        self._git("config", "user.name", "Docs fixture")
        self._git("config", "user.email", "docs@example.invalid")
        self._git("config", "core.autocrlf", "false")
        _write(self.page, "# Measured page\n\nmeasured_at: 2019-07-19\nvalue: 42\n")
        self.first = self._commit("2020-01-02T12:00:00+00:00")
        patcher = mock.patch.object(stage, "REPO_ROOT", self.repo)
        patcher.start()
        self.addCleanup(patcher.stop)

    def _git(self, *args, **kwargs):
        return subprocess.run(
            ["git", "-C", str(self.repo), *args], check=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs,
        ).stdout.decode("utf-8").strip()

    def _commit(self, timestamp):
        self._git("add", "docs")
        self._git("commit", "-qm", "Update fixture", env={
            **os.environ, "GIT_AUTHOR_DATE": timestamp, "GIT_COMMITTER_DATE": timestamp,
        })
        return self._git("rev-parse", "HEAD")

    def _stage(self, ref):
        stage.copy_docs_tree(
            source_root=self.docs, output_root=self.out, exclusions=("reports",),
            repo_url=REPO_URL, ref=ref,
        )
        text = (self.out / "guide" / "page.md").read_text(encoding="utf-8")
        self.assertTrue(text.startswith("---\n"))
        metadata, body = text[4:].split("\n---\n", 1)
        return yaml.safe_load(metadata), body

    def _assert_unknown(self, ref):
        metadata, _ = self._stage(ref)
        self.assertNotIn("revision_date", metadata)
        self.assertFalse(any(str(key).startswith(("git_revision_date", "git_creation_date")) for key in metadata))

    def test_rebuilding_later_preserves_source_date_and_measurement(self):
        before = self.page.read_bytes()
        initial = self._stage(self.first)
        os.utime(self.page, (4102444800, 4102444800))
        with mock.patch("time.time", return_value=4102444800):
            rebuilt = self._stage(self.first)
        self.assertEqual(initial, rebuilt)
        self.assertEqual(rebuilt[0]["revision_date"], "Source updated: 2020-01-02")
        self.assertIn("measured_at: 2019-07-19\nvalue: 42", rebuilt[1])
        self.assertEqual(self.page.read_bytes(), before)

    def test_unrelated_commit_and_rewritten_link_sha_preserve_content_date(self):
        _write(self.docs / "reports" / "audit.md")
        _write(self.page, "# Page\n\n[Audit](../reports/audit.md)\n")
        old = self._commit("2020-02-03T12:00:00+00:00")
        original_meta, original_body = self._stage(old)
        _write(self.docs / "unrelated.md")
        new = self._commit("2021-03-04T12:00:00+00:00")
        metadata, body = self._stage(new)
        self.assertEqual(metadata, original_meta)
        self.assertIn(f"/blob/{old}/docs/reports/audit.md", original_body)
        self.assertIn(f"/blob/{new}/docs/reports/audit.md", body)
        self.assertFalse((self.out / "reports").exists())

    def test_committed_content_change_gets_its_own_date(self):
        _write(self.page, "# Updated page\n")
        changed = self._commit("2022-04-05T12:00:00+00:00")
        self.assertEqual(self._stage(changed)[0]["revision_date"], "Source updated: 2022-04-05")
        self._assert_unknown(self.first)  # the worktree content is not the old blob

    def test_file_mode_change_does_not_refresh_content_date(self):
        self._git("update-index", "--chmod=+x", "docs/guide/page.md")
        self._git("commit", "-qm", "Change file mode", env={
            **os.environ, "GIT_AUTHOR_DATE": "2024-01-01T12:00:00+00:00",
            "GIT_COMMITTER_DATE": "2024-01-01T12:00:00+00:00",
        })
        self.assertEqual(self._stage(self._git("rev-parse", "HEAD"))[0]["revision_date"],
                         "Source updated: 2020-01-02")

    def test_renaming_unchanged_content_preserves_its_date(self):
        self._git("mv", "docs/guide/page.md", "docs/guide/renamed.md")
        self._commit("2021-01-01T12:00:00+00:00")
        self._git("mv", "docs/guide/renamed.md", "docs/guide/page.md")
        latest = self._commit("2022-01-01T12:00:00+00:00")
        self.assertEqual(self._stage(latest)[0]["revision_date"], "Source updated: 2020-01-02")

    def test_merge_resolution_uses_the_merge_content_date(self):
        self._git("checkout", "-qb", "topic")
        _write(self.page, "# Topic content\n")
        self._commit("2021-01-01T12:00:00+00:00")
        self._git("checkout", "-qb", "mainline", self.first)
        _write(self.docs / "unrelated.md")
        self._commit("2022-01-01T12:00:00+00:00")
        self._git("merge", "--no-ff", "--no-commit", "topic")
        _write(self.page, "# Content resolved during the merge\n")
        merged = self._commit("2023-01-01T12:00:00+00:00")
        self.assertEqual(self._stage(merged)[0]["revision_date"], "Source updated: 2023-01-01")

    def test_new_measurement_with_equal_values_retains_its_own_run_date(self):
        _write(self.page, "# Measured page\n\nmeasured_at: 2022-06-07\nvalue: 42\n")
        changed = self._commit("2022-06-08T12:00:00+00:00")
        metadata, body = self._stage(changed)
        self.assertEqual(metadata["revision_date"], "Source updated: 2022-06-08")
        self.assertIn("measured_at: 2022-06-07\nvalue: 42", body)

    def test_missing_ref_does_not_fall_back_to_head(self):
        for ref in (None, "", "missing-ref", "--all"):
            with self.subTest(ref=ref):
                self._assert_unknown(ref)

    def test_missing_git_or_repository_cannot_assert_a_date(self):
        with mock.patch.object(stage.subprocess, "run", side_effect=FileNotFoundError):
            self._assert_unknown(self.first)
        (self.repo / ".git").rename(self.repo / "saved-git")
        self._assert_unknown(self.first)

    def test_untracked_or_dirty_generated_content_cannot_assert_a_date(self):
        self._git("rm", "--cached", "docs/guide/page.md")
        self._git("commit", "-qm", "Untrack generated page")
        self._assert_unknown(self._git("rev-parse", "HEAD"))
        _write(self.page, "---\nrevision_date: 2099-01-01\ngit_revision_date_localized: Today\n"
               "git_creation_date_localized: Today\n---\n# Regenerated content\n")
        self._assert_unknown(self.first)

    def test_shallow_history_does_not_invent_a_change_at_clone_boundary(self):
        _write(self.docs / "unrelated.md")
        latest = self._commit("2024-01-01T12:00:00+00:00")
        shallow = self.repo.parent / "shallow"
        self._git("clone", "-q", "--depth", "1", self.repo.as_uri(), str(shallow))
        with mock.patch.object(stage, "REPO_ROOT", shallow):
            self.docs = shallow / "docs"
            self._assert_unknown(latest)

    def test_front_matter_unicode_crlf_and_legacy_dates_are_preserved_or_replaced(self):
        original = ("\ufeff---\r\ntitle: Überprüfung\r\nsearch:\r\n  exclude: true\r\n"
                    "tags: [one, two]\r\nrevision_date: stale\r\n"
                    "git_revision_date_localized: misleading\r\n"
                    "git_creation_date_localized: misleading\r\n...\r\n# Content\r\n")
        self.page.write_bytes(original.encode("utf-8"))
        changed = self._commit("2023-05-06T12:00:00+00:00")
        metadata, body = self._stage(changed)
        self.assertEqual(metadata, {"title": "Überprüfung", "search": {"exclude": True},
                                   "tags": ["one", "two"], "revision_date": "Source updated: 2023-05-06"})
        self.assertEqual(body, "# Content\n")
        self.assertEqual(self.page.read_bytes(), original.encode("utf-8"))
        self._assert_unknown("missing")

    def test_legacy_body_metadata_cannot_override_missing_source_evidence(self):
        _write(self.page, "revision_date: Today\ngit_revision_date_localized: Today\n\n# Content\n")
        self._assert_unknown(self.first)

    def test_deploy_configuration_cannot_recompute_dates_from_staged_paths(self):
        # BaseLoader reads the real config, including !ENV and Python tags,
        # without executing them; mike's second build uses this same config.
        config = yaml.load((ROOT / "mkdocs.yml").read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        names = {name for plugin in config["plugins"]
                 for name in (plugin if isinstance(plugin, dict) else [plugin])}
        self.assertNotIn("git-revision-date-localized", names)
        self.assertEqual(config["docs_dir"], ".site/public-docs")


if __name__ == "__main__":
    unittest.main()
