#!/usr/bin/env python3
"""Every script release_builds.yml runs must trigger release_builds.yml (#825).

The workflow's `paths:` filters list root-level `"*.py"`. A single `*` does not
span `/` in GitHub's filter syntax, so `tests/ci/anything.py` is NOT covered.
When the export-template jobs were changed to call
`tests/ci/resolve_export_template.py`, a PR touching only that file skipped the
Release Builds workflow entirely -- the resolver could pass its 19 unit tests
while neither real packaging path ran, and a break would first surface in a
scheduled or manual release run.

The hazard is general and worth naming: extracting logic out of a workflow into
a helper module improves testability and *silently reduces integration
coverage*, because the helper lands outside the paths the workflow watches. The
refactor that made round 2's console-wrapper fix verifiable is the same refactor
that opened this gap.

Listing the helpers by name in `paths:` fixes the instance. This file fixes the
class: it derives the set of scripts the workflow actually executes from the
workflow itself and fails if any of them is not covered by both filters. The
hand-written list therefore cannot drift -- a new helper fails here until it is
listed, instead of quietly reopening the gap.

`tests/ci/**` was rejected as the pattern: it would fire two editor builds plus
two template builds on every unrelated guard edit, which trains people to ignore
the lane.

No PyYAML. `tests/ci/run_module_tests.py --guard-only` runs under a bare
`actions/setup-python` interpreter, which ships no third-party packages, so the
two small readers below are hand-rolled -- and tested against inline fixtures
rather than trusted.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path
from typing import Dict, List, Set

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "release_builds.yml"
FILTERED_EVENTS = ("pull_request", "push")

# `python tests/ci/foo.py`, `python3 tests/ci/foo.py`, `python repo/tests/ci/foo.py`.
# The self-hosted jobs check out under `repo/`, so that prefix is stripped to get
# the repository-relative path a `paths:` filter is matched against.
SCRIPT_INVOCATION = re.compile(r"\bpython3?\s+((?:[\w./-]+/)?[\w.-]+\.py)\b")
CHECKOUT_PREFIX = "repo/"

LIST_ITEM = re.compile(r'^\s*-\s*["\']?([^"\'#]+?)["\']?\s*(?:#.*)?$')


def github_path_match(pattern: str, path: str) -> bool:
    """GitHub `paths:` semantics: `*` stops at `/`, `**` does not.

    `fnmatch` cannot be used here -- its `*` happily crosses `/`, which is the
    exact assumption that made `"*.py"` look like it covered `tests/ci/*.py`.
    """
    regex = ""
    index = 0
    while index < len(pattern):
        char = pattern[index]
        if char == "*":
            if pattern[index : index + 2] == "**":
                regex += ".*"
                index += 2
                continue
            regex += "[^/]*"
        elif char == "?":
            regex += "[^/]"
        else:
            regex += re.escape(char)
        index += 1
    return re.fullmatch(regex, path) is not None


def parse_event_paths(workflow_text: str) -> Dict[str, List[str]]:
    """Extract `on.<event>.paths` for each event that declares one.

    Indentation-driven: an event key sits at one indent level, its `paths:` key
    one level deeper, and the entries deeper still. Anything at or left of the
    `paths:` indent ends the block.
    """
    result: Dict[str, List[str]] = {}
    stack: List[tuple[int, str]] = []
    collecting_for: str | None = None
    paths_indent = -1

    for raw in workflow_text.splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip())
        stripped = raw.strip()

        if collecting_for is not None:
            if indent > paths_indent and stripped.startswith("-"):
                match = LIST_ITEM.match(raw)
                if match:
                    result[collecting_for].append(match.group(1).strip())
                continue
            collecting_for = None

        key_match = re.match(r"^([A-Za-z_][\w-]*):", stripped)
        if not key_match:
            continue
        key = key_match.group(1)

        while stack and stack[-1][0] >= indent:
            stack.pop()
        stack.append((indent, key))

        # on: -> <event>: -> paths:. YAML 1.1 would read a bare `on` as the
        # boolean True, but this reader works on the literal text, so the key is
        # simply "on".
        chain = [name for _, name in stack]
        if len(chain) == 3 and chain[0] == "on" and chain[2] == "paths":
            collecting_for = chain[1]
            paths_indent = indent
            result.setdefault(collecting_for, [])

    return result


def executed_scripts(workflow_text: str) -> Set[str]:
    """Repo-relative .py paths the workflow invokes with python/python3."""
    found: Set[str] = set()
    for match in SCRIPT_INVOCATION.findall(workflow_text):
        candidate = match[len(CHECKOUT_PREFIX) :] if match.startswith(CHECKOUT_PREFIX) else match
        if (ROOT / candidate).is_file():
            found.add(candidate)
    return found


def _workflow_text() -> str:
    return WORKFLOW.read_text(encoding="utf-8")


class PathsParserTests(unittest.TestCase):
    """The reader is hand-rolled, so it gets its own fixture."""

    FIXTURE = """\
name: Example
on:
  pull_request:
    branches: [master, main]
    paths:
      - "core/**"
      # a comment inside the block
      - "*.py"
      - "tests/ci/helper.py"
  push:
    branches: [master]
    tags:
      - "v*"
    paths:
      - "core/**"
  schedule:
    - cron: "30 2 * * *"
  workflow_dispatch:
    inputs:
      publish_channel:
        description: "not a path"
jobs:
  build:
    steps:
      - name: Go
        run: python tests/ci/helper.py
"""

    def test_extracts_each_events_paths(self) -> None:
        parsed = parse_event_paths(self.FIXTURE)
        self.assertEqual(parsed["pull_request"], ["core/**", "*.py", "tests/ci/helper.py"])
        self.assertEqual(parsed["push"], ["core/**"])

    def test_does_not_invent_paths_for_events_without_them(self) -> None:
        parsed = parse_event_paths(self.FIXTURE)
        self.assertNotIn("schedule", parsed)
        self.assertNotIn("workflow_dispatch", parsed)

    def test_tags_list_is_not_mistaken_for_paths(self) -> None:
        self.assertNotIn("v*", parse_event_paths(self.FIXTURE)["push"])

    def test_reads_the_real_workflow(self) -> None:
        parsed = parse_event_paths(_workflow_text())
        for event in FILTERED_EVENTS:
            self.assertIn(event, parsed, f"no paths: block parsed for {event}")
            self.assertIn("SConstruct", parsed[event])
            self.assertIn(".github/workflows/release_builds.yml", parsed[event])


class ExecutedScriptDiscoveryTests(unittest.TestCase):
    """The discovery itself must not be vacuous."""

    def test_discovery_finds_the_known_helpers(self) -> None:
        scripts = executed_scripts(_workflow_text())
        self.assertIn("tests/ci/resolve_export_template.py", scripts)
        self.assertIn("tests/ci/check_renderer_release_gates.py", scripts)
        self.assertIn("tests/ci/release_attestation.py", scripts)

    def test_discovery_is_not_empty(self) -> None:
        # A regex that silently stopped matching would make the contract test
        # below pass over an empty set.
        self.assertGreaterEqual(len(executed_scripts(_workflow_text())), 3)

    def test_repo_prefix_from_the_self_hosted_checkout_is_stripped(self) -> None:
        # publish_release runs `python repo/tests/ci/release_attestation.py`,
        # but `paths:` are matched repository-relative.
        matches = SCRIPT_INVOCATION.findall("python repo/tests/ci/release_attestation.py generate\n")
        self.assertEqual(matches, ["repo/tests/ci/release_attestation.py"])

    def test_nonexistent_script_paths_are_dropped(self) -> None:
        self.assertEqual(executed_scripts("run: python tests/ci/not_a_real_script.py\n"), set())


class PathFilterSemanticsTests(unittest.TestCase):
    """Pin the semantics the original gap depended on."""

    def test_single_star_does_not_span_a_directory_separator(self) -> None:
        self.assertTrue(github_path_match("*.py", "setup.py"))
        self.assertFalse(github_path_match("*.py", "tests/ci/resolve_export_template.py"))

    def test_double_star_does_span_directory_separators(self) -> None:
        self.assertTrue(github_path_match("modules/**", "modules/gaussian_splatting/core/x.cpp"))
        self.assertTrue(github_path_match("tests/ci/**", "tests/ci/resolve_export_template.py"))

    def test_exact_path_matches_itself_only(self) -> None:
        self.assertTrue(
            github_path_match(
                "tests/ci/resolve_export_template.py", "tests/ci/resolve_export_template.py"
            )
        )
        self.assertFalse(github_path_match("tests/ci/resolve_export_template.py", "tests/ci/other.py"))


class ExecutedScriptsAreTriggersTests(unittest.TestCase):
    """The class-level check: derived set vs. the hand-written filters."""

    def setUp(self) -> None:
        self.text = _workflow_text()
        self.filters = parse_event_paths(self.text)
        self.scripts = executed_scripts(self.text)

    def test_every_executed_script_triggers_the_workflow(self) -> None:
        for event in FILTERED_EVENTS:
            patterns = self.filters[event]
            for script in sorted(self.scripts):
                with self.subTest(event=event, script=script):
                    self.assertTrue(
                        any(github_path_match(pattern, script) for pattern in patterns),
                        f"{script} is executed by release_builds.yml but no `{event}` paths: "
                        f"filter matches it, so changing it would skip the lanes that run it. "
                        f"Add it to the {event} filter. Current filters: {patterns}",
                    )

    def test_both_event_filters_stay_in_sync(self) -> None:
        self.assertEqual(
            sorted(self.filters["pull_request"]),
            sorted(self.filters["push"]),
            "release_builds.yml's pull_request and push paths: filters have diverged; a script "
            "covered on one event and not the other is the same gap, half-closed.",
        )

    def test_the_workflow_file_itself_triggers_the_workflow(self) -> None:
        for event in FILTERED_EVENTS:
            self.assertTrue(
                any(
                    github_path_match(pattern, ".github/workflows/release_builds.yml")
                    for pattern in self.filters[event]
                ),
                f"release_builds.yml does not trigger itself on {event}.",
            )


if __name__ == "__main__":
    sys.exit(not unittest.main(exit=False, verbosity=2).result.wasSuccessful())
