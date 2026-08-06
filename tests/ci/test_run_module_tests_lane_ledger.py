#!/usr/bin/env python3
"""Pins the per-lane result ledger in run_module_tests.py (#705, slice 1).

Three properties are asserted here, and the order matters:

1. **EXIT-CODE PARITY.** The ledger observes; it never decides. For every
   outcome class the runner's exit code must be what the baseline produced.
   That is asserted twice over, deliberately:
     - against a pinned constant, documented per class with *why* the baseline
       returns it (a pinned constant alone only proves the code still does what
       it does), and
     - against the SAME scenario re-run with the ledger neutered, which is the
       property-shaped form: whatever the decision is, adding the ledger did not
       change it. A future path that lets the ledger gate a lane outcome fails
       here even if someone also "fixes" the pinned constant.

2. **TOTALITY.** Every lane the runner attempts produces exactly one record, in
   every outcome path. A lane that is absent from the ledger is the same class
   of defect as the bug being fixed - "did not run" reading as "passed" - so the
   ledger is pre-seeded and a lane can only ever be visibly NOT-RUN, never gone.
   Completeness is asserted mechanically against the lane declaration tables
   themselves - `MODULE_TEST_FILTERS`, and `REQUIRES_RD_TEST_FILTERS` whenever
   `run_gpu` says those lanes are built - not against a hand-written list of lane
   names, because a hand-maintained list of the things a guard must cover is an
   invariant that is already broken.

3. **UNKNOWN != ZERO.** A lane that crashed before printing a doctest summary
   records -1, never 0. `_parse_doctest_results()` returns 0 for a missing
   summary, and propagating that would make a crash indistinguishable from a
   lane that ran and passed nothing.

Every test method drives `_run_doctest_lanes()` and asserts on the emitted
records, so stubbing the lane loop out makes the whole file fail rather than
pass vacuously. The one exception is the CLI-argument test, which by
construction never reaches the lane loop; it is mutation-proven separately by
removing the `parser.error()` it pins.

`CI` is patched explicitly in every scenario rather than inherited, because
`_enforce_skipped_marker_policy` and `_tolerate_quarantined_lane` sit behind
`_is_ci()`: a file that behaves differently when CI happens to be set is a file
whose local green says nothing about CI.
"""

from __future__ import annotations

import contextlib
import errno
import importlib.util
import inspect
import io
import json
import math
import os
import re
import stat
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Iterable
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "run_module_tests.py"


def _load_harness():
    spec = importlib.util.spec_from_file_location("run_module_tests", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["run_module_tests"] = module
    spec.loader.exec_module(module)
    return module


harness = _load_harness()


# --------------------------------------------------------------------------
# Baseline exit codes, per outcome class, read off e9ddb27c285's
# _run_doctest_lanes() and unchanged by the two stacked branches beneath this
# one. Each constant carries the control-flow reason it holds, so a future
# change that alters a DECISION has to argue with the reason rather than just
# retype the number.
# --------------------------------------------------------------------------
# strict lane, exit 0, real coverage -> falls through to `return 0`.
BASELINE_RC_PASS = 0
# advisory lane, nonzero exit -> _report_failed_lane() returns True -> `continue`.
BASELINE_RC_ADVISORY_FAIL = 0
# advisory lane, exit 0, zero executed coverage -> _handle_no_executed_coverage()
# returns stats (not None) for strict=False -> loop continues.
BASELINE_RC_ADVISORY_NO_COVERAGE = 0
# strict lane, nonzero exit -> _report_failed_lane() returns False -> `return 1`.
BASELINE_RC_STRICT_FAIL = 1
# exit 0 with no doctest summary -> _validate_successful_lane() returns None ->
# `return 1`. This is the ONE advisory outcome that still gates at baseline.
BASELINE_RC_NO_SUMMARY = 1
# tests-unavailable binary under warn-only -> _report_unavailable_lane() returns
# True -> lanes_unavailable += 1, `continue`.
BASELINE_RC_UNAVAILABLE_WARN = 0
# ... and under strict mode without the opt-out -> returns False -> `return 1`.
BASELINE_RC_UNAVAILABLE_STRICT = 1
# quarantined lane whose failing cases all match an approved pattern ->
# _tolerate_quarantined_lane() returns None -> `continue`.
BASELINE_RC_QUARANTINE_TOLERATED = 0
# quarantined lane that PASSED -> QUARANTINE-STALE -> `return 1`.
BASELINE_RC_QUARANTINE_REJECTED = 1

FAILING_CASE = "[GaussianSplatting][Animation] plays a clip"

LANE_RESULT_RE = re.compile(
    r"^\[module-tests\]\[lane-result\] lane=(?P<lane>.+?) strict=(?P<strict>[01]) "
    r"outcome=(?P<outcome>\S+) passed_tests=(?P<passed_tests>-?\d+) "
    r"passed_assertions=(?P<passed_assertions>-?\d+) failed_tests=(?P<failed_tests>-?\d+) "
    r"failed_assertions=(?P<failed_assertions>-?\d+) skipped_markers=(?P<skipped_markers>-?\d+) "
    r"exit_code=(?P<exit_code>-?\d+) "
    r"exit_code_reported=(?P<exit_code_reported>[01]) "
    r"summary_reported=(?P<summary_reported>[01]) "
    r"zero_coverage=(?P<zero_coverage>-?[01])$"
)


# --------------------------------------------------------------------------
# doctest output fixtures, in the framing a real ConsoleReporter emits.
# --------------------------------------------------------------------------
# Every VALID shape below is anchored to a verbatim capture of a real headless
# run, `tests/ci/fixtures/doctest_env_skip_sample.txt` (#817). It is not
# decoration: the hand-authored summaries this file used until #822 round 6 were
# a shape doctest has never emitted. Real output is
#
#     [doctest] test cases:  9 |  9 passed | 0 failed | 2063 skipped
#     [doctest] assertions: 69 | 69 passed | 0 failed |
#
# - column-padded, with a fourth `| N skipped` field on the test-cases line, and
# a TRAILING `|` on the assertions line. The manufactured version had none of
# those. `_parse_doctest_results()` happens to tolerate both, so every outcome
# test in this file was green while its inputs were fiction: the suite could not
# have detected a wrong assumption about the producer's format, which is the
# definition of a self-certifying fixture (docs/governance/evidence-integrity.md).
CAPTURED_DOCTEST_SAMPLE = ROOT / "tests" / "ci" / "fixtures" / "doctest_env_skip_sample.txt"
# Quoted in failures so a missing capture says how to regenerate it rather than
# tempting the next person to type one out.
CAPTURE_COMMAND = (
    'bin/godot.windows.editor.dev.x86_64.console.exe --headless --test '
    '"--test-case=*Painterly*"'
)


def _captured_sample_text() -> str:
    """The capture, verbatim. Fails closed: a missing capture is not a skip."""
    if not CAPTURED_DOCTEST_SAMPLE.is_file():
        raise RuntimeError(
            f"missing captured doctest sample {CAPTURED_DOCTEST_SAMPLE}. Regenerate "
            f"with: {CAPTURE_COMMAND}. Refusing to fall back to a hand-authored "
            f"summary - that is the defect this anchoring removed."
        )
    return CAPTURED_DOCTEST_SAMPLE.read_text(encoding="utf-8")


CAPTURED_SAMPLE = _captured_sample_text()
# Counted from the capture by reading it; asserted exactly (never "> 0") in
# CapturedFixtureContractTests below, so a doctest format change breaks THERE
# instead of silently invalidating every outcome test in this file.
CAPTURED_PASSED_TESTS = 9
CAPTURED_FAILED_TESTS = 0
CAPTURED_PASSED_ASSERTIONS = 69
CAPTURED_FAILED_ASSERTIONS = 0
CAPTURED_SKIP_MARKERS = 3
# doctest's `N skipped` column = numTestCases - numTestCasesPassingFilters, i.e.
# cases the filter excluded. Read from the capture, not chosen.
CAPTURED_FILTERED_OUT = 2063
# The binary's REGISTERED case count, which is what the skipped column is
# computed against (ConsoleReporter::test_run_end,
# `const int numSkipped = p.numTestCases - p.numTestCasesPassingFilters;`).
# It does not move with the filter; the SELECTED count does, and the skipped
# column moves with it. Every summary this file renders claims to come from a
# binary of this size, so `filtered_out` is derived from it rather than pinned:
# until #822 round 11 `_summary()` defaulted `filtered_out` to the capture's
# 2063 for every fixture, so `_summary(5, 0, 120, 0)` announced 5 selected +
# 2063 skipped = a 2068-case binary in the same file that asserts the binary has
# 2072. `_parse_doctest_results()` ignores the column, so the fixtures stayed
# green while emitting a line the real producer cannot emit - a fixture certified
# by nothing but itself.
CAPTURED_TOTAL_CASES = (
    CAPTURED_PASSED_TESTS + CAPTURED_FAILED_TESTS + CAPTURED_FILTERED_OUT
)


def _captured_line(prefix: str) -> str:
    matches = [
        line for line in CAPTURED_SAMPLE.splitlines() if line.startswith(prefix)
    ]
    if len(matches) != 1:
        raise RuntimeError(
            f"expected exactly one {prefix!r} line in {CAPTURED_DOCTEST_SAMPLE.name}, "
            f"found {len(matches)}. Recapture with: {CAPTURE_COMMAND}"
        )
    return matches[0]


def _captured_repeated_line(prefix: str) -> str:
    """A line the capture repeats identically (e.g. the test-case separator)."""
    matches = {line for line in CAPTURED_SAMPLE.splitlines() if line.startswith(prefix)}
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one distinct {prefix!r} line in {CAPTURED_DOCTEST_SAMPLE.name}, "
            f"found {len(matches)}. Recapture with: {CAPTURE_COMMAND}"
        )
    return matches.pop()


CAPTURED_TEST_CASES_LINE = _captured_line("[doctest] test cases:")
CAPTURED_ASSERTIONS_LINE = _captured_line("[doctest] assertions:")
# The producer itself. The capture is a SNAPSHOT of its output; without a link
# back to the source, nothing notices when the producer changes underneath the
# snapshot (see ProducerContractTests and the declared limitation there).
VENDORED_DOCTEST_HEADER = ROOT / "thirdparty" / "doctest" / "doctest.h"
CAPTURED_DOCTEST_VERSION = "2.4.12"
CAPTURED_SEPARATOR_LINE = _captured_repeated_line("=======")
# The real MESSAGE framing, taken from the capture rather than retyped: doctest's
# log_message() calls file_line_to_stream() first, so a marker can never start a
# line. The previous hand-written version of this file asserted that framing in a
# comment while the rest of the file invented everything around it.
CAPTURED_SKIP_MARKER_LINE = _captured_line(
    "modules\\gaussian_splatting\\tests\\test_painterly_material.cpp(209)"
)


def _doctest_column_width(test_cases: int, assertions: int) -> int:
    """doctest's own column-width rule, mirrored.

    ConsoleReporter::test_run_end (thirdparty/doctest/doctest.h) computes

        width = int(ceil(log10(double(max(<test-case count>, <assertion count>)) + 1)))

    for each of the three columns and applies it with std::setw to BOTH summary
    lines. Round 6 re-spelled only the digits of the captured line, which kept the
    capture's two-character columns for every count - so `_summary(5, 0, 120, 0)`
    claimed two-wide columns where the producer emits three. That was the same
    self-certifying defect one level down, and it was self-sealing:
    `_parse_doctest_results()` accepts either spacing, and the skeleton test
    compared the generated line back against the same stale padding.
    """
    return math.ceil(math.log10(max(test_cases, assertions) + 1))


def _summary(
    passed_tests: int,
    failed_tests: int,
    passed_asserts: int,
    failed_asserts: int,
    filtered_out: int | None = None,
) -> str:
    """Render a doctest summary the way doctest renders one.

    This is a reimplementation of the producer's formatter, not a mutation of a
    captured string - and it is only trustworthy because
    `test_renderer_reproduces_the_capture_byte_for_byte` proves it reproduces the
    real capture exactly from the capture's own counts. Every literal and every
    column width below is therefore backed by real output.

    KNOWN GAP, stated rather than papered over: the repository's only doctest
    capture is a PASSING run, so no captured sample exists for a summary that
    reports failures or for a zero-assertion lane. Those shapes are rendered by
    this function; the framing and the widths are the producer's rule, the counts
    are the test's. Capturing a failing run would be strictly better and needs a
    lane sweep this change was told not to do.

    `filtered_out` DERIVES from the selected count by default (#822 round 11).
    doctest computes the column as `numTestCases - numTestCasesPassingFilters`,
    so for a given binary it is the registered total minus whatever this lane's
    filter selected - it cannot stand still while the selected count changes.
    Round 10 defaulted it to the capture's own 2063 for every fixture, which
    described a differently-sized binary in each one. Pass it explicitly only to
    assert a specific captured line.
    """
    total_tests = passed_tests + failed_tests
    total_asserts = passed_asserts + failed_asserts
    if filtered_out is None:
        filtered_out = CAPTURED_TOTAL_CASES - total_tests
    if filtered_out < 0:
        raise ValueError(
            f"a lane cannot select {total_tests} cases from a {CAPTURED_TOTAL_CASES}-case "
            f"binary; doctest could never emit this summary"
        )
    totwidth = _doctest_column_width(total_tests, total_asserts)
    passwidth = _doctest_column_width(passed_tests, passed_asserts)
    failwidth = _doctest_column_width(failed_tests, failed_asserts)
    return (
        f"[doctest] test cases: {total_tests:>{totwidth}} "
        f"| {passed_tests:>{passwidth}} passed "
        f"| {failed_tests:>{failwidth}} failed |"
        f" {filtered_out} skipped\n"
        f"[doctest] assertions: {total_asserts:>{totwidth}} "
        f"| {passed_asserts:>{passwidth}} passed "
        f"| {failed_asserts:>{failwidth}} failed |\n"
    )


def _case_failure_block(case: str = FAILING_CASE) -> str:
    """A failing test-case block.

    The separator and the `TEST CASE:  ` header framing are taken from the
    capture. The `ERROR:` line is NOT captured anywhere in this repository - the
    only sample is a passing run - so it is constructed, and that gap is stated
    here rather than left for a reader to assume otherwise.
    """
    return (
        CAPTURED_SEPARATOR_LINE + "\n"
        "modules/gaussian_splatting/tests/test_animation.h(42):\n"
        f"TEST CASE:  {case}\n"
        "\n"
        "modules/gaussian_splatting/tests/test_animation.h(50): ERROR: CHECK( a == b ) "
        "is NOT correct!\n"
        "  values: CHECK( 1 == 2 )\n"
        "\n"
    )


# --- Valid shapes: captured, or captured framing with substituted counts ------
PASS_OUTPUT = _summary(5, 0, 120, 0)
FAIL_OUTPUT = _case_failure_block() + _summary(2, 1, 9, 1) + "[doctest] Status: FAILURE!\n"
# The measured shape of the real `GPU Memory Stream` lane: one case selected,
# nothing executed, lane green.
NO_COVERAGE_OUTPUT = _summary(1, 0, 0, 0)
# Every selected test fails: both PASSED counts are zero while the lane executed
# the most coverage of any shape. The passed-count derivation removed in round 4
# called this "no coverage".
ALL_FAILING_OUTPUT = (
    _case_failure_block() + _summary(0, 4, 0, 12) + "[doctest] Status: FAILURE!\n"
)
# VERBATIM. A real clean run that self-skipped three cases - no substitution at
# all, so the skip-marker path is asserted against exactly what a binary printed.
PASS_WITH_SKIP_OUTPUT = CAPTURED_SAMPLE

# --- Deliberately malformed / absent shapes: constructed, which is the point --
# Construction is correct here because the construction IS the property: these
# assert what the runner does when there is NO well-formed producer output to
# parse. There is nothing to capture.
CRASH_OUTPUT = "engine booted\nAccess violation\n"
NO_SUMMARY_OUTPUT = "engine started\nfilter matched nothing\nengine exited\n"
# DECLARED GAP (#822 round 7). This is a WELL-FORMED producer response - Godot's
# argument parser refusing --test on a tests=no binary - so by the rule it belongs
# on the captured side, and it is not captured: this repository has no
# tests-disabled binary and building one is out of scope here. It is therefore
# invented, and rather than dress that up with a better-looking fake, the string
# is pinned to production's OWN marker list and driven through the real
# _run_godot()/_tests_unavailable() path (see
# TestsUnavailableDetectionTests), so at least "the runner recognises this text"
# is asserted rather than assumed. What remains unverified is whether a real
# tests-disabled Godot emits wording that any marker matches. That needs a
# non-test build; it is a follow-up, not something to invent past.
UNAVAILABLE_OUTPUT = "Unknown option '--test'.\n"

# Stands in for a previous, valid measurement on disk. Its exact content does
# not matter; that it is byte-for-byte unchanged after a failed or interrupted
# run does.
PRIOR_REPORT = '{"schema_version": 1, "note": "the previous measurement"}\n'

# --------------------------------------------------------------------------
# Doc-consistency (#822 P2-2).
#
# "Advisory lanes can never affect the exit code" is FALSE: an exit-0 lane whose
# doctest summary reports failures fails the run regardless of `strict`. The
# absolute claim was written into both docs while the table and the test right
# beside it had the exception correct - docs drift from code most easily at
# exactly the point where the author knows the exception and is summarising.
#
# This is a WORDING pin, and a wording pin is weak: it cannot prove a paragraph
# is true, only that a known-false phrasing has not come back. The behavioural
# leg in the same test is what proves the exception is real.
#
# The ban is on the PHRASE, so it also trips on a sentence that quotes the claim
# in order to refute it. That is a known wart, accepted deliberately: making the
# pattern negation-aware would make it guess at meaning, and a guessing guard is
# worse than one that forces the author to phrase the warning differently.
# --------------------------------------------------------------------------
# WHERE THE CLAIM CAN LIVE, derived - not where it happened to live when this
# guard was written (#822 round 8). The first version scanned exactly two
# markdown files, and the very commit that added it put the banned wording into
# run_module_tests.py's own header comment. The guard was green while the false
# claim it exists to prevent was live in the runner: a check that cannot observe
# the thing it claims to rule out, written specifically to rule out that thing.
#
# So membership is decided by CONTENT: any text file in the project's own subtree
# that discusses advisory lanes or the ledger is in scope, and a new file that
# starts discussing them is covered the day it is written. Extending a
# hand-written list of filenames would have rebuilt the same defect with a longer
# list.
#
# WHICH FILES ARE CANDIDATES, in the third and last form this has taken
# (#822 round 10). Round 8 replaced a list of two FILENAMES with a list of
# SUFFIXES; round 9 replaced the suffixes with content detection but kept a
# hand-written list of four DIRECTORIES - docs/, tests/,
# modules/gaussian_splatting/, .github/ - under a comment calling the FORBIDDEN
# check "repo-wide". It was not: root-level AGENTS.md, CONTRIBUTING.md and
# README.md, and any project-owned directory added later, were invisible to a
# guard whose stated property is that this claim cannot live anywhere in the
# project. Same defect, third costume.
#
# The fix is the POLARITY, not a longer list. Nothing is in scope because someone
# listed it. Every file in the working tree is a candidate UNLESS it falls in a
# subtree this repository declares as upstream Godot or vendored (AGENTS.md,
# "Repository map" / "Upstream Godot boundary"). Forgetting to update the list
# below can therefore only cost an unnecessary read - never create a blind spot -
# and a project-owned file is covered the day it is created, wherever it is put.
#
# The file list comes from git rather than from a directory walk, for two
# reasons: `--exclude-standard` uses .gitignore, which is the repository's own
# maintained ground truth for "generated, not authored", so build output does not
# need a second hand-written exclusion list; and `--others` includes files that
# exist but are not committed yet, so a claim is caught while it is still being
# written rather than one commit later. If git cannot be run, the scan reports
# that as an error and goes RED: a candidate list that could not be built is an
# unknown, and an unknown is never a pass.
#
# MEASURED, because "scan literally everything" was the first choice: the whole
# tree is 14413 files / 394 MB and takes ~173 s to read on this machine, against
# 1297 files / 98 MB and ~0.3 s warm once the upstream subtrees are dropped. The
# excluded areas are engine code this fork does not author; the module, the docs,
# the tests, the workflows and every root-level file remain in scope.
UPSTREAM_SUBTREES = (
    "core/",
    "doc/",
    "drivers/",
    "editor/",
    "main/",
    "misc/",
    "platform/",
    "scene/",
    "servers/",
    "thirdparty/",
)
# modules/ is upstream EXCEPT this fork's own module.
OWNED_MODULE_PREFIX = "modules/gaussian_splatting/"
GIT_LS_FILES = ("ls-files", "-z", "--cached", "--others", "--exclude-standard")
# WHICH FILES ARE TEXT is decided by CONTENT too (#822 round 9). Round 8 replaced
# a hand-written list of FILENAMES with a hand-written list of SUFFIXES - .md,
# .py, .yml, .yaml, .txt, .json - which excluded .h, .cpp and .gd, i.e. every
# language the module is actually written in. The regression that motivated this
# guard was a claim in a SOURCE COMMENT, so the list left out precisely the kind
# of file the defect came from: the claim could be written into module source and
# the guard would stay green. A hand-maintained list of the places an invariant
# can be violated is an invariant that is already broken, and this branch has now
# rebuilt that shape twice; the list is deleted rather than lengthened.
#
# A candidate is any project-owned regular file that is text, and "is text" is
# answered by reading it: a file containing a NUL byte is binary. That costs a
# full read of the in-scope files, which is the price of not guessing.
BINARY_MARKER = b"\x00"
# A file is in scope when it TALKS ABOUT the thing the rule is about.
LEDGER_TOPIC_RE = re.compile(
    r"advisory lane|ADVISORY-RED|ADVISORY-FAIL|lane-ledger|lane-result|"
    r"advisory \(strict=False\)",
    re.I,
)
# The same pattern over bytes, DERIVED from the one above rather than retyped, so
# the topic can never differ between the two. Every alternative is ASCII, and a
# file that is not ASCII-compatible enough for this to hold contains NUL bytes and
# was already classified binary. Matching bytes first means only the handful of
# files that actually discuss the ledger are decoded, instead of every text file
# in the roots.
LEDGER_TOPIC_BYTES_RE = re.compile(LEDGER_TOPIC_RE.pattern.encode("ascii"), re.I)
# The two documents that must additionally state the qualification positively.
# This list is for the REQUIRED-marker check (a doc must say the thing), which is
# a statement about these specific documents; the FORBIDDEN check above needs no
# list, because it covers the project by exclusion rather than by enumeration.
DOCS_REQUIRED_TO_STATE_THE_EXCEPTION = (
    ROOT / "docs" / "reference" / "build-test-ci.md",
    ROOT / "docs" / "architecture" / "adr-advisory-lane-ledger.md",
)


def _read_candidate(path: Path) -> str | None:
    """The file's text when it discusses the ledger, else None.

    None means "not a candidate to check" for one of two content reasons: the
    file is binary (it contains a NUL byte - decided by content, never by
    suffix), or it does not mention the subject at all. OSError is deliberately
    NOT caught here: an unreadable candidate is a fact the caller must report,
    not a file to quietly drop.
    """
    data = path.read_bytes()
    if BINARY_MARKER in data:
        return None
    if not LEDGER_TOPIC_BYTES_RE.search(data):
        return None
    return data.decode("utf-8", errors="replace")


def _is_upstream(relative: str) -> bool:
    """True for the subtrees this fork does not author (AGENTS.md repository map)."""
    normalized = relative.replace("\\", "/")
    if normalized.startswith(UPSTREAM_SUBTREES):
        return True
    return normalized.startswith("modules/") and not normalized.startswith(
        OWNED_MODULE_PREFIX
    )


def _decode_git_names(stdout: bytes) -> tuple[list[str], list[str]]:
    """`git ls-files -z` output as usable filenames, and every name that is not.

    Returns `(names, problems)` with the invariant that EVERY NUL-separated
    record git emitted is accounted for in exactly one of them.

    Round 10 decoded the whole stream with `errors="replace"`, which is a silent
    data-loss codec: a filename that is not valid UTF-8 - legal on POSIX, where a
    path is bytes - came back with U+FFFD substituted for the offending bytes.
    That `Path` then named nothing on disk, `is_file()` was False, and the
    scanner dropped it with no entry in `problems`. The result was a claim scan
    reporting clean over a file it had been told about and could not look at:
    the exact silent-skip shape round 9 closed for unreadable files, reopened one
    level up at enumeration.

    So the bytes are decoded with `os.fsdecode()`, which is the OS's own filename
    representation (surrogateescape on POSIX, surrogatepass on Windows) and
    therefore round-trips back to the bytes the kernel will be handed. The round
    trip is then VERIFIED rather than assumed, and any name that cannot decode or
    cannot round-trip is returned as a problem, so a future change back to a lossy
    codec fails the scan instead of shortening it.
    """
    names: list[str] = []
    problems: list[str] = []
    for raw in stdout.split(b"\0"):
        if not raw:
            continue
        try:
            name = os.fsdecode(raw)
        except (UnicodeDecodeError, ValueError) as exc:
            problems.append(
                f"git listed a path this platform cannot decode into a filename "
                f"({raw!r}: {type(exc).__name__}: {exc}); the claim scan cannot "
                f"open it and must not report clean over it"
            )
            continue
        if os.fsencode(name) != raw:
            problems.append(
                f"git listed a path that does not survive decoding ({raw!r} became "
                f"{name!r}); the scan would look for a file that does not exist and "
                f"report clean over the one that does"
            )
            continue
        names.append(name)
    return names, problems


def _project_files() -> tuple[list[Path], list[str]]:
    """Every project-owned file in the working tree, and why the list may be short.

    Returns `(paths, problems)`. `problems` is non-empty only when the list could
    not be built - git missing, git failing, an empty listing in a tree that
    self-evidently has files, or a listed name that cannot be turned back into the
    path git meant. Each entry makes the scan RED. A guard that cannot enumerate
    what it must examine has not found nothing; it has found out nothing, and the
    two must never print the same.
    """
    try:
        completed = subprocess.run(
            ["git", *GIT_LS_FILES],
            cwd=str(ROOT),
            capture_output=True,
            check=False,
        )
    except OSError as exc:
        return [], [
            f"could not enumerate the project's files with git ({type(exc).__name__}: "
            f"{exc}), so the claim scan has no candidate list and cannot report clean"
        ]
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        return [], [
            f"`git {' '.join(GIT_LS_FILES)}` failed with exit {completed.returncode} "
            f"({detail}); the claim scan has no candidate list and cannot report clean"
        ]
    names, problems = _decode_git_names(completed.stdout)
    if not names and not problems:
        return [], [
            "git listed no files at all for the claim scan; an empty candidate list "
            "in a non-empty repository is a broken scan, not a clean one"
        ]
    return [ROOT / name for name in names if not _is_upstream(name)], problems


def _files_discussing_the_ledger(
    candidates: Iterable[Path] | None = None,
) -> tuple[list[tuple[Path, str]], list[str]]:
    """Every project-owned text file that discusses advisory lanes / the ledger.

    Returns `(files, problems)`: the in-scope `(path, text)` pairs, and every
    reason this scan is incomplete - a candidate list that could not be built, or
    a candidate that could not be read.

    The second half exists because round 8 wrote `except OSError: continue`
    (#822 round 9). A candidate that a permission, a Windows sharing lock or an
    I/O error made unreadable was silently dropped, and the scan then reported
    "clean" over evidence it had never looked at - a clean result whose only
    support is that nothing was observed. The minimum-count assertion cannot see
    one missing file. So the failure is returned and the caller fails on it; a
    file that cannot be read is an unknown, and an unknown is never a pass.
    Round 10 puts the enumeration itself under the same rule.

    Round 11 puts the EXISTENCE test under it too. The loop opened with
    `if not path.is_file(): continue`, and `Path.is_file()` cannot report: its
    whole answer is a bool, so a candidate the OS will not describe comes back
    indistinguishable from one that is simply absent. Exactly WHICH wrong answer
    it gives is a CPython implementation detail and has already changed once -
    up to 3.13 `Path.is_file()` calls `self.stat()` and swallows
    ENOENT/ENOTDIR/EBADF/ELOOP and every ValueError into False while RE-RAISING
    the rest (so a permission or I/O error aborted the whole scan with an
    unhandled exception); from 3.14 it delegates to `os.path.isfile()`, which
    swallows every OSError and ValueError into False. A silent skip and a
    torn-down scan are different failures, but neither is "look at this file and
    say what you found", and no version of the line can produce the third answer
    the scan needs: "this candidate is a problem". `path.stat()` can. Only a
    genuinely absent path is a skip now; anything else the OS refuses to answer
    is a problem.

    `candidates` is a parameter so the scanner can be exercised against a planted
    set of files without faking the repository. The guard passes nothing and gets
    the real project-owned tree.

    Excludes only this file, and by identity rather than by name: it *defines*
    the forbidden patterns, so their appearance here is a pattern definition and
    not a claim. Nothing else is exempt.
    """
    problems: list[str] = []
    if candidates is None:
        candidates, problems = _project_files()
    self_path = Path(__file__).resolve()
    found: list[tuple[Path, str]] = []
    for path in sorted(candidates):
        try:
            status = path.stat()
        except (FileNotFoundError, NotADirectoryError):
            # A path git listed that is gone from the working tree, or one a
            # caller invented: there is no text here that was skipped.
            continue
        except (OSError, ValueError) as exc:
            # Everything else: a symlink loop or a bad descriptor, which
            # `is_file()` reported as a plain False; a permission, sharing lock
            # or I/O error, which it raised out of the scan entirely; and a path
            # the platform cannot encode, which it swallowed unconditionally.
            problems.append(f"{path}: cannot stat: {type(exc).__name__}: {exc}")
            continue
        if not stat.S_ISREG(status.st_mode):
            # A directory (a submodule gitlink, or one handed in by a caller):
            # its contents are somebody else's tree, not unread evidence.
            continue
        if path.resolve() == self_path:
            continue
        try:
            text = _read_candidate(path)
        except OSError as exc:
            problems.append(f"{path}: {type(exc).__name__}: {exc}")
            continue
        if text is not None:
            found.append((path, text))
    return sorted(found, key=lambda pair: pair[0]), sorted(problems)


# A candidate the OS refuses to describe, with no mock and no special filesystem:
# a NUL byte cannot be encoded into a path anywhere, so the refusal happens in the
# argument conversion UNDERNEATH every implementation of `Path.is_file()` - below
# `Path.stat` (CPython <= 3.13) and below `os.path.isfile` (3.14+, and on Windows
# that is the C `nt._path_isfile`, which has no Python-level seam at all). It is
# therefore the one refusal that can be injected into `is_file()` on any
# interpreter, which is what makes the measurement below portable.
UNENCODABLE_CANDIDATE = Path("unstattable\x00.md")


def _is_file_verdict(path: Path) -> str:
    """What `Path.is_file()` answers for a candidate the OS refuses to describe.

    Measured on the running interpreter THROUGH THE OS, so the round-11 claim
    about the code that was replaced ("a refusal comes back as a plain bool, so
    the candidate is never reported") is a fact here rather than a quotation from
    pathlib's private errno table - and stays one on every CPython.

    Mocking `Path.stat` would not do this, which is what #822 round 12 caught.
    `Path.is_file()` reaches the OS through `Path.stat` up to CPython 3.13 and
    through `os.path.isfile()` from 3.14 on, so a `Path.stat` mock measures one
    implementation and is silently bypassed by the other: on 3.14 the probe path
    really exists, `is_file()` never consults the mock, and the measurement comes
    back "True" - a verdict about nothing. Pinning it would pin a removed
    implementation detail. The scan's own `path.stat()` call IS mockable on both,
    which is why the planted-refusal proofs below still use a mock and only this
    measurement of `is_file()` goes to the real OS.
    """
    try:
        return "True" if path.is_file() else "False"
    except (OSError, ValueError) as exc:
        return f"raised {type(exc).__name__}"


def _claim_scan_errors(candidates: Iterable[Path] | None = None) -> list[str]:
    """Every reason the claim scan is RED, or [] when it is clean."""
    return _claim_scan_errors_from(*_files_discussing_the_ledger(candidates))


def _claim_scan_errors_from(
    files: Iterable[tuple[Path, str]], problems: Iterable[str]
) -> list[str]:
    """Every reason a completed scan is RED, or [] when it is clean.

    One rule, two callers: the guard asserts this is empty for the real project
    tree (reusing the scan it already did), and the mutation proofs assert it is
    NOT empty for a planted set of files. The guard's verdict is exactly `== []`,
    so a non-empty return here IS the guard going red.
    """
    errors = [
        f"the scan is incomplete, so it cannot claim the project is clean: {reason}"
        for reason in problems
    ]
    for path, text in files:
        for pattern in FORBIDDEN_ABSOLUTE_CLAIMS:
            match = pattern.search(text)
            if match is not None:
                errors.append(
                    f"{path} over-claims: {match.group(0)!r}. An advisory lane that "
                    f"exits 0 with a failing doctest summary DOES fail the run, and a "
                    f"run whose advisory lane went red can still exit nonzero because "
                    f"of a LATER strict lane"
                )
    return errors


FORBIDDEN_ABSOLUTE_CLAIMS = (
    re.compile(r"cannot change the (?:runner's )?exit code by any path", re.I),
    re.compile(r"(?:can )?never (?:change|affect|influence) the (?:runner's )?exit code", re.I),
    re.compile(r"all invisible to the exit code", re.I),
    re.compile(r"invisible to the exit code\b(?![^.]*exception)", re.I),
    # #822 P2-3: a RUN-WIDE claim an advisory record cannot make. The loop
    # continues past ADVISORY-FAIL, so a later strict lane can fail the run.
    # Note this targets "CI / the run still exits 0" only - "a LANE exits 0" is
    # a different and entirely legitimate statement.
    re.compile(r"\b(?:CI|the run)\s+still\s+exit(?:s|ed)\s+0", re.I),
)
# Each doc must positively state the exception, so deleting the qualification
# is caught as well as re-asserting the absolute claim.
# Every separator is \s+ because these sentences are hard-wrapped in the docs.
# A local worktree name (wt-595, wt-705, ...) is the clearest marker of evidence
# that cannot be reproduced from the repository. Deliberately narrow: the docs may
# discuss worktrees as a concept, just not name one machine's.
LOCAL_WORKTREE_RE = re.compile(r"\bwt-\d+\b")
REQUIRED_DOC_MARKERS = (
    re.compile(
        r"exits?\s+0\s+while\s+its\s+doctest\s+summary\s+\**reports\**\s+failures", re.I
    ),
    re.compile(r"(?:does|did)\s+not\s+itself\s+fail\s+the\s+run", re.I),
)
# `reason=<a|b|c>` as documented in either reference.
REASON_ALTERNATION_RE = re.compile(r"reason=<([a-z0-9|-]+)>")


def _evaluated_reason_order() -> list[str]:
    """The reason literals in the order `advisory_red_reason()` returns them.

    Derived from the implementation, not retyped beside it: the reason table in
    build-test-ci.md states that its row order IS the evaluation order, and a
    hand-copied order is a copy that drifts - which is exactly what it did.
    The property is a linear if-chain, so source order is evaluation order.
    """
    prop = harness.LaneLedgerRecord.advisory_red_reason
    source = inspect.getsource(getattr(prop, "fget", prop))
    return re.findall(r'return\s+"([^"]+)"', source)


def _documented_json_keys(text: str) -> list[str]:
    """The top-level keys of the `--lane-report` object as a document states it.

    Both references write the shape as an inline code span beginning
    `{schema_version, ...`; nested values (`lanes: [...]`, `totals: {...}`) are
    skipped by depth so only the top level is compared. The span may be
    hard-wrapped, so whitespace is collapsed first.
    """
    match = re.search(r"`\{schema_version[^`]*\}`", text, re.S)
    if match is None:
        return []
    body = " ".join(match.group(0).strip("`").split())
    body = body[1:-1] if body.startswith("{") and body.endswith("}") else body
    keys: list[str] = []
    current = ""
    depth = 0
    for character in body:
        if character in "{[":
            depth += 1
        elif character in "}]":
            depth -= 1
        if character == "," and depth == 0:
            keys.append(current)
            current = ""
        else:
            current += character
    keys.append(current)
    return [key.strip().split(":")[0].strip() for key in keys if key.strip()]


def _documented_reason_order(text: str) -> list[str]:
    """The `reason=` table's rows, in document order."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if not line.startswith("| `reason=`"):
            continue
        rows: list[str] = []
        # +2 skips the header and the |---|---| separator.
        for row in lines[index + 2 :]:
            match = re.match(r"\|\s*`([^`]+)`", row)
            if match is None:
                break
            rows.append(match.group(1))
        return rows
    return []


def _godot(ok: bool, skipped: bool, output: str, returncode: int | None):
    factory = getattr(harness, "GodotRunResult", None)
    if factory is None:
        # Pre-#705 the runner had no returncode-carrying result and every caller
        # unpacked a plain 3-tuple. Degrading here (rather than raising) is what
        # keeps the revert-proof honest: a reverted run must fail on a MISSING
        # RECORD assertion, not on an AttributeError, which would only prove
        # that this file measures import success.
        return (ok, skipped, output)
    return factory(ok, skipped, output, returncode)


def _call_lane_loop(test_runs, mode, allow_unavailable, lane_report):
    """Call `_run_doctest_lanes`, tolerating a signature without --lane-report."""
    accepts_report = "lane_report_path" in inspect.signature(
        harness._run_doctest_lanes
    ).parameters
    if accepts_report:
        return harness._run_doctest_lanes(
            "godot", test_runs, mode, allow_unavailable, lane_report
        )
    return harness._run_doctest_lanes("godot", test_runs, mode, allow_unavailable)


@contextlib.contextmanager
def _neutered_ledger():
    """Disable the ledger completely, leaving baseline control flow alone.

    This is the parity oracle: whatever the runner decides with the ledger, it
    must decide the same thing without it.
    """
    if not hasattr(harness, "LaneLedger"):
        yield
        return
    with contextlib.ExitStack() as stack:
        stack.enter_context(
            mock.patch.object(harness.LaneLedger, "record", lambda self, *a, **k: None)
        )
        stack.enter_context(
            mock.patch.object(
                harness.LaneLedger, "print_block", lambda self: harness.LaneLedgerTotals()
            )
        )
        stack.enter_context(
            mock.patch.object(harness.LaneLedger, "check_integrity", lambda self, **k: [])
        )
        yield


def _drive(
    lanes,
    results,
    *,
    mode: str = "warn-only",
    allow_unavailable: bool = False,
    quarantine: dict | None = None,
    lane_report: Path | None = None,
    ci: bool = False,
    neuter_ledger: bool = False,
):
    """Run the real lane loop over `lanes` with `_run_godot` stubbed.

    `lanes` is [(name, strict)]; `results` is either one GodotRunResult reused
    for every lane, or a list of one per lane.
    """
    test_runs = [(name, ["--headless", "--test"], strict) for name, strict in lanes]
    if isinstance(results, list):
        godot_stub = mock.patch.object(harness, "_run_godot", side_effect=list(results))
    else:
        godot_stub = mock.patch.object(harness, "_run_godot", return_value=results)

    buffer = io.StringIO()
    with contextlib.ExitStack() as stack:
        stack.enter_context(mock.patch.dict(os.environ, {"CI": "1" if ci else ""}))
        stack.enter_context(godot_stub)
        stack.enter_context(
            mock.patch.object(harness, "_load_quarantine", return_value=dict(quarantine or {}))
        )
        if neuter_ledger:
            stack.enter_context(_neutered_ledger())
        stack.enter_context(contextlib.redirect_stdout(buffer))
        rc = _call_lane_loop(test_runs, mode, allow_unavailable, lane_report)
    return rc, buffer.getvalue()


def _run_main(argv, *, drop_lane: bool = False, godot_result=None):
    """Drive main() with the guards and asset prep stubbed out.

    Only the lane-report preflight and the run-list integrity check need a real
    main(); everything before them is unrelated to this slice.
    """
    result = godot_result if godot_result is not None else _godot(True, False, PASS_OUTPUT, 0)
    with contextlib.ExitStack() as stack:
        stack.enter_context(mock.patch.dict(os.environ, {"CI": ""}))
        stack.enter_context(mock.patch.object(sys, "argv", argv))
        stack.enter_context(mock.patch.object(harness, "_run_ci_guard_steps", return_value=None))
        stack.enter_context(
            mock.patch.object(harness, "_prepare_synthetic_assets", return_value=(True, []))
        )
        stack.enter_context(mock.patch.object(harness, "_run_godot", return_value=result))
        stack.enter_context(mock.patch.object(harness, "_load_quarantine", return_value={}))
        if drop_lane:
            original = harness._build_module_test_runs
            stack.enter_context(
                mock.patch.object(
                    harness, "_build_module_test_runs", lambda gpu: original(gpu)[1:]
                )
            )
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        return harness.main()


def _grammar_padding(line: str) -> str:
    """The line's whitespace/punctuation skeleton, with digits collapsed per run."""
    return re.sub(r"\d", "#", line)


def _grammar_keys(line: str) -> list[str]:
    """The ordered `key=` names in a ledger line or a documented template of one."""
    return re.findall(r"(\w+)=", line)


def _records(output: str) -> dict[str, dict[str, str]]:
    found: dict[str, dict[str, str]] = {}
    for line in output.splitlines():
        match = LANE_RESULT_RE.match(line)
        if match:
            found[match.group("lane")] = match.groupdict()
    return found


def _record_order(output: str) -> list[str]:
    return [
        match.group("lane")
        for match in (LANE_RESULT_RE.match(line) for line in output.splitlines())
        if match
    ]


def _aggregate(output: str) -> dict[str, int]:
    for line in output.splitlines():
        if line.startswith("[module-tests][lane-ledger] lanes="):
            body = line.split("] ", 1)[1]
            return {k: int(v) for k, v in (pair.split("=") for pair in body.split(" "))}
    raise AssertionError(
        "no [module-tests][lane-ledger] aggregate line was printed; it must be emitted "
        "unconditionally so that absence of output can never read as absence of failures"
    )


def _advisory_red(output: str) -> dict[str, str]:
    red = {}
    for line in output.splitlines():
        if line.startswith("[module-tests][lane-ledger] ADVISORY-RED "):
            fields = line.split("ADVISORY-RED ", 1)[1]
            lane, reason = fields.rsplit(" reason=", 1)
            red[lane[len("lane="):]] = reason
    return red


class LaneLedgerOutcomeTests(unittest.TestCase):
    """One test per outcome class: exact record, aggregate counts, exit code."""

    maxDiff = None

    def _assert_parity(self, rc, expected_baseline_rc, scenario_kwargs, lanes, results):
        """rc must equal the baseline value AND the value the same run produces
        with the ledger disabled."""
        self.assertEqual(
            rc,
            expected_baseline_rc,
            f"exit code changed from the baseline value {expected_baseline_rc}",
        )
        rc_without_ledger, _ = _drive(lanes, results, neuter_ledger=True, **scenario_kwargs)
        self.assertEqual(
            rc,
            rc_without_ledger,
            "the ledger changed a decision: exit code differs with and without it",
        )

    def _assert_record(self, output, lane, **expected):
        records = _records(output)
        self.assertIn(
            lane,
            records,
            f"lane '{lane}' has no [module-tests][lane-result] record; an unrecorded "
            f"lane reads as a passed lane",
        )
        record = records[lane]
        for key, value in expected.items():
            self.assertEqual(
                record[key],
                str(value),
                f"lane '{lane}' record field {key}: {record[key]!r} != {str(value)!r}",
            )

    def test_pass_outcome(self) -> None:
        lanes = [("StrictLane", True)]
        results = _godot(True, False, PASS_OUTPUT, 0)
        rc, output = _drive(lanes, results)
        self.assertIn(
            "[module-tests][lane-result] lane=StrictLane strict=1 outcome=PASS "
            "passed_tests=5 passed_assertions=120 failed_tests=0 failed_assertions=0 "
            "skipped_markers=0 exit_code=0 exit_code_reported=1 summary_reported=1 "
            "zero_coverage=0",
            output,
        )
        totals = _aggregate(output)
        self.assertEqual(totals["lanes"], 1)
        self.assertEqual(totals["strict_lanes"], 1)
        self.assertEqual(totals["advisory_failures"], 0)
        self.assertEqual(totals["advisory_zero_coverage"], 0)
        self.assertEqual(totals["not_run"], 0)
        self._assert_parity(rc, BASELINE_RC_PASS, {}, lanes, results)

    def test_advisory_failure_is_recorded_and_still_exits_zero(self) -> None:
        lanes = [("AdvisoryLane", False)]
        results = _godot(False, False, FAIL_OUTPUT, 1)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output,
            "AdvisoryLane",
            strict=0,
            outcome="ADVISORY-FAIL",
            passed_tests=2,
            failed_tests=1,
            passed_assertions=9,
            failed_assertions=1,
            exit_code=1,
            summary_reported=1,
        )
        totals = _aggregate(output)
        self.assertEqual(
            totals["advisory_failures"],
            1,
            "a failed advisory lane must be counted in advisory_failures",
        )
        self.assertEqual(_advisory_red(output), {"AdvisoryLane": "failed"})
        # NO SUPPRESSION: the baseline's own line is still printed.
        self.assertIn("'AdvisoryLane' crashed or failed", output)
        self._assert_parity(rc, BASELINE_RC_ADVISORY_FAIL, {}, lanes, results)

    def test_advisory_crash_records_unknown_counts_not_zero(self) -> None:
        lanes = [("AdvisoryLane", False)]
        results = _godot(False, False, CRASH_OUTPUT, 3221225477)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output,
            "AdvisoryLane",
            outcome="ADVISORY-FAIL",
            passed_tests=-1,
            failed_tests=-1,
            passed_assertions=-1,
            failed_assertions=-1,
            summary_reported=0,
            zero_coverage=-1,
            exit_code=3221225477,
        )
        self.assertEqual(
            _advisory_red(output),
            {"AdvisoryLane": "crashed"},
            "a lane that never printed a summary crashed; it did not 'fail'",
        )
        self._assert_parity(rc, BASELINE_RC_ADVISORY_FAIL, {}, lanes, results)

    def test_strict_failure_records_fail_and_gates(self) -> None:
        lanes = [("StrictLane", True)]
        results = _godot(False, False, FAIL_OUTPUT, 1)
        rc, output = _drive(lanes, results)
        self._assert_record(output, "StrictLane", strict=1, outcome="FAIL", failed_tests=1)
        totals = _aggregate(output)
        self.assertEqual(totals["fail_outcomes"], 1)
        self.assertEqual(
            totals["advisory_failures"], 0, "a strict failure is not an advisory failure"
        )
        self.assertEqual(_advisory_red(output), {})
        self._assert_parity(rc, BASELINE_RC_STRICT_FAIL, {}, lanes, results)

    def test_advisory_zero_coverage_is_recorded(self) -> None:
        # The measured `GPU Memory Stream` shape: 1 passed | 0 failed, 0 assertions.
        lanes = [("GPU Memory Stream", False)]
        results = _godot(True, False, NO_COVERAGE_OUTPUT, 0)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output,
            "GPU Memory Stream",
            outcome="ADVISORY-NO-COVERAGE",
            passed_tests=1,
            passed_assertions=0,
            summary_reported=1,
            zero_coverage=1,
        )
        totals = _aggregate(output)
        self.assertEqual(totals["advisory_zero_coverage"], 1)
        self.assertEqual(_advisory_red(output), {"GPU Memory Stream": "no-coverage"})
        self._assert_parity(rc, BASELINE_RC_ADVISORY_NO_COVERAGE, {}, lanes, results)

    def test_exit_zero_without_summary_records_fail(self) -> None:
        # The one advisory outcome that gates at baseline: a harness error.
        lanes = [("AdvisoryLane", False)]
        results = _godot(True, False, NO_SUMMARY_OUTPUT, 0)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output, "AdvisoryLane", outcome="FAIL", passed_tests=-1, summary_reported=0
        )
        self.assertIn("missing doctest summary", output)
        self._assert_parity(rc, BASELINE_RC_NO_SUMMARY, {}, lanes, results)

    def test_a_lane_where_everything_fails_is_not_recorded_as_no_coverage(self) -> None:
        """#822 P2 round 4: executed coverage is passed + FAILED, never passed alone.

        A lane in which every test fails has both PASSED counts at zero while
        having executed the most coverage of any shape there is. Deriving
        "nothing ran" from the passed counts filed that case under "no coverage" -
        the inverse of what this field exists to expose, and the field a future
        coverage ratchet is meant to be armed on, so the catastrophe would have
        registered as improvement.
        """
        lanes = [("AdvisoryLane", False)]
        results = _godot(False, False, ALL_FAILING_OUTPUT, 1)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output,
            "AdvisoryLane",
            outcome="ADVISORY-FAIL",
            passed_tests=0,
            failed_tests=4,
            passed_assertions=0,
            failed_assertions=12,
            summary_reported=1,
            zero_coverage=0,
        )
        self.assertEqual(
            _aggregate(output)["advisory_zero_coverage"],
            0,
            "a lane that executed and failed everything has coverage",
        )
        self.assertEqual(_advisory_red(output), {"AdvisoryLane": "failed"})
        self._assert_parity(rc, BASELINE_RC_ADVISORY_FAIL, {}, lanes, results)

    def test_zero_coverage_and_failures_are_mutually_exclusive(self) -> None:
        """The INVARIANT, as a property over every record of every shape.

        Not a case: the removed passed-count formula satisfied every
        case-by-case test written for it and still violated this. Any future
        derivation must satisfy the property, whatever the formula.
        """
        lanes = [
            ("AllFailing", False),
            ("PartlyFailing", False),
            ("EmptyLane", False),
            ("PassLane", True),
            ("Crashed", False),
            ("Teardown", False),
        ]
        results = [
            _godot(False, False, ALL_FAILING_OUTPUT, 1),
            _godot(False, False, FAIL_OUTPUT, 1),
            _godot(True, False, NO_COVERAGE_OUTPUT, 0),
            _godot(True, False, PASS_OUTPUT, 0),
            _godot(False, False, CRASH_OUTPUT, 3221225477),
            _godot(False, False, PASS_OUTPUT, 1),
        ]
        rc, output = _drive(lanes, results)
        records = _records(output)
        self.assertEqual(len(records), len(lanes), "every lane must be recorded")
        for lane, record in records.items():
            with self.subTest(lane=lane):
                if record["zero_coverage"] != "1":
                    continue
                self.assertLessEqual(
                    int(record["failed_tests"]),
                    0,
                    f"{lane}: zero_coverage=1 with failed_tests="
                    f"{record['failed_tests']} - a record cannot say both 'nothing "
                    f"ran' and 'these ran and failed'",
                )
                self.assertLessEqual(
                    int(record["failed_assertions"]),
                    0,
                    f"{lane}: zero_coverage=1 with failed_assertions="
                    f"{record['failed_assertions']} - executed failures are proof "
                    f"that coverage ran",
                )
        # The invariant must not be satisfied vacuously by never setting the flag.
        self.assertEqual(
            records["EmptyLane"]["zero_coverage"],
            "1",
            "the lane that really executed nothing must still be flagged",
        )
        self.assertEqual(records["AllFailing"]["zero_coverage"], "0")
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_a_self_contradictory_record_is_an_integrity_failure(self) -> None:
        """The invariant is enforced in production, not only in this file."""
        contradictory = harness.LaneLedgerRecord(
            lane="Impossible",
            strict=False,
            outcome=harness.LANE_OUTCOME_ADVISORY_FAIL,
            zero_coverage=True,
            failed_tests=3,
            failed_assertions=9,
        )
        errors = harness._self_contradictory_records([contradictory])
        self.assertTrue(errors, "zero_coverage=1 with failures must be reported")
        self.assertIn("zero_coverage=1", errors[0])
        self.assertIn("Impossible", errors[0])

        # WIRING. Calling the helper directly only proves the helper works; it
        # says nothing about whether anything calls it, and a guard wired to
        # nothing is the failure mode this repository keeps re-learning. Force a
        # contradictory record through the real loop and require the run to fail
        # with the integrity line.
        def contradictory_lane(*_args, **_kwargs):
            return None, harness.LaneResult(
                outcome=harness.LANE_OUTCOME_ADVISORY_FAIL,
                summary_reported=True,
                zero_coverage=True,
                passed_tests=0,
                failed_tests=3,
                passed_assertions=0,
                failed_assertions=9,
                skipped_markers=0,
                detail="synthetic contradiction",
            )

        with mock.patch.object(harness, "_execute_lane", contradictory_lane):
            rc_bad, output_bad = _drive(
                [("AdvisoryLane", False)], _godot(False, False, ALL_FAILING_OUTPUT, 1)
            )
        self.assertIn(
            "[module-tests][lane-ledger][INTEGRITY]",
            output_bad,
            "check_integrity() must consult the invariant, not merely define it",
        )
        self.assertIn("cannot say both", output_bad)
        self.assertNotEqual(rc_bad, 0, "a self-contradictory ledger must not report success")

        # ... and the REAL derivation never produces that shape.
        lanes = [("AdvisoryLane", False)]
        rc, output = _drive(lanes, _godot(False, False, ALL_FAILING_OUTPUT, 1))
        self.assertNotIn(
            "[module-tests][lane-ledger][INTEGRITY]",
            output,
            "the real derivation must not produce a contradictory record",
        )
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_zero_coverage_is_counted_whatever_the_outcome(self) -> None:
        """#822 round 5: zero coverage is a property, not an outcome bucket.

        An advisory lane can execute nothing while its outcome is ADVISORY-FAIL
        (a crash whose summary still printed 0|0). Counting the aggregate off the
        ADVISORY-NO-COVERAGE outcome alone under-reported the exact thing the
        field exists to expose - and it is the field GS-705-2 ratchets on.
        """
        lanes = [("CrashedEmpty", False), ("QuietlyEmpty", False)]
        results = [
            # Nonzero exit -> ADVISORY-FAIL, but the summary shows nothing ran.
            _godot(False, False, NO_COVERAGE_OUTPUT, 1),
            # Exit 0 -> ADVISORY-NO-COVERAGE.
            _godot(True, False, NO_COVERAGE_OUTPUT, 0),
        ]
        rc, output = _drive(lanes, results)
        records = _records(output)
        self.assertEqual(records["CrashedEmpty"]["outcome"], "ADVISORY-FAIL")
        self.assertEqual(records["CrashedEmpty"]["zero_coverage"], "1")
        self.assertEqual(records["QuietlyEmpty"]["outcome"], "ADVISORY-NO-COVERAGE")
        self.assertEqual(
            _aggregate(output)["advisory_zero_coverage"],
            2,
            "both lanes executed nothing; only one of them has the "
            "ADVISORY-NO-COVERAGE outcome",
        )
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_unknown_coverage_is_never_counted_as_zero_coverage(self) -> None:
        """A lane with no summary is `-1` (not knowable) and must not be counted."""
        lanes = [("Crashed", False)]
        rc, output = _drive(lanes, _godot(False, False, CRASH_OUTPUT, 3221225477))
        self._assert_record(output, "Crashed", outcome="ADVISORY-FAIL", zero_coverage=-1)
        self.assertEqual(
            _aggregate(output)["advisory_zero_coverage"],
            0,
            "'not knowable' must never be silently read as 'zero'",
        )
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_an_advisory_lane_failure_is_never_charged_to_a_strict_lane(self) -> None:
        """#822 P2-1: FAIL is not the same thing as "a strict lane failed".

        An ADVISORY lane records FAIL when it exits 0 with a missing or failing
        summary. Deriving the strict-failure count from that outcome produced
        `strict_lanes=0 strict_failures=1` - an aggregate that is not merely
        imprecise but attributes the failure to a lane class that has no members
        in this run. A published aggregate that is wrong is worse than one that
        is missing, because it gets quoted.
        """
        lanes = [("AdvisoryLane", False)]
        results = _godot(True, False, NO_SUMMARY_OUTPUT, 0)
        rc, output = _drive(lanes, results)
        self._assert_record(output, "AdvisoryLane", strict=0, outcome="FAIL")
        totals = _aggregate(output)
        self.assertEqual(totals["strict_lanes"], 0, "the scenario has no strict lane")
        self.assertEqual(
            totals["fail_outcomes"],
            1,
            "the lane did fail the run, and the aggregate must say so",
        )
        self.assertNotIn(
            "strict_failures",
            totals,
            "strict_failures counted FAIL outcomes regardless of the lane's strict "
            "flag; the field must be named for what it counts",
        )
        self.assertEqual(rc, BASELINE_RC_NO_SUMMARY)

    def test_fail_outcomes_are_split_by_the_declared_strict_flag(self) -> None:
        """The split must come from record.strict, never from the outcome."""
        lanes = [("AdvisoryLane", False), ("StrictLane", True)]
        results = [
            _godot(True, False, NO_SUMMARY_OUTPUT, 0),  # advisory -> FAIL, aborts
            _godot(True, False, PASS_OUTPUT, 0),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "lane_ledger.json"
            rc, output = _drive(lanes, results, lane_report=report)
            payload = json.loads(report.read_text(encoding="utf-8"))
        totals = payload["totals"]
        self.assertEqual(totals["fail_outcomes"], 1)
        self.assertEqual(
            totals["fail_outcomes_on_advisory_lanes"],
            1,
            "the failing lane is declared strict=False",
        )
        self.assertEqual(totals["fail_outcomes_on_strict_lanes"], 0)
        self.assertEqual(_records(output)["AdvisoryLane"]["outcome"], "FAIL")
        self.assertEqual(rc, BASELINE_RC_NO_SUMMARY)

    def test_an_abort_that_records_no_fail_is_still_counted_as_run_ending(self) -> None:
        """#822 round 10: FAIL is not the only outcome that ends the run.

        `gating_failures` counted FAIL outcomes and was named as though it
        counted every failure that gates. Two abort paths record no FAIL at all:
        a strict tests-unavailable lane (UNAVAILABLE) and a stale or invalid
        quarantine (QUARANTINE-REJECTED). Both stop the loop and decide the exit
        code, so a gated run published `gating_failures=0` - a published
        aggregate that is wrong in the direction of "nothing gated", which is the
        direction this whole ledger exists to make impossible.

        Both aborts are driven here, and each is asserted against BOTH counts:
        the FAIL count must stay 0 (it is the narrow measurement, and inflating
        it would be the opposite error) while the run-ending count must be 1.
        """
        quarantine = {
            "QuarantinedLane": [
                {"test_case": "*plays a clip*", "issue_url": "https://example/issues/1"}
            ]
        }
        cases = (
            (
                "UNAVAILABLE",
                [("StrictLane", True), ("NeverReached", True)],
                _godot(True, True, UNAVAILABLE_OUTPUT, 1),
                {"mode": "strict"},
                BASELINE_RC_UNAVAILABLE_STRICT,
            ),
            (
                "QUARANTINE-REJECTED",
                [("QuarantinedLane", True), ("NeverReached", True)],
                # A quarantined lane that PASSED: the entry is stale.
                _godot(True, False, PASS_OUTPUT, 0),
                {"quarantine": quarantine},
                BASELINE_RC_QUARANTINE_REJECTED,
            ),
        )
        for outcome, lanes, results, kwargs, expected_rc in cases:
            with self.subTest(outcome=outcome):
                rc, output = _drive(lanes, results, **kwargs)
                self.assertEqual(
                    _records(output)[lanes[0][0]]["outcome"],
                    outcome,
                    "sanity: the abort path under test is the one that ran",
                )
                self.assertEqual(
                    _records(output)["NeverReached"]["outcome"],
                    "NOT-RUN",
                    "sanity: the run really did end at the first lane",
                )
                totals = _aggregate(output)
                self.assertEqual(
                    totals["fail_outcomes"],
                    0,
                    "no lane recorded FAIL, and the narrow count must not pretend "
                    "otherwise",
                )
                self.assertEqual(
                    totals["run_ending_outcomes"],
                    1,
                    f"{outcome} ended the run and set the exit code; an aggregate "
                    f"reporting 0 here tells a consumer nothing gated",
                )
                self.assertEqual(rc, expected_rc, "sanity: the run really was gated")

    def test_a_fail_outcome_is_counted_as_run_ending_exactly_once(self) -> None:
        """The broad count must include FAIL, not replace it.

        A run-ending count derived from something other than what the loop broke
        on could just as easily miss the ordinary case, so the ordinary case is
        pinned too: one FAIL, one abort, both counts at 1.
        """
        lanes = [("StrictLane", True), ("NeverReached", True)]
        results = _godot(False, False, FAIL_OUTPUT, 1)
        rc, output = _drive(lanes, results)
        totals = _aggregate(output)
        self.assertEqual(totals["fail_outcomes"], 1)
        self.assertEqual(totals["run_ending_outcomes"], 1)
        self.assertEqual(_records(output)["NeverReached"]["outcome"], "NOT-RUN")
        self.assertEqual(rc, BASELINE_RC_STRICT_FAIL)

    def test_a_run_nothing_ended_reports_no_run_ending_outcome(self) -> None:
        """The count must be able to say zero, or it is not a measurement.

        An advisory red does NOT end the run, so a ledger whose run-ending count
        were "any red lane" would report 1 here and make the field useless for
        the one question it answers.
        """
        lanes = [("AdvisoryLane", False), ("PassLane", True)]
        results = [
            _godot(False, False, FAIL_OUTPUT, 1),
            _godot(True, False, PASS_OUTPUT, 0),
        ]
        rc, output = _drive(lanes, results)
        totals = _aggregate(output)
        self.assertEqual(totals["advisory_failures"], 1, "sanity: a lane did go red")
        self.assertEqual(
            totals["run_ending_outcomes"],
            0,
            "an advisory failure does not end the run, so nothing ended it",
        )
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_a_missing_return_code_is_not_reported_as_a_signal(self) -> None:
        """#822 round 10: -1 cannot mean both "unknown" and "killed by SIGHUP".

        The ledger writes LANE_COUNT_UNKNOWN (-1) into `exit_code` when no return
        code is available, and `subprocess` reports a POSIX SIGHUP termination as
        the return code -1. Without `exit_code_reported` the two records are
        byte-identical, so "we never learned how this lane ended" and "this lane
        was signalled" read the same to every consumer of the field whose purpose
        is telling them apart.
        """
        lanes = [("SignalledLane", False)]
        rc, output = _drive(lanes, _godot(False, False, CRASH_OUTPUT, -1))
        signalled = _records(output)["SignalledLane"]
        self.assertEqual(signalled["exit_code"], "-1")
        self.assertEqual(
            signalled["exit_code_reported"],
            "1",
            "the process reported -1; that is a real return code, not an absence",
        )

        # Same printed exit_code, opposite meaning: a result carrying no return
        # code at all (the runner produces this when the process could not be
        # launched, and a pre-#705 stub produces it by returning a bare 3-tuple).
        rc_missing, output_missing = _drive(
            [("NoReturnCodeLane", False)], _godot(False, False, CRASH_OUTPUT, None)
        )
        missing = _records(output_missing)["NoReturnCodeLane"]
        self.assertEqual(missing["exit_code"], signalled["exit_code"])
        self.assertEqual(
            missing["exit_code_reported"],
            "0",
            "no return code exists for this lane, and the ledger must say so rather "
            "than publish a value that looks like SIGHUP",
        )
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)
        self.assertEqual(rc_missing, BASELINE_RC_ADVISORY_FAIL)

    def test_a_never_attempted_lane_reports_no_return_code(self) -> None:
        """A seeded NOT-RUN lane ran no process, so it reported no exit code."""
        lanes = [("StrictLane", True), ("NeverReached", True)]
        rc, output = _drive(lanes, _godot(False, False, FAIL_OUTPUT, 1))
        never = _records(output)["NeverReached"]
        self.assertEqual(never["outcome"], "NOT-RUN")
        self.assertEqual(never["exit_code_reported"], "0")
        self.assertEqual(never["exit_code"], "-1")
        self.assertEqual(rc, BASELINE_RC_STRICT_FAIL)

    def test_a_teardown_crash_is_not_reported_as_test_failures(self) -> None:
        """#822 P2-2: a clean all-pass summary plus a nonzero exit is not "failed".

        The repo already draws this distinction in
        _classify_quarantined_lane_outcome(); reporting it as reason=failed would
        announce "an advisory lane is failing tests" with both failed counts at
        zero.
        """
        lanes = [("AdvisoryLane", False)]
        # Every test passed; the process still exited nonzero.
        results = _godot(False, False, PASS_OUTPUT, 1)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output,
            "AdvisoryLane",
            outcome="ADVISORY-FAIL",
            passed_tests=5,
            failed_tests=0,
            failed_assertions=0,
            summary_reported=1,
            exit_code=1,
        )
        self.assertEqual(
            _advisory_red(output),
            {"AdvisoryLane": "nonzero-exit-no-test-failures"},
            "a summary with zero failures did not report a test failure",
        )
        self._assert_parity(rc, BASELINE_RC_ADVISORY_FAIL, {}, lanes, results)

    def test_a_nonzero_exit_with_nothing_executed_reports_no_coverage(self) -> None:
        """#822 round 8: reason= must not hide zero coverage behind a summary.

        A lane that exits nonzero after printing a summary in which nothing ran
        was reported as `nonzero-exit-no-test-failures` merely because a summary
        existed - telling a stdout consumer that tests ran and passed when none
        ran. The aggregate was meanwhile counting the same lane under
        advisory_zero_coverage, so the two disagreed inside one block.
        """
        lanes = [("EmptyAndFailing", False)]
        results = _godot(False, False, NO_COVERAGE_OUTPUT, 1)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output,
            "EmptyAndFailing",
            outcome="ADVISORY-FAIL",
            zero_coverage=1,
            summary_reported=1,
            failed_tests=0,
            failed_assertions=0,
        )
        self.assertEqual(
            _advisory_red(output),
            {"EmptyAndFailing": "no-coverage"},
            "nothing executed, so the reason is no-coverage - not a teardown crash",
        )
        # The stdout reason and the aggregate must agree in the same block.
        self.assertEqual(_aggregate(output)["advisory_zero_coverage"], 1)
        self._assert_parity(rc, BASELINE_RC_ADVISORY_FAIL, {}, lanes, results)

    def test_unknown_coverage_still_reports_crashed_not_no_coverage(self) -> None:
        """`is True`, not truthiness: -1 means not knowable and must not read as zero."""
        lanes = [("Crashed", False)]
        rc, output = _drive(lanes, _godot(False, False, CRASH_OUTPUT, 3221225477))
        self._assert_record(output, "Crashed", zero_coverage=-1, summary_reported=0)
        self.assertEqual(
            _advisory_red(output),
            {"Crashed": "crashed"},
            "a lane with no summary has UNKNOWN coverage; it must not be reported "
            "as having executed nothing",
        )
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_reason_distinguishes_all_four_advisory_red_shapes(self) -> None:
        """Each reason must be reachable and distinct; otherwise the field is noise."""
        lanes = [
            ("FailedLane", False),
            ("CrashedLane", False),
            ("TeardownLane", False),
            ("EmptyLane", False),
        ]
        results = [
            _godot(False, False, FAIL_OUTPUT, 1),
            _godot(False, False, CRASH_OUTPUT, 3221225477),
            _godot(False, False, PASS_OUTPUT, 1),
            _godot(True, False, NO_COVERAGE_OUTPUT, 0),
        ]
        rc, output = _drive(lanes, results)
        self.assertEqual(
            _advisory_red(output),
            {
                "FailedLane": "failed",
                "CrashedLane": "crashed",
                "TeardownLane": "nonzero-exit-no-test-failures",
                "EmptyLane": "no-coverage",
            },
        )
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_unavailable_binary_warn_only(self) -> None:
        lanes = [("StrictLane", True)]
        results = _godot(True, True, UNAVAILABLE_OUTPUT, 1)
        rc, output = _drive(lanes, results)
        self._assert_record(output, "StrictLane", outcome="UNAVAILABLE", summary_reported=0)
        self.assertEqual(_aggregate(output)["unavailable"], 1)
        self._assert_parity(rc, BASELINE_RC_UNAVAILABLE_WARN, {}, lanes, results)

    def test_unavailable_binary_strict_mode_records_and_gates(self) -> None:
        lanes = [("StrictLane", True)]
        results = _godot(True, True, UNAVAILABLE_OUTPUT, 1)
        kwargs = {"mode": "strict"}
        rc, output = _drive(lanes, results, **kwargs)
        self._assert_record(output, "StrictLane", outcome="UNAVAILABLE", summary_reported=0)
        self.assertEqual(
            _aggregate(output)["unavailable"],
            1,
            "the lane that ABORTED the run must still be in the ledger",
        )
        self._assert_parity(rc, BASELINE_RC_UNAVAILABLE_STRICT, kwargs, lanes, results)

    def test_quarantine_tolerated_is_recorded(self) -> None:
        lanes = [("StrictLane", True)]
        results = _godot(False, False, FAIL_OUTPUT, 1)
        kwargs = {
            "quarantine": {
                "StrictLane": [
                    {"test_case": "*plays a clip*", "issue_url": "https://example/issues/1"}
                ]
            }
        }
        rc, output = _drive(lanes, results, **kwargs)
        self._assert_record(output, "StrictLane", outcome="QUARANTINE-TOLERATED", failed_tests=1)
        self.assertEqual(_aggregate(output)["quarantine_tolerated"], 1)
        self.assertIn("[module-tests][QUARANTINE]", output)
        self._assert_parity(rc, BASELINE_RC_QUARANTINE_TOLERATED, kwargs, lanes, results)

    def test_quarantine_rejected_is_recorded(self) -> None:
        # Quarantined lane that PASSED -> stale entry -> the run fails.
        lanes = [("StrictLane", True)]
        results = _godot(True, False, PASS_OUTPUT, 0)
        kwargs = {
            "quarantine": {
                "StrictLane": [
                    {"test_case": "*plays a clip*", "issue_url": "https://example/issues/1"}
                ]
            }
        }
        rc, output = _drive(lanes, results, **kwargs)
        self._assert_record(output, "StrictLane", outcome="QUARANTINE-REJECTED")
        self.assertEqual(_aggregate(output)["quarantine_rejected"], 1)
        self.assertIn("[module-tests][QUARANTINE-STALE]", output)
        self._assert_parity(rc, BASELINE_RC_QUARANTINE_REJECTED, kwargs, lanes, results)

    def test_strict_ci_skip_policy_failure_is_recorded_with_its_skip_count(self) -> None:
        # Runs with CI=1, because _enforce_skipped_marker_policy is behind
        # _is_ci(): a local green says nothing about this path. The input is the
        # VERBATIM capture, so the asserted counts are a real binary's, not ours.
        lanes = [("StrictLane", True)]
        results = _godot(True, False, PASS_WITH_SKIP_OUTPUT, 0)
        kwargs = {"ci": True}
        rc, output = _drive(lanes, results, **kwargs)
        self._assert_record(
            output,
            "StrictLane",
            outcome="FAIL",
            skipped_markers=CAPTURED_SKIP_MARKERS,
            passed_tests=CAPTURED_PASSED_TESTS,
        )
        self.assertIn("skipped doctest coverage is not allowed in CI", output)
        self._assert_parity(rc, BASELINE_RC_STRICT_FAIL, kwargs, lanes, results)

    def test_skip_markers_are_reported_not_dropped(self) -> None:
        # Same capture, advisory lane, no CI: the lane passes and the ledger must
        # still carry the skip count.
        lanes = [("AdvisoryLane", False)]
        results = _godot(True, False, PASS_WITH_SKIP_OUTPUT, 0)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output,
            "AdvisoryLane",
            outcome="PASS",
            skipped_markers=CAPTURED_SKIP_MARKERS,
            passed_tests=CAPTURED_PASSED_TESTS,
        )
        self._assert_parity(rc, BASELINE_RC_PASS, {}, lanes, results)


class LaneLedgerTotalityTests(unittest.TestCase):
    maxDiff = None

    def test_every_module_test_filter_lane_is_recorded(self) -> None:
        """Completeness against MODULE_TEST_FILTERS itself, not a copied list."""
        lanes = [(name, strict) for name, _f, _e, strict in harness.MODULE_TEST_FILTERS]
        rc, output = _drive(lanes, _godot(True, False, PASS_OUTPUT, 0))
        recorded = _record_order(output)
        expected = [name for name, _f, _e, _s in harness.MODULE_TEST_FILTERS]
        self.assertEqual(
            recorded,
            expected,
            "the ledger must carry exactly one record per MODULE_TEST_FILTERS lane, in order",
        )
        totals = _aggregate(output)
        self.assertEqual(totals["lanes"], len(harness.MODULE_TEST_FILTERS))
        self.assertEqual(
            totals["strict_lanes"] + totals["advisory_lanes"], len(harness.MODULE_TEST_FILTERS)
        )
        self.assertEqual(totals["not_run"], 0)
        self.assertEqual(rc, BASELINE_RC_PASS)

    def test_mixed_outcomes_are_each_recorded_once(self) -> None:
        lanes = [
            ("PassLane", True),
            ("AdvisoryFailLane", False),
            ("NoCoverageLane", False),
            ("UnavailableLane", True),
        ]
        results = [
            _godot(True, False, PASS_OUTPUT, 0),
            _godot(False, False, FAIL_OUTPUT, 1),
            _godot(True, False, NO_COVERAGE_OUTPUT, 0),
            _godot(True, True, UNAVAILABLE_OUTPUT, 1),
        ]
        rc, output = _drive(lanes, results)
        self.assertEqual(
            _record_order(output),
            ["PassLane", "AdvisoryFailLane", "NoCoverageLane", "UnavailableLane"],
        )
        records = _records(output)
        self.assertEqual(records["PassLane"]["outcome"], "PASS")
        self.assertEqual(records["AdvisoryFailLane"]["outcome"], "ADVISORY-FAIL")
        self.assertEqual(records["NoCoverageLane"]["outcome"], "ADVISORY-NO-COVERAGE")
        self.assertEqual(records["UnavailableLane"]["outcome"], "UNAVAILABLE")
        totals = _aggregate(output)
        self.assertEqual(totals["lanes"], 4)
        self.assertEqual(totals["advisory_failures"], 1)
        self.assertEqual(totals["advisory_zero_coverage"], 1)
        self.assertEqual(totals["unavailable"], 1)
        self.assertEqual(totals["passed"], 1)
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_lanes_after_an_aborting_strict_failure_are_recorded_not_run(self) -> None:
        lanes = [("PassLane", True), ("StrictFailLane", True), ("NeverReached", False)]
        results = [
            _godot(True, False, PASS_OUTPUT, 0),
            _godot(False, False, FAIL_OUTPUT, 1),
            _godot(True, False, PASS_OUTPUT, 0),
        ]
        rc, output = _drive(lanes, results)
        records = _records(output)
        self.assertEqual(records["StrictFailLane"]["outcome"], "FAIL")
        self.assertEqual(
            records["NeverReached"]["outcome"],
            "NOT-RUN",
            "a lane the runner never reached must be printed as NOT-RUN, never omitted",
        )
        self.assertEqual(records["NeverReached"]["passed_tests"], "-1")
        totals = _aggregate(output)
        self.assertEqual(totals["not_run"], 1)
        self.assertEqual(totals["lanes"], 3)
        self.assertEqual(rc, BASELINE_RC_STRICT_FAIL)

    def test_aggregate_line_is_printed_when_nothing_is_red(self) -> None:
        """TRUTHFUL ZERO: absence of output must never read as absence of failures."""
        rc, output = _drive([("PassLane", True)], _godot(True, False, PASS_OUTPUT, 0))
        totals = _aggregate(output)
        self.assertEqual(totals["advisory_failures"], 0)
        self.assertEqual(_advisory_red(output), {})
        self.assertEqual(rc, BASELINE_RC_PASS)

    def test_missing_record_is_an_integrity_failure(self) -> None:
        """A path that forgets to record must fail, not pass quietly.

        Simulated at the seam a real regression would hit: a lane result that
        never reaches the ledger.
        """
        original = harness.LaneLedger.record

        def skip_unavailable(self, index, result, *, ended_run):
            if result.outcome == harness.LANE_OUTCOME_UNAVAILABLE:
                return
            original(self, index, result, ended_run=ended_run)

        lanes = [("UnavailableLane", True), ("PassLane", True)]
        results = [
            _godot(True, True, UNAVAILABLE_OUTPUT, 1),
            _godot(True, False, PASS_OUTPUT, 0),
        ]
        with mock.patch.object(harness.LaneLedger, "record", skip_unavailable):
            rc, output = _drive(lanes, results)
        self.assertEqual(_records(output)["UnavailableLane"]["outcome"], "NOT-RUN")
        self.assertIn("[module-tests][lane-ledger][INTEGRITY]", output)
        self.assertIn("'UnavailableLane' was attempted but produced no ledger record", output)
        self.assertNotEqual(rc, 0, "an incomplete ledger must not report success")

    def test_a_missing_record_before_an_abort_is_still_an_integrity_failure(self) -> None:
        """#822 round 5: an abort must not excuse the records already collected.

        Validation used to be skipped wholesale whenever the run aborted, so a
        lane whose record went missing BEFORE the aborting lane escaped the check
        entirely - the "did not run reads as passed" hole reopened for exactly
        the runs where something had already gone wrong.
        """
        original = harness.LaneLedger.record

        def skip_first_lane(self, index, result, *, ended_run):
            if index == 0:
                return
            original(self, index, result, ended_run=ended_run)

        lanes = [("AdvisoryLane", False), ("StrictLane", True), ("NeverReached", False)]
        results = [
            _godot(False, False, FAIL_OUTPUT, 1),  # advisory red, run continues
            _godot(False, False, FAIL_OUTPUT, 1),  # strict lane aborts the run
            _godot(True, False, PASS_OUTPUT, 0),
        ]
        with mock.patch.object(harness.LaneLedger, "record", skip_first_lane):
            rc, output = _drive(lanes, results)
        self.assertIn(
            "'AdvisoryLane' was attempted but produced no ledger record",
            output,
            "a lane attempted before the abort must still be validated",
        )
        self.assertNotIn(
            "'NeverReached' was attempted",
            output,
            "a lane the runner never reached must NOT be reported as missing",
        )
        self.assertEqual(_records(output)["NeverReached"]["outcome"], "NOT-RUN")
        self.assertNotEqual(rc, 0)

    def test_a_lane_cannot_be_recorded_twice(self) -> None:
        """An overwrite is how a FAIL would become a PASS."""
        lanes = [("AdvisoryLane", False)]
        results = _godot(False, False, FAIL_OUTPUT, 1)
        buffer = io.StringIO()
        with mock.patch.dict(os.environ, {"CI": ""}):
            with mock.patch.object(harness, "_run_godot", return_value=results):
                with mock.patch.object(harness, "_load_quarantine", return_value={}):
                    ledger = harness.LaneLedger([("AdvisoryLane", False)])
                    ledger.record(
                        0,
                        harness.LaneResult(outcome=harness.LANE_OUTCOME_ADVISORY_FAIL),
                        ended_run=False,
                    )
                    ledger.record(
                        0,
                        harness.LaneResult(outcome=harness.LANE_OUTCOME_PASS),
                        ended_run=False,
                    )
                    with contextlib.redirect_stdout(buffer):
                        ledger.print_block()
        self.assertEqual(_records(buffer.getvalue())["AdvisoryLane"]["outcome"], "ADVISORY-FAIL")
        errors = ledger.check_integrity(attempted_lanes=1)
        self.assertTrue(errors, "recording a lane twice must be an integrity error")
        self.assertIn("recorded twice", errors[0])
        # Non-vacuous under the anti-vacuous mutation too: also drive the loop.
        rc, output = _drive(lanes, results)
        self.assertEqual(_records(output)["AdvisoryLane"]["outcome"], "ADVISORY-FAIL")
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_declared_lanes_absent_from_the_run_list_are_reported(self) -> None:
        full = [(name, [], strict) for name, _f, _e, strict in harness.MODULE_TEST_FILTERS]
        self.assertEqual(harness._lane_runs_missing_from_module_filters(full, False), [])
        dropped = full[1:]
        errors = harness._lane_runs_missing_from_module_filters(dropped, False)
        self.assertTrue(errors, "a lane that vanished from the run list must be reported")
        self.assertIn(harness.MODULE_TEST_FILTERS[0][0], errors[0])
        # Drive the loop as well, so this method cannot pass with the lane loop
        # stubbed out.
        rc, output = _drive([("PassLane", True)], _godot(True, False, PASS_OUTPUT, 0))
        self.assertEqual(_records(output)["PassLane"]["outcome"], "PASS")
        self.assertEqual(rc, BASELINE_RC_PASS)

    def test_the_gpu_lanes_are_covered_by_the_same_totality_check(self) -> None:
        """The GPU lanes escaped the check they exist inside (#822 round 11).

        `_build_module_test_runs(run_gpu=True)` appends REQUIRES_RD_TEST_FILTERS,
        but the coverage check only ever iterated MODULE_TEST_FILTERS - so
        deleting or breaking that append left the "every declared lane is in the
        run list" verdict GREEN on exactly the configuration where those lanes
        are supposed to run. The totality guarantee this whole change exists to
        build was unenforced for a quarter of the lane surface's entry points.

        The mutation is the append being removed, which is precisely
        `_build_module_test_runs(False)` evaluated where `run_gpu` is True.
        """
        self.assertTrue(
            harness.REQUIRES_RD_TEST_FILTERS,
            "sanity: there must be GPU lanes for this to be about anything",
        )
        gpu_names = {name for name, *_ in harness.REQUIRES_RD_TEST_FILTERS}
        headless_names = {name for name, *_ in harness.MODULE_TEST_FILTERS}
        self.assertFalse(
            gpu_names & headless_names,
            "sanity: a GPU lane that is also a headless lane would be covered by "
            "accident, which would make this proof vacuous",
        )

        with_gpu = harness._build_module_test_runs(True)
        self.assertEqual(
            harness._lane_runs_missing_from_module_filters(with_gpu, True),
            [],
            "the unmutated GPU run list must be complete",
        )

        # THE MUTATION: the `if run_gpu:` append is gone. Before round 11 this
        # assertion read `[]`.
        without_the_append = harness._build_module_test_runs(False)
        errors = harness._lane_runs_missing_from_module_filters(without_the_append, True)
        self.assertTrue(
            errors,
            "a GPU run that stopped building the requires-RD lanes must be an "
            "integrity failure, not a silently shorter run",
        )
        for name in sorted(gpu_names):
            self.assertIn(name, errors[0])

        # ... and the headless run must NOT be made red by the same list: the
        # requirement follows run_gpu, so this cannot be "fixed" by demanding the
        # GPU lanes unconditionally.
        self.assertEqual(
            harness._lane_runs_missing_from_module_filters(without_the_append, False),
            [],
            "a headless run does not build the GPU lanes and must not be failed "
            "for their absence",
        )

    def test_main_fails_when_a_gpu_run_loses_the_requires_rd_lanes(self) -> None:
        """End to end through main(), because the call site is what passes run_gpu.

        A check that takes the flag is worthless if main() forgets to hand it
        over, so the mutation is driven through the real entry point with
        `--gpu`: `_build_module_test_runs` is replaced by one that never appends
        the GPU lanes, exactly as deleting the `if run_gpu:` block would.
        """
        original = harness._build_module_test_runs
        with mock.patch.object(
            harness, "_build_module_test_runs", lambda _gpu: original(False)
        ):
            rc_gpu = _run_main(
                ["run_module_tests.py", "--godot-binary", "fake", "--gpu"]
            )
        self.assertEqual(
            rc_gpu,
            1,
            "a --gpu run whose requires-RD lanes vanished must fail the integrity "
            "check instead of reporting a complete 26-lane ledger",
        )


class LaneReportTests(unittest.TestCase):
    maxDiff = None

    def test_json_report_matches_the_printed_records(self) -> None:
        lanes = [("PassLane", True), ("AdvisoryFailLane", False)]
        results = [
            _godot(True, False, PASS_OUTPUT, 0),
            _godot(False, False, FAIL_OUTPUT, 1),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "lane_ledger.json"
            rc, output = _drive(lanes, results, lane_report=report)
            self.assertTrue(report.is_file(), "--lane-report must write the file")
            payload = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(payload["schema_version"], harness.LANE_LEDGER_SCHEMA_VERSION)
        self.assertIn("baseline_note", payload)
        self.assertEqual([lane["lane"] for lane in payload["lanes"]], _record_order(output))
        by_lane = {lane["lane"]: lane for lane in payload["lanes"]}
        self.assertEqual(by_lane["AdvisoryFailLane"]["outcome"], "ADVISORY-FAIL")
        self.assertEqual(by_lane["AdvisoryFailLane"]["failed_tests"], 1)
        self.assertTrue(by_lane["AdvisoryFailLane"]["advisory_red"])
        self.assertEqual(by_lane["PassLane"]["outcome"], "PASS")
        # The JSON totals are a strict SUPERSET of the printed aggregate: every
        # field on stdout must be present and identical, and the JSON carries
        # the strict/advisory split of fail_outcomes that the one-line
        # aggregate has no room for. Superset, never contradiction.
        printed = _aggregate(output)
        json_totals = payload["totals"]
        for key, value in printed.items():
            self.assertIn(key, json_totals, f"stdout reports {key} but the JSON does not")
            self.assertEqual(
                json_totals[key], value, f"{key} differs between stdout and the JSON report"
            )
        self.assertEqual(
            set(json_totals) - set(printed),
            {"fail_outcomes_on_strict_lanes", "fail_outcomes_on_advisory_lanes"},
            "the JSON may only ADD the FAIL-outcome split to the printed aggregate",
        )
        self.assertEqual(
            json_totals["fail_outcomes_on_strict_lanes"]
            + json_totals["fail_outcomes_on_advisory_lanes"],
            json_totals["fail_outcomes"],
            "the split must account for exactly the FAIL outcomes, no more, no less",
        )
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_report_does_not_claim_the_run_exited_zero(self) -> None:
        """#822 P2-3: an advisory red plus a LATER strict failure.

        The loop continues past ADVISORY-FAIL, so the run can still exit 1 while
        the report on disk holds the advisory record. A note asserting "CI
        exited 0" is a run-wide claim the record cannot make; the true and
        stable claim is about the advisory RESULT not itself failing the run.
        """
        lanes = [("AdvisoryLane", False), ("StrictLane", True)]
        results = [
            _godot(False, False, FAIL_OUTPUT, 1),  # advisory red, run continues
            _godot(False, False, FAIL_OUTPUT, 1),  # strict lane then fails the run
        ]
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "lane_ledger.json"
            rc, output = _drive(lanes, results, lane_report=report)
            payload = json.loads(report.read_text(encoding="utf-8"))

        self.assertEqual(rc, 1, "the later strict lane must fail the run")
        self.assertEqual(_advisory_red(output), {"AdvisoryLane": "failed"})
        by_lane = {lane["lane"]: lane for lane in payload["lanes"]}
        self.assertTrue(by_lane["AdvisoryLane"]["advisory_red"])
        self.assertEqual(by_lane["StrictLane"]["outcome"], "FAIL")

        note = payload["baseline_note"]
        for claim in ("exited 0", "exits 0", "still exit"):
            self.assertNotIn(
                claim,
                note,
                f"baseline_note asserts a run-wide outcome ({claim!r}) that a later "
                f"lane can invalidate; this very report accompanies a run that "
                f"exited {rc}",
            )
        self.assertIn(
            "did not itself fail the run",
            note,
            "the note must describe the advisory RESULT, which is stable, rather than "
            "the run's final exit code, which is not known when the record is made",
        )
        self.assertEqual(
            payload["lane_loop_exit_code"],
            1,
            "the report must carry what the lane loop actually returned",
        )

    def test_a_freshly_written_report_agrees_with_the_process(self) -> None:
        """The write ordering, asserted rather than described (#822 round 10).

        `build-test-ci.md` used to justify the narrow name of
        `lane_loop_exit_code` by claiming the report is written BEFORE the
        integrity check, so either could still change the exit code afterwards.
        The order is the reverse - integrity is checked first and an untrustworthy
        ledger is never written - so after a write that succeeds nothing else
        moves the exit code, and a freshly written report and its process agree.
        That is the property the corrected paragraph states, so it is pinned here:
        a reader who trusts the doc is trusting this assertion.

        The narrow name still earns itself in the cases where no FRESH report
        exists: integrity failure (test above: the previous report is left
        untouched) and a failed write (test_an_interrupted_run_leaves_the_previous
        _report_intact). There the process exits nonzero while the path holds an
        older run's file, which is why `generated_utc` is in the payload.
        """
        for label, godot_result, expected_rc in (
            ("clean run", _godot(True, False, PASS_OUTPUT, 0), 0),
            ("gated run", _godot(False, False, FAIL_OUTPUT, 1), 1),
        ):
            with self.subTest(case=label), tempfile.TemporaryDirectory() as tmp:
                report = Path(tmp) / "lane_ledger.json"
                rc = _run_main(
                    [
                        "run_module_tests.py",
                        "--godot-binary",
                        "fake",
                        "--lane-report",
                        str(report),
                    ],
                    godot_result=godot_result,
                )
                self.assertTrue(report.is_file(), "the report must have been written")
                payload = json.loads(report.read_text(encoding="utf-8"))
                self.assertEqual(rc, expected_rc, "sanity: the scenario is the one asked for")
                self.assertEqual(
                    payload["lane_loop_exit_code"],
                    rc,
                    "nothing after a successful write may change the exit code; if "
                    "something now can, the reference's ordering paragraph is wrong "
                    "again",
                )

    def test_a_directory_destination_fails_before_any_lane_runs(self) -> None:
        """The preflight must reject it, not os.replace() 26 lanes later."""
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / "reports"
            directory.mkdir()
            with mock.patch.object(harness, "_run_godot") as godot:
                rc = _run_main(
                    [
                        "run_module_tests.py",
                        "--godot-binary",
                        "fake",
                        "--lane-report",
                        str(directory),
                    ]
                )
            self.assertEqual(rc, 1, "a directory destination must fail the run")
            self.assertEqual(
                godot.call_count,
                0,
                "no lane may be executed once the destination is known to be invalid",
            )

    def test_an_untrustworthy_ledger_does_not_overwrite_the_previous_report(self) -> None:
        """#822 round 5: atomicity is not the same guarantee as worth-keeping.

        temp -> os.replace guarantees the destination is never empty or partial.
        It does not decide whether a ledger that failed its OWN integrity check
        deserves to replace the last valid measurement. It does not.
        """
        original = harness.LaneLedger.record

        def skip_first_lane(self, index, result, *, ended_run):
            if index == 0:
                return
            original(self, index, result, ended_run=ended_run)

        lanes = [("AdvisoryLane", False), ("PassLane", True)]
        results = [
            _godot(False, False, FAIL_OUTPUT, 1),
            _godot(True, False, PASS_OUTPUT, 0),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "lane_ledger.json"
            dest.write_text(PRIOR_REPORT, encoding="utf-8")
            with mock.patch.object(harness.LaneLedger, "record", skip_first_lane):
                rc, output = _drive(lanes, results, lane_report=dest)
            surviving = dest.read_text(encoding="utf-8")
            leftovers = sorted(p.name for p in Path(tmp).iterdir())

        self.assertNotEqual(rc, 0, "an incomplete ledger must not report success")
        self.assertEqual(
            surviving,
            PRIOR_REPORT,
            "a ledger that failed its own integrity check overwrote the last valid "
            "measurement",
        )
        self.assertEqual(leftovers, ["lane_ledger.json"], "no temp file may survive")
        self.assertIn("refusing to write", output)
        self.assertIn(
            "[module-tests][lane-ledger][INTEGRITY]",
            output,
            "a report that was NOT written must say so; a silently missing report is "
            "the same absence-reads-as-success defect",
        )
        # The evidence is not lost: the full block is still on stdout.
        self.assertEqual(_records(output)["PassLane"]["outcome"], "PASS")

    def test_unwritable_report_path_fails_the_run(self) -> None:
        lanes = [("PassLane", True)]
        results = _godot(True, False, PASS_OUTPUT, 0)
        with tempfile.TemporaryDirectory() as tmp:
            unwritable = Path(tmp) / "no-such-dir" / "lane_ledger.json"
            rc, output = _drive(lanes, results, lane_report=unwritable)
        self.assertNotEqual(
            rc, 0, "a ledger that could not be persisted must not report success"
        )
        self.assertIn("--lane-report could not be written", output)
        self.assertEqual(
            _records(output)["PassLane"]["outcome"],
            "PASS",
            "the lane records are still printed when the report write fails",
        )

    def test_a_read_only_destination_fails_before_any_lane_runs(self) -> None:
        """#822 round 9: the second class the sibling probe cannot see.

        The probe asks "can I create a file NEXT TO this path". An existing
        read-only destination is not a directory and its parent still accepts a
        temp file, so the probe passed and the run died in `os.replace()` after
        26 lanes - the exact cost the preflight exists to avoid, for the second
        input class in a row.
        """
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "lane_ledger.json"
            dest.write_text(PRIOR_REPORT, encoding="utf-8")
            self.assertEqual(
                harness._preflight_lane_report_path(dest),
                [],
                "sanity: a writable existing destination is accepted",
            )
            os.chmod(dest, stat.S_IREAD)
            try:
                if os.access(dest, os.W_OK):
                    # Running as root (or a filesystem that ignores the bit):
                    # the condition cannot be created, so asserting on it would
                    # measure the environment rather than the code.
                    self.skipTest(
                        "this platform/user cannot make a file unwritable; the "
                        "read-only destination class is unreachable here"
                    )
                errors = harness._preflight_lane_report_path(dest)
                self.assertTrue(
                    errors,
                    "a destination that cannot be replaced must be rejected BEFORE "
                    "the lanes run, not by os.replace() after them",
                )
                self.assertIn("not writable", errors[0])
                self.assertEqual(
                    dest.read_text(encoding="utf-8"),
                    PRIOR_REPORT,
                    "the rejection must not touch the previous measurement",
                )
                self.assertEqual(
                    sorted(p.name for p in Path(tmp).iterdir()),
                    ["lane_ledger.json"],
                    "the rejected preflight must leave no scratch file behind",
                )
                # End to end: main() refuses before a single lane is executed.
                with mock.patch.object(harness, "_run_godot") as godot:
                    rc = _run_main(
                        [
                            "run_module_tests.py",
                            "--godot-binary",
                            "fake",
                            "--lane-report",
                            str(dest),
                        ]
                    )
                self.assertEqual(rc, 1, "a non-replaceable destination must fail the run")
                self.assertEqual(
                    godot.call_count,
                    0,
                    "no lane may run once the destination is known to be invalid",
                )
            finally:
                os.chmod(dest, stat.S_IWRITE)

    def test_preflight_rejects_an_unwritable_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp) / "ok.json"
            self.assertEqual(harness._preflight_lane_report_path(good), [])
            bad = Path(tmp) / "missing" / "nope.json"
            errors = harness._preflight_lane_report_path(bad)
            self.assertTrue(errors, "an unwritable --lane-report path must be rejected early")
            self.assertIn("not writable", errors[0])
            self.assertEqual(
                sorted(p.name for p in Path(tmp).iterdir()),
                [],
                "the writability probe must not leave scratch files behind",
            )

            # A DIRECTORY destination: the sibling probe succeeds for it, because
            # "can I write a file next to this path" is a different question from
            # "can I write this path". Without an explicit check the run does all
            # 26 lanes and only then dies in os.replace() - which is precisely the
            # cost the preflight exists to avoid.
            directory = Path(tmp) / "reports"
            directory.mkdir()
            dir_errors = harness._preflight_lane_report_path(directory)
            self.assertTrue(
                dir_errors,
                "a directory --lane-report destination must be rejected BEFORE the "
                "lanes run, not by os.replace() after them",
            )
            self.assertIn("is a directory", dir_errors[0])
            self.assertEqual(
                sorted(p.name for p in directory.iterdir()),
                [],
                "the rejected probe must leave nothing inside the directory",
            )
        # Also drive the loop, so this method is not vacuous when the lane loop
        # is stubbed out.
        rc, output = _drive([("PassLane", True)], _godot(True, False, PASS_OUTPUT, 0))
        self.assertEqual(_records(output)["PassLane"]["outcome"], "PASS")
        self.assertEqual(rc, BASELINE_RC_PASS)

    def test_preflight_does_not_truncate_an_existing_report(self) -> None:
        """The writability probe must not destroy the previous measurement.

        The first implementation opened the DESTINATION in "w" mode, which
        truncated the last valid report at second zero. This report is the
        evidence the whole slice exists to produce, so a check that could only
        ever confirm "we could have written something" must not consume the
        thing it is protecting.
        """
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "lane_ledger.json"
            dest.write_text(PRIOR_REPORT, encoding="utf-8")
            self.assertEqual(harness._preflight_lane_report_path(dest), [])
            self.assertEqual(
                dest.read_text(encoding="utf-8"),
                PRIOR_REPORT,
                "the preflight truncated an existing lane report",
            )
            self.assertEqual(
                sorted(p.name for p in Path(tmp).iterdir()),
                ["lane_ledger.json"],
                "the probe must be cleaned up and must not be the destination",
            )

    def test_an_interrupted_run_leaves_the_previous_report_intact(self) -> None:
        """Three ways a run can end before _write_lane_report(); all must preserve it."""
        # (a) main() aborts on the run-list integrity check, AFTER the preflight
        #     and BEFORE any lane runs.
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "lane_ledger.json"
            dest.write_text(PRIOR_REPORT, encoding="utf-8")
            rc = _run_main(
                ["run_module_tests.py", "--godot-binary", "fake", "--lane-report", str(dest)],
                drop_lane=True,
            )
            self.assertEqual(rc, 1, "a dropped lane must fail the run")
            self.assertEqual(
                dest.read_text(encoding="utf-8"),
                PRIOR_REPORT,
                "an aborted run destroyed the previous measurement",
            )

        # (b) the lane loop itself dies part-way through.
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "lane_ledger.json"
            dest.write_text(PRIOR_REPORT, encoding="utf-8")
            boom = RuntimeError("lane runner died")
            with mock.patch.object(harness, "_execute_lane", side_effect=boom):
                with self.assertRaises(RuntimeError):
                    _drive(
                        [("PassLane", True)],
                        _godot(True, False, PASS_OUTPUT, 0),
                        lane_report=dest,
                    )
            self.assertEqual(
                dest.read_text(encoding="utf-8"),
                PRIOR_REPORT,
                "a crash mid-run destroyed the previous measurement",
            )

        # (c) the payload cannot be serialized: nothing may touch the filesystem.
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "lane_ledger.json"
            dest.write_text(PRIOR_REPORT, encoding="utf-8")
            errors = harness._write_lane_report(dest, {"lanes": {1, 2, 3}})
            self.assertTrue(errors, "an unserializable payload must be reported")
            self.assertEqual(
                dest.read_text(encoding="utf-8"),
                PRIOR_REPORT,
                "a serialisation failure destroyed the previous measurement",
            )
            self.assertEqual(
                sorted(p.name for p in Path(tmp).iterdir()),
                ["lane_ledger.json"],
                "a failed write must not leave a temp file beside the report",
            )

    def test_a_successful_write_replaces_the_previous_report(self) -> None:
        """The other half of atomicity: it must still actually overwrite."""
        lanes = [("PassLane", True)]
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "lane_ledger.json"
            dest.write_text(PRIOR_REPORT, encoding="utf-8")
            rc, output = _drive(lanes, _godot(True, False, PASS_OUTPUT, 0), lane_report=dest)
            payload = json.loads(dest.read_text(encoding="utf-8"))
            self.assertEqual(
                sorted(p.name for p in Path(tmp).iterdir()),
                ["lane_ledger.json"],
                "no temp file may survive a successful write",
            )
        self.assertEqual([lane["lane"] for lane in payload["lanes"]], ["PassLane"])
        self.assertEqual(_records(output)["PassLane"]["outcome"], "PASS")
        self.assertEqual(rc, BASELINE_RC_PASS)

    def test_lane_report_is_rejected_with_guard_only(self) -> None:
        """An empty ledger from a lane-less run reads as 'nothing failed'.

        This method never reaches the lane loop by construction; it is
        mutation-proven by deleting the parser.error() it pins.
        """
        argv = ["run_module_tests.py", "--guard-only", "--lane-report", "x.json"]
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", argv):
            with contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as caught:
                    harness._parse_args()
        self.assertNotEqual(caught.exception.code, 0)
        # Named reason, not just "some nonzero exit": on a tree without the flag
        # argparse also exits nonzero, for the unrelated reason that the option
        # does not exist.
        self.assertIn("--lane-report cannot be combined with --guard-only", stderr.getvalue())
        # ... and the flag is accepted on its own.
        with mock.patch.object(sys, "argv", ["run_module_tests.py", "--lane-report", "x.json"]):
            self.assertEqual(harness._parse_args().lane_report, "x.json")


class CapturedFixtureContractTests(unittest.TestCase):
    """What the checked-in capture guarantees, stated exactly.

    THIS CLASS DETECTS:
      - an edit to the capture (`doctest_env_skip_sample.txt`);
      - an edit to `_parse_doctest_results()` that changes how the capture parses;
      - a renderer in this file that stops reproducing the capture byte for byte.

    IT DOES NOT DETECT producer drift on its own. Every assertion here reads a
    checked-in snapshot, so a change to the vendored `ConsoleReporter` alone
    leaves them all green. ProducerContractTests below adds the missing link;
    the residual limitation is declared there.

    Round 6 claimed this class made "a doctest format change break here, once".
    That was false, it was repeated to the owner, and a false claim of protection
    is worse than no claim because it stops the next person looking.
    """

    maxDiff = None

    def test_captured_sample_parses_to_the_values_the_helpers_assume(self) -> None:
        (
            passed_tests,
            failed_tests,
            passed_asserts,
            failed_asserts,
            skip_markers,
            summary_found,
        ) = harness._parse_doctest_results(CAPTURED_SAMPLE)
        self.assertTrue(
            summary_found,
            f"the runner no longer finds a summary in a real capture; recapture "
            f"with: {CAPTURE_COMMAND}",
        )
        self.assertEqual(passed_tests, CAPTURED_PASSED_TESTS)
        self.assertEqual(failed_tests, CAPTURED_FAILED_TESTS)
        self.assertEqual(passed_asserts, CAPTURED_PASSED_ASSERTIONS)
        self.assertEqual(failed_asserts, CAPTURED_FAILED_ASSERTIONS)
        self.assertEqual(
            skip_markers,
            CAPTURED_SKIP_MARKERS,
            "the skip-marker detector no longer counts the real captured markers",
        )

    def test_captured_summary_carries_the_framing_the_old_fixtures_omitted(self) -> None:
        """Pins the specific ways the manufactured shape was wrong.

        Without this, "anchored to a capture" could quietly decay back into a
        hand-authored string that merely looks plausible.
        """
        self.assertRegex(
            CAPTURED_TEST_CASES_LINE,
            r"\|\s*\d+\s*skipped\s*$",
            "real doctest reports a fourth `| N skipped` column on the test-cases "
            "line; the manufactured fixture had no such column",
        )
        self.assertTrue(
            CAPTURED_ASSERTIONS_LINE.rstrip().endswith("|"),
            "real doctest leaves a TRAILING `|` on the assertions line; the "
            "manufactured fixture ended at `N failed`",
        )
        self.assertIn(
            "MESSAGE:",
            CAPTURED_SKIP_MARKER_LINE,
            "the captured marker must carry doctest's file(line): MESSAGE: framing",
        )
        self.assertNotRegex(
            CAPTURED_SKIP_MARKER_LINE,
            r"^\s*(?:GS_ENV_SKIP|Skip)",
            "a marker can never start a line: log_message() emits the file/line "
            "header first",
        )

    def test_renderer_reproduces_the_capture_byte_for_byte(self) -> None:
        """The anchor: our formatter IS doctest's, proven against real output.

        Given the capture's own counts, `_summary()` must reproduce the captured
        lines exactly - padding included. Everything else in this file that uses
        a rendered summary rests on this one assertion.
        """
        rendered = _summary(
            CAPTURED_PASSED_TESTS,
            CAPTURED_FAILED_TESTS,
            CAPTURED_PASSED_ASSERTIONS,
            CAPTURED_FAILED_ASSERTIONS,
            filtered_out=CAPTURED_FILTERED_OUT,
        ).splitlines()
        self.assertEqual(rendered[0], CAPTURED_TEST_CASES_LINE)
        self.assertEqual(rendered[1], CAPTURED_ASSERTIONS_LINE)

    def test_column_widths_follow_the_counts_not_the_capture(self) -> None:
        """Padding is DERIVED, as doctest derives it - not inherited from the capture.

        Round 6 re-spelled the captured digits and kept its two-character
        columns, so a 120-assertion lane was rendered with the padding of a
        69-assertion one. `_parse_doctest_results()` accepts either, and the old
        skeleton test compared the result back against that same stale padding,
        so nothing could see it.
        """
        # max(5, 120) + 1 -> 3 columns, where the capture uses 2.
        wide = _summary(5, 0, 120, 0).splitlines()
        self.assertEqual(wide[0], "[doctest] test cases:   5 |   5 passed | 0 failed | 2067 skipped")
        self.assertEqual(wide[1], "[doctest] assertions: 120 | 120 passed | 0 failed |")
        self.assertNotEqual(
            _grammar_padding(wide[0]),
            _grammar_padding(CAPTURED_TEST_CASES_LINE),
            "a 120-assertion summary must NOT carry the capture's 69-assertion padding",
        )
        # A failing shape widens the third column, which is 0-wide in the capture.
        mixed = _summary(2, 1, 9, 1).splitlines()
        self.assertEqual(mixed[0], "[doctest] test cases:  3 | 2 passed | 1 failed | 2069 skipped")
        self.assertEqual(mixed[1], "[doctest] assertions: 10 | 9 passed | 1 failed |")

    def test_generated_summaries_carry_the_counts_they_were_asked_for(self) -> None:
        """The substitution must not silently mis-place a number."""
        parsed = harness._parse_doctest_results(_summary(2, 1, 9, 1))
        self.assertEqual(parsed[:5], (2, 1, 9, 1, 0))
        parsed_empty = harness._parse_doctest_results(_summary(1, 0, 0, 0))
        self.assertEqual(parsed_empty[:5], (1, 0, 0, 0, 0))

    def test_the_skipped_column_moves_with_the_selected_count(self) -> None:
        """A fixture must describe ONE binary, not a different one per shape.

        `_summary()` defaulted `filtered_out` to the capture's 2063 for every
        fixture, so `PASS_OUTPUT` reported 5 selected + 2063 skipped while the
        capture in the same file reports 9 + 2063. doctest computes the column as
        `numTestCases - numTestCasesPassingFilters`, so those two lines cannot
        both come from the same binary - and no real binary emits the first at
        all. `_parse_doctest_results()` ignores the column, so nothing here could
        go red over it: a fixture whose only certificate was itself.

        Asserted over the shipped fixtures, not over freshly built ones, because
        the shipped fixtures are what every outcome test in this file feeds the
        runner. Against round 10 every one of these fails.
        """
        self.assertEqual(
            CAPTURED_TOTAL_CASES,
            2072,
            "sanity: the capture's own 9 selected + 2063 skipped",
        )
        selected_re = re.compile(
            r"\[doctest\] test cases:\s*(\d+) \|\s*\d+ passed \|\s*\d+ failed \|"
            r"\s*(\d+) skipped"
        )
        shapes = {
            "PASS_OUTPUT": PASS_OUTPUT,
            "FAIL_OUTPUT": FAIL_OUTPUT,
            "NO_COVERAGE_OUTPUT": NO_COVERAGE_OUTPUT,
            "ALL_FAILING_OUTPUT": ALL_FAILING_OUTPUT,
            "PASS_WITH_SKIP_OUTPUT": PASS_WITH_SKIP_OUTPUT,
        }
        seen_selected = set()
        for label, text in shapes.items():
            with self.subTest(fixture=label):
                match = selected_re.search(text)
                self.assertIsNotNone(
                    match, f"{label} has no parseable test-cases line"
                )
                selected, skipped = int(match.group(1)), int(match.group(2))
                seen_selected.add(selected)
                self.assertEqual(
                    selected + skipped,
                    CAPTURED_TOTAL_CASES,
                    f"{label} claims a {selected + skipped}-case binary while the "
                    f"capture reports {CAPTURED_TOTAL_CASES}; doctest's skipped "
                    f"column is numTestCases - numTestCasesPassingFilters, so it "
                    f"MUST move with the selected count",
                )
        self.assertGreater(
            len(seen_selected),
            1,
            "sanity: the fixtures must select different numbers of cases, or a "
            "constant skipped column would satisfy this vacuously",
        )

    def test_a_summary_cannot_select_more_cases_than_the_binary_has(self) -> None:
        """The derivation fails closed rather than printing a negative column."""
        with self.assertRaises(ValueError):
            _summary(CAPTURED_TOTAL_CASES + 1, 0, 1, 0)

    def test_the_verbatim_capture_drives_the_skip_marker_path(self) -> None:
        """PASS_WITH_SKIP_OUTPUT is the capture itself, not a reconstruction."""
        self.assertEqual(PASS_WITH_SKIP_OUTPUT, CAPTURED_SAMPLE)
        rc, output = _drive([("AdvisoryLane", False)], _godot(True, False, CAPTURED_SAMPLE, 0))
        records = _records(output)
        self.assertEqual(records["AdvisoryLane"]["skipped_markers"], str(CAPTURED_SKIP_MARKERS))
        self.assertEqual(records["AdvisoryLane"]["passed_tests"], str(CAPTURED_PASSED_TESTS))
        self.assertEqual(
            records["AdvisoryLane"]["passed_assertions"], str(CAPTURED_PASSED_ASSERTIONS)
        )
        self.assertEqual(rc, BASELINE_RC_PASS)


class TestsUnavailableDetectionTests(unittest.TestCase):
    """Drive the unavailable path through the REAL detector, not around it.

    The outcome tests build their unavailable result with `_godot(True, True,
    ...)`, which sets `skipped=True` by hand and so never executes
    `_run_godot()` or `_tests_unavailable()`. That is convenient for asserting
    ledger behaviour and useless for asserting that the runner would ever
    classify a real binary's refusal as "tests unavailable" - the fixture was
    deciding the thing under test.

    DECLARED GAP: no tests-disabled binary exists in this repository, so the
    wording below is invented rather than captured. These tests assert only that
    production's own marker list recognises it and that the classification flows
    out of `_run_godot()`. Whether a real tests=no build emits matching wording
    is unverified and needs a non-test build (follow-up).
    """

    maxDiff = None

    def test_the_fixture_text_is_recognised_by_the_production_marker_list(self) -> None:
        self.assertTrue(
            harness._tests_unavailable(UNAVAILABLE_OUTPUT),
            "the invented unavailable fixture is not recognised by "
            "_tests_unavailable(); the outcome tests would then be asserting "
            "against text the runner cannot classify",
        )
        self.assertFalse(
            harness._tests_unavailable(PASS_OUTPUT),
            "a normal passing run must not be classified as tests-unavailable",
        )

    def test_run_godot_classifies_it_without_the_fixture_deciding(self) -> None:
        """skipped=True must come OUT of _run_godot, not be handed to it."""
        completed = subprocess.CompletedProcess(
            args=["godot"], returncode=1, stdout=UNAVAILABLE_OUTPUT, stderr=""
        )
        with mock.patch.object(subprocess, "run", return_value=completed):
            ok, skipped, output = harness._run_godot("godot", ["--headless", "--test"])
        self.assertTrue(ok, "a tests-unavailable binary is not a lane failure")
        self.assertTrue(
            skipped,
            "_run_godot must derive skipped=True from _tests_unavailable(output)",
        )
        self.assertEqual(output, UNAVAILABLE_OUTPUT)

    def test_a_nonzero_exit_without_a_marker_is_not_treated_as_unavailable(self) -> None:
        """The discriminating case: the detector must not swallow real failures."""
        completed = subprocess.CompletedProcess(
            args=["godot"], returncode=1, stdout="some other failure\n", stderr=""
        )
        with mock.patch.object(subprocess, "run", return_value=completed):
            ok, skipped, _output = harness._run_godot("godot", ["--headless", "--test"])
        self.assertFalse(ok)
        self.assertFalse(
            skipped,
            "an unrecognised nonzero exit is a FAILURE, not a tests-unavailable skip",
        )

    def test_the_unavailable_ledger_path_runs_on_a_detector_derived_result(self) -> None:
        """End to end: real detection feeding the real ledger."""
        completed = subprocess.CompletedProcess(
            args=["godot"], returncode=1, stdout=UNAVAILABLE_OUTPUT, stderr=""
        )
        buffer = io.StringIO()
        with mock.patch.dict(os.environ, {"CI": ""}):
            with mock.patch.object(subprocess, "run", return_value=completed):
                with mock.patch.object(harness, "_load_quarantine", return_value={}):
                    with contextlib.redirect_stdout(buffer):
                        rc = harness._run_doctest_lanes(
                            "godot",
                            [("StrictLane", ["--headless", "--test"], True)],
                            "warn-only",
                            False,
                        )
        output = buffer.getvalue()
        self.assertEqual(_records(output)["StrictLane"]["outcome"], "UNAVAILABLE")
        self.assertEqual(_aggregate(output)["unavailable"], 1)
        self.assertEqual(rc, BASELINE_RC_UNAVAILABLE_WARN)


class ProducerContractTests(unittest.TestCase):
    """Link the checked-in capture back to the producer that made it.

    A snapshot cannot notice the thing it is a snapshot OF changing. These
    assertions read `thirdparty/doctest/doctest.h` directly, so replacing or
    patching the vendored reporter fails HERE, with an instruction to recapture,
    instead of leaving every summary assertion in this file green against output
    the binary no longer produces.

    DECLARED LIMITATION - this is a source contract, not an execution one. It
    detects a version change and the removal or renaming of the format literals
    and width rule this file reimplements. It CANNOT detect a semantic change
    inside doctest that leaves all of those textually intact. Only running a real
    binary can close that, which needs a build and a lane run; #705's follow-up
    should either capture the summary shapes from a real failing run or assert
    the ledger against live lane output. Recorded rather than hidden, because the
    previous version of this file claimed a guarantee it did not have.
    """

    maxDiff = None

    def _reporter_source(self) -> str:
        self.assertTrue(
            VENDORED_DOCTEST_HEADER.is_file(),
            f"vendored doctest header missing at {VENDORED_DOCTEST_HEADER}",
        )
        return VENDORED_DOCTEST_HEADER.read_text(encoding="utf-8", errors="replace")

    def test_vendored_doctest_version_matches_the_capture(self) -> None:
        """A doctest upgrade must force a recapture rather than pass silently."""
        source = self._reporter_source()
        parts = []
        for macro in ("MAJOR", "MINOR", "PATCH"):
            match = re.search(rf"#define DOCTEST_VERSION_{macro}\s+(\d+)", source)
            self.assertIsNotNone(match, f"cannot read DOCTEST_VERSION_{macro}")
            parts.append(match.group(1))
        vendored = ".".join(parts)
        self.assertEqual(
            vendored,
            CAPTURED_DOCTEST_VERSION,
            f"vendored doctest is {vendored} but the capture was taken from "
            f"{CAPTURED_DOCTEST_VERSION}. Recapture with: {CAPTURE_COMMAND}",
        )
        # ... and the capture says so itself, so the two cannot drift apart
        # without one of these two assertions firing.
        self.assertIn(
            f'doctest version is "{CAPTURED_DOCTEST_VERSION}"',
            CAPTURED_SAMPLE,
            "the capture no longer records the doctest version it came from",
        )

    def test_summary_format_literals_still_exist_in_the_reporter(self) -> None:
        """The literals this file reimplements must still be the producer's."""
        source = self._reporter_source()
        for literal in (
            '"[doctest] "',
            '"test cases: "',
            '"assertions: "',
            '" passed"',
            '" failed"',
            '" skipped"',
        ):
            with self.subTest(literal=literal):
                self.assertIn(
                    literal,
                    source,
                    f"ConsoleReporter no longer emits {literal}; _summary() and "
                    f"_parse_doctest_results() were written against it. "
                    f"Recapture with: {CAPTURE_COMMAND}",
                )

    def test_reporter_still_derives_column_widths_from_the_counts(self) -> None:
        """The width rule `_doctest_column_width()` mirrors must still be there.

        If doctest stops computing widths this way, our renderer's padding is
        wrong even though every literal survived - which is exactly the class of
        drift round 7 found in the round-6 fix.
        """
        source = self._reporter_source()
        for expression in ("totwidth", "passwidth", "failwidth"):
            with self.subTest(width=expression):
                self.assertRegex(
                    source,
                    rf"auto\s+{expression}\s*=\s*int\(std::ceil\(log10\(",
                    f"ConsoleReporter no longer derives {expression} with "
                    f"ceil(log10(...)); _doctest_column_width() mirrors that rule. "
                    f"Recapture with: {CAPTURE_COMMAND}",
                )
        self.assertIn(
            "std::setw(totwidth)",
            source,
            "the computed width is no longer applied with std::setw",
        )

    def test_reporter_still_derives_skipped_from_registered_minus_selected(self) -> None:
        """The rule `CAPTURED_TOTAL_CASES` rests on, read from the producer.

        `_summary()` now derives the skipped column as
        `CAPTURED_TOTAL_CASES - selected`. That is only right while doctest
        computes it as `numTestCases - numTestCasesPassingFilters`; if the
        reporter starts reporting something else in that column, the derivation
        is wrong in a way no fixture assertion in this file could see.
        """
        source = self._reporter_source()
        self.assertRegex(
            source,
            r"numSkipped\s*=\s*p\.numTestCases\s*-\s*p\.numTestCasesPassingFilters",
            f"ConsoleReporter no longer computes the skipped column as "
            f"registered-minus-selected; CAPTURED_TOTAL_CASES and _summary()'s "
            f"derivation are written against that rule. "
            f"Recapture with: {CAPTURE_COMMAND}",
        )
        self.assertRegex(
            source,
            r"<<\s*numSkipped\s*\n?\s*<<\s*\" skipped\"",
            "the computed numSkipped is no longer what precedes the ' skipped' "
            "literal on the test-cases line",
        )


class DocConsistencyTests(unittest.TestCase):
    """The docs must not deny an exception the code implements (#822 P2-2)."""

    maxDiff = None

    def test_an_advisory_lane_exiting_zero_with_failures_does_gate(self) -> None:
        """The behavioural leg: the exception the docs must not deny is real."""
        lanes = [("AdvisoryLane", False)]
        # exit 0, but the doctest summary reports a failed test.
        results = _godot(True, False, FAIL_OUTPUT, 0)
        rc, output = _drive(lanes, results)
        self.assertEqual(
            _records(output)["AdvisoryLane"]["outcome"],
            "FAIL",
            "an advisory lane that exits 0 with a failing summary is recorded FAIL",
        )
        self.assertEqual(
            rc,
            1,
            "an advisory lane CAN fail the run: exit 0 with a failing doctest summary "
            "goes through _validate_successful_lane() regardless of strict",
        )

    def test_documented_grammar_matches_the_emitted_grammar(self) -> None:
        """The documented field names are DERIVED from the code, not hand-listed.

        #822 round 5 P2: renaming `executed` to `summary_reported` so that a
        stale parser breaks loudly accomplishes nothing while the documented
        grammar still advertises the old name. A hand-maintained copy of a
        format is a copy that drifts, so this compares the real emitted line
        against every documented template.
        """
        record = harness.LaneLedgerRecord(lane="SampleLane", strict=True)
        emitted_lane_keys = _grammar_keys(harness._format_lane_result_line(record))
        self.assertIn("summary_reported", emitted_lane_keys, "sanity: the field exists")

        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            harness.LaneLedger([("SampleLane", True)]).print_block()
        aggregate_line = next(
            line
            for line in buffer.getvalue().splitlines()
            if line.startswith("[module-tests][lane-ledger] lanes=")
        )
        emitted_aggregate_keys = _grammar_keys(aggregate_line)

        for doc in DOCS_REQUIRED_TO_STATE_THE_EXCEPTION:
            text = doc.read_text(encoding="utf-8")
            for label, prefix, expected in (
                ("per-lane", "[module-tests][lane-result] ", emitted_lane_keys),
                ("aggregate", "[module-tests][lane-ledger] lanes=", emitted_aggregate_keys),
            ):
                with self.subTest(doc=doc.name, line=label):
                    templates = [
                        line for line in text.splitlines() if line.startswith(prefix)
                    ]
                    self.assertTrue(
                        templates,
                        f"{doc.name} documents no {label} grammar line; the format must "
                        f"be written down somewhere a reader will find it",
                    )
                    for template in templates:
                        self.assertEqual(
                            _grammar_keys(template),
                            expected,
                            f"{doc.name}: the documented {label} grammar does not match "
                            f"what the runner emits",
                        )

    def test_docs_carry_no_local_worktree_identifiers(self) -> None:
        """#822 round 5 P1: no ephemeral agent artifacts in the repository.

        A local worktree name is the clearest machine-readable marker of
        evidence that cannot be reproduced from the repository. An ADR records
        why a decision was made and what would change it; the transcript and the
        binary provenance of one run on one afternoon belong in the pull request
        (AGENTS.md - transcripts, session IDs, scratch notes, dirty-worktree
        dumps, local task instances).
        """
        for doc in DOCS_REQUIRED_TO_STATE_THE_EXCEPTION:
            with self.subTest(doc=doc.name):
                match = LOCAL_WORKTREE_RE.search(doc.read_text(encoding="utf-8"))
                if match is not None:
                    self.fail(
                        f"{doc.name} names a local worktree ({match.group(0)!r}). That "
                        f"evidence is not reproducible from the repository and goes "
                        f"stale on the next measurement; put it in the PR body."
                    )

    def test_nothing_in_the_project_claims_advisory_lanes_can_never_gate(self) -> None:
        """Repo-wide, not doc-wide (#822 round 8).

        The previous version scanned two markdown files, and the commit that
        added it put the banned wording into run_module_tests.py. SOURCE
        COMMENTS make this claim to exactly the audience most likely to act on
        it, so the scan follows the subject rather than a file list.

        Round 9: an unreadable candidate is now a FAILURE rather than a silent
        skip, so "clean" can never rest on a file the scan did not read.

        Round 10: the candidate list is the whole working tree minus the
        upstream subtrees, so "repo-wide" is now literally what it does.
        """
        scanned, unreadable = _files_discussing_the_ledger()
        self.assertGreaterEqual(
            len(scanned),
            3,
            "the content-derived scan found almost nothing, which means the topic "
            "regex or the candidate enumeration stopped matching - a silently empty "
            "scan is the same absence-reads-as-success defect",
        )
        # The two places the claim has actually appeared must be in scope, or the
        # derivation has drifted away from the thing it is supposed to cover.
        scanned_resolved = {path.resolve() for path, _text in scanned}
        for required in (
            ROOT / "tests" / "ci" / "run_module_tests.py",
            *DOCS_REQUIRED_TO_STATE_THE_EXCEPTION,
        ):
            self.assertIn(
                required.resolve(),
                scanned_resolved,
                f"{required.name} discusses advisory lanes but the scan does not "
                f"cover it",
            )
        self.assertEqual(
            unreadable,
            [],
            "a candidate could not be read; the scan cannot report clean over a file "
            "it never opened",
        )
        self.assertEqual(_claim_scan_errors_from(scanned, unreadable), [])

    def test_a_claim_in_module_source_turns_the_guard_red(self) -> None:
        """The suffix hole, proven shut (#822 round 9).

        Round 8's whitelist was `.md .py .yml .yaml .txt .json`, so `.h`, `.cpp`
        and `.gd` - the languages the module is written in, and the file kind the
        motivating regression actually lived in - were invisible to a guard whose
        stated purpose is to catch that claim wherever it can live. This plants
        the banned wording in each excluded suffix, in the real tree, and asserts
        the guard is red. Against round 8 every one of these is green.
        """
        module_root = ROOT / "modules" / "gaussian_splatting"
        self.assertFalse(
            _is_upstream("modules/gaussian_splatting/x.cpp"),
            "the plant location must be inside the scanned project tree, or this "
            "proves nothing about the guard as configured",
        )
        planted_text = (
            "// An advisory lane can never change the exit code, so nothing here "
            "gates.\n"
        )
        with tempfile.TemporaryDirectory(
            dir=str(module_root), prefix=".claim_scan_probe_"
        ) as tmp:
            planted = []
            for suffix in (".h", ".cpp", ".gd", ".md"):
                path = Path(tmp) / f"claim_probe{suffix}"
                path.write_text(planted_text, encoding="utf-8")
                planted.append(path)

            scanned, unreadable = _files_discussing_the_ledger(sorted(planted))
            self.assertEqual(unreadable, [], "the planted files must all be readable")
            self.assertEqual(
                [path for path, _text in scanned],
                sorted(planted),
                "every planted suffix must be scanned; a suffix the scan cannot see "
                "is a place the claim can hide",
            )
            scoped_errors = _claim_scan_errors_from(scanned, unreadable)
            for path in planted:
                self.assertTrue(
                    any(str(path) in error for error in scoped_errors),
                    f"the ban did not fire for {path.name}",
                )

            # End to end, over the REAL candidate list: this is the guard's own
            # verdict, which is exactly `_claim_scan_errors_from(...) == []`. The
            # plants are untracked, and `git ls-files --others` lists them - a
            # claim is caught while it is being written, not one commit later.
            real_errors = _claim_scan_errors()
            missed = [
                path.name
                for path in planted
                if not any(str(path) in error for error in real_errors)
            ]
        self.assertEqual(
            missed,
            [],
            f"a prohibited claim inside the project tree left the guard green for "
            f"{missed}; every planted suffix must be reachable through the REAL "
            f"candidate enumeration, not only through a hand-passed file list",
        )

    def test_a_claim_in_a_root_level_file_turns_the_guard_red(self) -> None:
        """The directory whitelist, proven shut (#822 round 10).

        Round 9's search roots were docs/, tests/, modules/gaussian_splatting/
        and .github/. AGENTS.md, CONTRIBUTING.md and README.md are none of those,
        and they are exactly the files a contributor reads before touching CI - so
        the claim could sit in the most-read file in the repository while a guard
        described in its own comment as repo-wide reported clean. This plants the
        banned wording in a real root-level file and asserts the guard is red.
        Against round 9 it is green.
        """
        handle, name = tempfile.mkstemp(
            dir=str(ROOT), prefix="claim_scan_probe_", suffix=".md"
        )
        os.close(handle)
        planted = Path(name)
        try:
            planted.write_text(
                "Advisory lanes can never change the exit code, so a red advisory "
                "lane is nothing to act on.\n",
                encoding="utf-8",
            )
            self.assertEqual(
                planted.parent.resolve(),
                ROOT.resolve(),
                "sanity: the plant must be a ROOT-level file, which is the location "
                "the previous search roots excluded",
            )
            errors = _claim_scan_errors()
        finally:
            planted.unlink(missing_ok=True)
        self.assertTrue(
            any(str(planted) in error for error in errors),
            "a prohibited claim in a root-level file left the guard green; the scan "
            "is still enumerating a hand-picked set of directories",
        )

    def test_the_candidate_list_is_the_project_minus_upstream(self) -> None:
        """Scope by exclusion, asserted in both directions (#822 round 10).

        The list-shaped defect this replaces cannot be caught by a scan-is-clean
        assertion, so the ENUMERATION is checked directly: project-owned files
        outside the old four roots must be candidates, and the upstream subtrees
        must not be - a scope that quietly swallowed the module or the docs would
        be just as broken as one that missed the root.
        """
        files, problems = _project_files()
        self.assertEqual(problems, [], "the candidate list must build cleanly here")
        relative = {path.relative_to(ROOT).as_posix() for path in files}

        owned = (
            "AGENTS.md",
            "CONTRIBUTING.md",
            "README.md",
            "docs/reference/build-test-ci.md",
            "tests/ci/run_module_tests.py",
            "modules/gaussian_splatting/AGENTS.md",
        )
        for name in owned:
            with self.subTest(owned=name):
                self.assertTrue(
                    (ROOT / name).is_file(),
                    f"{name} is expected to exist in this repository; if it moved, "
                    f"this assertion is what should be updated",
                )
                self.assertIn(
                    name,
                    relative,
                    f"{name} is project-owned but is not a claim-scan candidate",
                )

        for prefix in (*UPSTREAM_SUBTREES, "modules/gltf/"):
            with self.subTest(upstream=prefix):
                self.assertFalse(
                    [name for name in relative if name.startswith(prefix)],
                    f"{prefix} is upstream Godot and must stay out of the scan",
                )
        self.assertFalse(
            _is_upstream("modules/gaussian_splatting/renderer/x.cpp"),
            "this fork's own module is not upstream",
        )

    def test_a_candidate_list_that_cannot_be_built_fails_the_scan(self) -> None:
        """An enumeration that failed is not an enumeration that found nothing.

        The round-9 hole was a candidate the scan could not READ; the same shape
        one level up is a candidate list the scan could not BUILD. If git is
        missing, fails, or returns nothing, a scan that then reported `[]` would
        be publishing "no prohibited claim exists" on the strength of having
        looked at no files at all.
        """
        class _Completed:
            def __init__(self, returncode: int, stdout: bytes = b"") -> None:
                self.returncode = returncode
                self.stdout = stdout
                self.stderr = b"fatal: not a git repository"

        cases = (
            ("git is missing", {"side_effect": FileNotFoundError(2, "no git")}),
            ("git fails", {"return_value": _Completed(128)}),
            ("git lists nothing", {"return_value": _Completed(0, b"")}),
        )
        for label, patch_kwargs in cases:
            with self.subTest(case=label):
                with mock.patch.object(subprocess, "run", **patch_kwargs):
                    files, problems = _project_files()
                self.assertEqual(files, [], "no list means no candidates")
                self.assertTrue(problems, f"{label} must be reported, not swallowed")
                self.assertTrue(
                    _claim_scan_errors_from([], problems),
                    f"{label} left the guard green over a scan that examined nothing",
                )

    def test_every_name_git_lists_is_either_usable_or_reported(self) -> None:
        """The lossy-decode hole, proven shut (#822 round 11).

        Round 10 decoded `git ls-files -z` with `errors="replace"`. A filename
        that is not valid UTF-8 - ordinary on POSIX, where a path is bytes - came
        back with U+FFFD in place of the offending bytes, so the `Path` named
        nothing on disk, `is_file()` was False, and the candidate vanished with
        NOTHING added to `problems`. The scan then reported the project clean
        over a file it had been handed and never opened.

        The property asserted is the one that closes it regardless of platform:
        every NUL-separated record git emits is accounted for exactly once, and
        every name that survives must re-encode to the exact bytes it came from.
        Against round 10 the corrupted record appears in neither list.
        """
        good = b"docs/reference/build-test-ci.md"
        # Latin-1 'e-acute' as a bare byte: a legal POSIX filename, not UTF-8.
        undecodable = b"modules/gaussian_splatting/caf\xe9_notes.md"
        names, problems = _decode_git_names(good + b"\0" + undecodable + b"\0")

        self.assertIn(good.decode("ascii"), names, "sanity: a plain name still lists")
        for name in names:
            self.assertEqual(
                os.fsencode(name),
                good if name == good.decode("ascii") else undecodable,
                "a name the scan keeps must re-encode to the bytes git emitted, or "
                "it names a different file than the one git meant",
            )
        accounted = {os.fsencode(name) for name in names}
        reported = [problem for problem in problems if "caf" in problem]
        self.assertTrue(
            undecodable in accounted or reported,
            f"the non-UTF-8 record was neither kept intact nor reported: "
            f"names={names!r} problems={problems!r}",
        )
        if undecodable not in accounted:
            self.assertTrue(
                _claim_scan_errors_from([], problems),
                "a candidate the enumeration could not represent must make the scan "
                "RED; dropping it reports clean over a file that was never read",
            )
        self.assertNotIn(
            "�",
            "".join(names),
            "a replacement character in a candidate name means the decode was lossy "
            "and the path no longer refers to the file git listed",
        )

    def test_a_candidate_the_os_refuses_to_describe_fails_the_scan(self) -> None:
        """`Path.is_file()` cannot report a refusal, only answer one (#822 round 11).

        The loop opened with `if not path.is_file(): continue`. Whatever the
        running CPython makes of a candidate the OS will not describe - a plain
        False, which is the same silent skip round 9 removed one line further
        down, or a re-raise that tears the whole scan down with an unhandled
        exception - it is never "this candidate is a problem", because the
        function's entire vocabulary is a bool. Three refusals are planted below
        and all three must come back as exactly one reported problem.

        The refusals are injected at `Path.stat`, the call the fixed scan makes
        itself, so these proofs hold on every interpreter. What they deliberately
        do NOT do is pin what `is_file()` would have done with them: that routes
        through `Path.stat` up to CPython 3.13 and through `os.path.isfile()`
        from 3.14 on, so a `Path.stat` patch measures one implementation and is
        bypassed by the other (#822 round 12 - the pinned dict failed on 3.14,
        which the repository's documented "Python 3.10+" support includes). The
        one characterization kept is measured against the real OS instead.

        A genuinely absent path stays a skip: git lists cached paths, and a
        locally deleted file holds no text to have missed. That distinction is
        the whole point, and it is the distinction `is_file()` destroys - which
        the first assertion measures rather than asserts.
        """
        # Factories, so every raise is a fresh exception rather than one object
        # accumulating tracebacks across the subtests.
        refusals = {
            "swallowed-into-False": lambda: OSError(
                errno.ELOOP, "Too many symbolic links"
            ),
            "raised-out-of-the-scan": lambda: PermissionError(
                errno.EACCES, "Permission denied"
            ),
            "swallowed ValueError": lambda: ValueError("embedded null character"),
        }
        # What `is_file()` ACTUALLY does with a candidate the OS refuses - measured
        # through the OS, not through a mock, so this is the running interpreter's
        # real behaviour whichever pathlib it ships (see `_is_file_verdict`). The
        # point is not which bool comes back. It is that the SAME bool comes back
        # for a candidate the OS refused to describe and for one that simply is not
        # there: `is_file()` destroys the distinction the scan is built on, while
        # `path.stat()` preserves it, which is exactly why the loop below can report
        # the first and skip the second. Kept as a hard equality rather than a
        # tolerance, so an interpreter that starts answering differently fails here
        # loudly instead of quietly making this proof about nothing.
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(
                (
                    _is_file_verdict(UNENCODABLE_CANDIDATE),
                    _is_file_verdict(Path(tmp) / "never-existed.md"),
                ),
                ("False", "False"),
                "sanity: this stands for a refusal `is_file()` reports as an ordinary "
                "absence; if it no longer does, this proof is about nothing",
            )
        for label, make_refusal in refusals.items():
            with self.subTest(refusal=label), tempfile.TemporaryDirectory() as tmp:
                readable = Path(tmp) / "readable.md"
                readable.write_text("This documents the lane-ledger.\n", encoding="utf-8")
                opaque = Path(tmp) / "opaque.md"
                opaque.write_text(
                    "This also documents the lane-ledger.\n", encoding="utf-8"
                )
                absent = Path(tmp) / "deleted-since-git-listed-it.md"

                real_stat = Path.stat

                def refuse_one(self, *args, _make=make_refusal, _target=opaque, **kwargs):
                    if self == _target:
                        raise _make()
                    return real_stat(self, *args, **kwargs)

                with mock.patch.object(Path, "stat", refuse_one):
                    scanned, problems = _files_discussing_the_ledger(
                        [readable, opaque, absent]
                    )

                self.assertEqual(
                    [path for path, _text in scanned],
                    [readable],
                    "sanity: the describable candidate is still scanned",
                )
                self.assertEqual(
                    len(problems),
                    1,
                    f"exactly the unstattable candidate is a problem: {problems!r}",
                )
                self.assertIn(str(opaque), problems[0])
                self.assertNotIn(
                    str(absent),
                    "".join(problems),
                    "a path that is genuinely gone holds no evidence and must not be "
                    "a problem, or every locally deleted file turns the guard red",
                )
                self.assertTrue(
                    _claim_scan_errors_from(scanned, problems),
                    "a candidate the OS would not describe must make the scan RED",
                )

        # The same defect once more with NO mock anywhere: the refusal comes from
        # the OS itself, so this half of the proof cannot be affected by which
        # pathlib implementation is running or by what a `Path.stat` patch does and
        # does not intercept. Restore `if not path.is_file(): continue` and this
        # candidate goes back to being silently skipped on every interpreter.
        with self.subTest(refusal="unencodable path, unmocked"), \
                tempfile.TemporaryDirectory() as tmp:
            readable = Path(tmp) / "readable.md"
            readable.write_text("This documents the lane-ledger.\n", encoding="utf-8")

            scanned, problems = _files_discussing_the_ledger(
                [readable, UNENCODABLE_CANDIDATE]
            )

            self.assertEqual(
                [path for path, _text in scanned],
                [readable],
                "sanity: the describable candidate is still scanned",
            )
            self.assertEqual(
                len(problems),
                1,
                f"the unstattable candidate is the one problem: {problems!r}",
            )
            self.assertIn(str(UNENCODABLE_CANDIDATE), problems[0])
            self.assertTrue(
                _claim_scan_errors_from(scanned, problems),
                "a candidate the OS would not describe must make the scan RED",
            )

    def test_documented_json_shape_matches_the_emitted_shape(self) -> None:
        """The report's documented object is DERIVED from to_json() (#822 round 10).

        The ADR advertised `{schema_version, baseline_note, generated_utc, lanes,
        totals}` while every written report also carried `lane_loop_exit_code` -
        and the same ADR went on to tell consumers to read that field. A
        hand-copied shape is a copy that drifts, exactly as the per-line grammar
        did in round 5, so it is pinned the same way rather than re-typed
        correctly and left to drift again.
        """
        ledger = harness.LaneLedger([("SampleLane", True)])
        emitted = set(ledger.to_json(ledger.totals(), lane_loop_exit_code=0))
        self.assertIn("lane_loop_exit_code", emitted, "sanity: the field exists")
        for doc in DOCS_REQUIRED_TO_STATE_THE_EXCEPTION:
            with self.subTest(doc=doc.name):
                documented = _documented_json_keys(doc.read_text(encoding="utf-8"))
                self.assertTrue(
                    documented,
                    f"{doc.name} documents no JSON shape for --lane-report; the "
                    f"format must be written down where a reader will find it",
                )
                self.assertEqual(
                    set(documented),
                    emitted,
                    f"{doc.name}: the documented JSON object is not what the runner "
                    f"writes",
                )

    def test_an_unreadable_candidate_fails_instead_of_being_skipped(self) -> None:
        """`except OSError: continue` reported clean over unread evidence.

        A candidate made unreadable by a permission, a Windows sharing lock or an
        I/O error was silently dropped, so the scan's "clean" rested on a file it
        never opened - and the minimum-count assertion cannot detect one missing
        file. Absence of a signal is not a passing signal.
        """
        with tempfile.TemporaryDirectory() as tmp:
            readable = Path(tmp) / "readable.md"
            readable.write_text("This documents the lane-ledger.\n", encoding="utf-8")
            locked = Path(tmp) / "locked.md"
            locked.write_text("This also documents the lane-ledger.\n", encoding="utf-8")

            real_reader = _read_candidate

            def refuse_one(path: Path):
                if path == locked:
                    raise PermissionError(13, "Permission denied")
                return real_reader(path)

            with mock.patch.object(sys.modules[__name__], "_read_candidate", refuse_one):
                scanned, unreadable = _files_discussing_the_ledger([readable, locked])

        self.assertEqual(
            [path for path, _text in scanned],
            [readable],
            "sanity: the readable candidate is still scanned",
        )
        self.assertEqual(len(unreadable), 1, "the unreadable candidate must be reported")
        self.assertIn(str(locked), unreadable[0])
        self.assertTrue(
            _claim_scan_errors_from(scanned, unreadable),
            "an unreadable candidate must make the scan RED; a scan that skips it "
            "reports clean over evidence it never read",
        )

    def test_documented_reason_order_matches_the_evaluated_order(self) -> None:
        """The reason table claims to BE the evaluation order, so pin it (#822 round 9).

        `build-test-ci.md` listed `nonzero-exit-no-test-failures` before
        `no-coverage`, said "the reasons are evaluated in that order", and in the
        very same sentence said `no-coverage` is checked first - so the canonical
        reference contradicted itself and the code. The order is derived from the
        `return` statements of `advisory_red_reason()` rather than retyped.
        """
        evaluated = _evaluated_reason_order()
        self.assertEqual(
            evaluated,
            ["failed", "no-coverage", "nonzero-exit-no-test-failures", "crashed"],
            "sanity: the reasons were read off the implementation in branch order",
        )
        documented = _documented_reason_order(
            (ROOT / "docs" / "reference" / "build-test-ci.md").read_text(encoding="utf-8")
        )
        self.assertEqual(
            documented,
            evaluated,
            "the documented reason table is not in evaluation order, and it says it "
            "is; a reader following the table would classify a lane the way the code "
            "does not",
        )
        for doc in DOCS_REQUIRED_TO_STATE_THE_EXCEPTION:
            text = doc.read_text(encoding="utf-8")
            alternations = REASON_ALTERNATION_RE.findall(text)
            with self.subTest(doc=doc.name):
                self.assertTrue(
                    alternations, f"{doc.name} documents no reason= grammar at all"
                )
                for alternation in alternations:
                    self.assertEqual(
                        alternation.split("|"),
                        evaluated,
                        f"{doc.name}: the documented reason= alternation is not the "
                        f"set the code emits, in evaluation order",
                    )

    def test_the_two_reference_documents_state_the_qualification(self) -> None:
        """Avoiding the false claim is not the same as stating the true one."""
        for doc in DOCS_REQUIRED_TO_STATE_THE_EXCEPTION:
            with self.subTest(doc=doc.name):
                self.assertTrue(doc.is_file(), f"missing doc: {doc}")
                text = doc.read_text(encoding="utf-8")
                for marker in REQUIRED_DOC_MARKERS:
                    if marker.search(text) is None:
                        # Deliberately not assertRegex: it dumps the whole
                        # document into the failure, burying the sentence at issue.
                        self.fail(
                            f"{doc.name} must state the qualification, not merely avoid "
                            f"denying it; deleting it is the same defect. Expected a "
                            f"sentence matching {marker.pattern!r}"
                        )


if __name__ == "__main__":
    unittest.main()
