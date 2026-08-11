#!/usr/bin/env python3
"""Nothing publishes a release artifact that no lane has executed (#825).

`export_smoke_windows` is the first and only lane that RUNS what
`build_windows_export_template` builds and what `publish_release` ships. It was
added as a sibling job: `publish_release` listed it under neither `needs:` nor
its `if:`, so GitHub was free to attach the assets and move the tag while the
smoke test was still running, and equally free to do so after it had failed. The
workflow described a blocking check and wired a decorative one.

Two halves, and neither is sufficient alone
-------------------------------------------
* The `needs:` entry is the only thing that makes a job WAIT. GitHub derives job
  ordering from `needs:` and from nothing else, so without it "blocking" is not
  even a scheduling claim.
* Under `always()` a `needs:` entry blocks NOTHING -- the job runs whatever its
  dependencies did. The result has to be asserted in the `if:`. This workflow
  already records that lesson for `finite_math_guard`; the export smoke test
  reproduced the defect it exists to fix.

So this guard checks both, and checks them by EVALUATING the real `if:`
expression over a truth table rather than by grepping for a substring. A
grep-shaped guard passes on `needs.export_smoke_windows.result != 'success'`,
on a clause `||`-ed with something always true, and on a clause parenthesised
into irrelevance. The question a gate guard has to answer is "for which states
of the world does publication happen", and only evaluation answers it.

Which jobs are gated is DERIVED, not listed: any job holding
`permissions: contents: write` can create or mutate a release, so any such job
must have the smoke test in its transitive `needs:` closure. A future
write-scoped job is caught by construction.

Fail-closed: any `if:`, `needs:` or `permissions:` form this file cannot model
raises rather than being skipped. No PyYAML -- `run_module_tests.py --guard-only`
runs under a bare `actions/setup-python` interpreter.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "release_builds.yml"

SMOKE_JOB = "export_smoke_windows"
PUBLISHING_CHANNELS = ("stable", "nightly")
# Job results GitHub can report. `skipped` and `cancelled` are the two that a
# `== 'failure'` check would wave through, which is why the workflow spells its
# assertions `== 'success'` / `!= 'success'`.
JOB_RESULTS = ("success", "failure", "cancelled", "skipped")

JOB_KEY = re.compile(r"^  ([A-Za-z_][A-Za-z0-9_-]*):\s*$")
JOB_LEVEL_KEY = re.compile(r"^    ([A-Za-z_][A-Za-z0-9_-]*):(.*)$")
LIST_ITEM = re.compile(r'^\s*-\s*["\']?([A-Za-z_][A-Za-z0-9_-]*)["\']?\s*(?:#.*)?$')
BLOCK_SCALARS = ("|", ">", "|-", ">-", "|+", ">+")


class UnmodelledWorkflowConstruct(RuntimeError):
    """A workflow construct this guard refuses to reason about."""


class ExpressionError(RuntimeError):
    """A GitHub expression this guard refuses to evaluate."""


# --------------------------------------------------------------------------
# Workflow reading
# --------------------------------------------------------------------------


def _workflow_lines() -> List[str]:
    return WORKFLOW.read_text(encoding="utf-8").splitlines()


def _strip_comment(value: str) -> str:
    # Only a comment that starts the token; `'#'` inside a quoted string would
    # need real YAML. None of the keys read here carry quoted `#`, and a value
    # that did would be caught by the parsers below rather than mangled.
    hash_at = value.find("#")
    return value if hash_at < 0 else value[:hash_at]


def _parse_needs(rest: str, lines: List[str], index: int, job: str) -> Tuple[List[str], int]:
    """`needs:` in block-list or inline-flow form. Anything else raises."""
    value = _strip_comment(rest).strip()
    if value.startswith("["):
        if not value.endswith("]"):
            raise UnmodelledWorkflowConstruct(
                f"Job {job!r} has a multi-line inline `needs:` this guard cannot read."
            )
        body = value[1:-1].strip()
        entries = [item.strip().strip("\"'") for item in body.split(",") if item.strip()]
        return entries, index + 1
    if value:
        # `needs: build_linux` -- a single scalar.
        return [value.strip("\"'")], index + 1

    entries = []
    index += 1
    while index < len(lines):
        line = lines[index]
        if not line.strip():
            index += 1
            continue
        if not line.startswith("      "):
            break
        match = LIST_ITEM.match(line)
        if not match:
            raise UnmodelledWorkflowConstruct(
                f"Job {job!r} has a `needs:` entry this guard cannot read: {line!r}"
            )
        entries.append(match.group(1))
        index += 1
    if not entries:
        raise UnmodelledWorkflowConstruct(f"Job {job!r} has an empty `needs:` block.")
    return entries, index


def _parse_block(rest: str, lines: List[str], index: int, job: str, key: str) -> Tuple[str, int]:
    """A job-level scalar that may be a block scalar. Returns (joined text, next index)."""
    value = rest.strip()
    if value in BLOCK_SCALARS:
        block: List[str] = []
        index += 1
        while index < len(lines):
            nxt = lines[index]
            if nxt.strip() and not nxt.startswith("      "):
                break
            block.append(nxt.strip())
            index += 1
        return " ".join(part for part in block if part), index
    if not value:
        raise UnmodelledWorkflowConstruct(
            f"Job {job!r} has an empty `{key}:`; this guard cannot tell what it evaluates to."
        )
    return value, index + 1


def _parse_permissions(rest: str, lines: List[str], index: int, job: str) -> Tuple[Dict[str, str], int]:
    value = _strip_comment(rest).strip()
    if value:
        raise UnmodelledWorkflowConstruct(
            f"Job {job!r} declares `permissions: {value}` in a scalar form this guard cannot "
            "map to a contents scope."
        )
    scopes: Dict[str, str] = {}
    index += 1
    while index < len(lines):
        line = lines[index]
        if not line.strip():
            index += 1
            continue
        if not line.startswith("      "):
            break
        body = _strip_comment(line).strip()
        if not body:
            index += 1
            continue
        if ":" not in body:
            raise UnmodelledWorkflowConstruct(
                f"Job {job!r} has a `permissions:` entry this guard cannot read: {line!r}"
            )
        scope, level = body.split(":", 1)
        scopes[scope.strip()] = level.strip().strip("\"'")
        index += 1
    return scopes, index


def parse_jobs(lines: Optional[List[str]] = None) -> Dict[str, Dict[str, object]]:
    """Job name -> {"needs": [...], "if": raw or None, "permissions": {...}}."""
    if lines is None:
        lines = _workflow_lines()

    jobs: Dict[str, Dict[str, object]] = {}
    in_jobs = False
    current: Optional[str] = None

    index = 0
    while index < len(lines):
        line = lines[index]
        if line.rstrip() == "jobs:":
            in_jobs = True
            index += 1
            continue
        if in_jobs and line.strip() and not line.startswith(" "):
            in_jobs = False
        if not in_jobs:
            index += 1
            continue

        job_match = JOB_KEY.match(line)
        if job_match:
            current = job_match.group(1)
            jobs[current] = {"needs": [], "if": None, "permissions": {}}
            index += 1
            continue

        key_match = JOB_LEVEL_KEY.match(line) if current else None
        if not key_match:
            index += 1
            continue

        key, rest = key_match.group(1), key_match.group(2)
        if key == "needs":
            jobs[current]["needs"], index = _parse_needs(rest, lines, index, current)
            continue
        if key == "if":
            jobs[current]["if"], index = _parse_block(rest, lines, index, current, "if")
            continue
        if key == "permissions":
            jobs[current]["permissions"], index = _parse_permissions(rest, lines, index, current)
            continue
        index += 1

    if not jobs:
        raise UnmodelledWorkflowConstruct("No jobs were parsed out of release_builds.yml.")
    return jobs


def needs_closure(jobs: Dict[str, Dict[str, object]], job: str) -> Set[str]:
    seen: Set[str] = set()
    pending = list(jobs[job]["needs"])
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        if name not in jobs:
            raise UnmodelledWorkflowConstruct(
                f"Job {job!r} needs {name!r}, which is not a job in this workflow."
            )
        seen.add(name)
        pending.extend(jobs[name]["needs"])
    return seen


def release_side_effect_jobs(jobs: Dict[str, Dict[str, object]]) -> List[str]:
    """Jobs that can create or mutate a release: `permissions: contents: write`."""
    return sorted(
        name for name, data in jobs.items() if data["permissions"].get("contents") == "write"
    )


# --------------------------------------------------------------------------
# GitHub expression evaluation
# --------------------------------------------------------------------------
#
# A deliberately tiny recursive-descent reader for the subset this workflow
# uses. Everything it does not know raises: an expression this guard cannot
# evaluate must not read as an expression that gates.

TOKEN = re.compile(
    r"""\s*(?:
        (?P<string>'(?:[^']|'')*')
      | (?P<op>\(|\)|&&|\|\||==|!=|!)
      | (?P<ident>[A-Za-z_][A-Za-z0-9_.\-]*(?:\(\))?)
    )""",
    re.VERBOSE,
)


def _tokenize(expression: str) -> List[Tuple[str, str]]:
    tokens: List[Tuple[str, str]] = []
    position = 0
    text = expression.strip()
    if text.startswith("${{") and text.endswith("}}"):
        text = text[3:-2].strip()
    while position < len(text):
        match = TOKEN.match(text, position)
        if not match:
            raise ExpressionError(f"Cannot tokenize {text[position:]!r} in {expression!r}")
        position = match.end()
        for kind in ("string", "op", "ident"):
            value = match.group(kind)
            if value is not None:
                tokens.append((kind, value))
                break
    if not tokens:
        raise ExpressionError(f"Empty expression: {expression!r}")
    return tokens


class _Parser:
    def __init__(self, tokens: Sequence[Tuple[str, str]], context: Dict[str, object]) -> None:
        self.tokens = list(tokens)
        self.position = 0
        self.context = context

    def peek(self) -> Optional[Tuple[str, str]]:
        return self.tokens[self.position] if self.position < len(self.tokens) else None

    def take(self) -> Tuple[str, str]:
        token = self.peek()
        if token is None:
            raise ExpressionError("Unexpected end of expression")
        self.position += 1
        return token

    def parse(self) -> object:
        value = self.parse_or()
        if self.peek() is not None:
            raise ExpressionError(f"Trailing tokens: {self.tokens[self.position:]}")
        return value

    def parse_or(self) -> object:
        left = self.parse_and()
        while self.peek() == ("op", "||"):
            self.take()
            right = self.parse_and()
            left = _truthy(left) or _truthy(right)
        return left

    def parse_and(self) -> object:
        left = self.parse_comparison()
        while self.peek() == ("op", "&&"):
            self.take()
            right = self.parse_comparison()
            left = _truthy(left) and _truthy(right)
        return left

    def parse_comparison(self) -> object:
        left = self.parse_unary()
        token = self.peek()
        if token is not None and token[0] == "op" and token[1] in ("==", "!="):
            self.take()
            right = self.parse_unary()
            return left == right if token[1] == "==" else left != right
        return left

    def parse_unary(self) -> object:
        token = self.peek()
        if token == ("op", "!"):
            self.take()
            return not _truthy(self.parse_unary())
        return self.parse_primary()

    def parse_primary(self) -> object:
        kind, value = self.take()
        if kind == "op" and value == "(":
            inner = self.parse_or()
            closing = self.take()
            if closing != ("op", ")"):
                raise ExpressionError(f"Expected ')', got {closing!r}")
            return inner
        if kind == "string":
            return value[1:-1].replace("''", "'")
        if kind == "ident":
            return self._resolve(value)
        raise ExpressionError(f"Unexpected token {value!r}")

    def _resolve(self, name: str) -> object:
        if name == "always()":
            return True
        if name.endswith("()"):
            # success()/failure()/cancelled()/hashFiles(...) change what a
            # condition means; refusing is the only safe answer.
            raise ExpressionError(
                f"Unmodelled function {name!r}: this guard will not guess what it returns."
            )
        parts = name.split(".")
        node: object = self.context
        for part in parts:
            if not isinstance(node, dict) or part not in node:
                raise ExpressionError(
                    f"Unknown context reference {name!r}; the truth table does not define it, so "
                    "the expression's meaning is not established."
                )
            node = node[part]
        return node


NEEDS_REFERENCE = re.compile(r"\bneeds\.([A-Za-z_][A-Za-z0-9_-]*)\b")


def referenced_needs(expression: Optional[str]) -> Set[str]:
    """Job names an `if:` reads out of the `needs` context."""
    return set(NEEDS_REFERENCE.findall(expression or ""))


def _truthy(value: object) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value != ""
    raise ExpressionError(f"Cannot coerce {value!r} to a boolean")


def evaluate(expression: str, context: Dict[str, object]) -> bool:
    return _truthy(_Parser(_tokenize(expression), context).parse())


def publish_context(
    *,
    channel: str,
    smoke: str,
    build_windows: str = "success",
    build_linux: str = "success",
    finite_math_guard: str = "success",
    release_candidate_gate: str = "success",
    publish: str = "true",
) -> Dict[str, object]:
    return {
        "github": {"event_name": "push"},
        "needs": {
            "release_metadata": {"outputs": {"channel": channel, "publish": publish}},
            "build_linux": {"result": build_linux},
            "build_windows": {"result": build_windows},
            "build_windows_export_template": {"result": "success"},
            "finite_math_guard": {"result": finite_math_guard},
            "release_candidate_gate": {"result": release_candidate_gate},
            SMOKE_JOB: {"result": smoke},
        },
    }


# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------


class PublicationDependencyGraphTests(unittest.TestCase):
    def setUp(self) -> None:
        self.jobs = parse_jobs()

    def test_the_smoke_job_exists(self) -> None:
        self.assertIn(SMOKE_JOB, self.jobs)

    def test_every_release_side_effect_job_waits_for_the_smoke_test(self) -> None:
        gated = release_side_effect_jobs(self.jobs)
        self.assertTrue(gated, "No job holds `permissions: contents: write`; derivation is broken.")
        for job in gated:
            with self.subTest(job=job):
                closure = needs_closure(self.jobs, job)
                self.assertIn(
                    SMOKE_JOB,
                    closure,
                    f"{job} can write releases but {SMOKE_JOB} is not in its `needs:` closure "
                    f"({sorted(closure)}), so GitHub may run it while the smoke test is still "
                    "running. Job ordering comes from `needs:` and nothing else.",
                )

    def test_the_candidate_gate_also_waits_for_it(self) -> None:
        # Same reasoning as the fast-math guard being asserted in both places:
        # the gate is the single authority for "may this publish", so the smoke
        # test belongs inside its dependency graph too, not only on the edge.
        self.assertIn(SMOKE_JOB, self.jobs["release_candidate_gate"]["needs"])

    def test_every_asserted_job_is_in_that_jobs_own_needs(self) -> None:
        """`needs.X` only resolves for jobs in the SAME job's `needs:` list.

        The failure mode this catches is silent and total: drop `X` from
        `needs:` while leaving `needs.X.result == 'success'` in the `if:`, and
        GitHub resolves `needs.X` to null, the comparison is false forever, and
        the job never runs -- or, with the polarity flipped, always runs. Either
        way the condition has stopped meaning what it reads as. A transitive
        dependency does NOT make the reference resolve, which is exactly why
        this cannot be folded into the closure check above.
        """
        for job, data in self.jobs.items():
            declared = set(data["needs"])
            for referenced in referenced_needs(data["if"]):
                with self.subTest(job=job, referenced=referenced):
                    self.assertIn(
                        referenced,
                        declared,
                        f"{job}'s `if:` reads needs.{referenced}, but {referenced} is not in "
                        f"{job}'s own `needs:` ({sorted(declared)}). GitHub resolves that to "
                        "null, so the assertion silently stops being an assertion.",
                    )

    def test_the_smoke_result_is_asserted_where_it_is_needed(self) -> None:
        # The other direction of the same pair: `needs:` without an assertion
        # blocks nothing under `always()`.
        for job in ("publish_release", "release_candidate_gate"):
            with self.subTest(job=job):
                self.assertIn(SMOKE_JOB, referenced_needs(self.jobs[job]["if"]))

    def test_the_smoke_job_does_not_depend_on_the_publishers(self) -> None:
        # A cycle would make the whole graph unschedulable; assert the direction.
        closure = needs_closure(self.jobs, SMOKE_JOB)
        for publisher in ("publish_release", "release_candidate_gate"):
            self.assertNotIn(publisher, closure)


class PublicationConditionTests(unittest.TestCase):
    """Evaluate the real `if:` expressions; do not grep them.

    Each gating job is checked over the same truth table, because a clause that
    is present but structurally inert (negated, `||`-ed with something always
    true, parenthesised into irrelevance) passes a substring check and gates
    nothing.
    """

    GATED_JOBS = ("publish_release", "release_candidate_gate")

    def setUp(self) -> None:
        self.jobs = parse_jobs()

    def _condition(self, job: str) -> str:
        condition = self.jobs[job]["if"]
        self.assertIsNotNone(condition, f"{job} has no job-level `if:` to evaluate.")
        return str(condition)

    def _allows(self, job: str, **kwargs) -> bool:
        return evaluate(self._condition(job), publish_context(**kwargs))

    def test_the_baseline_publishes(self) -> None:
        # Discrimination: if nothing publishes, "it blocks" proves nothing.
        for job in self.GATED_JOBS:
            for channel in PUBLISHING_CHANNELS:
                with self.subTest(job=job, channel=channel):
                    self.assertTrue(self._allows(job, channel=channel, smoke="success"))

    def test_a_non_success_smoke_result_blocks_a_stable_release(self) -> None:
        for job in self.GATED_JOBS:
            for smoke in JOB_RESULTS:
                if smoke == "success":
                    continue
                with self.subTest(job=job, smoke=smoke):
                    self.assertFalse(
                        self._allows(job, channel="stable", smoke=smoke),
                        f"{job} still runs with {SMOKE_JOB}={smoke} on a stable release.",
                    )

    def test_a_stable_release_has_no_windows_outage_tolerance(self) -> None:
        # The nightly escape hatch must not leak into the stable path.
        for job in self.GATED_JOBS:
            for smoke in ("skipped", "failure"):
                with self.subTest(job=job, smoke=smoke):
                    self.assertFalse(
                        self._allows(
                            job, channel="stable", smoke=smoke, build_windows="failure"
                        )
                    )

    def test_a_nightly_that_ships_windows_bytes_requires_the_smoke_test(self) -> None:
        # build_windows green => a Windows archive is attached => it must have
        # been executed. This is the case a blanket nightly tolerance would miss:
        # the template build failing, or the GPU pool being busy, skips the smoke
        # test while the Windows editor archive still ships.
        for job in self.GATED_JOBS:
            for smoke in ("failure", "skipped", "cancelled"):
                with self.subTest(job=job, smoke=smoke):
                    self.assertFalse(
                        self._allows(
                            job, channel="nightly", smoke=smoke, build_windows="success"
                        ),
                        f"{job} publishes a nightly Windows archive with {SMOKE_JOB}={smoke}.",
                    )

    def test_a_nightly_windows_outage_still_publishes_linux_only(self) -> None:
        # The documented tolerance, kept intact: no Windows payload, nothing
        # unexecuted ships, the cadence continues.
        for job in self.GATED_JOBS:
            for windows in ("failure", "skipped", "cancelled"):
                with self.subTest(job=job, build_windows=windows):
                    self.assertTrue(
                        self._allows(
                            job, channel="nightly", smoke="skipped", build_windows=windows
                        ),
                        f"{job} blocks a Linux-only nightly, which the documented tolerance "
                        "allows when no Windows bytes are published.",
                    )

    def test_the_existing_fast_math_assertion_is_still_load_bearing(self) -> None:
        # #590/#612/#620. Pinned here because the same "listed under needs: but
        # never asserted" mistake is what this guard exists to catch, and until
        # now that invariant lived only in a comment.
        for job in self.GATED_JOBS:
            for channel in PUBLISHING_CHANNELS:
                with self.subTest(job=job, channel=channel):
                    self.assertFalse(
                        self._allows(
                            job, channel=channel, smoke="success", finite_math_guard="failure"
                        )
                    )

    def test_publication_still_requires_the_candidate_gate(self) -> None:
        self.assertFalse(
            self._allows("publish_release", channel="stable", smoke="success",
                         release_candidate_gate="failure")
        )

    def test_nothing_publishes_when_publish_is_false(self) -> None:
        for job in self.GATED_JOBS:
            with self.subTest(job=job):
                self.assertFalse(
                    self._allows(job, channel="ci", smoke="success", publish="false")
                )

    def test_the_two_gates_agree_on_every_smoke_state(self) -> None:
        # Belt-and-braces only works if both copies say the same thing; a
        # divergence would mean one of them is the real gate and the other is
        # decoration.
        for channel in PUBLISHING_CHANNELS:
            for smoke in JOB_RESULTS:
                for windows in JOB_RESULTS:
                    with self.subTest(channel=channel, smoke=smoke, build_windows=windows):
                        self.assertEqual(
                            self._allows(
                                "publish_release", channel=channel, smoke=smoke,
                                build_windows=windows
                            ),
                            self._allows(
                                "release_candidate_gate", channel=channel, smoke=smoke,
                                build_windows=windows
                            ),
                        )


class ExpressionEvaluatorTests(unittest.TestCase):
    """The evaluator is the guard; test it against inline fixtures, not trust."""

    CONTEXT = publish_context(channel="stable", smoke="failure")

    def test_precedence_of_and_over_or(self) -> None:
        self.assertTrue(evaluate("'a' == 'b' || 'c' == 'c' && 'd' == 'd'", self.CONTEXT))
        self.assertFalse(evaluate("'c' == 'c' && 'd' == 'e' || 'a' == 'b'", self.CONTEXT))

    def test_parentheses_override_precedence(self) -> None:
        self.assertFalse(evaluate("('a' == 'b' || 'c' == 'c') && 'd' == 'e'", self.CONTEXT))

    def test_negation(self) -> None:
        self.assertTrue(evaluate("!('a' == 'b')", self.CONTEXT))

    def test_always_is_true(self) -> None:
        self.assertTrue(evaluate("always()", self.CONTEXT))

    def test_context_lookup(self) -> None:
        self.assertTrue(
            evaluate("needs.release_metadata.outputs.channel == 'stable'", self.CONTEXT)
        )
        self.assertTrue(evaluate(f"needs.{SMOKE_JOB}.result != 'success'", self.CONTEXT))

    def test_block_scalar_joined_expression_round_trips(self) -> None:
        joined = "always() && needs.build_linux.result == 'success'"
        self.assertTrue(evaluate(joined, self.CONTEXT))

    def test_an_unknown_context_reference_raises(self) -> None:
        with self.assertRaises(ExpressionError):
            evaluate("needs.no_such_job.result == 'success'", self.CONTEXT)

    def test_an_unmodelled_function_raises(self) -> None:
        # `success()` would silently change what every condition means.
        for expression in ("success()", "cancelled() || always()"):
            with self.subTest(expression=expression):
                with self.assertRaises(ExpressionError):
                    evaluate(expression, self.CONTEXT)

    def test_garbage_raises_rather_than_evaluating_to_false(self) -> None:
        for expression in ("&& 'a'", "needs.build_linux.result ==", "("):
            with self.subTest(expression=expression):
                with self.assertRaises(ExpressionError):
                    evaluate(expression, self.CONTEXT)

    def test_a_dropped_clause_is_detectable(self) -> None:
        # The mutation this guard has to catch: remove the smoke assertion and
        # the condition must start allowing a failed smoke test through.
        with_clause = (
            "always() && "
            f"(needs.{SMOKE_JOB}.result == 'success' || "
            "(needs.release_metadata.outputs.channel != 'stable' && "
            "needs.build_windows.result != 'success'))"
        )
        without_clause = "always()"
        self.assertFalse(evaluate(with_clause, self.CONTEXT))
        self.assertTrue(evaluate(without_clause, self.CONTEXT))


class ParserFailClosedTests(unittest.TestCase):
    def test_inline_needs_is_read(self) -> None:
        lines = [
            "jobs:",
            "  a:",
            "    runs-on: ubuntu-latest",
            "  b:",
            "    needs: [a]",
            "    runs-on: ubuntu-latest",
        ]
        self.assertEqual(parse_jobs(lines)["b"]["needs"], ["a"])

    def test_block_needs_is_read(self) -> None:
        lines = [
            "jobs:",
            "  a:",
            "    runs-on: ubuntu-latest",
            "  b:",
            "    needs:",
            "      - a",
            "    runs-on: ubuntu-latest",
        ]
        self.assertEqual(parse_jobs(lines)["b"]["needs"], ["a"])

    def test_an_empty_if_raises(self) -> None:
        lines = ["jobs:", "  a:", "    if:", "    runs-on: ubuntu-latest"]
        with self.assertRaises(UnmodelledWorkflowConstruct):
            parse_jobs(lines)

    def test_a_scalar_permissions_value_raises(self) -> None:
        # `permissions: write-all` grants contents:write without naming it.
        lines = ["jobs:", "  a:", "    permissions: write-all", "    runs-on: ubuntu-latest"]
        with self.assertRaises(UnmodelledWorkflowConstruct):
            parse_jobs(lines)

    def test_an_unknown_needs_target_raises(self) -> None:
        lines = ["jobs:", "  a:", "    needs: [ghost]", "    runs-on: ubuntu-latest"]
        with self.assertRaises(UnmodelledWorkflowConstruct):
            needs_closure(parse_jobs(lines), "a")

    def test_the_on_block_is_not_mistaken_for_jobs(self) -> None:
        jobs = parse_jobs()
        for key in ("pull_request", "push", "schedule", "workflow_dispatch"):
            self.assertNotIn(key, jobs)

    def test_the_real_workflow_parses(self) -> None:
        jobs = parse_jobs()
        self.assertIn("publish_release", jobs)
        self.assertIn("release_candidate_gate", jobs)
        self.assertEqual(
            release_side_effect_jobs(jobs), ["prune_nightly_history", "publish_release"]
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
