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

**Which jobs are self-hosted is decided by label routing, not by the word
`self-hosted`.** GitHub routes a job to any runner carrying *all* of the job's
labels; the `self-hosted` label is conventional, not required. A job declaring
`runs-on: [Windows, X64, godotgs]` therefore lands on the persistent runner just
as surely as one that spells out `self-hosted`. So this guard classifies by
label set:

* `self-hosted` present -> self-hosted;
* otherwise, labels that are a subset of the **persistent-runner label
  vocabulary** -> self-hosted. That vocabulary is *derived* from every
  `runs-on:` in `.github/workflows/` that does spell out `self-hosted`
  (`persistent_runner_labels()`), never hand-written here -- a hand-written list
  of "labels that mean self-hosted" is the same shape of already-broken
  invariant this guard exists to replace;
* otherwise, labels that are all GitHub-hosted runner *images*
  (`ubuntu-latest`, `windows-2022`, `macos-14`, ...) -> GitHub-hosted;
* anything else -> `UnmodelledWorkflowConstruct`. A label this guard has never
  seen paired with `self-hosted` and that is not a hosted image may or may not
  select the persistent runner, and "may" is not "no".

No PyYAML: `tests/ci/validate_automation.py` treats PyYAML as optional, so a
guard that imports it would silently degrade on a runner without it.

Run directly (``python tests/ci/test_release_builds_runner_trust.py``) or via
``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import re
import tempfile
import unittest
from pathlib import Path
from typing import Dict, List, Optional

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = ROOT / ".github" / "workflows"
WORKFLOW = WORKFLOW_DIR / "release_builds.yml"
README = WORKFLOW_DIR / "README.md"

SELF_HOSTED_LABEL = "self-hosted"

# A GitHub-hosted runner image label: `ubuntu-latest`, `ubuntu-24.04`,
# `windows-2022`, `macos-14`, plus the documented `-arm` / `-large` /
# `-<n>-cores` variants. Deliberately narrow: a label that does not match and is
# not in the derived persistent-runner vocabulary is unmodelled, not "hosted".
GITHUB_HOSTED_IMAGE = re.compile(
    r"^(?:ubuntu|windows|macos)-(?:latest|\d{1,4}(?:\.\d{2})?)"
    r"(?:-(?:arm|large|xlarge|\d{1,3}-cores))*$"
)
RUNS_ON_LINE = re.compile(r"^\s*runs-on:(.*)$")

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

# For every guard form actually in use, the README bullet has to show the form
# itself, not just tag jobs with its name -- a tag whose meaning is only defined
# somewhere else is not a trust policy a reader can check.
GUARD_FORM_README_LITERAL = {
    "strict": "`if: github.event_name != 'pull_request'`",
    "standard": "github.event.pull_request.head.repo.full_name == github.repository",
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
# ``build_windows` (strict)` -- the job name plus the guard form README claims for
# it. The form token is captured loosely so an unknown one fails loudly instead of
# simply not matching.
DECLARED_JOB_ANNOTATION = re.compile(r"`([^`]+)`\s*\(([^)]*)\)")
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


def persistent_runner_labels(paths: Optional[List[Path]] = None) -> frozenset:
    """Custom labels that are known to select a persistent self-hosted runner.

    Derived, never declared here: every `runs-on:` in `.github/workflows/` that
    names `self-hosted` contributes its remaining labels. That is what makes the
    subset rule in :func:`classify_runner` self-maintaining -- a new persistent
    runner label shows up in this vocabulary as soon as one job spells it out
    next to `self-hosted`, and until then any job using it alone is unmodelled
    (a failure) rather than assumed GitHub-hosted.
    """
    if paths is None:
        paths = sorted(WORKFLOW_DIR.glob("*.yml")) + sorted(WORKFLOW_DIR.glob("*.yaml"))

    labels = set()
    declarations = 0
    for path in paths:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = RUNS_ON_LINE.match(line)
            if not match or SELF_HOSTED_LABEL not in match.group(1):
                continue
            parsed = _parse_runs_on(match.group(1), f"{path.name}:{number}")
            lowered = {str(label).lower() for label in parsed}
            if SELF_HOSTED_LABEL not in lowered:
                # e.g. a label that merely contains the substring.
                continue
            declarations += 1
            labels |= lowered - {SELF_HOSTED_LABEL}

    if not declarations:
        raise UnmodelledWorkflowConstruct(
            "No `runs-on:` in .github/workflows/ names `self-hosted`, so the persistent "
            "runner's label vocabulary cannot be derived. Without it this guard cannot "
            "tell a custom-label job from a GitHub-hosted one; it refuses to guess."
        )
    return frozenset(labels)


def classify_runner(labels: List[str], job: str, persistent: Optional[frozenset] = None) -> str:
    """"self-hosted" / "github-hosted" for a job's `runs-on:` labels.

    Raises `UnmodelledWorkflowConstruct` for a label set that is neither, because
    a label GitHub might route to the persistent runner must not be filed under
    "safe" by default.
    """
    if persistent is None:
        persistent = persistent_runner_labels()

    lowered = {str(label).lower() for label in labels}
    if not lowered:
        raise UnmodelledWorkflowConstruct(
            f"Job {job!r} declares an empty `runs-on:` label set; which runner it selects "
            "is undecidable."
        )
    if SELF_HOSTED_LABEL in lowered:
        return "self-hosted"
    if lowered <= persistent:
        # GitHub routes a job to any runner carrying all of the job's labels, so
        # these select the persistent runner even without the `self-hosted` label.
        return "self-hosted"
    if all(GITHUB_HOSTED_IMAGE.match(label) for label in lowered):
        return "github-hosted"
    raise UnmodelledWorkflowConstruct(
        f"Job {job!r} selects its runner with labels {sorted(lowered)}, which are neither a "
        f"GitHub-hosted runner image nor a subset of the derived persistent-runner label "
        f"vocabulary {sorted(persistent)}. This guard cannot tell whether they route to the "
        "persistent self-hosted runner, and it will not assume they do not. Add the "
        "`self-hosted` label if they do, or extend this guard."
    )


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


def self_hosted_jobs(
    jobs: Optional[Dict[str, Dict[str, object]]] = None,
    persistent: Optional[frozenset] = None,
) -> Dict[str, Dict[str, object]]:
    if jobs is None:
        jobs = parse_jobs()
    if persistent is None:
        persistent = persistent_runner_labels()
    return {
        name: data
        for name, data in jobs.items()
        if classify_runner(data["runs_on"], name, persistent) == "self-hosted"
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


def readme_declared_job_forms() -> Dict[str, str]:
    """Job name -> the guard form ("standard"/"strict") README claims it carries."""
    return extract_declared_job_forms(readme_release_builds_bullet())


def _job_clause(bullet: str) -> str:
    marker = bullet.find(README_JOB_LIST_MARKER)
    if marker < 0:
        raise UnmodelledWorkflowConstruct(
            "The release_builds.yml Runner Trust Boundary bullet does not declare its "
            f"job list as `{README_JOB_LIST_MARKER} `a` (form), `b` (form).`, so this "
            "guard cannot check the documented set against the workflow. Keep the clause."
        )
    return bullet[marker + len(README_JOB_LIST_MARKER) :].split(".", 1)[0]


def extract_declared_jobs(bullet: str) -> List[str]:
    names = BACKTICKED.findall(_job_clause(bullet))
    if not names:
        raise UnmodelledWorkflowConstruct(
            "The release_builds.yml Runner Trust Boundary bullet declares a job clause "
            "with no job names in it."
        )
    return names


def extract_declared_job_forms(bullet: str) -> Dict[str, str]:
    """Per-job guard forms from the README clause: ``\\`job\\` (strict), ...``.

    Per job, because a set of the forms in use is not a claim about any one job:
    with one job on each accepted form, a set-membership check confirms only that
    README mentions each form *somewhere*, and the bullet stays green while it
    tells the reader the wrong thing about a specific job.
    """
    clause = _job_clause(bullet)
    names = extract_declared_jobs(bullet)
    annotated = DECLARED_JOB_ANNOTATION.findall(clause)
    if [name for name, _ in annotated] != names:
        raise UnmodelledWorkflowConstruct(
            "Every job in the release_builds.yml Runner Trust Boundary job clause must be "
            "tagged with the guard form it carries, as ``job` (strict)`. Untagged: "
            f"{sorted(set(names) - {name for name, _ in annotated})}."
        )

    accepted = sorted(set(ACCEPTED_GUARDS.values()))
    forms: Dict[str, str] = {}
    for name, raw_form in annotated:
        form = raw_form.strip()
        if form not in accepted:
            raise UnmodelledWorkflowConstruct(
                f"The Runner Trust Boundary bullet tags job {name!r} with the unknown guard "
                f"form {form!r}; this guard only models {accepted}."
            )
        if name in forms:
            raise UnmodelledWorkflowConstruct(
                f"Job {name!r} is declared twice in the Runner Trust Boundary job clause."
            )
        forms[name] = form
    return forms


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

    def test_the_readme_documents_the_guard_form_of_each_self_hosted_job(self) -> None:
        # Per job. The two accepted forms differ in whether trusted same-repo PRs
        # run, so "README mentions this form somewhere in the bullet" is not a
        # statement about the job whose form changed.
        declared = readme_declared_job_forms()
        for name, data in sorted(self.self_hosted.items()):
            with self.subTest(job=name):
                self.assertIn(
                    name,
                    declared,
                    f"self-hosted job {name!r} carries no guard-form tag in the Runner "
                    "Trust Boundary bullet.",
                )
                self.assertEqual(
                    declared[name],
                    classify_guard(data["if"]),
                    f"the Runner Trust Boundary bullet documents job {name!r} as using the "
                    f"{declared[name]!r} guard form, but the workflow has "
                    f"if={data['if']!r}. The documented trust boundary must describe the "
                    "guard each job actually carries.",
                )

    def test_the_readme_shows_every_guard_form_in_use(self) -> None:
        # A per-job tag is only checkable prose if the bullet also shows what the
        # tag means for the forms actually in use.
        bullet = readme_release_builds_bullet()
        for form in sorted(
            form for form in {classify_guard(d["if"]) for d in self.self_hosted.values()} if form
        ):
            with self.subTest(form=form):
                self.assertIn(
                    GUARD_FORM_README_LITERAL[form],
                    bullet,
                    f"release_builds.yml self-hosted job(s) use the {form!r} guard form, but "
                    "the README bullet does not show that form.",
                )


class RunnerLabelClassificationTests(unittest.TestCase):
    """Routing is by label set, so `self-hosted` being optional must not be a hole."""

    VOCABULARY = frozenset({"windows", "x64", "godotgs", "gpu"})

    def test_the_derived_vocabulary_is_real_and_not_degenerate(self) -> None:
        labels = persistent_runner_labels()
        self.assertIn("godotgs", labels, "the persistent runner's custom label was not derived")
        self.assertNotIn(SELF_HOSTED_LABEL, labels, "`self-hosted` must not be in the custom set")
        self.assertNotIn("ubuntu-latest", labels, "a GitHub-hosted image leaked into the vocabulary")

    def _corpus(self, text: str) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "w.yml"
        path.write_text(text, encoding="utf-8")
        return path

    def test_the_vocabulary_is_derived_from_the_workflows_not_hardcoded(self) -> None:
        # Feed it a synthetic corpus: a new persistent-runner label must appear
        # without anyone editing this module.
        corpus = self._corpus("jobs:\n  a:\n    runs-on: [self-hosted, Linux, brand-new-rig]\n")
        self.assertEqual(persistent_runner_labels([corpus]), frozenset({"linux", "brand-new-rig"}))

    def test_a_corpus_without_any_self_hosted_declaration_raises(self) -> None:
        corpus = self._corpus("jobs:\n  a:\n    runs-on: ubuntu-latest\n")
        with self.assertRaises(UnmodelledWorkflowConstruct):
            persistent_runner_labels([corpus])

    def test_custom_labels_without_the_self_hosted_label_are_self_hosted(self) -> None:
        # The hole this class exists for: GitHub routes `[Windows, X64, godotgs]`
        # to the persistent runner, `self-hosted` label or not.
        self.assertEqual(
            classify_runner(["Windows", "X64", "godotgs"], "sneaky", self.VOCABULARY),
            "self-hosted",
        )

    def test_a_custom_label_job_is_swept_into_the_self_hosted_set(self) -> None:
        lines = [
            "jobs:",
            "  sneaky:",
            "    runs-on: [Windows, X64, godotgs]",
            "  hosted:",
            "    runs-on: ubuntu-latest",
        ]
        jobs = parse_jobs(lines)
        self.assertEqual(sorted(self_hosted_jobs(jobs, self.VOCABULARY)), ["sneaky"])
        self.assertIsNone(classify_guard(jobs["sneaky"]["if"]))

    def test_the_self_hosted_label_alone_is_still_self_hosted(self) -> None:
        self.assertEqual(classify_runner(["self-hosted"], "a", self.VOCABULARY), "self-hosted")

    def test_github_hosted_images_are_not_self_hosted(self) -> None:
        for label in ("ubuntu-latest", "ubuntu-24.04", "windows-2022", "macos-14", "ubuntu-24.04-arm"):
            with self.subTest(label=label):
                self.assertEqual(classify_runner([label], "a", self.VOCABULARY), "github-hosted")

    def test_an_unknown_label_raises_instead_of_being_called_hosted(self) -> None:
        # Fail closed: a label never seen next to `self-hosted` and not a hosted
        # image might route anywhere, and "might" is not "no".
        for labels in (["mystery-rig"], ["godotgs", "mystery-rig"], ["Windows", "arm64"]):
            with self.subTest(labels=labels):
                with self.assertRaises(UnmodelledWorkflowConstruct):
                    classify_runner(labels, "a", self.VOCABULARY)

    def test_an_empty_label_set_raises(self) -> None:
        with self.assertRaises(UnmodelledWorkflowConstruct):
            classify_runner([], "a", self.VOCABULARY)


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

    def test_the_real_readme_tags_every_declared_job_with_a_guard_form(self) -> None:
        forms = readme_declared_job_forms()
        self.assertEqual(sorted(forms), sorted(readme_declared_jobs()))

    def test_per_job_forms_are_read_off_the_clause(self) -> None:
        bullet = (
            "- `release_builds.yml` — self-hosted jobs `build_windows` (strict), "
            "`build_x` (standard). They run on `push` only."
        )
        self.assertEqual(
            extract_declared_job_forms(bullet),
            {"build_windows": "strict", "build_x": "standard"},
        )

    def test_an_untagged_job_raises(self) -> None:
        # Fail closed: an untagged job is an undocumented guard form, not a
        # default one.
        bullet = "- `release_builds.yml` — self-hosted jobs `build_windows` (strict), `build_x`."
        with self.assertRaises(UnmodelledWorkflowConstruct):
            extract_declared_job_forms(bullet)

    def test_an_unknown_form_tag_raises(self) -> None:
        bullet = "- `release_builds.yml` — self-hosted jobs `build_windows` (both)."
        with self.assertRaises(UnmodelledWorkflowConstruct):
            extract_declared_job_forms(bullet)

    def test_a_duplicate_job_declaration_raises(self) -> None:
        bullet = (
            "- `release_builds.yml` — self-hosted jobs `build_windows` (strict), "
            "`build_windows` (standard)."
        )
        with self.assertRaises(UnmodelledWorkflowConstruct):
            extract_declared_job_forms(bullet)


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
