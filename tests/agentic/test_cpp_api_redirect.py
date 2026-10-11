#!/usr/bin/env python3
"""Tests for publishing the Doxygen C++ API with the `latest` docs version only.

Covers scripts/docs/redirect_cpp_api_to_latest.py (the staged api/cpp tree becomes
one redirect page whose relative target lands on latest's copy) and pins its
wiring in .github/workflows/docs_pages.yml: the deploy job runs it for `v*` tag
deploys only, before the media budget and the strict build, and the required
`docs-build` job neither runs it nor gains a job-level `if:` or a pull_request
path filter.

These tests live in tests/agentic because `python -m unittest discover -s
tests/agentic` runs in the required agentic-pr-gate job on every PR, which
installs the pinned PyYAML used below.
"""

from __future__ import annotations

import importlib.util
import io
import posixpath
import re
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "docs" / "redirect_cpp_api_to_latest.py"
WORKFLOW = ROOT / ".github" / "workflows" / "docs_pages.yml"
API_INDEX = ROOT / "docs" / "api" / "index.md"

spec = importlib.util.spec_from_file_location("redirect_cpp_api_to_latest", SCRIPT)
assert spec and spec.loader
redirect = importlib.util.module_from_spec(spec)
spec.loader.exec_module(redirect)

SCRIPT_CALL = "scripts/docs/redirect_cpp_api_to_latest.py"
TAG_TEST = '[[ "${GITHUB_REF}" == refs/tags/v* ]]'


def _write(path: Path, text: str = "x") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _meta_refresh_target(page: str) -> str:
    match = re.search(r'<meta http-equiv="refresh" content="0; url=([^"]+)">', page)
    assert match, "redirect page has no meta refresh"
    return match.group(1)


class RedirectScriptTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.stage = Path(self._tmp.name) / "public-docs"
        _write(self.stage / "api" / "index.md", "# API\n")
        _write(self.stage / "api" / "cpp" / "html" / "index.html", "<html>doxygen</html>")
        _write(self.stage / "api" / "cpp" / "html" / "classFoo.html", "<html>Foo</html>")
        _write(self.stage / "api" / "cpp" / "html" / "search" / "all_0.js", "var x;")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _run(self, *argv: str) -> int:
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return redirect.main(list(argv))

    def test_tree_becomes_a_single_redirect_page(self) -> None:
        self.assertEqual(self._run("--stage", str(self.stage)), 0)
        remaining = sorted(p.relative_to(self.stage).as_posix() for p in (self.stage / "api" / "cpp").rglob("*") if p.is_file())
        self.assertEqual(remaining, ["api/cpp/html/index.html"])
        page = (self.stage / "api" / "cpp" / "html" / "index.html").read_text(encoding="utf-8")
        self.assertNotIn("doxygen", page)
        self.assertIn('<a href="../../../../latest/api/cpp/html/index.html">', page)
        # Pages outside api/cpp are untouched.
        self.assertEqual((self.stage / "api" / "index.md").read_text(encoding="utf-8"), "# API\n")

    def test_redirect_lands_on_latest_from_a_mike_version_directory(self) -> None:
        # mike publishes version V at <site>/V/; resolve the relative target the
        # way a browser does from <site>/V/api/cpp/html/index.html.
        self.assertEqual(self._run("--stage", str(self.stage), "--version", "latest"), 0)
        page = (self.stage / "api" / "cpp" / "html" / "index.html").read_text(encoding="utf-8")
        target = _meta_refresh_target(page)
        for version in ("v0.1.0", "v1.2.3-rc1"):
            here = f"/godotGS/{version}/api/cpp/html/index.html"
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(here), target))
            self.assertEqual(resolved, "/godotGS/latest/api/cpp/html/index.html")

    def test_missing_staged_api_fails(self) -> None:
        empty = Path(self._tmp.name) / "empty-stage"
        empty.mkdir()
        self.assertEqual(self._run("--stage", str(empty)), 1)
        self.assertFalse((empty / "api").exists())

    def test_invalid_version_is_rejected(self) -> None:
        for bad in ("", "..", "a/b"):
            with self.subTest(version=bad):
                self.assertEqual(self._run("--stage", str(self.stage), "--version", bad), 2)
        # A rejected run leaves the staged tree alone.
        self.assertTrue((self.stage / "api" / "cpp" / "html" / "classFoo.html").exists())

    def test_redirect_page_is_where_the_docs_link_points(self) -> None:
        # docs/api/index.md links to cpp/html/index.html relative to api/; the
        # redirect must occupy exactly that path or tagged versions get a 404.
        text = API_INDEX.read_text(encoding="utf-8")
        self.assertIn('href="cpp/html/index.html"', text)
        self.assertEqual(redirect.INDEX_RELATIVE.as_posix(), "api/cpp/html/index.html")


class DocsPagesWorkflowWiringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
        cls.jobs = cls.workflow["jobs"]

    def _steps_text(self, job: str) -> list[tuple[str, str]]:
        return [(step.get("name", ""), step.get("run", "")) for step in self.jobs[job]["steps"]]

    def test_deploy_runs_the_redirect_for_tag_deploys_only(self) -> None:
        steps = self._steps_text("deploy")
        calls = [(i, run) for i, (_, run) in enumerate(steps) if SCRIPT_CALL in run]
        self.assertEqual(len(calls), 1, "deploy must run the redirect script in exactly one step")
        index, run = calls[0]
        lines = run.splitlines()
        call_line = next(n for n, line in enumerate(lines) if SCRIPT_CALL in line)
        guard_line = next((n for n, line in enumerate(lines) if TAG_TEST in line), None)
        self.assertIsNotNone(guard_line, "the redirect must sit under the v* tag test")
        self.assertLess(guard_line, call_line)
        self.assertTrue(lines[guard_line].strip().startswith("if "))
        fi_line = next(n for n in range(call_line, len(lines)) if lines[n].strip() == "fi")
        self.assertNotIn("else", "\n".join(lines[guard_line:fi_line]), "no else branch may run the redirect")
        self.assertIn("--version latest", " ".join(line.strip().rstrip("\\") for line in lines[call_line:fi_line]))
        # Same tag test as the mike step, so a tag deploy never publishes the tree
        # and a latest deploy always does.
        mike = [run for _, run in steps if "mike deploy" in run]
        self.assertEqual(len(mike), 1)
        self.assertIn(TAG_TEST, mike[0])
        # After staging, before the media budget and the strict build.
        self.assertLess(run.index("stage_public_docs.py"), run.index(SCRIPT_CALL))
        self.assertLess(run.index(SCRIPT_CALL), run.index("check_docs_media_budget.py"))
        strict = next(i for i, (_, step_run) in enumerate(steps) if "mkdocs build --strict" in step_run)
        self.assertLess(index, strict)

    def test_docs_build_stays_an_always_reporting_check(self) -> None:
        job = self.jobs["docs-build"]
        self.assertNotIn("if", job, "a job-level if: on the required docs-build check reports skipped as success")
        self.assertFalse(any(SCRIPT_CALL in run for _, run in self._steps_text("docs-build")))
        triggers = self.workflow.get("on", self.workflow.get(True))
        self.assertIn("pull_request", triggers)
        pull_request = triggers["pull_request"] or {}
        self.assertNotIn("paths", pull_request)
        self.assertNotIn("paths-ignore", pull_request)

    def test_deploy_stays_independent_of_docs_build(self) -> None:
        self.assertNotIn("needs", self.jobs["deploy"])


if __name__ == "__main__":
    unittest.main()
