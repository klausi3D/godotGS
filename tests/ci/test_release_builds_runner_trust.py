#!/usr/bin/env python3
"""Every self-hosted job in release_builds.yml is guarded AND documented (#825).

`.github/workflows/AGENTS.md` makes two promises about the persistent
self-hosted Windows/GPU runners:

1. **Security.** A self-hosted job must carry a fork guard so fork-PR code never
   executes on the runner.
2. **Process.** `.github/workflows/README.md` must be kept in sync with "the
   runner trust policy" -- the reader has to be able to see, per workflow, which
   jobs sit on the persistent runner and on which events they run.

`release_builds.yml` had exactly one self-hosted job (`build_windows`) for a
long time, and README's Runner Trust Boundary section named it. #825 added a
second one (`build_windows_export_template`). The guard was copied correctly;
the README bullet was not updated, so the documented trust boundary silently
became a partial list. Nothing failed, because the promise was carried by a
hand-written prose list -- which is the shape of invariant that is already
broken by the time anyone reads it.

So this guard derives both directions from source:

* every self-hosted job in the workflow must carry an **accepted** fork guard,
  and must be named in README's Runner Trust Boundary section;
* every job name README claims is a self-hosted `release_builds.yml` job must
  actually be one -- so the bullet cannot go stale in the other direction
  either.

Two guard forms are accepted, and only two:

``STANDARD_FORK_GUARD``
    The repository standard from `.github/workflows/AGENTS.md`. Fork PRs skip;
    same-repo PRs, `push`, `schedule` and `workflow_dispatch` run.

``STRICT_NO_PULL_REQUEST_GUARD``
    Strictly narrower: **all** pull requests skip, fork and same-repo alike.
    `release_builds.yml`'s Windows lane deliberately uses this one -- see the
    README bullet for the rationale. It is accepted here because it is stricter
    than the standard, never weaker; it cannot let fork code onto the runner.

Anything else -- including a missing `if:` -- is a failure, not a silence. The
same applies to the parser: a `runs-on:` or job-level `if:` form this module
cannot model raises `UnmodelledWorkflowConstruct` rather than being skipped,
because "the sweep found nothing" and "there is nothing to find" must never
produce the same result.

No PyYAML: `tests/ci/validate_automation.py` treats PyYAML as optional, so a
guard that imports it would silently degrade on a runner without it.

Run directly (``python tests/ci/test_release_builds_runner_trust.py``) or via
``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path
from typing import Dict, List, Optional

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "release_builds.yml"
README = ROOT / ".github" / "workflows" / "README.md"

SELF_HOSTED_LABEL = "self-hosted"

# The repository standard (`.github/workflows/AGENTS.md`): fork PRs skip, trusted
# same-repository PRs still run.
STANDARD_FORK_GUARD = (
    "github.event_name != 'pull_request' || "
    "github.event.pull_request.head.repo.full_name == github.repository"
)
# Strictly narrower than the standard: every pull request skips. Accepted only
# because it cannot admit anything the standard guard excludes.
STRICT_NO_PULL_REQUEST_GUARD = "github.event_name != 'pull_request'"

ACCEPTED_GUARDS = {
    STANDARD_FORK_GUARD: "standard",
    STRICT_NO_PULL_REQUEST_GUARD: "strict",
}

README_SECTION_HEADING = "## Runner Trust Boundary"
# The bullet has to declare its job list in a form a guard can read back. Prose
# alone is not checkable: an earlier version of this check scraped every
# backticked token out of the bullet and "found" the job `push` inside
# "runs on `push`/tag/schedule/dispatch".
README_JOB_LIST_MARKER = "self-hosted jobs"

JOB_KEY = re.compile(r"^  ([A-Za-z_][A-Za-z0-9_-]*):\s*$")
JOB_LEVEL_KEY = re.compile(r"^    ([A-Za-z_][A-Za-z0-9_-]*):(.*)$")
BACKTICKED = re.compile(r"`([^`]+)`")
# A job key looks like this; a workflow file name or a YAML snippet does not.
JOB_NAME_SHAPE = re.compile(r"^[a-z][a-z0-9_]*$")


class UnmodelledWorkflowConstruct(RuntimeError):
    """A `runs-on:`/`if:` form this guard cannot reason about.

    Raised instead of guessing. A guard that quietly skips what it does not
    understand reports "no violations" for the one job that needed checking.
    """


def _workflow_lines() -> List[str]:
    return WORKFLOW.read_text(encoding="utf-8").splitlines()


def _strip_comment(value: str) -> str:
    # `runs-on: ubuntu-latest  # comment`. Quotes are handled by the callers,
    # which reject anything they cannot normalize.
    return value.split("#", 1)[0].strip() if "#" in value else value.strip()


def _parse_runs_on(raw: str, job: str) -> List[str]:
    value = _strip_comment(raw)
    if not value:
        raise UnmodelledWorkflowConstruct(
            f"Job {job!r} declares `runs-on:` with a block value. This guard models "
            "only a scalar (`runs-on: ubuntu-latest`) and a flow sequence "
            "(`runs-on: [self-hosted, ...]`). Extend the parser rather than "
            "leaving the job unchecked."
        )
    if "${{" in value:
        raise UnmodelledWorkflowConstruct(
            f"Job {job!r} computes its `runs-on:` from an expression ({value!r}); this "
            "guard cannot tell whether it lands on the persistent self-hosted runner. "
            "Pin the labels literally, or extend this guard."
        )
    if value.startswith("[") and value.endswith("]"):
        labels = [label.strip().strip("\"'") for label in value[1:-1].split(",")]
        return [label for label in labels if label]
    if value.startswith(("{", "&", "*")):
        raise UnmodelledWorkflowConstruct(
            f"Job {job!r} uses an unmodelled `runs-on:` form ({value!r})."
        )
    return [value.strip("\"'")]


def _normalize_guard(expression: str) -> str:
    text = " ".join(expression.split())
    if text.startswith("${{") and text.endswith("}}"):
        text = " ".join(text[3:-2].split())
    return text


def parse_jobs(lines: Optional[List[str]] = None) -> Dict[str, Dict[str, object]]:
    """Job name -> {"runs_on": [labels], "if": raw job-level `if:` or None}.

    Only the block under a column-0 `jobs:` is read, so the two-space keys under
    `on:` (`pull_request:`, `push:`, ...) are never mistaken for jobs.
    """
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
            # Back to a column-0 key: the jobs block ended.
            in_jobs = False
        if not in_jobs:
            index += 1
            continue

        job_match = JOB_KEY.match(line)
        if job_match:
            current = job_match.group(1)
            jobs[current] = {"runs_on": None, "if": None}
            index += 1
            continue

        key_match = JOB_LEVEL_KEY.match(line) if current else None
        if key_match:
            key, rest = key_match.group(1), key_match.group(2)
            if key == "runs-on":
                jobs[current]["runs_on"] = _parse_runs_on(rest, current)
            elif key == "if":
                value = rest.strip()
                if value in ("|", ">", "|-", ">-", "|+", ">+"):
                    # Block scalar: consume the more-indented continuation.
                    block: List[str] = []
                    index += 1
                    while index < len(lines):
                        nxt = lines[index]
                        if nxt.strip() and not nxt.startswith("      "):
                            break
                        block.append(nxt.strip())
                        index += 1
                    jobs[current]["if"] = " ".join(part for part in block if part)
                    continue
                if not value:
                    raise UnmodelledWorkflowConstruct(
                        f"Job {current!r} has an empty job-level `if:`; this guard cannot "
                        "tell what it evaluates to."
                    )
                jobs[current]["if"] = value
        index += 1

    for job, data in jobs.items():
        if data["runs_on"] is None:
            raise UnmodelledWorkflowConstruct(
                f"Job {job!r} has no job-level `runs-on:` this guard could find. Every "
                "job must declare one literally so its runner trust level is decidable."
            )
    return jobs


def self_hosted_jobs(jobs: Optional[Dict[str, Dict[str, object]]] = None) -> Dict[str, Dict[str, object]]:
    if jobs is None:
        jobs = parse_jobs()
    return {
        name: data
        for name, data in jobs.items()
        if SELF_HOSTED_LABEL in [str(label).lower() for label in data["runs_on"]]
    }


def classify_guard(raw_if: Optional[str]) -> Optional[str]:
    """"standard" / "strict" for an accepted guard, else None (a failure)."""
    if raw_if is None:
        return None
    return ACCEPTED_GUARDS.get(_normalize_guard(raw_if))


def readme_trust_section() -> str:
    text = README.read_text(encoding="utf-8")
    start = text.find(README_SECTION_HEADING)
    if start < 0:
        raise UnmodelledWorkflowConstruct(
            f"{README.name} has no {README_SECTION_HEADING!r} section; the runner trust "
            "policy this guard cross-checks against is gone."
        )
    end = text.find("\n## ", start + len(README_SECTION_HEADING))
    return text[start:] if end < 0 else text[start:end]


def readme_release_builds_bullet() -> str:
    """The Runner Trust Boundary bullet(s) that talk about release_builds.yml."""
    section = readme_trust_section()
    bullets = [
        line for line in section.splitlines() if line.lstrip().startswith("- ") and "release_builds.yml" in line
    ]
    if not bullets:
        raise UnmodelledWorkflowConstruct(
            "The Runner Trust Boundary section has no bullet for release_builds.yml, so "
            "its self-hosted jobs are undocumented."
        )
    return "\n".join(bullets)


def readme_declared_jobs() -> List[str]:
    """The job names the README bullet declares for release_builds.yml.

    Read from a delimited `self-hosted jobs \\`a\\`, \\`b\\`.` clause, not from
    "every backtick in the bullet" -- the bullet also quotes event names and a
    YAML fragment, and a scrape cannot tell those from a job key.
    """
    return extract_declared_jobs(readme_release_builds_bullet())


def extract_declared_jobs(bullet: str) -> List[str]:
    marker = bullet.find(README_JOB_LIST_MARKER)
    if marker < 0:
        raise UnmodelledWorkflowConstruct(
            "The release_builds.yml Runner Trust Boundary bullet does not declare its "
            f"job list as `{README_JOB_LIST_MARKER} `a`, `b`.`, so this guard "
            "cannot check the documented set against the workflow. Keep the clause."
        )
    clause = bullet[marker + len(README_JOB_LIST_MARKER) :]
    clause = clause.split(".", 1)[0]
    names = BACKTICKED.findall(clause)
    if not names:
        raise UnmodelledWorkflowConstruct(
            "The release_builds.yml Runner Trust Boundary bullet declares a job clause "
            "with no job names in it."
        )
    return names


class RunnerTrustTests(unittest.TestCase):
    def setUp(self) -> None:
        self.jobs = parse_jobs()
        self.self_hosted = self_hosted_jobs(self.jobs)

    def test_parser_is_not_degenerate(self) -> None:
        # A parser that classified everything one way would make every check
        # below pass vacuously, in one direction or the other.
        self.assertTrue(self.self_hosted, "no self-hosted job found in release_builds.yml")
        self.assertTrue(
            set(self.jobs) - set(self.self_hosted),
            "every job parsed as self-hosted; the runs-on parser is misreading the file",
        )

    def test_every_self_hosted_job_carries_an_accepted_fork_guard(self) -> None:
        for name, data in sorted(self.self_hosted.items()):
            with self.subTest(job=name):
                self.assertIsNotNone(
                    classify_guard(data["if"]),
                    f"Job {name!r} runs on the persistent self-hosted runner with "
                    f"if={data['if']!r}, which is neither the repository standard fork "
                    f"guard ({STANDARD_FORK_GUARD!r}) nor the stricter "
                    f"{STRICT_NO_PULL_REQUEST_GUARD!r}. Untrusted fork-PR code must never "
                    "reach that runner -- see .github/workflows/AGENTS.md.",
                )

    def test_every_self_hosted_job_is_documented_in_the_readme(self) -> None:
        section = readme_trust_section()
        for name in sorted(self.self_hosted):
            with self.subTest(job=name):
                self.assertIn(
                    f"`{name}`",
                    section,
                    f"release_builds.yml job {name!r} runs on the persistent self-hosted "
                    "runner but is not named in the Runner Trust Boundary section of "
                    ".github/workflows/README.md. AGENTS.md requires README to stay in "
                    "sync with the runner trust policy.",
                )

    def test_the_readme_bullet_has_no_stale_job_names(self) -> None:
        claimed = readme_declared_jobs()
        for name in claimed:
            with self.subTest(job=name):
                self.assertRegex(
                    name,
                    JOB_NAME_SHAPE,
                    f"{name!r} is declared in the release_builds.yml trust bullet's job "
                    "clause but is not shaped like a job key.",
                )
        stale = sorted(set(claimed) - set(self.self_hosted))
        self.assertEqual(
            stale,
            [],
            f"the Runner Trust Boundary bullet names {stale} as self-hosted "
            "release_builds.yml job(s), but the workflow has no such self-hosted job. "
            "A renamed or deleted job leaves the documented trust boundary describing "
            "something that no longer exists.",
        )

    def test_the_readme_bullet_declares_every_self_hosted_job(self) -> None:
        missing = sorted(set(self.self_hosted) - set(readme_declared_jobs()))
        self.assertEqual(
            missing,
            [],
            f"self-hosted release_builds.yml job(s) {missing} are missing from the "
            "Runner Trust Boundary bullet's job clause.",
        )

    def test_the_readme_documents_which_guard_form_each_job_uses(self) -> None:
        # The two accepted forms differ in whether trusted same-repo PRs run, so a
        # bullet that names the jobs without naming the form is not a trust policy.
        bullet = readme_release_builds_bullet()
        forms = {classify_guard(data["if"]) for data in self.self_hosted.values()}
        if "strict" in forms:
            self.assertIn(
                "github.event_name != 'pull_request'",
                bullet,
                "release_builds.yml self-hosted job(s) use the stricter all-PR-skip "
                "guard, but the README bullet does not show that form.",
            )


class ParserFailClosedTests(unittest.TestCase):
    def test_computed_runs_on_raises(self) -> None:
        with self.assertRaises(UnmodelledWorkflowConstruct):
            parse_jobs(["jobs:", "  a:", "    runs-on: ${{ matrix.os }}"])

    def test_block_sequence_runs_on_raises(self) -> None:
        with self.assertRaises(UnmodelledWorkflowConstruct):
            parse_jobs(["jobs:", "  a:", "    runs-on:", "      - self-hosted"])

    def test_missing_runs_on_raises(self) -> None:
        with self.assertRaises(UnmodelledWorkflowConstruct):
            parse_jobs(["jobs:", "  a:", "    needs: b"])

    def test_on_block_keys_are_not_parsed_as_jobs(self) -> None:
        lines = [
            "on:",
            "  pull_request:",
            "    branches: [master]",
            "jobs:",
            "  a:",
            "    runs-on: ubuntu-latest",
        ]
        self.assertEqual(sorted(parse_jobs(lines)), ["a"])

    def test_step_level_if_is_not_read_as_the_job_guard(self) -> None:
        lines = [
            "jobs:",
            "  a:",
            "    runs-on: [self-hosted, Windows]",
            "    steps:",
            "      - name: x",
            "        if: github.event_name != 'pull_request'",
        ]
        self.assertIsNone(parse_jobs(lines)["a"]["if"])

    def test_block_scalar_if_is_joined(self) -> None:
        lines = [
            "jobs:",
            "  a:",
            "    runs-on: ubuntu-latest",
            "    if: |",
            "      github.event_name != 'pull_request' ||",
            "      github.event.pull_request.head.repo.full_name == github.repository",
            "    steps:",
            "      - name: x",
        ]
        self.assertEqual(classify_guard(parse_jobs(lines)["a"]["if"]), "standard")

    def test_flow_sequence_labels_are_split(self) -> None:
        lines = ["jobs:", "  a:", "    runs-on: [self-hosted, Windows, X64, godotgs]"]
        self.assertEqual(parse_jobs(lines)["a"]["runs_on"], ["self-hosted", "Windows", "X64", "godotgs"])
        self.assertEqual(sorted(self_hosted_jobs(parse_jobs(lines))), ["a"])


class ReadmeClauseTests(unittest.TestCase):
    def test_the_real_readme_declares_a_parseable_job_clause(self) -> None:
        self.assertTrue(readme_declared_jobs())

    def test_a_bullet_without_the_marker_raises(self) -> None:
        # Fail closed: prose that stops declaring its job list must break the
        # guard, not silently reduce it to "nothing to check".
        with self.assertRaises(UnmodelledWorkflowConstruct):
            extract_declared_jobs("- `release_builds.yml` — runs on `push` only.")

    def test_a_marker_with_no_names_raises(self) -> None:
        with self.assertRaises(UnmodelledWorkflowConstruct):
            extract_declared_jobs("- `release_builds.yml` — self-hosted jobs: none listed.")

    def test_only_the_declaring_clause_is_read(self) -> None:
        bullet = (
            "- `release_builds.yml` — self-hosted jobs `build_windows`, `build_x`. "
            "They run on `push`/tag/schedule/dispatch only."
        )
        self.assertEqual(extract_declared_jobs(bullet), ["build_windows", "build_x"])


class GuardClassificationTests(unittest.TestCase):
    def test_missing_if_is_not_an_accepted_guard(self) -> None:
        self.assertIsNone(classify_guard(None))

    def test_expression_wrapper_is_accepted(self) -> None:
        self.assertEqual(classify_guard("${{ " + STANDARD_FORK_GUARD + " }}"), "standard")

    def test_strict_form_is_accepted(self) -> None:
        self.assertEqual(classify_guard(STRICT_NO_PULL_REQUEST_GUARD), "strict")

    def test_a_guard_that_admits_fork_prs_is_rejected(self) -> None:
        self.assertIsNone(classify_guard("github.event_name == 'pull_request'"))
        self.assertIsNone(classify_guard("always()"))
        self.assertIsNone(
            classify_guard(STRICT_NO_PULL_REQUEST_GUARD + " || github.event_name == 'pull_request'")
        )

    def test_head_ref_lookalike_is_rejected(self) -> None:
        # `head.repo.fork == false` reads like the standard guard and is not it:
        # it is unset for non-PR events, so the job would stop running on push.
        self.assertIsNone(
            classify_guard(
                "github.event_name != 'pull_request' || github.event.pull_request.head.repo.fork == false"
            )
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
