"""Machinery-listed check scripts must be WIRED into the guard-only runner.

`.agentic/policy.json` lists `tests/ci/check_*.py` scripts as deterministic-
check machinery (classification rule "CI deterministic-check / release-gate
machinery"). Each such script is executed by `tests/ci/run_module_tests.py
--guard-only` through a `_run_*_guard` helper referenced from the runner's
guard registry. That registry is the ONLY wiring: deleting one tuple entry
silently drops the guard while `--guard-only` stays green, leaving the check
decorative (Codex #924 review finding; same shape as the skipped-required-check
bypass). A guard cannot police its own wiring — if the tuple is gone it never
runs — so this test enforces it from a different lane: `python -m unittest
discover -s tests/agentic` runs inside the required `agentic-pr-gate` job on
every PR.

Coverage is DERIVED from the policy (never a hand-kept list here): every
`tests/ci/check_*.py` path named in any classification rule's `path_globs`
must (1) exist, (2) be referenced by at least one runner helper function, and
(3) have at least one of those helpers referenced OUTSIDE its own `def` line —
i.e. from the registry (or any other call site) — in the runner's source.

Out of scope, by design: removing the script from `.agentic/policy.json`
itself is a risk-policy diff, which the classifier force-escalates to the top
risk class (self-referential rule) and human review controls; and an
adversarial edit that keeps a dangling reference while neutering the call is
the review policy's problem, not a static test's.
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
POLICY_PATH = REPO_ROOT / ".agentic" / "policy.json"
RUNNER_PATH = REPO_ROOT / "tests" / "ci" / "run_module_tests.py"

_CHECK_SCRIPT_PATTERN = re.compile(r"^tests/ci/check_[A-Za-z0-9_]+\.py$")


def _machinery_check_scripts() -> list[str]:
    """Every literal tests/ci/check_*.py path named in classification path_globs."""
    policy = json.loads(POLICY_PATH.read_text(encoding="utf-8"))
    scripts: list[str] = []
    for rule in policy.get("classification", {}).get("rules", []):
        for glob in rule.get("path_globs", []):
            if _CHECK_SCRIPT_PATTERN.match(glob):
                scripts.append(glob)
    return sorted(set(scripts))


def _strip_comments(source: str) -> str:
    return re.sub(r"#[^\n]*", "", source)


def _runner_helpers_referencing(source: str, basename: str) -> list[str]:
    """Names of `def _run_*` functions whose body mentions the script.

    A helper mentions the script either via its basename directly or via a
    module-level constant assigned from it (NAME = ROOT / "tests" / "ci" /
    "<basename>").
    """
    tokens = {f'"{basename}"'}
    for m in re.finditer(
        r"^(\w+)\s*=\s*ROOT\s*/\s*\"tests\"\s*/\s*\"ci\"\s*/\s*\"" + re.escape(basename) + r"\"",
        source,
        flags=re.M,
    ):
        tokens.add(m.group(1))
    helpers: list[str] = []
    blocks = re.split(r"(?=^def )", source, flags=re.M)
    for block in blocks:
        m = re.match(r"def (_run_\w+)\(", block)
        if m and any(token in block for token in tokens):
            helpers.append(m.group(1))
    return helpers


def _non_def_reference_count(source: str, name: str) -> int:
    """References to `name` outside its own `def` line, comments stripped."""
    stripped = _strip_comments(source)
    total = len(re.findall(r"\b" + re.escape(name) + r"\b", stripped))
    defs = len(re.findall(r"^def " + re.escape(name) + r"\(", stripped, flags=re.M))
    return total - defs


class GuardWiringTest(unittest.TestCase):
    def setUp(self) -> None:
        self.scripts = _machinery_check_scripts()
        self.runner_source = RUNNER_PATH.read_text(encoding="utf-8")

    def test_policy_names_at_least_one_check_script(self):
        """Derivation sanity: an empty derived set would make every other test
        here vacuously green (the vacuous-pass shape)."""
        self.assertTrue(
            self.scripts,
            f"no tests/ci/check_*.py machinery entries derived from {POLICY_PATH}",
        )

    def test_every_machinery_check_script_exists(self):
        for rel in self.scripts:
            with self.subTest(script=rel):
                self.assertTrue(
                    (REPO_ROOT / rel).is_file(),
                    f"policy machinery names {rel} but the file does not exist",
                )

    def test_every_machinery_check_script_is_wired_into_the_runner(self):
        for rel in self.scripts:
            basename = rel.rsplit("/", 1)[-1]
            with self.subTest(script=rel):
                helpers = _runner_helpers_referencing(self.runner_source, basename)
                self.assertTrue(
                    helpers,
                    f"{RUNNER_PATH.name}: no _run_* helper references {basename} — "
                    "the machinery-listed check has no runner at all",
                )
                wired = [h for h in helpers if _non_def_reference_count(self.runner_source, h) > 0]
                self.assertTrue(
                    wired,
                    f"{RUNNER_PATH.name}: helper(s) {helpers} for {basename} are defined "
                    "but never referenced outside their own def — the guard registry "
                    "entry was removed, so --guard-only silently skips this check",
                )


if __name__ == "__main__":
    unittest.main()
