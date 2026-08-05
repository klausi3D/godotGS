#!/usr/bin/env python3
"""Every script release_builds.yml depends on must also trigger it (#825).

The workflow's `paths:` filters list root-level `"*.py"`. A single `*` does not
span `/` in GitHub's filter syntax, so `tests/ci/anything.py` is NOT covered.
When the export-template jobs were changed to call
`tests/ci/resolve_export_template.py`, a PR touching only that file skipped the
Release Builds workflow entirely -- the resolver could pass its unit tests while
neither real packaging path ran, and a break would first surface in a scheduled
or manual release run.

The hazard is general and worth naming: extracting logic out of a workflow into
a helper module improves testability and *silently reduces integration
coverage*, because the helper lands outside the paths the workflow watches. The
refactor that made the console-wrapper fix verifiable is the same refactor that
opened this gap.

Design: a MANIFEST, cross-checked by discovery
----------------------------------------------
`RELEASE_HELPER_SCRIPTS` is the declared list of scripts this workflow depends
on, and it is what the filter check runs against. Discovery is used only to
prove the manifest is not missing anything it *can* see -- never as the source
of truth, because a regex sweep is exactly what failed here before: an earlier
`run_module_tests.py` sweep missed `run_baseline_qa.py` because the invocation
was one level of indirection away, and widening the regex would have fixed that
instance while leaving the class.

So the rule is: **anything this file cannot model is an error, not silence.**
Unsupported filter patterns raise. Unsupported `python` invocation forms raise.
A local composite action raises. None of them fall through to "covered".

DECLARED LIMITATIONS
--------------------
Each of these fails loudly rather than being silently mis-answered:

* Ordered include/exclude `paths:` evaluation is NOT modelled. GitHub evaluates
  `paths:` as an ordered list where a later `!pattern` excludes, so
  `["tests/ci/**", "!tests/ci/resolve_export_template.py"]` would skip a
  resolver-only change while a naive `any()` reports it covered -- green over
  the exact gap this guard exists to close. Rather than ship an unreviewed
  semantics model for a construct the workflow does not use, any `!` pattern
  raises `UnsupportedFilterPattern`. Teaching the guard last-match-wins is a
  small change; doing it blind is not.
* Glob constructs beyond `*`, `**` and `?` (character classes, `+`, braces,
  backslash escapes) raise rather than being matched literally.
* `python` invocations that are not a module (`-m`), inline (`-c`), a
  flags-only run (`--version`), or a resolvable script path raise
  `UnsupportedInvocation` -- including a quoted or variable script argument, or
  a `.py` path that does not exist in the tree.
* Script references are only looked for on lines that name an interpreter, or
  on `./`-prefixed tokens. A helper reached through a wrapper the workflow does
  not name on the same line is out of reach of discovery -- which is why the
  manifest, not discovery, is the source of truth.
* Non-python interpreter lines (`bash x.sh`, `pwsh x.ps1`) contribute only
  paths that exist in the tree; unlike the python parser, a reference to a
  missing file is skipped rather than raised, because the argument grammar of
  an arbitrary shell is not modelled.
* Steps that `uses:` a LOCAL composite action (`uses: ./...`) raise: their
  `run:` bodies live in another file this guard does not read. Third-party
  `uses:` steps are out of scope by design; they are not repository paths.
* Commented-out lines are ignored, in YAML and in shell bodies alike.

No PyYAML. `tests/ci/run_module_tests.py --guard-only` runs under a bare
`actions/setup-python` interpreter, which ships no third-party packages, so the
readers below are hand-rolled -- and tested against inline fixtures rather than
trusted.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path
from typing import Dict, List, Sequence, Set

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "release_builds.yml"
FILTERED_EVENTS = ("pull_request", "push")

# The declared dependency set. The filter check runs against THIS, not against
# whatever a regex happened to find. Add an entry when a job starts depending on
# a repository script -- discovery below will fail the guard if you forget one
# it can see, and the limitations above say which forms it cannot.
RELEASE_HELPER_SCRIPTS = (
    "tests/ci/resolve_export_template.py",
    "tests/ci/check_renderer_release_gates.py",
    "tests/ci/release_attestation.py",
)

# The self-hosted jobs check out under `repo/`; `paths:` are repository-relative.
CHECKOUT_PREFIX = "repo/"
SCRIPT_SUFFIXES = (".py", ".sh", ".ps1")
# Neither boundary can be `\b`: it matches INSIDE
# `actions/setup-python@<sha>` (hyphen before) and inside `python-version:`
# (hyphen after), so the pinned action and the setup-python input both parsed
# as python invocations. Hyphens must be excluded on both sides.
INTERPRETERS = re.compile(r"(?<![\w./-])(?:python3?|bash|sh|pwsh|powershell)(?![\w-])")
PYTHON_TOKEN = re.compile(r"(?<![\w./-])python3?(?![\w-])")
LIST_ITEM = re.compile(r'^\s*-\s*["\']?([^"\'#]+?)["\']?\s*(?:#.*)?$')
LOCAL_ACTION = re.compile(r"^\s*(?:-\s*)?uses:\s*\./")

# python flags that take no argument and never name a script.
PY_FLAGS_NO_ARG = frozenset(
    {"-u", "-E", "-s", "-S", "-O", "-OO", "-B", "-q", "-I", "-v", "-b", "-d"}
)
# python flags that consume the following token.
PY_FLAGS_WITH_ARG = frozenset({"-X", "-W", "--check-hash-based-pycs"})
# Forms that terminate parsing without naming a repository script.
PY_TERMINAL_OK = frozenset({"-m", "-c", "--version", "-V", "-h", "--help", "-"})
# Unmodelled glob constructs; matching them literally would be wrong.
UNSUPPORTED_GLOB_CHARS = "[]{}+\\"


class UnsupportedFilterPattern(RuntimeError):
    """A `paths:` pattern this guard refuses to reason about."""


class UnsupportedInvocation(RuntimeError):
    """A workflow line that runs something this guard cannot resolve."""


def github_path_match(pattern: str, path: str) -> bool:
    """GitHub `paths:` glob semantics: `*` stops at `/`, `**` does not.

    `fnmatch` cannot be used here -- its `*` happily crosses `/`, which is the
    exact assumption that made `"*.py"` look like it covered `tests/ci/*.py`.
    """
    if pattern.startswith("!"):
        raise UnsupportedFilterPattern(
            f"Negated paths: pattern {pattern!r}. GitHub evaluates paths: as an ORDERED "
            "include/exclude list, and this guard does not model that -- reporting such a "
            "filter as 'covered' would be green over the exact gap it exists to close. "
            "Either drop the negation, or teach this guard last-match-wins evaluation and "
            "remove this refusal (see DECLARED LIMITATIONS)."
        )
    bad = sorted({char for char in pattern if char in UNSUPPORTED_GLOB_CHARS})
    if bad:
        raise UnsupportedFilterPattern(
            f"paths: pattern {pattern!r} uses glob construct(s) {bad} that this guard does not "
            "model (only *, ** and ? are). Matching them literally would silently mis-answer "
            "coverage."
        )

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


def path_is_covered(patterns: Sequence[str], path: str) -> bool:
    """Does `path` trigger a workflow filtered by `patterns`?

    Every pattern is evaluated EAGERLY before the result is reduced. A
    generator inside `any()` short-circuits on the first positive match and
    would never reach a later `!pattern` -- which is exactly the defect this
    function exists to refuse, one layer down. The list comprehension is
    load-bearing; do not "simplify" it back into a generator.
    """
    matches = [github_path_match(pattern, path) for pattern in patterns]
    return any(matches)


def parse_event_paths(workflow_text: str) -> Dict[str, List[str]]:
    """Extract `on.<event>.paths` for each event that declares one.

    Indentation-driven: an event key sits one level inside `on:`, its `paths:`
    key one level deeper, and the entries deeper still.
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

        # on: -> <event>: -> paths:. This reader works on literal text, so the
        # key is simply "on" (YAML 1.1 would read a bare `on` as boolean True).
        chain = [name for _, name in stack]
        if len(chain) == 3 and chain[0] == "on" and chain[2] == "paths":
            collecting_for = chain[1]
            paths_indent = indent
            result.setdefault(collecting_for, [])

    return result


def _normalise(token: str) -> str:
    token = token.strip().strip("\"'")
    token = token.replace("\\", "/")
    if token.startswith("./"):
        token = token[2:]
    if token.startswith(CHECKOUT_PREFIX):
        token = token[len(CHECKOUT_PREFIX) :]
    return token


def _content_lines(workflow_text: str) -> List[str]:
    """Lines that are neither blank nor a YAML/shell comment."""
    return [
        line
        for line in workflow_text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def check_python_invocations(workflow_text: str) -> Set[str]:
    """Repo scripts run via python; raises on any form it cannot classify.

    Silence is the failure mode being designed out here: an invocation this
    parser does not understand must stop the guard, not be skipped.
    """
    found: Set[str] = set()
    for line in _content_lines(workflow_text):
        for match in PYTHON_TOKEN.finditer(line):
            argv = line[match.end() :].split()
            index = 0
            while index < len(argv):
                token = argv[index]
                if token in PY_TERMINAL_OK or token.startswith("-c") and token != "-c":
                    break
                if token in PY_FLAGS_WITH_ARG:
                    index += 2
                    continue
                if token in PY_FLAGS_NO_ARG:
                    index += 1
                    continue
                if token.startswith("-"):
                    raise UnsupportedInvocation(
                        f"Unrecognised python flag {token!r} in: {line.strip()!r}. This guard "
                        "cannot tell whether a script follows it, so it refuses rather than "
                        "guessing (see DECLARED LIMITATIONS)."
                    )
                candidate = _normalise(token)
                if not candidate.endswith(".py"):
                    raise UnsupportedInvocation(
                        f"python argument {token!r} is not a literal .py path in: "
                        f"{line.strip()!r}. A variable or computed script path cannot be "
                        "checked against paths:; name it literally or add it to "
                        "RELEASE_HELPER_SCRIPTS and extend this parser."
                    )
                if not (ROOT / candidate).is_file():
                    raise UnsupportedInvocation(
                        f"python script {candidate!r} does not exist in the tree "
                        f"(from: {line.strip()!r})."
                    )
                found.add(candidate)
                break
            else:
                # `python` with no arguments at all.
                raise UnsupportedInvocation(
                    f"Bare python invocation with no resolvable argument in: {line.strip()!r}."
                )
    return found


def referenced_scripts(workflow_text: str) -> Set[str]:
    """Repo scripts (.py/.sh/.ps1) the workflow names on an interpreter line.

    Deliberately narrow, and narrowness is declared rather than hidden: this is
    a cross-check on the manifest, never a replacement for it.
    """
    found: Set[str] = set(check_python_invocations(workflow_text))
    for line in _content_lines(workflow_text):
        interpreted = INTERPRETERS.search(line) is not None
        for token in line.split():
            normalised = _normalise(token)
            if not normalised.endswith(SCRIPT_SUFFIXES):
                continue
            if not (interpreted or token.strip("\"'").startswith(("./", ".\\"))):
                continue
            if (ROOT / normalised).is_file():
                found.add(normalised)
    return found


def local_composite_actions(workflow_text: str) -> List[str]:
    return [line.strip() for line in _content_lines(workflow_text) if LOCAL_ACTION.match(line)]


def _workflow_text() -> str:
    return WORKFLOW.read_text(encoding="utf-8")


class NegatedPatternTests(unittest.TestCase):
    """The Medium this round: an ordered include/exclude list must not read as covered."""

    NEGATED = ["tests/ci/**", "!tests/ci/resolve_export_template.py"]

    def test_negated_pattern_raises_rather_than_matching_literally(self) -> None:
        with self.assertRaises(UnsupportedFilterPattern) as ctx:
            github_path_match("!tests/ci/resolve_export_template.py", "tests/ci/other.py")
        self.assertIn("ORDERED", str(ctx.exception))

    def test_coverage_check_refuses_a_filter_list_containing_a_negation(self) -> None:
        """GitHub would SKIP a resolver-only change here; the guard must not say 'covered'."""
        with self.assertRaises(UnsupportedFilterPattern):
            path_is_covered(self.NEGATED, "tests/ci/resolve_export_template.py")

    def test_negation_is_refused_even_when_an_earlier_pattern_matches(self) -> None:
        # `any()` would have short-circuited on "tests/ci/**" and returned True
        # before ever seeing the negation. Order must not hide it.
        with self.assertRaises(UnsupportedFilterPattern):
            path_is_covered(self.NEGATED, "tests/ci/anything_at_all.py")

    def test_unmodelled_glob_constructs_raise(self) -> None:
        for pattern in ("tests/ci/[abc].py", "tests/{ci,runtime}/x.py", "tests/ci/a+.py"):
            with self.subTest(pattern=pattern):
                with self.assertRaises(UnsupportedFilterPattern):
                    github_path_match(pattern, "tests/ci/a.py")

    def test_the_real_workflow_uses_no_unsupported_pattern(self) -> None:
        filters = parse_event_paths(_workflow_text())
        for event in FILTERED_EVENTS:
            for pattern in filters[event]:
                with self.subTest(event=event, pattern=pattern):
                    github_path_match(pattern, "some/path.py")


class UnsupportedInvocationTests(unittest.TestCase):
    """Unparseable invocations are errors, not silence."""

    def test_variable_script_path_raises(self) -> None:
        with self.assertRaises(UnsupportedInvocation):
            check_python_invocations('        run: python "$SCRIPT" --flag\n')

    def test_nonexistent_script_raises(self) -> None:
        with self.assertRaises(UnsupportedInvocation):
            check_python_invocations("        run: python tests/ci/not_a_real_script.py\n")

    def test_unknown_flag_raises_rather_than_being_skipped(self) -> None:
        with self.assertRaises(UnsupportedInvocation):
            check_python_invocations("        run: python --frobnicate tests/ci/release_attestation.py\n")

    def test_bare_python_raises(self) -> None:
        with self.assertRaises(UnsupportedInvocation):
            check_python_invocations("        run: python\n")

    def test_dash_u_and_other_no_arg_flags_still_find_the_script(self) -> None:
        found = check_python_invocations("        run: python -u tests/ci/release_attestation.py x\n")
        self.assertEqual(found, {"tests/ci/release_attestation.py"})

    def test_module_and_inline_forms_are_accepted_without_a_script(self) -> None:
        for body in (
            "        run: python -m pip install --upgrade pip\n",
            "        run: python -m SCons @args\n",
            '        run: python -c "import sys; print(sys.version)"\n',
            "        run: python --version\n",
        ):
            with self.subTest(body=body.strip()):
                self.assertEqual(check_python_invocations(body), set())

    def test_repo_prefix_from_the_self_hosted_checkout_is_stripped(self) -> None:
        found = check_python_invocations("        run: python repo/tests/ci/release_attestation.py generate\n")
        self.assertEqual(found, {"tests/ci/release_attestation.py"})

    def test_commented_out_invocations_are_ignored(self) -> None:
        self.assertEqual(check_python_invocations("          # python $UNPARSEABLE\n"), set())

    def test_the_real_workflow_parses_cleanly(self) -> None:
        # If this raises, a new invocation form landed and the guard is telling
        # you it can no longer vouch for coverage.
        self.assertTrue(check_python_invocations(_workflow_text()))


class ManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = _workflow_text()
        self.filters = parse_event_paths(self.text)

    def test_every_manifest_script_triggers_the_workflow(self) -> None:
        for event in FILTERED_EVENTS:
            patterns = self.filters[event]
            for script in RELEASE_HELPER_SCRIPTS:
                with self.subTest(event=event, script=script):
                    self.assertTrue(
                        path_is_covered(patterns, script),
                        f"{script} is a declared release_builds.yml dependency but no `{event}` "
                        f"paths: filter matches it, so changing it would skip the lanes that run "
                        f"it. Add it to the {event} filter. Current filters: {patterns}",
                    )

    def test_manifest_has_no_stale_entries(self) -> None:
        for script in RELEASE_HELPER_SCRIPTS:
            with self.subTest(script=script):
                self.assertTrue((ROOT / script).is_file(), f"{script} no longer exists")
                self.assertIn(
                    script,
                    self.text,
                    f"{script} is in RELEASE_HELPER_SCRIPTS but release_builds.yml no longer "
                    "mentions it; drop it from the manifest and from paths:.",
                )

    def test_discovery_finds_nothing_the_manifest_omits(self) -> None:
        discovered = referenced_scripts(self.text)
        # `paths:` list entries are not interpreter lines, so they are not
        # discovered; only real invocations are.
        missing = sorted(discovered - set(RELEASE_HELPER_SCRIPTS))
        self.assertEqual(
            missing,
            [],
            f"release_builds.yml references {missing} but RELEASE_HELPER_SCRIPTS does not list "
            "them, so a change to one would skip the workflow.",
        )

    def test_discovery_is_not_vacuous(self) -> None:
        # A sweep that silently stopped matching would make the check above pass
        # over an empty set -- which is how this class of gap survives.
        self.assertGreaterEqual(len(referenced_scripts(self.text)), len(RELEASE_HELPER_SCRIPTS))

    def test_no_local_composite_actions(self) -> None:
        self.assertEqual(
            local_composite_actions(self.text),
            [],
            "release_builds.yml uses a local composite action; its run: bodies live in another "
            "file this guard does not read, so the manifest can no longer be cross-checked. "
            "Extend the guard to follow it, or list its scripts explicitly.",
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
                path_is_covered(self.filters[event], ".github/workflows/release_builds.yml"),
                f"release_builds.yml does not trigger itself on {event}.",
            )


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

    def test_negated_entries_survive_parsing_so_the_matcher_can_refuse_them(self) -> None:
        parsed = parse_event_paths(
            'on:\n  push:\n    paths:\n      - "tests/ci/**"\n      - "!tests/ci/x.py"\n'
        )
        self.assertEqual(parsed["push"], ["tests/ci/**", "!tests/ci/x.py"])

    def test_reads_the_real_workflow(self) -> None:
        parsed = parse_event_paths(_workflow_text())
        for event in FILTERED_EVENTS:
            self.assertIn(event, parsed, f"no paths: block parsed for {event}")
            self.assertIn("SConstruct", parsed[event])
            self.assertIn(".github/workflows/release_builds.yml", parsed[event])


class LocalCompositeActionTests(unittest.TestCase):
    def test_local_action_is_detected(self) -> None:
        for body in (
            "      - uses: ./.github/actions/build\n",
            "        uses: ./.github/actions/build\n",
        ):
            with self.subTest(body=body.strip()):
                found = local_composite_actions(body)
                self.assertEqual(len(found), 1)
                self.assertIn("./.github/actions/build", found[0])

    def test_third_party_action_is_not_flagged(self) -> None:
        self.assertEqual(local_composite_actions("      - uses: actions/checkout@v4\n"), [])


if __name__ == "__main__":
    sys.exit(not unittest.main(exit=False, verbosity=2).result.wasSuccessful())
