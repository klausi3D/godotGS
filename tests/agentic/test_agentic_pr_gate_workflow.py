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

STEP_RE = re.compile(r"^      - name: (.+)$", re.MULTILINE)

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
        """The single step whose non-comment body invokes `script`."""
        found = [
            (name, block)
            for name, block in self.steps
            if any(
                script in line and not line.lstrip().startswith("#")
                for line in block.splitlines()
            )
        ]
        self.assertEqual(
            1,
            len(found),
            f"expected exactly one step invoking {script}; found {[n for n, _ in found]}",
        )
        return found[0][1]

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
    def block(self) -> str:
        # Comment-stripped: a flag named only in a comment is not wiring, and the
        # comments next to this step name several of the flags asserted below.
        return self.body(self.step_invoking("scripts/agentic/classify_change.py"))

    def test_the_classifier_runs_against_a_base_ref(self):
        self.assertIn("--base-ref", self.block())

    def test_every_base_bearing_trigger_is_covered_by_the_base_expression(self):
        """Derived from `on:`, not from a list written here.

        A trigger this file does not recognise fails closed: it is either a new
        base-bearing event whose payload field must be added to the expression, or
        a baseless one that must be declared as such deliberately.
        """
        block = self.block()
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
                block,
                f"the risk-class step has no base for '{event}'; on that event it "
                f"would classify against an empty base and fail the required check",
            )
        self.assertTrue(checked, "no base-bearing trigger was checked; the derivation broke")

    def test_the_step_is_not_restricted_to_a_single_event(self):
        """It used to carry `if: github.event_name == 'pull_request'`.

        A step skipped on `merge_group` cannot enforce anything in the merge queue,
        which is the last boundary before `master`.
        """
        for line in self.block().splitlines():
            if re.match(r"^        if:", line):
                self.assertIn(
                    "merge_group",
                    line,
                    "the risk-class step is conditioned in a way that can skip the merge queue",
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


class NoAdvisoryStepTest(WorkflowScan):
    def test_no_step_in_the_required_gate_is_advisory(self):
        """A required check whose steps swallow failures is decorative."""
        for pattern in ("continue-on-error", "|| true", "|| exit 0"):
            self.assertNotIn(pattern, self.text, f"{pattern!r} makes the required gate advisory")


if __name__ == "__main__":
    unittest.main(verbosity=2)
