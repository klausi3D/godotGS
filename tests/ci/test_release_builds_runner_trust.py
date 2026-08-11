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

Scope: this guard is about the TRUST boundary (which jobs reach the persistent
runner, and under which guard form), not about which jobs gate publication.
README's other stale claim about the same jobs -- that both export-template
builds are ungated -- is checked by `tests/ci/test_release_publication_gating.py`
(`GatingDocumentationTests`), which already derives the `needs:` closure it
needs. Extend that one rather than teaching this file a second policy.

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
  vocabulary** -> self-hosted;
* otherwise, a label set of **exactly one** label that the repository has
  explicitly declared as a GitHub-hosted runner label -> GitHub-hosted;
* anything else -> `UnmodelledWorkflowConstruct`.

Both label sets come from a **declared runner-label policy**, not from pattern
matching on the label text (`runner_label_policy()`). Rounds 5 and 6 inferred
"GitHub-hosted" from the *shape* of a label -- anything matching
`^(ubuntu|windows|macos)-(latest|<version>)$` was treated as a safe hosted image
-- and that inference was wrong twice, because nothing stops a self-hosted runner
from carrying a label that looks like a hosted image. `windows-2022` is a
perfectly ordinary custom label for a self-hosted Windows Server 2022 box.
Worse, GitHub routes an *array* to a runner matching **every** label, so
`runs-on: [ubuntu-latest, windows-2022]` cannot name a GitHub-hosted image at
all: no hosted runner carries two image labels, so the only machine that can
satisfy it is a self-hosted one whose custom labels happen to spell those two
words. The shape-based fallback nevertheless called that "github-hosted", which
dropped the job out of `self_hosted_jobs()` -- so it received neither the fork
guard check nor the README checks.

Hence:

* the persistent runner's label inventory and the GitHub-hosted labels this
  repository actually uses are **declared** in the Runner Trust Boundary section
  of `.github/workflows/README.md`, where a human reviews them against the real
  runner inventory. That declaration is the only place "this label is not on our
  self-hosted runner" is ever asserted;
* the declaration is cross-checked against the workflows: every label derived
  from a `runs-on:` that spells out `self-hosted` (`persistent_runner_labels()`)
  must appear in the declared persistent inventory, and the declared hosted
  labels must be **disjoint** from it. A new persistent-runner label therefore
  fails the guard until it is declared, instead of being silently filed under
  "unknown, probably fine";
* a GitHub-hosted classification additionally requires a **single** label,
  because a GitHub-hosted runner matches exactly one image label. Any multi-label
  set that is not a subset of the persistent vocabulary is unmodelled.

The residual assumption -- that the declared hosted labels really are absent
from the self-hosted runner inventory -- is not inferable from workflow text at
all. It is a maintainer statement, made once, in a reviewed location, instead of
a regex re-deriving it wrongly on every run.

Both halves of that arrangement have to be read *strictly*, because both are
places where a near-miss reads as a pass:

* the workflow scan is **normalized before detection**. GitHub matches runner
  labels case-insensitively, so `runs-on: [Self-Hosted, ...]` is an ordinary
  declaration; a case-sensitive pre-filter skipped it while the lowercase
  declarations elsewhere kept the scan non-empty, so nothing raised and that
  line's labels never reached the cross-check;
* the README declaration must be a **delimited clause**: the marker begins the
  bullet, and the rest is a comma-separated list of backticked labels and
  nothing else. "A bullet containing the marker, scraped for backticks" accepted
  prose that declared no policy at all (a negative or historical sentence) and
  read trailing prose as policy -- a counter-example naming `windows-2022`
  became a declared GitHub-hosted label, which is exactly the classification
  that drops a job out of `self_hosted_jobs()`.

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
from typing import Dict, Iterable, List, NamedTuple, Optional, Set, Tuple

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = ROOT / ".github" / "workflows"
WORKFLOW = WORKFLOW_DIR / "release_builds.yml"
README = WORKFLOW_DIR / "README.md"

SELF_HOSTED_LABEL = "self-hosted"

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

# The declared runner-label policy, read from two bullets in the same README
# section. Declared, because "this label does not select our self-hosted runner"
# is a fact about the runner inventory that no amount of reading workflow text
# can establish.
README_PERSISTENT_LABELS_MARKER = "Persistent self-hosted runner labels:"
README_GITHUB_HOSTED_LABELS_MARKER = "GitHub-hosted runner labels:"
# The declaration bullet must be exactly `- <marker> `a`, `b`, `c`[.]` and nothing
# else. Two things go wrong when the clause is merely "a bullet containing the
# marker, scraped for backticks":
#
# * a historical/negative/explanatory bullet ("We no longer declare GitHub-hosted
#   runner labels: `ubuntu-latest`.") satisfies the check without declaring a
#   policy at all;
# * trailing prose is scraped as policy, so a counter-example
#   ("... `ubuntu-latest`. For example `windows-2022` is not one.") becomes a
#   declared GitHub-hosted label -- and `windows-2022` is exactly the
#   self-hosted-looking label rounds 5/6 got wrong.
#
# So: the marker must BEGIN the bullet, and what follows must be a comma-separated
# list of backticked labels with an optional terminating period. Anything else
# raises rather than being partly believed.
DECLARED_LABEL_CLAUSE = re.compile(r"^`[^`]+`(?:, *`[^`]+`)*\.?$")

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


def _reject_runs_on_that_continues_off_this_line(value: str, job: str) -> None:
    """Raise unless `value` is the *whole* `runs-on:` value, readable on this line.

    Both callers see one line at a time and both then decide something about it:
    :func:`_parse_runs_on` splits it into labels, and :func:`persistent_runner_labels`
    runs a `self-hosted` substring pre-filter over it. A value that continues onto
    the following lines makes those decisions on a *fragment*, and a fragment
    answers "no" to every question -- which is why this is a rejection and not a
    best-effort parse.

    Refused, once, for every caller:

    * an empty inline value -- the block sequence form (`runs-on:` with
      `- self-hosted` on the following lines);
    * an unbalanced flow collection -- `runs-on: [` or `runs-on: [self-hosted,`
      with the closing `]` on a later line. This is ordinary valid YAML and its
      inline value is *non-empty*, so the empty-value check cannot see it: the
      pre-filter finds no `self-hosted` in `[`, skips the declaration, and the
      other declarations keep `declarations` non-zero, so the derived vocabulary
      is silently partial and nothing raises. Same hole as the block form, one
      YAML shape over;
    * a folded or literal block scalar (`>`, `|`) -- the value is on the next line;
    * a flow mapping, an anchor or an alias (`{`, `&`, `*`) -- not a label list
      that can be resolved without following it somewhere else.

    Expressions (`${{ ... }}`) are deliberately *not* rejected here: they are a
    complete inline value, so the scan can honestly conclude they spell no literal
    `self-hosted` label. `_parse_runs_on` still refuses to classify a job by one.
    """
    if not value:
        raise UnmodelledWorkflowConstruct(
            f"Job {job!r} declares `runs-on:` with a block value. This guard models "
            "only a scalar (`runs-on: ubuntu-latest`) and a flow sequence "
            "(`runs-on: [self-hosted, ...]`). Extend the parser rather than "
            "leaving the job unchecked."
        )
    # Ordered: an anchored sequence (`&name [a, b]`) ends with `]` without
    # starting with `[`, so the balance check below would reject it for the wrong
    # stated reason. Both answers are a refusal -- this only keeps the message true.
    if value.startswith(("{", "&", "*", ">", "|")):
        raise UnmodelledWorkflowConstruct(
            f"Job {job!r} uses an unmodelled `runs-on:` form ({value!r})."
        )
    if value.startswith("[") != value.endswith("]"):
        raise UnmodelledWorkflowConstruct(
            f"Job {job!r} declares `runs-on:` as a multiline flow sequence ({value!r}); "
            "the labels continue on the following lines. This guard reads the value off "
            "the `runs-on:` line, so it would decide on a fragment -- and a fragment "
            "never contains `self-hosted`, which is precisely how a persistent-runner "
            "declaration goes missing without anything failing. Put the labels on one "
            "line (`runs-on: [self-hosted, ...]`), or extend the parser."
        )


def _parse_runs_on(raw: str, job: str) -> List[str]:
    value = _strip_comment(raw)
    _reject_runs_on_that_continues_off_this_line(value, job)
    if "${{" in value:
        raise UnmodelledWorkflowConstruct(
            f"Job {job!r} computes its `runs-on:` from an expression ({value!r}); this "
            "guard cannot tell whether it lands on the persistent self-hosted runner. "
            "Pin the labels literally, or extend this guard."
        )
    if value.startswith("[") and value.endswith("]"):
        labels = [label.strip().strip("\"'") for label in value[1:-1].split(",")]
        return [label for label in labels if label]
    # Anchors, aliases, flow mappings and block scalars were already refused by
    # `_reject_runs_on_that_continues_off_this_line`, so what is left is a scalar.
    return [value.strip("\"'")]


def persistent_runner_labels(paths: Optional[List[Path]] = None) -> frozenset:
    """Custom labels that are known to select a persistent self-hosted runner.

    Derived, never declared here: every `runs-on:` in `.github/workflows/` that
    names `self-hosted` contributes its remaining labels. That is what makes the
    subset rule in :func:`classify_runner` self-maintaining -- a new persistent
    runner label shows up in this vocabulary as soon as one job spells it out
    next to `self-hosted`, and until then any job using it alone is unmodelled
    (a failure) rather than assumed GitHub-hosted.

    Case-insensitive throughout. GitHub matches runner labels case-insensitively,
    so `runs-on: [Self-Hosted, Windows, X64, godotgs]` is a perfectly ordinary
    declaration. A case-*sensitive* pre-filter here would skip that line while the
    lowercase declarations elsewhere keep ``declarations`` non-zero -- the scan
    stays non-empty, nothing raises, and the labels that line pairs with
    `self-hosted` never reach the cross-check in
    :func:`build_runner_label_policy`. A label sitting on the persistent runner
    could then remain declared GitHub-hosted, which drops an unguarded job out of
    :func:`self_hosted_jobs` entirely.

    A `runs-on:` whose value continues onto the following lines is the same hole
    with a different cause, and it has two shapes, not one. The block sequence
    leaves the inline value *empty*. The multiline flow sequence (`runs-on: [`, or
    `runs-on: [self-hosted,` with the `]` further down) leaves it **non-empty** --
    so an empty-value check cannot see it, and the substring pre-filter decides on
    `[` and skips the declaration while every ordinary declaration keeps
    `declarations` non-zero. Both are refused by
    :func:`_reject_runs_on_that_continues_off_this_line`, called on every
    `runs-on:` line *before* the pre-filter, rather than contributing no labels to
    a vocabulary that then looks complete.
    """
    if paths is None:
        paths = sorted(WORKFLOW_DIR.glob("*.yml")) + sorted(WORKFLOW_DIR.glob("*.yaml"))

    labels = set()
    declarations = 0
    for path in paths:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = RUNS_ON_LINE.match(line)
            if not match:
                continue
            site = f"{path.name}:{number}"
            # Every `runs-on:` line, before anything is concluded from it: the
            # substring pre-filter below is only allowed to run against a value
            # that is entirely present on this line. A block sequence (empty
            # inline value), a flow collection still open at end of line
            # (`runs-on: [`, `runs-on: [self-hosted,`), a block scalar, an anchor
            # or an alias would all be filtered on a fragment, and a fragment
            # never contains `self-hosted` -- so the declaration would drop out of
            # the derived vocabulary with nothing raised, while the other
            # declarations keep `declarations` non-zero. That is exactly the
            # "the sweep found nothing" == "there is nothing to find" confusion
            # this module refuses to make. The scan must never be more permissive
            # than the parser it shares a corpus with, because the vocabulary it
            # derives *is* the trust boundary.
            _reject_runs_on_that_continues_off_this_line(
                _strip_comment(match.group(1)), site
            )
            # Normalize before detecting: routing is case-insensitive, so the
            # pre-filter must be too, or a valid mixed-case declaration is
            # silently dropped from the derived vocabulary.
            if SELF_HOSTED_LABEL not in match.group(1).lower():
                continue
            parsed = _parse_runs_on(match.group(1), site)
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


class RunnerLabelPolicy(NamedTuple):
    """The declared, cross-checked label inventory this guard classifies against.

    `persistent` never contains `self-hosted` itself -- that label is handled
    separately because it is the one label whose meaning is fixed by GitHub.
    """

    persistent: frozenset
    github_hosted: frozenset


def build_runner_label_policy(
    declared_persistent: Iterable[str],
    declared_github_hosted: Iterable[str],
    derived_persistent: Iterable[str],
) -> RunnerLabelPolicy:
    """Validate a declared policy against the labels derived from the workflows.

    Three things have to hold, or the guard refuses to classify anything:

    * the declaration is non-empty on both sides -- an empty hosted set would
      make every hosted job unmodelled, an empty persistent set would silently
      stop the subset rule from ever firing;
    * every label the workflows pair with `self-hosted` is declared persistent,
      so a newly added persistent-runner label fails loudly instead of being
      classified by omission;
    * the declared hosted labels are disjoint from the persistent inventory.
      This is the assumption the shape-based fallback got wrong: a hosted-looking
      label that is *also* on the self-hosted runner routes to the runner.
    """
    persistent = {str(label).lower() for label in declared_persistent} - {SELF_HOSTED_LABEL}
    hosted = {str(label).lower() for label in declared_github_hosted}
    derived = {str(label).lower() for label in derived_persistent} - {SELF_HOSTED_LABEL}

    if not persistent:
        raise UnmodelledWorkflowConstruct(
            "The declared persistent self-hosted runner label inventory is empty. Without it "
            "a job that reaches the runner through its custom labels alone cannot be "
            "recognized."
        )
    if not hosted:
        raise UnmodelledWorkflowConstruct(
            "The declared GitHub-hosted runner label set is empty, so no job could ever be "
            "classified GitHub-hosted. Declare the hosted labels this repository uses."
        )

    undeclared = sorted(derived - persistent)
    if undeclared:
        raise UnmodelledWorkflowConstruct(
            f"Label(s) {undeclared} appear next to `self-hosted` in .github/workflows/ but are "
            "not in the declared persistent-runner inventory in the Runner Trust Boundary "
            "section of .github/workflows/README.md. Declare them there (and confirm they are "
            "not also GitHub-hosted labels) so the trust boundary stays reviewable."
        )

    overlap = sorted(hosted & (persistent | {SELF_HOSTED_LABEL}))
    if overlap:
        raise UnmodelledWorkflowConstruct(
            f"Label(s) {overlap} are declared both as GitHub-hosted labels and as persistent "
            "self-hosted runner labels. A job carrying such a label may land on the persistent "
            "runner, so it cannot be treated as GitHub-hosted. The two declared sets must be "
            "disjoint."
        )

    return RunnerLabelPolicy(frozenset(persistent), frozenset(hosted))


def runner_label_policy(
    paths: Optional[List[Path]] = None,
    declared: Optional[Tuple[Iterable[str], Iterable[str]]] = None,
) -> RunnerLabelPolicy:
    """The repository's runner-label policy: declared in README, checked against the workflows."""
    if declared is None:
        declared = declared_runner_labels()
    return build_runner_label_policy(declared[0], declared[1], persistent_runner_labels(paths))


def classify_runner(
    labels: List[str], job: str, policy: Optional[RunnerLabelPolicy] = None
) -> str:
    """"self-hosted" / "github-hosted" for a job's `runs-on:` labels.

    Raises `UnmodelledWorkflowConstruct` for a label set that is neither, because
    a label GitHub might route to the persistent runner must not be filed under
    "safe" by default.
    """
    if policy is None:
        policy = runner_label_policy()

    lowered = {str(label).lower() for label in labels}
    if not lowered:
        raise UnmodelledWorkflowConstruct(
            f"Job {job!r} declares an empty `runs-on:` label set; which runner it selects "
            "is undecidable."
        )
    if SELF_HOSTED_LABEL in lowered:
        return "self-hosted"
    if lowered <= policy.persistent:
        # GitHub routes a job to any runner carrying all of the job's labels, so
        # these select the persistent runner even without the `self-hosted` label.
        return "self-hosted"
    if len(lowered) == 1 and lowered <= policy.github_hosted:
        # Single label only: a GitHub-hosted runner matches exactly one image
        # label, so a multi-label set can only be satisfied by a machine carrying
        # all of them -- which, for two image-shaped labels, means a self-hosted
        # one with custom labels.
        return "github-hosted"
    raise UnmodelledWorkflowConstruct(
        f"Job {job!r} selects its runner with labels {sorted(lowered)}, which are neither a "
        f"subset of the declared persistent-runner label inventory {sorted(policy.persistent)} "
        f"nor a single declared GitHub-hosted label {sorted(policy.github_hosted)}. GitHub "
        "routes a job to any runner carrying all of its labels, and a self-hosted runner may "
        "carry a label that merely looks like a hosted image, so this guard will not assume "
        "these route away from the persistent runner. Add the `self-hosted` label if they "
        "route to it, or declare the labels in the Runner Trust Boundary section of "
        ".github/workflows/README.md."
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
    policy: Optional[RunnerLabelPolicy] = None,
) -> Dict[str, Dict[str, object]]:
    if jobs is None:
        jobs = parse_jobs()
    if policy is None:
        policy = runner_label_policy()
    return {
        name: data
        for name, data in jobs.items()
        if classify_runner(data["runs_on"], name, policy) == "self-hosted"
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


def _declared_labels_from_section(section: str, marker: str) -> Set[str]:
    """The labels declared by the one bullet that *is* the ``marker`` declaration.

    Deliberately not "any bullet mentioning the marker, scraped for backticks".
    That accepted prose which does not declare a policy (a negative or historical
    sentence with the marker embedded mid-bullet) and, worse, read trailing prose
    as policy -- a counter-example such as ``windows-2022`` named after the list
    became a declared GitHub-hosted label, which is precisely the classification
    that lets a self-hosted job out of :func:`self_hosted_jobs`.

    So the bullet must *begin* with the marker and its remainder must be exactly a
    comma-separated list of backticked labels (:data:`DECLARED_LABEL_CLAUSE`).
    Anything ambiguous -- a second bullet that merely mentions the marker
    included -- raises.
    """
    mentioning = [
        line.strip()
        for line in section.splitlines()
        if line.lstrip().startswith("- ") and marker in line
    ]
    if len(mentioning) != 1:
        raise UnmodelledWorkflowConstruct(
            f"The Runner Trust Boundary section of {README.name} must contain exactly one "
            f"bullet mentioning {marker!r}, found {len(mentioning)}. That declaration is how "
            "this guard learns which labels reach the persistent runner; a second bullet "
            "mentioning it -- even as an aside -- makes the declaration ambiguous, so the "
            "guard refuses to classify any job."
        )

    bullet = mentioning[0]
    prefix = f"- {marker}"
    if not bullet.startswith(prefix):
        raise UnmodelledWorkflowConstruct(
            f"The {marker!r} bullet in {README.name} does not *begin* with that clause "
            f"({bullet!r}). A marker embedded mid-sentence can be part of a historical, "
            "negative or explanatory statement ('We no longer declare ...'), which is not a "
            f"policy declaration. Write the bullet as `- {marker} `label`, `label``."
        )

    clause = bullet[len(prefix) :].strip()
    if not DECLARED_LABEL_CLAUSE.match(clause):
        raise UnmodelledWorkflowConstruct(
            f"The {marker!r} bullet in {README.name} must be followed by a comma-separated "
            "list of backticked labels and nothing else (an optional trailing period is "
            f"allowed); found {clause!r}. Trailing prose is not ignorable here: every "
            "backticked token after the marker used to be read as declared policy, so a "
            "counter-example like `windows-2022` silently became a GitHub-hosted label. Put "
            "explanation in its own paragraph, outside this bullet."
        )

    labels = {name.strip().lower() for name in BACKTICKED.findall(clause)}
    labels.discard("")
    if not labels:
        raise UnmodelledWorkflowConstruct(
            f"The {marker!r} bullet in {README.name} lists no backticked labels."
        )
    return labels


def declared_runner_labels() -> Tuple[Set[str], Set[str]]:
    """(persistent self-hosted labels, GitHub-hosted labels) as declared in README."""
    section = readme_trust_section()
    return (
        _declared_labels_from_section(section, README_PERSISTENT_LABELS_MARKER),
        _declared_labels_from_section(section, README_GITHUB_HOSTED_LABELS_MARKER),
    )


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


def guard_form_violations(
    self_hosted: Dict[str, Dict[str, object]], declared_forms: Dict[str, str]
) -> List[str]:
    """Per-job disagreements between the workflow's guards and README's tags.

    Extracted from the test body so a synthetic workflow + README pair can drive
    the *production* comparison end to end. That matters here specifically: with
    every real self-hosted job on the same guard form, a comparison that only
    checked *set* membership ("README mentions `strict` somewhere") would agree
    with this one on the real repository and disagree only on a mixed-form
    workflow -- the one case the real files cannot produce.
    """
    violations: List[str] = []
    for name, data in sorted(self_hosted.items()):
        actual = classify_guard(data["if"])
        if name not in declared_forms:
            violations.append(
                f"self-hosted job {name!r} (guard form {actual!r}) carries no guard-form tag in "
                "the Runner Trust Boundary bullet."
            )
            continue
        if declared_forms[name] != actual:
            violations.append(
                f"the Runner Trust Boundary bullet documents job {name!r} as using the "
                f"{declared_forms[name]!r} guard form, but the workflow has if={data['if']!r} "
                f"({actual!r}). The documented trust boundary must describe the guard each job "
                "actually carries."
            )
    return violations


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
        violations = guard_form_violations(self.self_hosted, readme_declared_job_forms())
        self.assertEqual(violations, [], "\n".join(violations))

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

    POLICY = RunnerLabelPolicy(
        persistent=frozenset({"windows", "x64", "godotgs", "gpu"}),
        github_hosted=frozenset({"ubuntu-latest"}),
    )

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

    def test_a_mixed_case_self_hosted_declaration_is_detected(self) -> None:
        # GitHub matches labels case-insensitively, so `Self-Hosted` is a valid
        # spelling of the declaration and its companions belong in the vocabulary.
        corpus = self._corpus("jobs:\n  a:\n    runs-on: [Self-Hosted, Windows, Brand-New-Rig]\n")
        self.assertEqual(persistent_runner_labels([corpus]), frozenset({"windows", "brand-new-rig"}))

    def test_a_block_form_declaration_raises_instead_of_vanishing(self) -> None:
        # `runs-on:` with the labels on following lines: the inline value is
        # empty, so a substring pre-filter can never see `self-hosted` in it. The
        # scan must reject the shape the way `_parse_runs_on` does, not skip it.
        #
        # The message is asserted, not just the exception type: a corpus whose
        # only declaration is block-form also trips the "no declarations at all"
        # guard at the end of the sweep, so a bare `assertRaises` here would pass
        # against the unfixed scan -- the right exception for the wrong reason.
        corpus = self._corpus(
            "jobs:\n  a:\n    runs-on:\n      - self-hosted\n      - brand-new-rig\n"
        )
        with self.assertRaisesRegex(UnmodelledWorkflowConstruct, "block value"):
            persistent_runner_labels([corpus])

    def test_a_block_form_declaration_cannot_hide_behind_a_flow_form_one(self) -> None:
        # The dangerous arrangement: one ordinary flow-form declaration keeps the
        # scan non-empty, so `declarations` is non-zero and nothing raises at the
        # end -- while a second, block-form declaration contributes no labels at
        # all. The derived vocabulary would then be silently partial, and a label
        # that really does sit on the persistent runner could stay classified
        # GitHub-hosted, dropping an unguarded job out of `self_hosted_jobs`.
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        flow = root / "flow.yml"
        flow.write_text("jobs:\n  a:\n    runs-on: [self-hosted, Windows, godotgs]\n", encoding="utf-8")
        block = root / "block.yml"
        block.write_text(
            "jobs:\n  b:\n    runs-on:\n      - Self-Hosted\n      - brand-new-rig\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(UnmodelledWorkflowConstruct, "block value"):
            persistent_runner_labels([flow, block])

    def test_a_multiline_flow_declaration_cannot_hide_behind_an_ordinary_one(self) -> None:
        # The block form leaves the inline value EMPTY, so an empty-value check
        # catches it. The multiline flow form does not: `runs-on: [` is valid YAML
        # with a NON-empty inline value, so it walks straight past that check and
        # into the `self-hosted` substring pre-filter, which sees `[`, finds no
        # `self-hosted` in it, and skips the declaration. Meanwhile the ordinary
        # declaration next to it keeps `declarations` non-zero, so the sweep ends
        # without raising and `brand-new-rig` -- a label that really is on the
        # persistent runner -- never reaches the cross-check in
        # `build_runner_label_policy`. It could then stay declared GitHub-hosted,
        # dropping an unguarded `release_builds.yml` job out of `self_hosted_jobs`.
        #
        # Both spellings are exercised: `[` alone, and `[self-hosted,` -- the
        # second one gets *past* the pre-filter and is then dropped one step later
        # because the fragment `[self-hosted,` is not the label `self-hosted`.
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        ordinary = root / "flow.yml"
        ordinary.write_text(
            "jobs:\n  a:\n    runs-on: [self-hosted, Windows, godotgs]\n", encoding="utf-8"
        )
        for name, text in (
            ("open-bracket", "jobs:\n  b:\n    runs-on: [\n      self-hosted,\n      brand-new-rig,\n    ]\n"),
            ("first-label", "jobs:\n  b:\n    runs-on: [self-hosted,\n      brand-new-rig]\n"),
        ):
            with self.subTest(spelling=name):
                multiline = root / f"{name}.yml"
                multiline.write_text(text, encoding="utf-8")
                # The message is asserted, not just the type: a corpus can trip the
                # "no declarations at all" guard at the end of the sweep for an
                # entirely different reason, and that would be the right exception
                # for the wrong reason. `ordinary` rules that out here anyway, but
                # the assertion says which rejection is being proved.
                with self.assertRaisesRegex(
                    UnmodelledWorkflowConstruct, "multiline flow sequence"
                ):
                    persistent_runner_labels([ordinary, multiline])
                # And the shape must be rejected, not half-parsed: before this was
                # fixed the parser answered `['[']` / `['[self-hosted,']` -- bogus
                # labels invented from a fragment.
                with self.assertRaisesRegex(
                    UnmodelledWorkflowConstruct, "multiline flow sequence"
                ):
                    _parse_runs_on(text.splitlines()[2].split("runs-on:", 1)[1], "b")

    def test_a_runs_on_whose_value_is_elsewhere_is_rejected_not_skipped(self) -> None:
        # The same fail-open shape reached through the other YAML constructs whose
        # value is not on the `runs-on:` line. Each has a non-empty inline value
        # that does not contain `self-hosted`, so each was silently skipped by the
        # pre-filter while the ordinary declaration kept the sweep non-empty.
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        ordinary = root / "flow.yml"
        ordinary.write_text(
            "jobs:\n  a:\n    runs-on: [self-hosted, Windows, godotgs]\n", encoding="utf-8"
        )
        for name, value in (
            ("alias", "*persistent_runner"),
            ("anchor", "&persistent_runner [self-hosted, brand-new-rig]"),
            ("folded-scalar", ">-"),
            ("literal-scalar", "|"),
            ("flow-mapping", "{"),
        ):
            with self.subTest(form=name):
                path = root / f"{name}.yml"
                path.write_text(f"jobs:\n  b:\n    runs-on: {value}\n", encoding="utf-8")
                with self.assertRaisesRegex(
                    UnmodelledWorkflowConstruct, "unmodelled `runs-on:` form"
                ):
                    persistent_runner_labels([ordinary, path])

    def test_an_expression_runs_on_is_still_tolerated_by_the_sweep(self) -> None:
        # The counterweight to the two tests above: `${{ ... }}` IS a complete
        # inline value, so the sweep can honestly conclude it declares no literal
        # `self-hosted` label and move on. Tightening the sweep must not turn every
        # upstream matrix workflow into a hard failure of this guard -- only
        # `_parse_runs_on`, reached when such a job is actually being classified,
        # refuses it.
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        ordinary = root / "flow.yml"
        ordinary.write_text(
            "jobs:\n  a:\n    runs-on: [self-hosted, Windows, godotgs]\n", encoding="utf-8"
        )
        computed = root / "matrix.yml"
        computed.write_text(
            "jobs:\n  b:\n    runs-on: ${{ matrix.runner }}\n", encoding="utf-8"
        )
        self.assertEqual(
            persistent_runner_labels([ordinary, computed]),
            frozenset({"windows", "godotgs"}),
        )
        with self.assertRaisesRegex(UnmodelledWorkflowConstruct, "expression"):
            _parse_runs_on(" ${{ matrix.runner }}", "b")

    def test_custom_labels_without_the_self_hosted_label_are_self_hosted(self) -> None:
        # The hole this class exists for: GitHub routes `[Windows, X64, godotgs]`
        # to the persistent runner, `self-hosted` label or not.
        self.assertEqual(
            classify_runner(["Windows", "X64", "godotgs"], "sneaky", self.POLICY),
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
        self.assertEqual(sorted(self_hosted_jobs(jobs, self.POLICY)), ["sneaky"])
        self.assertIsNone(classify_guard(jobs["sneaky"]["if"]))

    def test_the_self_hosted_label_alone_is_still_self_hosted(self) -> None:
        self.assertEqual(classify_runner(["self-hosted"], "a", self.POLICY), "self-hosted")

    def test_a_single_declared_github_hosted_label_is_hosted(self) -> None:
        self.assertEqual(classify_runner(["ubuntu-latest"], "a", self.POLICY), "github-hosted")

    def test_an_undeclared_image_shaped_label_raises(self) -> None:
        # Round 5/6 classified all of these "github-hosted" purely because they
        # match `^(ubuntu|windows|macos)-...`. Nothing stops a self-hosted runner
        # from carrying exactly such a custom label -- `windows-2022` is the
        # obvious one for a self-hosted Windows Server 2022 box -- so the shape of
        # the string is not evidence about where the job lands. Only the declared
        # inventory in README is.
        for label in ("ubuntu-24.04", "windows-2022", "macos-14", "ubuntu-24.04-arm"):
            with self.subTest(label=label):
                with self.assertRaises(UnmodelledWorkflowConstruct):
                    classify_runner([label], "a", self.POLICY)

    def test_a_multi_image_label_set_raises(self) -> None:
        # The round-6 hole. GitHub routes an array to a runner matching EVERY
        # label, so no GitHub-hosted runner can satisfy two image labels at once;
        # the only machine that can is a self-hosted one carrying both as custom
        # labels. Round 6 answered "github-hosted", which dropped such a job out
        # of `self_hosted_jobs()` -- no fork-guard check, no README check.
        for labels in (
            ["ubuntu-latest", "windows-2022"],
            ["ubuntu-latest", "ubuntu-24.04"],
            ["ubuntu-latest", "macos-14"],
        ):
            with self.subTest(labels=labels):
                with self.assertRaises(UnmodelledWorkflowConstruct):
                    classify_runner(labels, "a", self.POLICY)

    def test_a_multi_image_label_job_cannot_slip_past_the_sweep(self) -> None:
        # End to end: a job with no fork guard and no README entry must make the
        # sweep raise rather than quietly omit it.
        lines = [
            "jobs:",
            "  sneaky:",
            "    runs-on: [ubuntu-latest, windows-2022]",
            "  real:",
            "    runs-on: [self-hosted, Windows, X64, godotgs]",
            "    if: github.event_name != 'pull_request'",
        ]
        jobs = parse_jobs(lines)
        self.assertIsNone(classify_guard(jobs["sneaky"]["if"]))
        with self.assertRaises(UnmodelledWorkflowConstruct):
            self_hosted_jobs(jobs, self.POLICY)

    def test_an_image_shaped_custom_label_job_cannot_slip_past_the_sweep(self) -> None:
        lines = [
            "jobs:",
            "  sneaky:",
            "    runs-on: windows-2022",
            "  real:",
            "    runs-on: [self-hosted, Windows, X64, godotgs]",
            "    if: github.event_name != 'pull_request'",
        ]
        jobs = parse_jobs(lines)
        self.assertIsNone(classify_guard(jobs["sneaky"]["if"]))
        with self.assertRaises(UnmodelledWorkflowConstruct):
            self_hosted_jobs(jobs, self.POLICY)

    def test_an_unknown_label_raises_instead_of_being_called_hosted(self) -> None:
        # Fail closed: a label never seen next to `self-hosted` and not declared
        # GitHub-hosted might route anywhere, and "might" is not "no".
        for labels in (["mystery-rig"], ["godotgs", "mystery-rig"], ["Windows", "arm64"]):
            with self.subTest(labels=labels):
                with self.assertRaises(UnmodelledWorkflowConstruct):
                    classify_runner(labels, "a", self.POLICY)

    def test_a_declared_hosted_label_combined_with_anything_raises(self) -> None:
        # `[ubuntu-latest, godotgs]` needs a machine carrying both; the persistent
        # runner carries `godotgs`, so this must not be waved through as hosted.
        for labels in (["ubuntu-latest", "godotgs"], ["ubuntu-latest", "mystery-rig"]):
            with self.subTest(labels=labels):
                with self.assertRaises(UnmodelledWorkflowConstruct):
                    classify_runner(labels, "a", self.POLICY)

    def test_an_empty_label_set_raises(self) -> None:
        with self.assertRaises(UnmodelledWorkflowConstruct):
            classify_runner([], "a", self.POLICY)


class RunnerLabelPolicyTests(unittest.TestCase):
    """The declared policy is only useful if it is cross-checked against the workflows."""

    DECLARED_PERSISTENT = ["self-hosted", "Windows", "X64", "godotgs", "gpu"]
    DECLARED_HOSTED = ["ubuntu-latest"]

    def _corpus(self, text: str) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "w.yml"
        path.write_text(text, encoding="utf-8")
        return path

    def test_the_real_readme_declares_a_policy_that_covers_the_real_workflows(self) -> None:
        policy = runner_label_policy()
        self.assertIn("godotgs", policy.persistent)
        self.assertNotIn(SELF_HOSTED_LABEL, policy.persistent)
        self.assertIn("ubuntu-latest", policy.github_hosted)
        self.assertFalse(
            policy.persistent & policy.github_hosted,
            "the declared hosted labels overlap the persistent-runner inventory",
        )

    def test_the_real_readme_policy_classifies_the_real_workflow(self) -> None:
        # Not vacuous: the declared policy has to actually decide every job in
        # release_builds.yml, in both directions.
        jobs = parse_jobs()
        self_hosted = self_hosted_jobs(jobs)
        self.assertTrue(self_hosted)
        self.assertTrue(set(jobs) - set(self_hosted))

    def test_the_real_policy_routes_a_custom_label_only_job_to_the_runner(self) -> None:
        # Anchored to the *shipped* README, not to a synthetic policy: the review
        # question is whether THIS repository's declared labels catch a job that
        # reaches the persistent runner without ever writing `self-hosted`.
        policy = runner_label_policy()
        self.assertEqual(
            classify_runner(["Windows", "X64", "godotgs"], "sneaky", policy), "self-hosted"
        )
        lines = [
            "jobs:",
            "  sneaky:",
            "    runs-on: [Windows, X64, godotgs]",
            "  hosted:",
            "    runs-on: ubuntu-latest",
        ]
        jobs = parse_jobs(lines)
        self.assertEqual(sorted(self_hosted_jobs(jobs, policy)), ["sneaky"])
        # ... and it is unguarded, so the fork-guard check has something to fail on.
        self.assertIsNone(classify_guard(jobs["sneaky"]["if"]))

    def test_a_persistent_label_missing_from_the_declaration_raises(self) -> None:
        # A new label bolted onto the runner shows up in the derived vocabulary
        # first; until a human declares it, the guard refuses to classify.
        corpus = self._corpus("jobs:\n  a:\n    runs-on: [self-hosted, Windows, brand-new-rig]\n")
        with self.assertRaises(UnmodelledWorkflowConstruct):
            runner_label_policy([corpus])

    def test_declaring_the_new_label_makes_the_policy_valid_again(self) -> None:
        corpus = self._corpus("jobs:\n  a:\n    runs-on: [self-hosted, Windows, brand-new-rig]\n")
        policy = runner_label_policy(
            [corpus], (self.DECLARED_PERSISTENT + ["brand-new-rig"], self.DECLARED_HOSTED)
        )
        self.assertIn("brand-new-rig", policy.persistent)

    def test_a_mixed_case_declaration_cannot_hide_behind_a_lowercase_one(self) -> None:
        # The fail-open this test exists for: a case-SENSITIVE pre-filter skips
        # the `Self-Hosted` line, while the lowercase declaration in the other
        # file keeps the scan non-empty so nothing raises. `ubuntu-latest` sits on
        # the persistent runner in that second file, yet stays declared
        # GitHub-hosted -- and an unguarded `runs-on: ubuntu-latest` job then
        # drops out of `self_hosted_jobs()` with no fork-guard and no README
        # check. Normalizing first turns that into a hard failure.
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        lower = Path(directory.name) / "a.yml"
        lower.write_text(
            "jobs:\n  real:\n    runs-on: [self-hosted, Windows, X64, godotgs]\n", encoding="utf-8"
        )
        mixed = Path(directory.name) / "b.yml"
        mixed.write_text(
            "jobs:\n  other:\n    runs-on: [Self-Hosted, Windows, X64, godotgs, ubuntu-latest]\n",
            encoding="utf-8",
        )
        # Not vacuous: the lowercase file alone declares a valid policy, so the
        # failure below is caused by the mixed-case line and nothing else.
        self.assertIn(
            "godotgs",
            runner_label_policy([lower], (self.DECLARED_PERSISTENT, self.DECLARED_HOSTED)).persistent,
        )
        with self.assertRaises(UnmodelledWorkflowConstruct):
            runner_label_policy([lower, mixed], (self.DECLARED_PERSISTENT, self.DECLARED_HOSTED))

    def test_a_hosted_label_that_is_also_a_persistent_label_raises(self) -> None:
        # The disjointness the shape-based fallback assumed without evidence.
        with self.assertRaises(UnmodelledWorkflowConstruct):
            build_runner_label_policy(self.DECLARED_PERSISTENT, ["godotgs"], ["godotgs"])
        with self.assertRaises(UnmodelledWorkflowConstruct):
            build_runner_label_policy(self.DECLARED_PERSISTENT, ["self-hosted"], ["godotgs"])

    def test_an_empty_declared_set_raises(self) -> None:
        with self.assertRaises(UnmodelledWorkflowConstruct):
            build_runner_label_policy([], self.DECLARED_HOSTED, [])
        with self.assertRaises(UnmodelledWorkflowConstruct):
            build_runner_label_policy(self.DECLARED_PERSISTENT, [], ["godotgs"])

    def test_the_readme_policy_bullets_are_parsed_not_guessed(self) -> None:
        persistent, hosted = declared_runner_labels()
        self.assertIn("godotgs", persistent)
        self.assertIn("ubuntu-latest", hosted)
        self.assertNotIn("ubuntu-latest", persistent)

    def test_a_missing_policy_bullet_raises(self) -> None:
        section = "## Runner Trust Boundary\n\n- Nothing declared here.\n"
        with self.assertRaises(UnmodelledWorkflowConstruct):
            _declared_labels_from_section(section, README_PERSISTENT_LABELS_MARKER)

    def test_a_duplicated_policy_bullet_raises(self) -> None:
        # Two declarations are two policies; the guard will not pick one.
        section = (
            "## Runner Trust Boundary\n"
            f"- {README_GITHUB_HOSTED_LABELS_MARKER} `ubuntu-latest`\n"
            f"- {README_GITHUB_HOSTED_LABELS_MARKER} `windows-2022`\n"
        )
        with self.assertRaises(UnmodelledWorkflowConstruct):
            _declared_labels_from_section(section, README_GITHUB_HOSTED_LABELS_MARKER)

    def test_a_policy_bullet_with_no_labels_raises(self) -> None:
        section = f"## Runner Trust Boundary\n- {README_PERSISTENT_LABELS_MARKER} none.\n"
        with self.assertRaises(UnmodelledWorkflowConstruct):
            _declared_labels_from_section(section, README_PERSISTENT_LABELS_MARKER)

    def _hosted_section(self, bullet: str) -> str:
        return f"## Runner Trust Boundary\n{bullet}\n"

    def test_a_well_formed_declaration_is_still_accepted(self) -> None:
        # Guard the guard: the strictness below must not have made every bullet
        # fail, which would turn the tests after it into vacuous assertRaises.
        for bullet in (
            f"- {README_GITHUB_HOSTED_LABELS_MARKER} `ubuntu-latest`",
            f"- {README_GITHUB_HOSTED_LABELS_MARKER} `ubuntu-latest`.",
            f"- {README_GITHUB_HOSTED_LABELS_MARKER} `ubuntu-latest`, `ubuntu-24.04`.",
        ):
            with self.subTest(bullet=bullet):
                self.assertIn(
                    "ubuntu-latest",
                    _declared_labels_from_section(
                        self._hosted_section(bullet), README_GITHUB_HOSTED_LABELS_MARKER
                    ),
                )

    def test_a_negative_or_historical_bullet_is_not_a_declaration(self) -> None:
        # The marker embedded mid-sentence says the opposite of what the guard
        # would read off it. An inverted claim must not satisfy the requirement.
        for bullet in (
            f"- We no longer declare {README_GITHUB_HOSTED_LABELS_MARKER} `ubuntu-latest`.",
            f"- Historically the {README_GITHUB_HOSTED_LABELS_MARKER} `ubuntu-latest`.",
        ):
            with self.subTest(bullet=bullet):
                with self.assertRaises(UnmodelledWorkflowConstruct):
                    _declared_labels_from_section(
                        self._hosted_section(bullet), README_GITHUB_HOSTED_LABELS_MARKER
                    )

    def test_trailing_prose_after_the_declared_list_raises(self) -> None:
        # Every backticked token after the marker used to be read as policy, so a
        # counter-example became a declared label.
        bullet = (
            f"- {README_GITHUB_HOSTED_LABELS_MARKER} `ubuntu-latest`. For example "
            "`windows-2022` is a self-hosted label, not one of these."
        )
        with self.assertRaises(UnmodelledWorkflowConstruct):
            _declared_labels_from_section(
                self._hosted_section(bullet), README_GITHUB_HOSTED_LABELS_MARKER
            )

    def test_trailing_prose_would_otherwise_reach_classify_runner(self) -> None:
        # Why the clause above matters: the scraped counter-example does not stay
        # inert, it becomes a routable "GitHub-hosted" label, and a self-hosted
        # job carrying it then leaves the sweep unguarded and undocumented.
        hosted = {"ubuntu-latest", "windows-2022"}
        policy = build_runner_label_policy(["Windows", "X64", "godotgs"], hosted, ["godotgs"])
        self.assertEqual(classify_runner(["windows-2022"], "sneaky", policy), "github-hosted")

    def test_an_extra_bullet_mentioning_the_marker_raises(self) -> None:
        # Ambiguity is not resolved in favour of the "real" one; two mentions
        # means the guard cannot tell which is the policy.
        section = (
            "## Runner Trust Boundary\n"
            f"- {README_GITHUB_HOSTED_LABELS_MARKER} `ubuntu-latest`\n"
            f"- Note: `windows-2022` is not among the {README_GITHUB_HOSTED_LABELS_MARKER} above.\n"
        )
        with self.assertRaises(UnmodelledWorkflowConstruct):
            _declared_labels_from_section(section, README_GITHUB_HOSTED_LABELS_MARKER)

    def test_the_real_readme_bullets_satisfy_the_strict_clause(self) -> None:
        # The strictness is only credible if the shipped README already meets it.
        section = readme_trust_section()
        self.assertIn(
            "godotgs", _declared_labels_from_section(section, README_PERSISTENT_LABELS_MARKER)
        )
        self.assertEqual(
            _declared_labels_from_section(section, README_GITHUB_HOSTED_LABELS_MARKER),
            {"ubuntu-latest"},
        )


class GuardFormCrossCheckTests(unittest.TestCase):
    """End-to-end on a *mixed-form* workflow, which the real files cannot produce.

    Both real self-hosted jobs carry the `strict` form and both README tags say
    `strict`, so on the real repository a set-membership check
    (``{forms in README} == {forms in workflow}``) and a per-job check agree.
    Rewriting the production comparison back to set membership would therefore
    stay green against `release_builds.yml`. These cases build a synthetic
    workflow with one `strict` job and one `standard` job and swap the README
    tags between them: the multiset of forms is identical, so only a per-job
    comparison can fail.
    """

    POLICY = RunnerLabelPolicy(
        persistent=frozenset({"windows", "x64", "godotgs", "gpu"}),
        github_hosted=frozenset({"ubuntu-latest"}),
    )
    LINES = [
        "jobs:",
        "  build_strict:",
        "    runs-on: [self-hosted, Windows, X64, godotgs]",
        f"    if: {STRICT_NO_PULL_REQUEST_GUARD}",
        "  build_standard:",
        "    runs-on: [self-hosted, Windows, X64, godotgs]",
        f"    if: ${{{{ {STANDARD_FORK_GUARD} }}}}",
    ]
    MATCHING_BULLET = (
        "- `release_builds.yml` — self-hosted jobs `build_standard` (standard), "
        "`build_strict` (strict). They run on `push` only."
    )
    SWAPPED_BULLET = (
        "- `release_builds.yml` — self-hosted jobs `build_standard` (strict), "
        "`build_strict` (standard). They run on `push` only."
    )

    def _self_hosted(self):
        return self_hosted_jobs(parse_jobs(self.LINES), self.POLICY)

    def test_the_synthetic_workflow_really_is_mixed_form(self) -> None:
        # Guard the guard: if both synthetic jobs ended up on the same form, the
        # swap below would be a no-op and the discrimination test would be
        # vacuous.
        forms = {name: classify_guard(d["if"]) for name, d in self._self_hosted().items()}
        self.assertEqual(forms, {"build_strict": "strict", "build_standard": "standard"})

    def test_matching_annotations_pass(self) -> None:
        self.assertEqual(
            guard_form_violations(
                self._self_hosted(), extract_declared_job_forms(self.MATCHING_BULLET)
            ),
            [],
        )

    def test_swapped_annotations_fail(self) -> None:
        declared = extract_declared_job_forms(self.SWAPPED_BULLET)
        # The multiset of forms is unchanged by the swap, so a set-membership
        # comparison cannot see this.
        self.assertEqual(
            sorted(declared.values()),
            sorted(
                classify_guard(d["if"]) for d in self._self_hosted().values()
            ),
        )
        violations = guard_form_violations(self._self_hosted(), declared)
        self.assertEqual(len(violations), 2, violations)
        self.assertTrue(any("build_strict" in v for v in violations), violations)
        self.assertTrue(any("build_standard" in v for v in violations), violations)

    def test_an_undocumented_self_hosted_job_is_a_violation(self) -> None:
        bullet = "- `release_builds.yml` — self-hosted jobs `build_strict` (strict). On `push`."
        violations = guard_form_violations(self._self_hosted(), extract_declared_job_forms(bullet))
        self.assertEqual(len(violations), 1, violations)
        self.assertIn("build_standard", violations[0])


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
