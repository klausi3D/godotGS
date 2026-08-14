#!/usr/bin/env python3
"""Pins the wiring of the required merge gate, `.github/workflows/agentic_pr_gate.yml`.

`agentic-pr-gate` is the only required status check on `master`. The scripts it
runs are unit-tested next door, but nothing asserted that the workflow actually
*invokes* them in their enforcing form -- and it did not: `validate_repo_contract.py`
ran without `--strict-hierarchy` (so the entire AGENTS.md / docs/governance
hierarchy could be deleted with the gate green) and the only PR-derived step merely
printed a risk class that `classify_change.py` returns with exit code 0 for every
class, on `pull_request` only (GS-AUDIT-TEST-001).

Every assertion here is a mutation target: remove the flag, drop the classify step,
gate it back to one event, or make a step advisory, and one of these goes red. A
repaired script whose wiring nobody checks is the "guard wired to nothing" shape in
`docs/governance/evidence-integrity.md`.

The trigger coverage is DERIVED from the workflow's own `on:` block rather than
written out here, and an event this file does not recognise is a hard failure
rather than an assumption of safety -- a hand-written event list is how the merge
queue was left without a review base once already (see
`tests/ci/test_run_module_tests_skip_marker.py`).

## Why this file matches TEXT instead of parsing YAML

Every check here is a text matcher over `agentic_pr_gate.yml`, which makes it a
ratchet over NAMED shapes rather than a structural guarantee -- see the threat
model enumerated on `invocation_re`. Parsing the workflow as real YAML is the
correct long-term fix: it closes the job-vs-step scope gap structurally (a
job-level `if:` is a different key, not a different indent) and retires most of
the regex-evasion category at once.

It is not done here for one concrete reason. **No workflow in this repository
pip-installs PyYAML**, and `actions/setup-python@v5` provisions a bare tool-cache
interpreter, so `import yaml` at module scope in `tests/agentic/` would raise
`ImportError` during unittest DISCOVERY -- failing the only required status check
on `master`, on every PR, with `enforce_admins: true`. (The guarded
`try: import yaml / except ImportError` in `tests/ci/validate_automation.py` is
suggestive but is NOT the evidence, because no workflow invokes that file.)
Making PyYAML a mandatory gate dependency, and then porting this file to a real
parse, is tracked as a follow-up on T6 / #894.
"""

from __future__ import annotations

import importlib.util
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "agentic_pr_gate.yml"

POLICY = json.loads((ROOT / ".agentic" / "policy.json").read_text(encoding="utf-8"))
TASK_SCHEMA = json.loads((ROOT / ".agentic" / "schemas" / "task.schema.json").read_text(encoding="utf-8"))
TEMPLATE = json.loads((ROOT / ".agentic" / "templates" / "task.json").read_text(encoding="utf-8"))


def _load(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cpc = _load("check_pr_contract", "scripts/agentic/check_pr_contract.py")

# The GitHub payload field that carries a review base, per event. An event listed
# here MUST be covered by the classify step's base expression; an event in
# BASELESS_EVENTS has no review base at all. An event in neither is unknown and
# fails the coverage test rather than being waved through.
BASE_FIELD_BY_EVENT = {
    "pull_request": "pull_request.base.sha",
    "merge_group": "merge_group.base_sha",
}
BASELESS_EVENTS = {"push", "schedule", "workflow_dispatch", "workflow_call"}

# Events this gate MUST keep triggering on. Deriving the event list from `on:`
# (below) is fail-closed against an event being ADDED and fail-OPEN against one
# being removed: delete `merge_group:` and every derived check silently narrows to
# the events that remain. This list is the other half.
REQUIRED_TRIGGERS = ("pull_request", "merge_group")

STEP_RE = re.compile(r"^      - name: (.+)$", re.MULTILINE)


def invocation_re(script: str) -> "re.Pattern[str]":
    """Matches a line that actually RUNS `script`, not one that mentions it.

    Both step shapes are accepted: a single-line `run: python <script> …` and a
    line inside a `run: |` block scalar. What is deliberately NOT accepted is any
    line where something else precedes the interpreter -- `echo python <script>`,
    `true python <script>`, `: python <script>`. That is not academic: prefixing
    the command with `echo` reconstitutes the exact print-only step
    GS-AUDIT-TEST-001 is about, and a substring scan for the script path cannot
    tell the two apart. `EchoPrefixIsNotAnInvocationTest` pins the distinction.

    ## Threat model, stated rather than implied

    This is a text matcher and is therefore evadable by construction. It defends
    against **accidental** loss of enforcement -- a refactor, a debugging `echo`
    left in, a step quietly neutered -- and NOT against an adversarial author, who
    has merge rights and is defended against by human review instead. Written out
    so nobody reads a green run as "the gate cannot be disabled":

    * **Correctly accepted** (semantics preserved, matcher agrees): `python3`,
      extra spaces or tabs around the interpreter and the path, and any rename of
      the enclosing step -- step selection is by invocation, never by name.
    * **Safe false-REDs** (annoying, not dangerous): `env python <script>` and the
      `python -m <module>` form are legitimate invocations this pattern rejects.
      Note the asymmetry, because it is the whole reason a text matcher is
      acceptable here: a reflow that changes the TEXT but not the SEMANTICS costs
      a false RED and a one-line pattern update, while only the opposite -- a
      change of SEMANTICS that preserves the matched text -- is dangerous, and
      that is what the negative cases pin. Reindentation is a third,
      self-detecting case: push it past YAML validity and `yaml.safe_load` raises
      `ParserError`, i.e. GitHub rejects the workflow loudly rather than running a
      weakened one.
    * **Deliberately out of scope**: `bash -c '…'` obfuscation, trailing `&`
      backgrounding the command, `;`-chained no-ops after it, and decoy comment
      lines (those are removed by `WorkflowScan.body()` before matching, so they
      cannot create a false GREEN, only be ignored). Each of these requires
      intent, which puts it in the review threat model, not this one.

    The structural fix for the whole category is a real YAML parse; see the
    module docstring for why that is blocked today and where it is tracked.
    """
    return re.compile(
        r"^[ \t]*(?:run:[ \t]*)?python3?[ \t]+" + re.escape(script) + r"(?=[ \t\\]|$)",
        re.MULTILINE,
    )

TEXT = WORKFLOW.read_text(encoding="utf-8") if WORKFLOW.is_file() else ""


def _parse_steps(text: str) -> list[tuple[str, str]]:
    """[(step name, step block)] for the job's steps, in file order."""
    matches = list(STEP_RE.finditer(text))
    return [
        (
            match.group(1).strip(),
            text[match.start() : (matches[i + 1].start() if i + 1 < len(matches) else len(text))],
        )
        for i, match in enumerate(matches)
    ]


STEPS = _parse_steps(TEXT)


class WorkflowScan(unittest.TestCase):
    """Shared helpers. Not a test case of its own -- see ParserControlTest."""

    text = TEXT
    steps = STEPS

    def step_invoking(self, script: str) -> str:
        """The single step that actually EXECUTES `script`.

        Selection is by `invocation_re`, not by substring: a step that merely
        names the script -- in a comment, in an `echo`, in a disabled line -- is
        not a step that runs it, and treating it as one is how a print-only gate
        passes a wiring test.
        """
        pattern = invocation_re(script)
        found = [
            (name, block)
            for name, block in self.steps
            if pattern.search(self.body(block))
        ]
        self.assertEqual(
            1,
            len(found),
            f"expected exactly one step INVOKING {script} (not merely mentioning it); "
            f"found {[n for n, _ in found]}",
        )
        return found[0][1]

    def env_value(self, block: str, name: str) -> str:
        """The value of `name` in the step's `env:` mapping, folded scalars included.

        Needed because the base expression is a `>-` block: the payload fields sit
        on continuation lines, and the point of reading it here is to follow the
        chain `--base-ref "${VAR}"` -> `env: VAR:` -> `github.event.*` rather than
        to scan the step for a string that happens to appear somewhere in it.
        """
        lines = self.body(block).splitlines()
        start = indent = None
        collected: list[str] = []
        for index, line in enumerate(lines):
            match = re.match(r"^([ \t]+)" + re.escape(name) + r":(.*)$", line)
            if match:
                start, indent = index, len(match.group(1))
                collected.append(match.group(2).strip())
                break
        self.assertIsNotNone(start, f"the step declares no env entry '{name}':\n{block}")
        for line in lines[start + 1 :]:
            if not line.strip():
                continue
            if len(line) - len(line.lstrip()) <= indent:
                break
            collected.append(line.strip())
        return "\n".join(collected)

    @staticmethod
    def body(block: str) -> str:
        """The step with its YAML comments removed.

        A comment that mentions a flag is not an invocation of it -- the same
        distinction `tests/ci/test_run_module_tests_skip_marker.py` had to make.
        Without this, the explanatory comment added next to a step is read as the
        step's own arguments.
        """
        return "\n".join(
            line for line in block.splitlines() if not line.lstrip().startswith("#")
        )

    def trigger_events(self) -> set[str]:
        lines = self.text.splitlines()
        try:
            start = next(i for i, line in enumerate(lines) if re.match(r"^on:\s*$", line))
        except StopIteration:
            self.fail("workflow has no top-level 'on:' block")
        events = set()
        for line in lines[start + 1 :]:
            if line and not line[0].isspace():
                break
            match = re.match(r"^  ([A-Za-z_][A-Za-z0-9_]*):", line)
            if match:
                events.add(match.group(1))
        self.assertTrue(events, "derived no trigger events; the parser is broken")
        return events


class ParserControlTest(WorkflowScan):
    def test_the_parser_sees_the_workflow(self):
        """Positive control: every assertion below is vacuous if this fails."""
        self.assertTrue(WORKFLOW.is_file(), f"{WORKFLOW} is missing")
        self.assertGreaterEqual(len(self.steps), 5, [name for name, _ in self.steps])
        self.assertIn("agentic-pr-gate", self.text)
        self.assertIn("pull_request", self.trigger_events())

    def test_the_required_triggers_are_still_declared(self):
        """The half that a derived event list cannot supply.

        Everything else here (and `WorkflowBaseExportTests` in
        `tests/ci/test_run_module_tests_skip_marker.py`) derives its expectations
        FROM `on:`. That is fail-closed against an event being added and fail-OPEN
        against one being removed: delete `merge_group:` and every derived check
        quietly narrows to the events that remain, with nothing red. Both suites
        were measured green with `merge_group` deleted before this test existed.
        """
        events = self.trigger_events()
        for trigger in REQUIRED_TRIGGERS:
            self.assertIn(
                trigger,
                events,
                f"the required gate no longer triggers on '{trigger}'; every "
                f"event-derived check in this repo silently narrows with it",
            )


class EchoPrefixIsNotAnInvocationTest(unittest.TestCase):
    """Pins the detector that the whole file's step selection rests on.

    A wiring test that matches the script PATH cannot distinguish
    `python x.py` from `echo python x.py`, and the second is precisely the
    print-only step GS-AUDIT-TEST-001 recorded. This is the
    `BASELINE_SKIP_MARKER_RE`-inertness pattern from
    `tests/ci/test_run_module_tests_skip_marker.py`: assert what the matcher must
    NOT see, or a broken matcher passes every test written on top of it.
    """

    SCRIPT = "scripts/agentic/classify_change.py"

    def test_real_invocation_shapes_are_detected(self):
        for line in (
            "          python scripts/agentic/classify_change.py \\",
            "        run: python scripts/agentic/classify_change.py --base-ref x",
            "          python3 scripts/agentic/classify_change.py",
            "          python scripts/agentic/classify_change.py",
        ):
            self.assertIsNotNone(invocation_re(self.SCRIPT).search(line), line)

    def test_neutered_shapes_are_not_detected(self):
        for line in (
            "          echo python scripts/agentic/classify_change.py \\",
            "          true python scripts/agentic/classify_change.py \\",
            "          : python scripts/agentic/classify_change.py",
            "          # python scripts/agentic/classify_change.py",
            "          echo 'run scripts/agentic/classify_change.py by hand'",
            "          pythonx scripts/agentic/classify_change.py",
        ):
            self.assertIsNone(invocation_re(self.SCRIPT).search(line), line)

    def test_a_prefixed_workflow_is_rejected_end_to_end(self):
        """The mutation itself, run through the real selection path.

        `echo`-prefixing the command in a copy of the live workflow must make step
        selection find zero invoking steps -- i.e. the tests that call
        `step_invoking` go red rather than passing on a decorative step.
        """
        pattern = invocation_re(self.SCRIPT)
        lines = TEXT.splitlines(keepends=True)
        indexes = [i for i, line in enumerate(lines) if pattern.search(line)]
        self.assertEqual(
            1,
            len(indexes),
            f"expected exactly one line invoking {self.SCRIPT} in the live workflow; "
            f"found {len(indexes)}",
        )
        for prefix in ("echo ", "true ", ": "):
            mutated = list(lines)
            original = mutated[indexes[0]]
            stripped = original.lstrip()
            mutated[indexes[0]] = original[: len(original) - len(stripped)] + prefix + stripped
            steps = [
                name
                for name, block in _parse_steps("".join(mutated))
                if pattern.search(
                    "\n".join(
                        ln for ln in block.splitlines() if not ln.lstrip().startswith("#")
                    )
                )
            ]
            self.assertEqual([], steps, f"prefix {prefix!r} still counted as an invocation")


class ControlPlaneValidationTest(WorkflowScan):
    def test_repo_contract_validation_is_strict(self):
        """Without --strict-hierarchy the whole governance hierarchy is optional.

        `validate_repo_contract.py` only requires the AGENTS.md files and
        `docs/governance/*` under that flag (HIERARCHY_FILES); the default run
        checks the self-contained `.agentic/` control plane alone. The required
        gate ran the default form, so deleting `AGENTS.md`,
        `docs/governance/review-policy.md` or `github-settings.md` left it green.
        """
        block = self.body(self.step_invoking("scripts/agentic/validate_repo_contract.py"))
        self.assertIn("--strict-hierarchy", block, block)


class RiskClassStepTest(WorkflowScan):
    SCRIPT = "scripts/agentic/classify_change.py"

    def raw_block(self) -> str:
        return self.step_invoking(self.SCRIPT)

    def block(self) -> str:
        # Comment-stripped: a flag named only in a comment is not wiring, and the
        # comments next to this step name several of the flags asserted below.
        return self.body(self.raw_block())

    def test_the_classifier_is_executed_not_merely_named(self):
        """The step must RUN the classifier.

        `step_invoking` already selects on `invocation_re`, so this restates the
        requirement at the point a reader looks for it and fails with a readable
        message if the command is ever prefixed (`echo`, `true`, `:`) back into a
        print-only step.
        """
        block = self.block()
        self.assertIsNotNone(
            invocation_re(self.SCRIPT).search(block),
            f"the risk-class step does not execute the classifier:\n{block}",
        )

    def base_ref_variable(self) -> str:
        """The shell variable `--base-ref` is actually given.

        Asserting only that `--base-ref` appears somewhere is not enough: a
        hardcoded `--base-ref "master"` satisfies that and classifies every PR
        against the wrong base. Worse than the empty case, because a hardcoded but
        *resolvable* ref (`origin/master`, `HEAD~1`) misclassifies silently instead
        of failing closed.
        """
        match = re.search(r'--base-ref[ \t]+"\$\{([A-Za-z_][A-Za-z0-9_]*)\}"', self.block())
        self.assertIsNotNone(
            match,
            "--base-ref must receive a shell variable expanded from the step's env "
            '(expected --base-ref "${SOME_VAR}"); a literal ref would classify every '
            f"PR against a fixed base:\n{self.block()}",
        )
        return match.group(1)

    def test_the_base_ref_argument_comes_from_the_step_environment(self):
        self.assertEqual("GS_PR_BASE_SHA", self.base_ref_variable())

    def test_every_base_bearing_trigger_is_covered_by_the_base_expression(self):
        """Derived from `on:`, not from a list written here.

        The chain checked is `--base-ref "${VAR}"` -> `env: VAR:` -> the event's
        payload field. Scanning the whole step for the field name is not the same
        assertion: the `env:` block could keep declaring a perfectly correct
        variable that `--base-ref` never uses.

        A trigger this file does not recognise fails closed: it is either a new
        base-bearing event whose payload field must be added to the expression, or
        a baseless one that must be declared as such deliberately.
        """
        expression = self.env_value(self.raw_block(), self.base_ref_variable())
        checked = 0
        for event in sorted(self.trigger_events()):
            if event in BASELESS_EVENTS:
                continue
            self.assertIn(
                event,
                BASE_FIELD_BY_EVENT,
                f"unknown trigger '{event}': declare its review-base field in "
                f"BASE_FIELD_BY_EVENT or add it to BASELESS_EVENTS",
            )
            checked += 1
            self.assertIn(
                BASE_FIELD_BY_EVENT[event],
                expression,
                f"the base handed to --base-ref has no value for '{event}'; on that "
                f"event it would be empty and the required check would fail",
            )
        self.assertTrue(checked, "no base-bearing trigger was checked; the derivation broke")

    def test_the_step_carries_no_condition_at_all(self):
        """Unconditional, and with the right polarity.

        The predecessor of this test looped over `if:` lines and asserted only
        *inside* the loop, so with no `if:` present it executed ZERO assertions --
        a zero-assertion test inside the file written to stop zero-assertion tests.
        Its polarity was wrong too: `if: ${{ github.event_name != 'merge_group' }}`
        mentions `merge_group` and would have satisfied it while skipping the merge
        queue, which is the original defect (the step used to be
        `if: github.event_name == 'pull_request'`).

        A step of the only required gate must simply always run, so any `if:` on it
        is a failure and the reviewer decides deliberately.
        """
        conditions = [
            line for line in self.block().splitlines() if re.match(r"^ {8}if:", line)
        ]
        self.assertEqual(
            [],
            conditions,
            "the risk-class step is conditional; a required-gate step that can be "
            "skipped enforces nothing on the events where it is skipped",
        )

    def test_the_step_publishes_the_class_obligations_to_the_job_summary(self):
        """A human merging an R2 PR must see "runtime/GPU evidence required" on the
        required check itself, not only in a doc they might read."""
        self.assertIn("--github-step-summary", self.block())

    def test_the_step_fails_when_no_base_is_available(self):
        """An empty base expression must be an error, not an empty diff.

        `classify_change.py --base-ref ""` fails anyway, but the explicit guard
        names the event in the log instead of leaving a bare git error.
        """
        block = self.block()
        self.assertRegex(block, r'if \[ -z "\$\{GS_PR_BASE_SHA\}" \]')
        self.assertIn("exit 1", block)


class TemplateSelfTestStepTest(WorkflowScan):
    SCRIPT = "scripts/agentic/check_pr_contract.py"

    def test_the_template_step_is_fed_fixture_paths_not_pr_paths(self):
        """This step self-tests the validators on shipped fixtures.

        Its `--paths` argument must stay a path the shipped template declares as
        owned. Feeding it the PR's real changed paths -- the "make it validate the
        PR" reading that GS-AUDIT-TEST-001 recorded -- would fail the required gate
        for every PR, because the fixture's `owned_paths` are fixture paths.
        Asserted by running the fixture contract against that literal argument
        rather than by matching the string.
        """
        block = self.body(self.step_invoking(self.SCRIPT))
        match = re.search(r"--paths\s+([^\s\\]+)", block)
        self.assertIsNotNone(match, f"no --paths argument in the template step:\n{block}")
        errors = [
            error
            for error in cpc.check_contract(TEMPLATE, POLICY, TASK_SCHEMA, [match.group(1)])
            if not error.startswith("note:")
        ]
        self.assertEqual(
            [], errors, f"--paths {match.group(1)} does not satisfy the shipped template"
        )

    def test_the_step_name_does_not_claim_to_validate_this_pr(self):
        """The step's name is the whole reason it was read as PR enforcement."""
        names = [name for name, block in self.steps if self.SCRIPT in block]
        self.assertEqual(1, len(names), names)
        self.assertRegex(names[0].lower(), r"template|fixture|self-test")


# `continue-on-error:` set to anything other than an explicit false. Written as a
# match on the VALUE rather than on the key, so `continue-on-error: false` -- which
# states the safe intent -- is not reported as a violation.
CONTINUE_ON_ERROR_RE = re.compile(r"continue-on-error:[ \t]*(?!false[ \t]*$)\S")
# Shell constructs that turn a failing command into a passing step. `|| :` is here
# because `:` is the POSIX no-op and a blacklist of the literal strings "|| true"
# and "|| exit 0" does not catch it.
SWALLOWED_FAILURE_RE = re.compile(
    r"\|\|[ \t]*(?:true\b|:(?=[ \t]|$)|exit[ \t]+0\b)|(?<![\w-])set[ \t]+\+e\b"
)


class GateAlwaysRunsTest(WorkflowScan):
    """The two ways to switch the whole gate off without touching a single step.

    Both were GREEN against every other assertion in this file: they operate one
    nesting level up from, or entirely outside, the steps the rest of the file
    inspects. Same "guard wired to nothing" shape, larger blast radius.
    """

    JOB = "agentic-pr-gate"
    AGENTIC_SUITE_RE = re.compile(
        r"^[ \t]*(?:run:[ \t]*)?python3?[ \t]+-m[ \t]+unittest[ \t]+discover[ \t]+-s[ \t]+"
        r"tests/agentic(?=[ \t\\]|$)",
        re.MULTILINE,
    )

    def job_header(self) -> str:
        """The job's own keys, i.e. everything above its `steps:`.

        `enforce_admins: true` plus a single required context means this job IS
        the merge gate; a key here applies to all of it at once.

        DECLARED LIMIT: `JOB` is matched against the job's YAML **key**, while the
        required status-check context is the job's **`name:` value**. Changing only
        `name:` and keeping the key leaves every test here green. That is a
        false-RED brick (the context never reports, so nothing merges) rather than
        a bypass, so it cannot let a bad PR through; it is recorded as a follow-up
        on #887 rather than asserted here.
        """
        lines = self.body(self.text).splitlines()
        try:
            start = next(
                index
                for index, line in enumerate(lines)
                if re.match(r"^  " + re.escape(self.JOB) + r":[ \t]*$", line)
            )
        except StopIteration:
            self.fail(
                f"no job named '{self.JOB}'; that name IS the required status check's "
                f"context, so renaming it makes the context never report and every PR "
                f"stops at 'Expected - waiting for status to be reported'. That is a "
                f"repo-wide merge BLOCK, not a bypass - the opposite failure mode from "
                f"the job-level 'if:' below. Caught here so it is found in review "
                f"rather than by a frozen queue."
            )
        header: list[str] = []
        for line in lines[start + 1 :]:
            if re.match(r"^    steps:[ \t]*$", line):
                break
            if line.strip() and len(line) - len(line.lstrip()) <= 2:
                break  # dedented out of this job
            header.append(line)
        return "\n".join(header)

    def test_the_job_itself_carries_no_condition(self):
        """A job-level `if:` disables every step at once.

        `test_the_step_carries_no_condition_at_all` is anchored at `^ {8}if:` --
        step level. Moving the same expression up to the job header
        (`jobs.agentic-pr-gate.if:`, four spaces) parses fine, skips the entire
        required gate on the events it excludes, and was GREEN against all 76
        assertions. A skipped required job reports success, so there is nothing to
        notice at the merge boundary either. GitHub's *Troubleshooting required
        status checks* states that a job "skipped by a conditional" reports
        Success, and that "Successful check statuses are `success`, `skipped`, and
        `neutral`". This repository already demonstrates the mechanism:
        `baseline_qa.yml`'s `cpu-tests` carries
        `if: github.event_name != 'pull_request'` and its check-run reports
        `conclusion: skipped`.

        Note the asymmetry against the two neighbouring shapes, because it is the
        whole reason this one is the dangerous member of the family. Removing
        `merge_group:` from `on:` is a *workflow*-level skip: no check is created,
        it stays Pending, and it BLOCKS merging. Renaming the job blocks merging
        too. Only a *job*-level `if:` converts into a passing status -- it is the
        one gate-wide edit that is silent, which is why it needs its own assertion
        rather than being folded into the rename check.
        """
        conditions = [
            line for line in self.job_header().splitlines() if re.match(r"^ {4}if:", line)
        ]
        self.assertEqual(
            [],
            conditions,
            "the required gate's JOB is conditional; on the events it excludes every "
            "check in this workflow is skipped and the gate still reports success",
        )

    def test_the_gate_still_runs_this_test_suite(self):
        """Nothing else pins that these guards execute in CI at all.

        Deleting the `Run agentic unit tests` step leaves every assertion in this
        file green while none of them runs on any PR again -- the single edit with
        the largest blast radius, and the exact shape the file exists to catch.
        Matched by invocation shape, so an `echo`-prefixed revival does not count.
        """
        self.assertIsNotNone(
            self.AGENTIC_SUITE_RE.search(self.body(self.text)),
            "the required gate no longer runs 'python -m unittest discover -s "
            "tests/agentic'; every guard in this file stops executing in CI while "
            "still passing locally",
        )


class NoAdvisoryStepTest(WorkflowScan):
    """A required check whose steps swallow failures is decorative."""

    def test_no_step_declares_continue_on_error(self):
        offenders = [
            line for line in self.body(self.text).splitlines() if CONTINUE_ON_ERROR_RE.search(line)
        ]
        self.assertEqual([], offenders, "a step of the required gate cannot continue on error")

    def test_no_command_swallows_its_own_failure(self):
        """Matched by shape, not by a list of three literal strings.

        The predecessor checked for `continue-on-error`, `|| true` and `|| exit 0`
        as substrings. `|| :` is the same construct and walked straight through it,
        and `continue-on-error: false` -- an explicit statement of the safe intent
        -- was reported as a violation.
        """
        offenders = [
            line for line in self.body(self.text).splitlines() if SWALLOWED_FAILURE_RE.search(line)
        ]
        self.assertEqual([], offenders, "a failing command in the required gate must fail the step")

    def test_the_detector_recognises_every_swallowing_shape(self):
        """Self-test: a matcher nobody probes is a matcher nobody can trust."""
        for line in (
            "          python x.py || true",
            "          python x.py || :",
            "          python x.py ||:",
            "          python x.py || exit 0",
            "          set +e",
        ):
            self.assertRegex(line, SWALLOWED_FAILURE_RE.pattern, line)
        for line in (
            "          python x.py || exit 1",
            "          python x.py || echo failed",
            "          set -euo pipefail",
        ):
            self.assertIsNone(SWALLOWED_FAILURE_RE.search(line), line)
        self.assertRegex("        continue-on-error: true", CONTINUE_ON_ERROR_RE.pattern)
        self.assertIsNone(CONTINUE_ON_ERROR_RE.search("        continue-on-error: false"))

    def test_a_comment_is_neutralised_by_body_not_by_the_regex(self):
        """Where comment immunity actually comes from -- stated honestly.

        The previous version of this case listed a comment line among the negative
        samples and then searched `""` instead of the line, so it asserted nothing
        at all: a tautology inside the anti-vacuity self-test. The regex is NOT
        immune to comments -- it matches `# historically this used || true` -- and
        pretending otherwise would hide which component is load-bearing. `body()`
        is, so `body()` is what is pinned here. Delete the comment stripping and
        this goes red.
        """
        comment = "          # historically this used || true"
        self.assertRegex(comment, SWALLOWED_FAILURE_RE.pattern)
        self.assertEqual("", self.body(comment))


if __name__ == "__main__":
    unittest.main(verbosity=2)
