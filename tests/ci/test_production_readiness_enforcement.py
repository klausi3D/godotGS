#!/usr/bin/env python3
"""Production readiness evidence is enforced, not only collected (#1164).

Three defects shared one shape -- evidence produced, uploaded, and compared by
nothing (`docs/governance/evidence-integrity.md`, "Green where the gate cannot
fire"):

* The "Enforce readiness booleans" step in `gaussian_production_gates.yml` ran
  only on a `workflow_dispatch` with `enforce_gpu_readiness=true`, whose default
  was false. On pull_request, push and merge_group a false readiness boolean
  passed (N-12).
* `collect_production_evidence.ps1` ran the module tests and the #943 budget
  check but no readiness boolean read either, and the script ended with
  "Evidence collection completed." whatever its sub-commands returned (N-26).
* The script judged runtime scenarios by exit code, and its only skip detector
  (`Skipping .*headless mode`) matched none of the scenarios' real
  `[RUNTIME_SKIP] <reason>` + `quit(0)` output, so a skipped run counted as a
  pass (N-27).

How the cases below can fail:

* The workflow condition is EVALUATED per event with the Actions expression
  semantics that matter here -- in particular `null == false` is true, so a
  naive `inputs.enforce_gpu_readiness != false` would skip every non-dispatch
  run. `test_evaluator_sees_the_original_defect` pins that the evaluator returns
  "skip" for the pre-#1164 condition, so a green here is not a constant.
* The skip/pass marker patterns are taken from the script and matched against
  lines DERIVED from the producers (`gs_runtime_report.gd` and the four
  scenario scripts' own skip reasons), not hand-written marker lines.
* The enforced key list is compared against the keys the script actually
  writes under `issues`, so a new readiness boolean cannot be collected and
  left unenforced.

Declared limits: the PowerShell script is not executed here (no `pwsh` in the
guard lane); its checks are structural over the script text. The behaviour on
the self-hosted runner is evidence only once a GPU run of the Production Gates
has executed it.
"""

from __future__ import annotations

import math
import re
import sys
import unittest
from pathlib import Path
from typing import Any, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "gaussian_production_gates.yml"
SCRIPT = ROOT / "tests" / "ci" / "collect_production_evidence.ps1"
RUNTIME_DIR = ROOT / "tests" / "runtime"
RUNTIME_REPORT = RUNTIME_DIR / "gs_runtime_report.gd"

# The four scenarios collect_production_evidence.ps1 runs; cross-checked
# against the script's own $runtimeScripts table below, so this is not a
# hand-kept coverage list.
ENFORCE_STEP_NAME = "Enforce readiness booleans"
NON_DISPATCH_EVENTS = ("pull_request", "push", "merge_group")


# --------------------------------------------------------------------------
# Minimal GitHub Actions expression evaluator.
#
# Covers exactly what step conditions in this workflow use: literals, dotted
# context paths, `!`, `==`, `!=`, `&&`, `||`, parentheses and the status
# functions. Equality follows the documented loose semantics: same-type values
# compare directly (strings case-insensitively); otherwise both sides are
# coerced to numbers (null -> 0, false -> 0, true -> 1, '' -> 0, other strings
# parsed or NaN). Unknown syntax raises instead of guessing.
# --------------------------------------------------------------------------

_TOKEN_RE = re.compile(
    r"\s*(?:(?P<str>'(?:[^']|'')*')|(?P<op>&&|\|\||==|!=|!|\(|\)|,)"
    r"|(?P<num>-?\d+(?:\.\d+)?)|(?P<ident>[A-Za-z_][A-Za-z0-9_\-]*(?:\.[A-Za-z0-9_\-]+)*))"
)


def _tokenize(expr: str) -> List[tuple]:
    tokens: List[tuple] = []
    pos = 0
    expr = expr.strip()
    while pos < len(expr):
        match = _TOKEN_RE.match(expr, pos)
        if match is None or match.end() == pos:
            raise ValueError(f"unsupported expression syntax at {expr[pos:pos + 30]!r}")
        pos = match.end()
        kind = match.lastgroup
        tokens.append((kind, match.group(kind)))
    return tokens


def _to_number(value: Any) -> float:
    if value is None:
        return 0.0
    if isinstance(value, bool):
        return 1.0 if value else 0.0
    if isinstance(value, (int, float)):
        return float(value)
    text = str(value).strip()
    if text == "":
        return 0.0
    try:
        return float(text)
    except ValueError:
        return math.nan


def _loose_equal(left: Any, right: Any) -> bool:
    if type(left) is type(right):
        if isinstance(left, str):
            return left.lower() == right.lower()
        return left == right
    a, b = _to_number(left), _to_number(right)
    if math.isnan(a) or math.isnan(b):
        return False
    return a == b


def _truthy(value: Any) -> bool:
    if value is None or value is False:
        return False
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return not (value == 0 or math.isnan(value))
    if isinstance(value, str):
        return value != ""
    return True


class _Evaluator:
    def __init__(self, tokens: List[tuple], context: Dict[str, Any]) -> None:
        self.tokens = tokens
        self.pos = 0
        self.context = context

    def _peek(self) -> Optional[tuple]:
        return self.tokens[self.pos] if self.pos < len(self.tokens) else None

    def _take(self, value: Optional[str] = None) -> tuple:
        token = self._peek()
        if token is None or (value is not None and token[1] != value):
            raise ValueError(f"expected {value!r}, got {token!r}")
        self.pos += 1
        return token

    def parse(self) -> Any:
        result = self._or()
        if self._peek() is not None:
            raise ValueError(f"trailing tokens: {self.tokens[self.pos:]}")
        return result

    def _or(self) -> Any:
        left = self._and()
        while self._peek() == ("op", "||"):
            self._take()
            right = self._and()
            left = left if _truthy(left) else right
        return left

    def _and(self) -> Any:
        left = self._cmp()
        while self._peek() == ("op", "&&"):
            self._take()
            right = self._cmp()
            left = right if _truthy(left) else left
        return left

    def _cmp(self) -> Any:
        left = self._unary()
        while self._peek() in (("op", "=="), ("op", "!=")):
            op = self._take()[1]
            right = self._unary()
            equal = _loose_equal(left, right)
            left = equal if op == "==" else not equal
        return left

    def _unary(self) -> Any:
        if self._peek() == ("op", "!"):
            self._take()
            return not _truthy(self._unary())
        return self._primary()

    def _primary(self) -> Any:
        token = self._take()
        kind, text = token
        if kind == "op" and text == "(":
            value = self._or()
            self._take(")")
            return value
        if kind == "str":
            return text[1:-1].replace("''", "'")
        if kind == "num":
            return float(text)
        if kind == "ident":
            if text == "true":
                return True
            if text == "false":
                return False
            if text == "null":
                return None
            if self._peek() == ("op", "("):
                self._take("(")
                self._take(")")
                if text in ("always", "success"):
                    return True
                if text in ("failure", "cancelled"):
                    return False
                raise ValueError(f"unsupported function {text}()")
            return self.context.get(text)
        raise ValueError(f"unexpected token {token!r}")


def evaluate(expression: str, context: Dict[str, Any]) -> Any:
    expr = expression.strip()
    if expr.startswith("${{") and expr.endswith("}}"):
        expr = expr[3:-2]
    return _Evaluator(_tokenize(expr), context).parse()


# --------------------------------------------------------------------------
# Workflow / script readers.
# --------------------------------------------------------------------------


def _workflow_text() -> str:
    return WORKFLOW.read_text(encoding="utf-8")


def _script_text() -> str:
    return SCRIPT.read_text(encoding="utf-8")


def _step_block(text: str, step_name: str) -> List[str]:
    """Non-comment lines of the single step called `step_name`."""
    lines = text.splitlines()
    hits = [i for i, line in enumerate(lines) if line.strip() == f"- name: {step_name}"]
    if len(hits) != 1:
        raise AssertionError(f"expected exactly one step named {step_name!r}, found {len(hits)}")
    indent = len(lines[hits[0]]) - len(lines[hits[0]].lstrip())
    block = [lines[hits[0]]]
    for line in lines[hits[0] + 1:]:
        stripped = line.strip()
        if stripped and not stripped.startswith("#"):
            line_indent = len(line) - len(line.lstrip())
            if line_indent <= indent:
                break
        block.append(line)
    return [line for line in block if not line.strip().startswith("#")]


def _step_condition(text: str, step_name: str) -> str:
    conditions = [line.strip()[len("if:"):].strip() for line in _step_block(text, step_name) if line.strip().startswith("if:")]
    if len(conditions) != 1:
        raise AssertionError(f"expected exactly one single-line if: on {step_name!r}, found {conditions}")
    return conditions[0]


def _dispatch_input_default(text: str, name: str) -> Any:
    match = re.search(
        rf"^\s+{re.escape(name)}:\n(?:\s+(?:#.*|description:.*|required:.*|type:.*)\n)*\s+default:\s*(\S+)",
        text,
        re.MULTILINE,
    )
    if match is None:
        raise AssertionError(f"workflow_dispatch input {name!r} has no default")
    raw = match.group(1).strip("'\"")
    return {"true": True, "false": False}.get(raw, raw)


def _enforced_keys(text: str) -> List[str]:
    block = "\n".join(_step_block(text, ENFORCE_STEP_NAME))
    match = re.search(r"\$required\s*=\s*@\(([^)]*)\)", block)
    if match is None:
        raise AssertionError("could not locate the $required key list in the enforce step")
    return re.findall(r'"([^"]+)"', match.group(1))


def _script_issue_keys(script: str) -> List[str]:
    match = re.search(r"\bissues\s*=\s*\[ordered\]@\{(.*?)\n\s*\}", script, re.DOTALL)
    if match is None:
        raise AssertionError("could not locate the summary `issues` table in the script")
    return re.findall(r'"([^"]+)"\s*=', match.group(1))


def _ps_single_quoted(script: str, variable: str) -> str:
    match = re.search(rf"^\${re.escape(variable)}\s*=\s*'([^']*)'\s*$", script, re.MULTILINE)
    if match is None:
        raise AssertionError(f"${variable} is not assigned a single-quoted pattern in the script")
    return match.group(1)


def _gd_const(text: str, name: str) -> str:
    match = re.search(rf'^const {re.escape(name)}\s*:?=\s*"([^"]*)"', text, re.MULTILINE)
    if match is None:
        raise AssertionError(f"const {name} not found")
    return match.group(1)


def _runtime_scenarios(script: str) -> List[tuple]:
    return re.findall(r'@\{\s*name\s*=\s*"(runtime_[a-z_]+)";\s*script\s*=\s*"([^"]+)"\s*\}', script)


def _scenario_skip_lines(script_rel: str, skip_marker: str) -> List[str]:
    """The skip line(s) the scenario prints, derived from its own source.

    Every scenario prints `"%s %s" % [SKIP_MARKER, skip_reason]` where
    `skip_reason` is a string literal assigned just above. Reproduce exactly
    that formatting.
    """
    source = (ROOT / script_rel).read_text(encoding="utf-8")
    if 'print("%s %s" % [SKIP_MARKER, skip_reason])' not in source:
        raise AssertionError(f"{script_rel}: skip print format changed; update the derivation")
    reasons = re.findall(r'var skip_reason\s*:?=\s*"([^"]*)"', source)
    if not reasons:
        raise AssertionError(f"{script_rel}: no skip_reason literal found")
    return [f"{skip_marker} {reason}" for reason in reasons]


# --------------------------------------------------------------------------
# Tests.
# --------------------------------------------------------------------------

PRE_1164_CONDITION = (
    "${{ (inputs.enforce_gpu_readiness == true || inputs.enforce_gpu_readiness == 'true') "
    "&& needs.gpu-evidence-requirement.outputs.run_windows_gpu == 'true' "
    "&& steps.evidence.outcome == 'success' && steps.evidence.outputs.dir != '' }}"
)


def _context(event: str, **overrides: Any) -> Dict[str, Any]:
    ctx: Dict[str, Any] = {
        "github.event_name": event,
        "needs.gpu-evidence-requirement.outputs.run_windows_gpu": "true",
        "steps.evidence.outcome": "success",
        "steps.evidence.outputs.dir": "C:\\runner\\artifacts\\evidence\\20261004_000000",
    }
    ctx.update(overrides)
    return ctx


class ReadinessEnforcementWorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = _workflow_text()
        self.condition = _step_condition(self.text, ENFORCE_STEP_NAME)

    def test_evaluator_sees_the_original_defect(self) -> None:
        """Non-vacuity: the pre-#1164 condition and the null==false trap both skip a push."""
        for event in NON_DISPATCH_EVENTS:
            self.assertFalse(_truthy(evaluate(PRE_1164_CONDITION, _context(event))), event)
            self.assertFalse(
                _truthy(evaluate("${{ inputs.enforce_gpu_readiness != false }}", _context(event))),
                f"{event}: Actions treats null == false as true; the evaluator must too",
            )

    def test_enforcement_runs_on_every_non_dispatch_event(self) -> None:
        for event in NON_DISPATCH_EVENTS:
            with self.subTest(event=event):
                self.assertTrue(
                    _truthy(evaluate(self.condition, _context(event))),
                    f"'{ENFORCE_STEP_NAME}' is skipped on {event} although evidence was collected: "
                    f"{self.condition}",
                )

    def test_no_input_value_can_skip_enforcement_outside_dispatch(self) -> None:
        """Property, not mechanism: whatever `inputs` holds, a non-dispatch run enforces."""
        for event in NON_DISPATCH_EVENTS:
            for value in (None, False, "false", True, "", 0):
                with self.subTest(event=event, value=value):
                    ctx = _context(event, **{"inputs.enforce_gpu_readiness": value})
                    self.assertTrue(_truthy(evaluate(self.condition, ctx)))

    def test_manual_dispatch_enforces_by_default(self) -> None:
        default = _dispatch_input_default(self.text, "enforce_gpu_readiness")
        self.assertIs(default, True, "enforce_gpu_readiness must default to true")
        ctx = _context("workflow_dispatch", **{"inputs.enforce_gpu_readiness": default})
        self.assertTrue(_truthy(evaluate(self.condition, ctx)))

    def test_explicit_manual_opt_out_still_works(self) -> None:
        """The legal route: a maintainer's diagnostic dispatch may collect without enforcing."""
        ctx = _context("workflow_dispatch", **{"inputs.enforce_gpu_readiness": False})
        self.assertFalse(_truthy(evaluate(self.condition, ctx)))

    def test_condition_still_requires_collected_evidence(self) -> None:
        for overrides in (
            {"needs.gpu-evidence-requirement.outputs.run_windows_gpu": "false"},
            {"steps.evidence.outputs.dir": ""},
            {"steps.evidence.outcome": "skipped"},
        ):
            with self.subTest(overrides=overrides):
                self.assertFalse(_truthy(evaluate(self.condition, _context("push", **overrides))))

    def test_every_collected_readiness_boolean_is_enforced(self) -> None:
        emitted = _script_issue_keys(_script_text())
        self.assertTrue(emitted, "the script's issues table is empty")
        enforced = _enforced_keys(self.text)
        self.assertEqual(
            sorted(set(emitted) - set(enforced)),
            [],
            "readiness booleans collected into summary.json but never enforced",
        )
        self.assertEqual(sorted(set(enforced) - set(emitted)), [], "enforced keys the script never writes")

    def test_evidence_step_fails_on_a_nonzero_script_exit(self) -> None:
        block = "\n".join(_step_block(self.text, "Collect production evidence"))
        invoke = block.find("collect_production_evidence.ps1")
        capture = block.find("$evidenceExit = $LASTEXITCODE")
        self.assertNotEqual(invoke, -1)
        self.assertGreater(capture, invoke, "the script's exit code is not captured right after it runs")
        self.assertRegex(block, r"if \(\$null -eq \$evidenceExit -or \$evidenceExit -ne 0\) \{\s*\n\s*throw")


class CollectProductionEvidenceScriptTests(unittest.TestCase):
    def setUp(self) -> None:
        self.script = _script_text()
        report = RUNTIME_REPORT.read_text(encoding="utf-8")
        self.skip_marker = _gd_const(report, "SKIP_MARKER")
        self.pass_marker = _gd_const(report, "PASS_MARKER")
        self.fail_marker = _gd_const(report, "FAIL_MARKER")
        self.scenarios = _runtime_scenarios(self.script)

    def test_scenarios_are_found(self) -> None:
        self.assertEqual(len(self.scenarios), 4, self.scenarios)

    def test_skip_pattern_matches_every_scenarios_real_skip_line(self) -> None:
        pattern = re.compile(_ps_single_quoted(self.script, "RuntimeSkipMarkerPattern"), re.IGNORECASE)
        for name, script_rel in self.scenarios:
            for line in _scenario_skip_lines(script_rel, self.skip_marker):
                with self.subTest(scenario=name):
                    self.assertIsNotNone(pattern.search(line), f"{name}: skip line not detected: {line!r}")

    def test_no_dead_headless_skip_regex_remains(self) -> None:
        self.assertNotIn("Skipping .*headless mode", self.script)

    def test_skip_pattern_feeds_both_skip_detectors(self) -> None:
        self.assertRegex(
            self.script,
            r"\$runtimeHeadlessHits = @\(Select-String [^\n]*-Pattern \$RuntimeSkipMarkerPattern",
        )
        self.assertRegex(self.script, r'regex = \$RuntimeSkipMarkerPattern')

    def test_pass_pattern_matches_emit_pass_and_not_an_echo(self) -> None:
        report = RUNTIME_REPORT.read_text(encoding="utf-8")
        self.assertIn('print("%s %s" % [PASS_MARKER, JSON.stringify(payload)])', report)
        pattern = re.compile(_ps_single_quoted(self.script, "RuntimePassMarkerPattern"))
        emitted = f'{self.pass_marker} {{"scenario": "GPU Streaming Stress", "assertions": 3}}'
        self.assertIsNotNone(pattern.search(emitted))
        self.assertIsNone(pattern.search(f"ERROR: scenario quoted {self.pass_marker} in a log"))
        fail = re.compile(_ps_single_quoted(self.script, "RuntimeFailMarkerPattern"))
        self.assertIsNotNone(fail.search(f"ERROR: {self.fail_marker} GPU streaming stress test detected failures"))

    def test_marker_verdict_requires_one_pass_and_no_skip_or_fail(self) -> None:
        match = re.search(r"function Set-RuntimeMarkerVerdict \{(.*?)\n\}", self.script, re.DOTALL)
        self.assertIsNotNone(match, "Set-RuntimeMarkerVerdict is missing")
        body = match.group(1)
        for required in (
            "[int]$Record.exit_code -eq 0",
            "$passCount -eq 1",
            "$skipCount -eq 0",
            "$failCount -eq 0",
            "$Record.passed =",
        ):
            self.assertIn(required, body)

    def test_every_runtime_invocation_is_marker_classified(self) -> None:
        """Wiring: each runtime Invoke-LoggedCommand result goes through the marker verdict."""
        invocations = re.findall(
            r"\$(\w+) = Invoke-LoggedCommand -Name \$(runtimeScript\.name|loopName) [^\n]*\n\s*(.*)",
            self.script,
        )
        self.assertEqual(len(invocations), 2, invocations)
        for variable, _name, next_line in invocations:
            self.assertEqual(next_line.strip(), f"${variable} = Set-RuntimeMarkerVerdict -Record ${variable}")

    def test_readiness_reads_the_verdict_not_the_exit_code(self) -> None:
        match = re.search(r"function Test-ResultPassed \{(.*?)\n\}", self.script, re.DOTALL)
        self.assertIsNotNone(match)
        self.assertIn("[bool]$result.passed", match.group(1))
        self.assertNotIn("exit_code", match.group(1))
        self.assertIn("$loopFailCount = @($loopResults | Where-Object { -not [bool]$_.passed }).Count", self.script)

    def test_902_ready_reads_module_tests_and_943(self) -> None:
        self.assertIn('$moduleTestsPassed = (Test-ResultPassed -Results $results -Name "module_tests")', self.script)
        match = re.search(r"\$issue902Ready = \((.*?)\n\)", self.script, re.DOTALL)
        self.assertIsNotNone(match, "$issue902Ready assignment not found")
        terms = set(re.findall(r"\$\w+", match.group(1)))
        self.assertEqual(
            terms,
            {"$moduleTestsPassed", "$issue897Ready", "$issue900Ready", "$issue943Ready", "$issue871Ready", "$issue815Ready"},
        )

    def test_script_exits_nonzero_when_a_sub_command_failed(self) -> None:
        tail = self.script.rstrip().splitlines()[-20:]
        joined = "\n".join(tail)
        self.assertIn("$failedCommands = @($results | Where-Object { -not [bool]$_.passed })", joined)
        self.assertRegex(joined, r"if \(\$failedCommands\.Count -gt 0\) \{(?:.|\n)*?\n\s+exit 1\n\}")
        self.assertEqual(tail[-1].strip(), "exit 0", "the script must end with an explicit exit")


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1)
