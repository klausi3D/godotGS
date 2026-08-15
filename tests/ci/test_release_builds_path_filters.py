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
Discovery is fail-CLOSED by default -- an execution-shaped line must fully
resolve to a repository path or it raises. That default was inverted after
three consecutive rounds found discovery fail-open in a different way each time
(the regex shape, then invocation indirection, then module form / quoted local
actions / variable interpreters), each fix having enumerated only the forms it
had just been shown. Enumerating cases is what kept failing; the structural
rule is the fix, and the case tests are its evidence rather than its substance.

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
* Only `*` and a trailing `**` are modelled. `?` and `+` raise, because in
  GitHub's syntax they quantify the PRECEDING character rather than matching an
  arbitrary one -- an fnmatch-style translation would claim
  `tests/ci/x.?y` covers `tests/ci/x.py`, which GitHub does not. `**/` raises
  because it must also match ZERO directory levels (`a/**/b` covers `a/b`).
  Character classes, braces and escapes raise outright.
* `python` invocations must resolve: `-c` and flags-only runs are accepted,
  `-m` is resolved against the tree (so `python -m tests.ci.helper` IS
  discovered) and otherwise must be in `ALLOWED_PYTHON_MODULES`, and anything
  else must be a literal `.py` path that exists. Unknown flags, variable paths
  and missing files raise.
* Non-python interpreters (`bash`, `sh`, `pwsh`, `powershell`) are parsed the
  same way: `-c`/`-Command` terminate, known flags are skipped, and the first
  remaining token must resolve to a repository file or it raises.
* A repository script named on a line with NO literally named interpreter -- a
  variable interpreter (`$PYTHON x.py`), a wrapper, or another YAML key --
  raises, because the guard cannot confirm how it runs.
* Steps that `uses:` a LOCAL composite action raise, quoted or not: their
  `run:` bodies live in another file this guard does not read. Third-party
  `uses:` steps are out of scope by design; they are not repository paths.
* The `on:` block is excluded from execution scanning -- its `paths:` entries
  are themselves quoted script paths.
* Script paths embedded inside a GitHub expression (e.g.
  `hashFiles('methods.py')`) are not treated as executions: tokens are matched
  literally after quote-stripping only. Expressions cannot run repository code,
  so this is a deliberate boundary rather than a gap.
* Commented-out lines are ignored, in YAML and in shell bodies alike.

No PyYAML. `tests/ci/run_module_tests.py --guard-only` runs under a bare
`actions/setup-python` interpreter, which ships no third-party packages, so the
readers below are hand-rolled -- and tested against inline fixtures rather than
trusted.
"""

from __future__ import annotations

import ast
import re
import subprocess
import sys
import unittest
from pathlib import Path
from typing import Dict, List, NamedTuple, Sequence, Set

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
    "tests/runtime/run_export_smoke.py",
)

# --- The smoke test's DATA dependencies --------------------------------------
#
# A second gap of the same shape, one layer out. The manifest above covers the
# scripts the workflow *executes*; `export_smoke_windows` also consumes files
# that no interpreter line names -- the preset template it substitutes, the
# project it exports, the probe it runs inside the exported binary, the fixture
# generator. A push touching only the probe matched none of the filters, so the
# blocking smoke test never validated the behaviour that changed.
#
# DERIVED, not listed. `run_export_smoke.py` already names every one of those
# files as a module-level `Path` constant, so the constants ARE the dependency
# set and are read back here instead of being transcribed. Transcription is what
# failed the first time: a hand-kept list drifts silently, and this file's own
# doctrine is that an invariant guarded by a hand-written list is already broken.
SMOKE_RUNNER = ROOT / "tests" / "runtime" / "run_export_smoke.py"
SMOKE_RUNNER_DIR = ROOT / "tests" / "runtime"

# Directory-valued constants cannot be turned into a filter entry mechanically:
# `tests/examples/godot/test_project/**` would fire two editor builds and two
# template builds on every fixture rebake, which is the same objection the
# `tests/ci/**` decision above records. So each directory constant needs an
# explicit disposition naming the files inside it that must trigger the
# workflow, WITH the reason. Fail-closed: a directory constant that is not
# dispositioned here raises, so a new one cannot be silently ignored.
#
# A disposition is NOT a coverage claim
# -------------------------------------
# The first version of this said `PROJECT_DIR` was "narrowed deliberately to the
# project manifest" and stopped there, which read as though the three named
# files were the smoke test's project inputs. They are not. The preset the run
# writes sets `export_filter="all_resources"`
# (tests/runtime/export_smoke_preset.cfg.in), which
# `editor/export/editor_export.cpp` maps to `EXPORT_ALL_RESOURCES` and
# `editor/export/editor_export_platform.cpp` implements by walking the WHOLE
# EditorFileSystem -- and the run's first step is `--path <project> --import`,
# which imports the whole project before any of that. Every tracked file under
# the project is an input to a blocking gate, and the filter covers five of
# them.
#
# So the disposition now has to carry both halves: what triggers, and what
# knowingly does NOT. `whole_tree` is the second half. The untriggered set is
# DERIVED (tracked files minus covered files), printed by the guard so the
# accepted risk appears in its output rather than only in this comment, and
# bounded by two checks that must hold rather than by an assurance:
#   * `compensating_workflow` -- a workflow that DOES trigger on those files and
#     loads the same project, so an import/load regression in one of them is
#     still caught on the same push; and
#   * `release_builds.yml`'s own `schedule:` trigger, which bounds how long an
#     untriggered change can go without the smoke test running at all.
# What remains uncovered after those two is the export/packaging interaction
# specifically, deferred to the next matching push or the nightly. That is the
# accepted risk, and it is stated instead of being implied by a short list.
WHOLE_TREE_EXPORT_FILTERS = ("all_resources",)


class DirectoryDisposition(NamedTuple):
    triggering: Sequence[str]
    whole_tree: bool = False
    compensating_workflow: str = ""
    accepted_risk: str = ""


DIRECTORY_DEPENDENCY_DISPOSITIONS: Dict[str, DirectoryDisposition] = {
    # The repository itself (subprocess cwd). Every source path is already
    # covered by the coarse `core/**`, `modules/**`, ... filters.
    "ROOT": DirectoryDisposition(triggering=()),
    # Only used to build the file constants below and as a `sys.path` entry; the
    # module it imports from there is covered by `_imported_runtime_modules()`.
    "RUNTIME_DIR": DirectoryDisposition(triggering=()),
    # Passed to the editor as `--path`, imported wholesale and exported under
    # `export_filter="all_resources"`, so the WHOLE tree is an input. Only the
    # project manifest is named here; the probe and the fixture are separate
    # constants covered on their own. Everything else under the directory is
    # knowingly untriggered -- it is dominated by rebaked binary fixtures, and a
    # `tests/examples/godot/test_project/**` filter would fire two editor builds
    # plus two template builds on every rebake.
    "PROJECT_DIR": DirectoryDisposition(
        triggering=("tests/examples/godot/test_project/project.godot",),
        whole_tree=True,
        compensating_workflow=".github/workflows/gaussian_production_gates.yml",
        accepted_risk=(
            "A push that changes only one of these files does not run the blocking export "
            "smoke test on that commit. It DOES run the compensating workflow, which loads "
            "the same project with a real editor, so an import or load regression still "
            "surfaces on the same push; what is deferred is the export/packaging "
            "interaction specifically, to the next push that matches a filter or to the "
            "nightly schedule."
        ),
    ),
}


class UnmodelledDependency(RuntimeError):
    """A `run_export_smoke.py` dependency this guard refuses to reason about."""


def _import_smoke_runner():
    """Import `run_export_smoke` for its constants (stdlib-only, no side effects)."""
    if str(SMOKE_RUNNER_DIR) not in sys.path:
        sys.path.insert(0, str(SMOKE_RUNNER_DIR))
    import run_export_smoke  # noqa: PLC0415 -- deliberately late, see docstring

    return run_export_smoke


def _repo_relative(path: Path, origin: str) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(ROOT).as_posix()
    except ValueError as exc:
        raise UnmodelledDependency(
            f"run_export_smoke.{origin} points outside the repository ({resolved}); a `paths:` "
            "filter cannot cover it, so this guard will not pretend it is covered."
        ) from exc


def _imported_runtime_modules() -> Set[str]:
    """Repository modules `run_export_smoke.py` imports at module scope.

    `run_runtime_validation.py` is a real dependency that the executed-script
    derivation cannot see -- it is imported, never invoked -- and was therefore
    hand-listed in the filters with a comment. Reading the import statements
    turns that comment into a derivation.
    """
    tree = ast.parse(SMOKE_RUNNER.read_text(encoding="utf-8"))
    found: Set[str] = set()
    for node in ast.walk(tree):
        modules: List[str] = []
        if isinstance(node, ast.Import):
            modules = [alias.name for alias in node.names]
        elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
            modules = [node.module]
        for module in modules:
            candidate = SMOKE_RUNNER_DIR.joinpath(*module.split(".")).with_suffix(".py")
            if candidate.is_file():
                found.add(_repo_relative(candidate, f"import {module}"))
    return found


EXPORT_FILTER_LINE = re.compile(r'^\s*export_filter\s*=\s*"([^"]*)"\s*$')


def export_filter_from_text(text: str) -> str:
    """The preset template's `export_filter` value. Ambiguity raises."""
    values = [
        match.group(1)
        for match in (EXPORT_FILTER_LINE.match(line) for line in text.splitlines())
        if match
    ]
    if len(values) != 1:
        raise UnmodelledDependency(
            f"Expected exactly one `export_filter=` line in the preset template, found "
            f"{len(values)} ({values}). The smoke test's project input set is derived from "
            "this value, so an ambiguous or missing one is refused rather than assumed."
        )
    return values[0]


def smoke_export_filter() -> str:
    """Read `export_filter` off the preset template the runner names."""
    module = _import_smoke_runner()
    template = module.PRESET_TEMPLATE
    _repo_relative(template, "PRESET_TEMPLATE")
    return export_filter_from_text(template.read_text(encoding="utf-8"))


def assert_project_input_model() -> str:
    """Bind the PROJECT_DIR disposition to the mechanism it reasons about.

    Both directions are refused, because either one on its own turns the
    disposition back into an unchecked assertion:

    * an `export_filter` this guard has no input model for -- `resources`,
      `scenes` and `customized` all export a dependency CLOSURE the editor
      computes, which cannot be derived from the tree here, so claiming any
      coverage over it would be a guess; and
    * a preset that still exports the whole project while the disposition has
      stopped saying so, which is exactly how the three-file list came to read
      as complete.
    """
    export_filter = smoke_export_filter()
    disposition = DIRECTORY_DEPENDENCY_DISPOSITIONS["PROJECT_DIR"]
    if export_filter not in WHOLE_TREE_EXPORT_FILTERS:
        raise UnmodelledDependency(
            f"The export smoke preset now uses export_filter={export_filter!r}, which is not "
            f"one of {WHOLE_TREE_EXPORT_FILTERS}. Every other value exports a dependency "
            "closure computed by the editor, and this guard cannot derive that from the tree, "
            "so it will not go on reporting the project input set as modelled. Work out the "
            "new input set and re-disposition PROJECT_DIR."
        )
    if not disposition.whole_tree:
        raise UnmodelledDependency(
            f"export_filter={export_filter!r} exports the whole project, but the PROJECT_DIR "
            "disposition no longer records `whole_tree`. Dropping that flag hides the "
            "untriggered inputs again instead of covering them."
        )
    return export_filter


def tracked_files_under(directory: Path) -> List[str]:
    """Repo-relative tracked files under `directory`.

    Tracked, not walked: a `paths:` filter is evaluated over the files a push
    actually changes, and untracked local debris (a developer's
    `export_presets.cfg`, the `.godot/` import cache) is not that. Fail-closed
    -- no git, no answer.
    """
    relative = _repo_relative(directory, "tracked_files_under")
    try:
        result = subprocess.run(
            ["git", "-C", str(ROOT), "ls-files", "-z", "--", relative],
            capture_output=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise UnmodelledDependency(
            f"Could not list tracked files under {relative} with git ({exc}). This guard "
            "derives the smoke test's project input set from the tracked tree; without it "
            "the untriggered set would silently read as empty."
        ) from exc
    files = [name for name in result.stdout.decode("utf-8").split("\0") if name]
    if not files:
        raise UnmodelledDependency(
            f"No tracked files under {relative}; the derivation has stopped seeing the "
            "project it is supposed to model."
        )
    return sorted(files)


class ProjectInputDisposition(NamedTuple):
    constant: str
    export_filter: str
    inputs: Sequence[str]
    triggering: Sequence[str]
    untriggered: Sequence[str]
    compensating_workflow: str
    accepted_risk: str


def project_input_disposition(event: str) -> ProjectInputDisposition:
    """What the smoke test consumes under PROJECT_DIR, split by what triggers."""
    export_filter = assert_project_input_model()
    disposition = DIRECTORY_DEPENDENCY_DISPOSITIONS["PROJECT_DIR"]
    module = _import_smoke_runner()
    inputs = tracked_files_under(module.PROJECT_DIR)
    patterns = parse_event_paths(_workflow_text())[event]
    triggering = [path for path in inputs if path_is_covered(patterns, path)]
    untriggered = [path for path in inputs if path not in set(triggering)]
    return ProjectInputDisposition(
        constant="PROJECT_DIR",
        export_filter=export_filter,
        inputs=inputs,
        triggering=triggering,
        untriggered=untriggered,
        compensating_workflow=disposition.compensating_workflow,
        accepted_risk=disposition.accepted_risk,
    )


def describe_project_input_disposition(event: str) -> str:
    """The accepted risk, in the guard's OUTPUT rather than only in a comment."""
    state = project_input_disposition(event)
    return (
        f"[SMOKE_INPUT_DISPOSITION] {state.constant} export_filter={state.export_filter!r} "
        f"event={event}: {len(state.inputs)} tracked project files are inputs of the blocking "
        f"export_smoke_windows job (the editor imports and exports all of them); "
        f"{len(state.triggering)} trigger release_builds.yml, {len(state.untriggered)} "
        f"knowingly do NOT. Compensating lane: {state.compensating_workflow}. "
        f"{state.accepted_risk}"
    )


def derived_smoke_dependencies() -> List[str]:
    """Repo-relative files `export_smoke_windows` consumes, read off the runner."""
    module = _import_smoke_runner()
    assert_project_input_model()
    dependencies: Set[str] = {_repo_relative(SMOKE_RUNNER, "SMOKE_RUNNER")}

    for name, value in sorted(vars(module).items()):
        if not isinstance(value, Path) or name.startswith("_"):
            continue
        relative = _repo_relative(value, name)
        if value.resolve().is_dir():
            if name not in DIRECTORY_DEPENDENCY_DISPOSITIONS:
                raise UnmodelledDependency(
                    f"run_export_smoke.{name} is a DIRECTORY ({relative}) and has no entry in "
                    "DIRECTORY_DEPENDENCY_DISPOSITIONS. Decide which files inside it must trigger "
                    "release_builds.yml and say why -- a whole-directory filter would fire four "
                    "release builds on unrelated churn, and ignoring it would reopen the gap."
                )
            dependencies.update(DIRECTORY_DEPENDENCY_DISPOSITIONS[name].triggering)
            continue
        dependencies.add(relative)

    dependencies.update(_imported_runtime_modules())
    return sorted(dependencies)

# The self-hosted jobs check out under `repo/`; `paths:` are repository-relative.
CHECKOUT_PREFIX = "repo/"
SCRIPT_SUFFIXES = (".py", ".sh", ".ps1")
# Neither boundary can be `\b`: it matches INSIDE
# `actions/setup-python@<sha>` (hyphen before) and inside `python-version:`
# (hyphen after), so the pinned action and the setup-python input both parsed
# as python invocations. Hyphens must be excluded on both sides.
INTERPRETER_TOKEN = re.compile(r"(?<![\w./-])(python3?|bash|sh|pwsh|powershell)(?![\w-])")
LIST_ITEM = re.compile(r'^\s*-\s*["\']?([^"\'#]+?)["\']?\s*(?:#.*)?$')
# Quotes allowed: `uses: "./.github/actions/x"` is a local action too.
LOCAL_ACTION = re.compile(r"""^\s*(?:-\s*)?uses:\s*["']?\.""")
USES_KEY = re.compile(r"^\s*(?:-\s*)?uses:\s*(.+?)\s*$")
SHELL_KEY = re.compile(r"^\s*shell:\s*[\w-]+\s*$")

# python flags that take no argument and never name a script.
PY_FLAGS_NO_ARG = frozenset(
    {"-u", "-E", "-s", "-S", "-O", "-OO", "-B", "-q", "-I", "-v", "-b", "-d"}
)
# python flags that consume the following token.
PY_FLAGS_WITH_ARG = frozenset({"-X", "-W", "--check-hash-based-pycs"})
# Forms that terminate parsing without naming a repository script. NOTE: `-m` is
# NOT here -- a module can be repository code (`python -m tests.ci.helper`), so
# it is resolved against the tree, not waved through.
PY_TERMINAL_OK = frozenset({"-c", "--version", "-V", "-h", "--help", "-"})

# The ONLY `python -m` targets accepted without resolving to a repository file.
# Keep this minimal and say why each one is not repository code.
ALLOWED_PYTHON_MODULES = frozenset(
    {
        "pip",  # `python -m pip install ...` -- stdlib-adjacent installer.
        "SCons",  # `python -m SCons @args` -- the build system, a pip dependency.
    }
)

SHELL_FLAGS_NO_ARG = frozenset({"-e", "-u", "-x", "-l", "-i", "-NoProfile", "-NoLogo"})
SHELL_FLAGS_WITH_ARG = frozenset({"-o", "-ExecutionPolicy"})
SHELL_TERMINAL_OK = frozenset({"-c", "-Command", "-command", "-EncodedCommand"})
# Unmodelled glob constructs; matching them literally would be wrong.
#
# `?` is here because GitHub's `?` means "zero or one of the PRECEDING
# character", not "exactly one arbitrary non-slash character" as in fnmatch.
# Modelling it the fnmatch way claimed `tests/ci/x.?y` covers `tests/ci/x.py`,
# which GitHub does not. `+` has the same shape (one or more of the preceding
# character), and character classes / braces / escapes are unmodelled outright.
UNSUPPORTED_GLOB_CHARS = "?[]{}+\\"
# `**/` must match ZERO directory levels too (`a/**/b` covers `a/b`), which the
# straight `**` -> `.*` translation below gets wrong. Refused rather than
# approximated.
UNSUPPORTED_GLOB_SEQUENCES = ("**/",)


class UnsupportedFilterPattern(RuntimeError):
    """A `paths:` pattern this guard refuses to reason about."""


class UnsupportedInvocation(RuntimeError):
    """A workflow line that runs something this guard cannot resolve."""


def github_path_match(pattern: str, path: str) -> bool:
    """GitHub `paths:` glob semantics for the subset this guard models.

    Models exactly two constructs: a leading/embedded `*` (any run of characters
    that does not cross `/`) and `**` (any run, `/` included) when it is not
    followed by `/`. Everything else raises.

    `fnmatch` cannot be used here -- its `*` happily crosses `/`, which is the
    exact assumption that made `"*.py"` look like it covered `tests/ci/*.py`.
    And its `?` means something different from GitHub's, which is why `?` is
    refused rather than translated (see UNSUPPORTED_GLOB_CHARS).
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
            "model (only * and ** are). In GitHub's syntax `?` and `+` quantify the PRECEDING "
            "character rather than matching an arbitrary one, so translating them the fnmatch "
            "way would claim coverage GitHub does not give. Implement and test them against "
            "the documented behaviour, or keep them out of paths:."
        )
    for sequence in UNSUPPORTED_GLOB_SEQUENCES:
        if sequence in pattern:
            raise UnsupportedFilterPattern(
                f"paths: pattern {pattern!r} contains {sequence!r}, which must also match ZERO "
                "directory levels (`a/**/b` covers `a/b`). This guard's `**` translation does "
                "not, so it would under-report coverage here and over-report it elsewhere. "
                "Implement the zero-level case with tests, or keep it out of paths:."
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


def _body_lines(workflow_text: str) -> List[str]:
    """Content lines OUTSIDE the top-level `on:` block.

    The `paths:` entries are themselves quoted script paths, so scanning them
    for executions would flag the very list this guard checks against.
    """
    lines: List[str] = []
    in_on_block = False
    for line in _content_lines(workflow_text):
        indent = len(line) - len(line.lstrip())
        if indent == 0:
            in_on_block = re.match(r"^on:\s*$", line.strip()) is not None
            if in_on_block:
                continue
        if in_on_block:
            continue
        lines.append(line)
    return lines


def _resolve_python_module(module: str, line: str) -> Set[str]:
    if module in ALLOWED_PYTHON_MODULES:
        return set()
    as_path = module.replace(".", "/")
    for candidate in (f"{as_path}.py", f"{as_path}/__main__.py"):
        if (ROOT / candidate).is_file():
            return {candidate}
    raise UnsupportedInvocation(
        f"`python -m {module}` is neither a repository module nor an allowlisted external one "
        f"(from: {line.strip()!r}). If it is repository code, name it so it can be checked "
        f"against paths:; if it is a third-party tool, add it to ALLOWED_PYTHON_MODULES with a "
        "comment saying why it is not a repository path."
    )


def _resolve_python(argv: List[str], line: str) -> Set[str]:
    index = 0
    while index < len(argv):
        token = argv[index]
        if token == "-m":
            if index + 1 >= len(argv):
                raise UnsupportedInvocation(f"`python -m` with no module in: {line.strip()!r}")
            return _resolve_python_module(_normalise(argv[index + 1]), line)
        if token in PY_TERMINAL_OK:
            return set()
        if token in PY_FLAGS_WITH_ARG:
            index += 2
            continue
        if token in PY_FLAGS_NO_ARG:
            index += 1
            continue
        if token.startswith("-"):
            raise UnsupportedInvocation(
                f"Unrecognised python flag {token!r} in: {line.strip()!r}. This guard cannot "
                "tell whether a script follows it, so it refuses rather than guessing."
            )
        candidate = _normalise(token)
        if not candidate.endswith(".py"):
            raise UnsupportedInvocation(
                f"python argument {token!r} is not a literal .py path in: {line.strip()!r}. "
                "A variable or computed script path cannot be checked against paths:; name it "
                "literally, or list it in RELEASE_HELPER_SCRIPTS and extend this parser."
            )
        if not (ROOT / candidate).is_file():
            raise UnsupportedInvocation(
                f"python script {candidate!r} does not exist in the tree (from: {line.strip()!r})."
            )
        return {candidate}
    raise UnsupportedInvocation(
        f"Bare python invocation with no resolvable argument in: {line.strip()!r}."
    )


def _resolve_shell(interpreter: str, argv: List[str], line: str) -> Set[str]:
    index = 0
    while index < len(argv):
        token = argv[index]
        if token in SHELL_TERMINAL_OK:
            return set()
        if token in SHELL_FLAGS_WITH_ARG:
            index += 2
            continue
        if token in SHELL_FLAGS_NO_ARG:
            index += 1
            continue
        if token.startswith("-"):
            raise UnsupportedInvocation(
                f"Unrecognised {interpreter} flag {token!r} in: {line.strip()!r}. Refusing "
                "rather than guessing whether a script follows it."
            )
        candidate = _normalise(token)
        if not (ROOT / candidate).is_file():
            raise UnsupportedInvocation(
                f"{interpreter} target {token!r} does not resolve to a repository file "
                f"(from: {line.strip()!r}). A wrapper that runs repository code must be named "
                "literally so its path can be checked against paths:."
            )
        return {candidate}
    raise UnsupportedInvocation(
        f"Bare {interpreter} invocation with no resolvable argument in: {line.strip()!r}."
    )


def referenced_scripts(workflow_text: str) -> Set[str]:
    """Repo scripts this workflow executes. Fail-CLOSED: unresolved forms raise.

    The default is inverted deliberately. Three rounds of review found this
    discovery fail-open in a different way each time -- first the regex shape,
    then invocation indirection, then module form / quoted local actions /
    variable interpreters -- and each fix had enumerated only the forms just
    demonstrated. So the rule is no longer "recognise these executions"; it is
    "everything execution-shaped must fully resolve, and anything else raises".

    Still a cross-check on RELEASE_HELPER_SCRIPTS, never a replacement for it.
    """
    found: Set[str] = set()
    for line in _body_lines(workflow_text):
        uses = USES_KEY.match(line)
        if uses:
            target = _normalise(uses.group(1).split("#")[0])
            if uses.group(1).strip().strip("\"'").startswith((".", "/")):
                raise UnsupportedInvocation(
                    f"Local composite action {target!r} in: {line.strip()!r}. Its run: bodies "
                    "live in another file this guard does not read, so the scripts it executes "
                    "cannot be cross-checked. Extend the guard to follow it, or list its "
                    "scripts in RELEASE_HELPER_SCRIPTS explicitly."
                )
            continue  # `owner/repo@sha` -- not a repository path.

        if SHELL_KEY.match(line):
            continue  # `shell: pwsh` names an interpreter but runs nothing.

        attributed: Set[str] = set()
        for match in INTERPRETER_TOKEN.finditer(line):
            interpreter = match.group(1)
            argv = line[match.end() :].split()
            if interpreter.startswith("python"):
                attributed |= _resolve_python(argv, line)
            else:
                attributed |= _resolve_shell(interpreter, argv, line)
        found |= attributed

        # A script path that no named interpreter accounted for: `$PYTHON x.py`,
        # `%PY% x.py`, `./wrapper.sh`, or a bare path under some other key. If it
        # exists in the tree it is a dependency the guard cannot attribute, so it
        # raises rather than being passed over.
        for token in line.split():
            candidate = _normalise(token)
            if not candidate.endswith(SCRIPT_SUFFIXES) or candidate in attributed:
                continue
            if (ROOT / candidate).is_file():
                raise UnsupportedInvocation(
                    f"Repository script {candidate!r} appears in: {line.strip()!r} without a "
                    "literally named interpreter (a variable interpreter, a wrapper, or another "
                    "key). This guard cannot confirm how it runs, so it refuses; name the "
                    "interpreter literally or list the script in RELEASE_HELPER_SCRIPTS."
                )
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

    def test_question_mark_is_refused_not_treated_as_fnmatch(self) -> None:
        """GitHub's `?` is zero-or-one of the PRECEDING character.

        Translating it the fnmatch way (`[^/]`) claimed
        `tests/ci/resolve_export_template.?y` covers `...template.py`, which
        GitHub does not. Refused until implemented against the documented
        behaviour.
        """
        with self.assertRaises(UnsupportedFilterPattern) as ctx:
            github_path_match(
                "tests/ci/resolve_export_template.?y", "tests/ci/resolve_export_template.py"
            )
        self.assertIn("PRECEDING", str(ctx.exception))

    def test_double_star_slash_is_refused_because_zero_levels_must_match(self) -> None:
        """`a/**/b` must also cover `a/b`; the `**` -> `.*` translation does not."""
        with self.assertRaises(UnsupportedFilterPattern) as ctx:
            github_path_match("tests/**/helper.py", "tests/helper.py")
        self.assertIn("ZERO", str(ctx.exception))

    def test_trailing_double_star_is_still_supported(self) -> None:
        # `core/**` has no following slash, so it stays modelled -- the real
        # workflow depends on it.
        self.assertTrue(github_path_match("core/**", "core/io/file.cpp"))

    def test_the_real_workflow_uses_no_unsupported_pattern(self) -> None:
        filters = parse_event_paths(_workflow_text())
        for event in FILTERED_EVENTS:
            for pattern in filters[event]:
                with self.subTest(event=event, pattern=pattern):
                    github_path_match(pattern, "some/path.py")


class UnsupportedInvocationTests(unittest.TestCase):
    """Unresolved invocations are errors, not silence."""

    def test_variable_script_path_raises(self) -> None:
        with self.assertRaises(UnsupportedInvocation):
            referenced_scripts('        run: python "$SCRIPT" --flag\n')

    def test_nonexistent_script_raises(self) -> None:
        with self.assertRaises(UnsupportedInvocation):
            referenced_scripts("        run: python tests/ci/not_a_real_script.py\n")

    def test_unknown_flag_raises_rather_than_being_skipped(self) -> None:
        with self.assertRaises(UnsupportedInvocation):
            referenced_scripts("        run: python --frobnicate tests/ci/release_attestation.py\n")

    def test_bare_python_raises(self) -> None:
        with self.assertRaises(UnsupportedInvocation):
            referenced_scripts("        run: python\n")

    def test_dash_u_and_other_no_arg_flags_still_find_the_script(self) -> None:
        found = referenced_scripts("        run: python -u tests/ci/release_attestation.py x\n")
        self.assertEqual(found, {"tests/ci/release_attestation.py"})

    def test_inline_and_allowlisted_module_forms_need_no_script(self) -> None:
        for body in (
            "        run: python -m pip install --upgrade pip\n",
            "        run: python -m SCons @args\n",
            '        run: python -c "import sys"\n',
            "        run: python --version\n",
        ):
            with self.subTest(body=body.strip()):
                self.assertEqual(referenced_scripts(body), set())

    def test_repo_prefix_from_the_self_hosted_checkout_is_stripped(self) -> None:
        found = referenced_scripts("        run: python repo/tests/ci/release_attestation.py generate\n")
        self.assertEqual(found, {"tests/ci/release_attestation.py"})

    def test_commented_out_invocations_are_ignored(self) -> None:
        self.assertEqual(referenced_scripts("          # python $UNPARSEABLE\n"), set())

    def test_the_real_workflow_resolves_cleanly(self) -> None:
        # If this raises, a new invocation form landed and the guard is telling
        # you it can no longer vouch for coverage.
        self.assertTrue(referenced_scripts(_workflow_text()))


class FailClosedExecutionFormTests(unittest.TestCase):
    """The four fail-open forms found in round 6, each now resolved or refused.

    Enumerating cases is what failed three rounds running, so the rule that
    covers them is structural: an execution-shaped line must fully resolve or
    raise. These tests are the evidence for that rule, not the rule itself.
    """

    def test_module_form_resolves_to_repository_code(self) -> None:
        # `python -m tests.ci.release_attestation` runs repository code and must
        # be DISCOVERED, not waved through as "a module, so no script".
        found = referenced_scripts("        run: python -m tests.ci.release_attestation generate\n")
        self.assertEqual(found, {"tests/ci/release_attestation.py"})

    def test_unknown_non_repository_module_raises(self) -> None:
        with self.assertRaises(UnsupportedInvocation) as ctx:
            referenced_scripts("        run: python -m tests.ci.release_helper\n")
        self.assertIn("ALLOWED_PYTHON_MODULES", str(ctx.exception))

    def test_quoted_local_composite_action_raises(self) -> None:
        for body in (
            '      - uses: "./.github/actions/helper"\n',
            "      - uses: './.github/actions/helper'\n",
            "      - uses: ./.github/actions/helper\n",
        ):
            with self.subTest(body=body.strip()):
                with self.assertRaises(UnsupportedInvocation):
                    referenced_scripts(body)

    def test_third_party_action_is_still_accepted(self) -> None:
        self.assertEqual(
            referenced_scripts("      - uses: actions/checkout@11d5960a # v4\n"), set()
        )

    def test_variable_interpreter_raises(self) -> None:
        for body in (
            "        run: $PYTHON tests/ci/release_attestation.py\n",
            "        run: ${PYTHON} tests/ci/release_attestation.py\n",
            "        run: %PY% tests/ci/release_attestation.py\n",
        ):
            with self.subTest(body=body.strip()):
                with self.assertRaises(UnsupportedInvocation) as ctx:
                    referenced_scripts(body)
                self.assertIn("without a literally named interpreter", str(ctx.exception))

    def test_shell_wrapper_running_repository_code_is_discovered(self) -> None:
        found = referenced_scripts("        run: bash misc/scripts/gitignore_check.sh\n")
        self.assertEqual(found, {"misc/scripts/gitignore_check.sh"})

    def test_shell_wrapper_that_does_not_resolve_raises(self) -> None:
        with self.assertRaises(UnsupportedInvocation):
            referenced_scripts("        run: bash tools/not_in_the_tree.sh\n")

    def test_shell_key_declaring_an_interpreter_is_not_an_invocation(self) -> None:
        self.assertEqual(referenced_scripts("        shell: pwsh\n"), set())

    def test_paths_entries_are_not_scanned_as_executions(self) -> None:
        # They are quoted script paths with no interpreter; scanning them would
        # flag the very list this guard checks against.
        self.assertEqual(
            referenced_scripts(
                'on:\n  push:\n    paths:\n      - "tests/ci/release_attestation.py"\n'
            ),
            set(),
        )


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


class SmokeDataDependencyTests(unittest.TestCase):
    """The export smoke test's non-script inputs must trigger the workflow too.

    `export_smoke_windows` is a BLOCKING job, so a push that changes only the
    preset template or the probe script and skips `release_builds.yml` entirely
    is the gate not running on the change it exists to validate -- the same
    defect as the helper-script gap above, one layer out from the executed
    scripts the manifest covers.
    """

    def setUp(self) -> None:
        self.text = _workflow_text()
        self.filters = parse_event_paths(self.text)
        self.dependencies = derived_smoke_dependencies()

    def test_every_derived_dependency_triggers_the_workflow(self) -> None:
        for event in FILTERED_EVENTS:
            for dependency in self.dependencies:
                with self.subTest(event=event, dependency=dependency):
                    self.assertTrue(
                        path_is_covered(self.filters[event], dependency),
                        f"{dependency} is consumed by export_smoke_windows (derived from a "
                        f"run_export_smoke.py constant) but no `{event}` paths: filter matches "
                        f"it, so changing it would skip the blocking smoke test. Current "
                        f"filters: {self.filters[event]}",
                    )

    def test_the_derivation_is_not_vacuous(self) -> None:
        # A derivation that silently stopped finding anything would make the
        # check above pass over an empty set -- how this class of gap survives.
        # These four are the inputs the review named; if the derivation stops
        # producing them it has broken, whatever else it still returns.
        for expected in (
            "tests/runtime/export_smoke_preset.cfg.in",
            "tests/examples/godot/test_project/project.godot",
            "tests/examples/godot/test_project/tests/export_smoke_probe.gd",
            "tests/runtime/prepare_synthetic_assets.py",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, self.dependencies)

    def test_the_derivation_finds_the_imported_module(self) -> None:
        # Imported, never invoked: invisible to the executed-script discovery,
        # so it used to be hand-listed in the filters with a comment.
        self.assertIn("tests/runtime/run_runtime_validation.py", self.dependencies)

    def test_every_derived_dependency_exists(self) -> None:
        for dependency in self.dependencies:
            with self.subTest(dependency=dependency):
                self.assertTrue(
                    (ROOT / dependency).is_file(),
                    f"{dependency} is derived as a smoke-test input but does not exist; the "
                    "constant it came from is stale.",
                )

    def test_an_undispositioned_directory_constant_fails_closed(self) -> None:
        module = _import_smoke_runner()
        sentinel = "GUARD_PROBE_DIR"
        setattr(module, sentinel, ROOT / "tests" / "ci")
        self.addCleanup(delattr, module, sentinel)
        with self.assertRaises(UnmodelledDependency) as ctx:
            derived_smoke_dependencies()
        self.assertIn(sentinel, str(ctx.exception))

    def test_a_new_file_constant_is_required_to_be_covered(self) -> None:
        # Discrimination for the coverage test: it must actually be able to
        # fail. A constant naming a file no filter matches has to come back
        # uncovered rather than being quietly dropped by the derivation.
        module = _import_smoke_runner()
        sentinel = "GUARD_PROBE_FILE"
        setattr(module, sentinel, ROOT / "misc" / "hooks" / "pre-commit")
        self.addCleanup(delattr, module, sentinel)
        derived = derived_smoke_dependencies()
        self.assertIn("misc/hooks/pre-commit", derived)
        for event in FILTERED_EVENTS:
            self.assertFalse(path_is_covered(self.filters[event], "misc/hooks/pre-commit"))

    def test_a_dependency_outside_the_repository_fails_closed(self) -> None:
        module = _import_smoke_runner()
        sentinel = "GUARD_PROBE_OUTSIDE"
        setattr(module, sentinel, ROOT.parent / "somewhere-else" / "thing.cfg")
        self.addCleanup(delattr, module, sentinel)
        with self.assertRaises(UnmodelledDependency):
            derived_smoke_dependencies()

    def test_dispositioned_directories_are_still_directories(self) -> None:
        module = _import_smoke_runner()
        for name, disposition in DIRECTORY_DEPENDENCY_DISPOSITIONS.items():
            with self.subTest(constant=name):
                value = getattr(module, name, None)
                self.assertIsInstance(
                    value,
                    Path,
                    f"run_export_smoke.{name} is dispositioned as a directory but no longer "
                    "exists as a Path constant; drop the disposition or fix the name.",
                )
                self.assertTrue(value.resolve().is_dir())
                for relative in disposition.triggering:
                    self.assertTrue(
                        (ROOT / relative).is_file(),
                        f"{relative} is dispositioned for {name} but does not exist.",
                    )


class WholeProjectInputDispositionTests(unittest.TestCase):
    """The smoke test's project inputs are broader than its triggers -- say so.

    Round 2 dispositioned `PROJECT_DIR` down to `project.godot` and gave a real
    reason (a `test_project/**` filter fires four release builds on every
    fixture rebake). The reason is sound; the presentation was not. The preset
    exports `all_resources`, the run imports the whole project first, so the
    true input set of a BLOCKING job is every tracked file under it -- and a
    three-file disposition read as "this is covered" rather than as "we chose
    not to cover this".

    Three routes were on the table and two were rejected on evidence:

    * a minimal dedicated smoke project, or a selective `export_filter`. Both
      shrink the input set honestly, and both change what the blocking gate
      actually exports -- which cannot be verified without running the export on
      the GPU runner. Shipping an unverified change to a blocking gate to settle
      a documentation-honesty finding is the worse trade. The selective filter
      is additionally self-defeating here: `resources`/`scenes`/`customized`
      export a dependency CLOSURE the editor computes, so the guard would still
      be unable to enumerate the input set -- a smaller gap, and an invisible
      one.
    * covering the actual inputs with `tests/examples/godot/test_project/**`.
      Truthful, and it reopens the four-builds-per-rebake problem the
      disposition exists to avoid. Those fixtures are rebaked routinely.

    So the trigger stays narrow and the disposition is made honest instead: the
    untriggered set is derived and printed, and the accepted risk is bounded by
    two things that are CHECKED -- a compensating workflow that triggers on
    those files and loads the same project, and this workflow's own nightly
    schedule. Silence is what was rejected, not the narrow filter.
    """

    def setUp(self) -> None:
        self.text = _workflow_text()
        self.filters = parse_event_paths(self.text)

    def test_the_disposition_is_reported_in_the_guards_output(self) -> None:
        # Not a comment: a maintainer running this guard must SEE that the
        # blocking smoke test consumes far more than it is triggered by.
        for event in FILTERED_EVENTS:
            description = describe_project_input_disposition(event)
            print(description)
            self.assertIn("knowingly do NOT", description)
            self.assertIn("Compensating lane", description)

    def test_the_untriggered_set_is_real_and_named(self) -> None:
        """The check that the record is a record, not an empty formality."""
        for event in FILTERED_EVENTS:
            state = project_input_disposition(event)
            with self.subTest(event=event):
                self.assertTrue(
                    state.triggering,
                    "No project file triggers release_builds.yml at all; the filter has "
                    "stopped covering the inputs it is supposed to cover.",
                )
                self.assertTrue(
                    state.untriggered,
                    "Every tracked project file now triggers release_builds.yml, so the "
                    "`whole_tree` disposition is recording a risk that no longer exists. "
                    "Drop it rather than leaving a stale accepted-risk note.",
                )
                # The export packs scripts and scenes, and those are the inputs
                # whose regressions the smoke test would actually notice. If the
                # untriggered set were only binary fixtures the risk statement
                # would be overstated; it is not, and that is the point.
                self.assertTrue(
                    [path for path in state.untriggered if path.endswith((".gd", ".tscn"))],
                    "The untriggered set contains no .gd/.tscn resources, which is not what "
                    "the tree looks like; the derivation has narrowed unexpectedly.",
                )

    def test_the_accepted_risk_names_a_compensating_lane_that_covers_it(self) -> None:
        """The bound on the risk is checked, not asserted.

        `gaussian_production_gates.yml` filters on `tests/**` and loads the same
        project with a real editor, so an import or load regression in an
        untriggered file still fails on the same push. If that stops being true
        -- the filter narrows, the workflow stops using the project -- the
        accepted risk grows and this fails rather than going quiet.
        """
        state = project_input_disposition("push")
        compensating = ROOT / state.compensating_workflow
        self.assertTrue(compensating.is_file(), f"{state.compensating_workflow} does not exist")
        text = compensating.read_text(encoding="utf-8")
        filters = parse_event_paths(text)

        project_relative = _repo_relative(_import_smoke_runner().PROJECT_DIR, "PROJECT_DIR")
        self.assertTrue(
            any(project_relative in line for line in _body_lines(text)),
            f"{state.compensating_workflow} never names {project_relative} outside its `on:` "
            "block, so it cannot be the lane that still exercises the untriggered inputs.",
        )

        for event in FILTERED_EVENTS:
            self.assertIn(event, filters, f"{state.compensating_workflow} has no {event} paths:")
            uncovered = [
                path for path in state.untriggered if not path_is_covered(filters[event], path)
            ]
            self.assertEqual(
                uncovered,
                [],
                f"{len(uncovered)} project files trigger neither release_builds.yml nor the "
                f"compensating lane {state.compensating_workflow} on {event} (e.g. "
                f"{uncovered[:3]}), so a change to one of them would be validated by no lane "
                "that loads this project. Either cover them or re-state the accepted risk.",
            )

    def test_a_scheduled_run_bounds_how_long_an_untriggered_change_goes_unexecuted(self) -> None:
        # The other half of the bound: whatever a push skips, the nightly runs.
        # Without a schedule the accepted risk is unbounded in time and the
        # disposition would have to be rewritten.
        self.assertRegex(
            self.text,
            r"(?m)^  schedule:\s*$",
            "release_builds.yml no longer has a `schedule:` trigger, so an untriggered change "
            "to a project input could go indefinitely without the blocking smoke test running "
            "on it. The PROJECT_DIR accepted-risk statement depends on that schedule.",
        )

    def test_an_unmodelled_export_filter_fails_closed(self) -> None:
        for value in ("resources", "scenes", "customized", "exclude"):
            with self.subTest(export_filter=value):
                self.assertNotIn(value, WHOLE_TREE_EXPORT_FILTERS)

    def test_the_preset_still_exports_the_whole_project(self) -> None:
        self.assertIn(smoke_export_filter(), WHOLE_TREE_EXPORT_FILTERS)

    def test_an_ambiguous_preset_raises(self) -> None:
        for text in ("", 'export_filter="a"\nexport_filter="b"\n'):
            with self.subTest(text=text):
                with self.assertRaises(UnmodelledDependency):
                    export_filter_from_text(text)

    def test_the_reader_ignores_commented_lines(self) -> None:
        # `.cfg` comments start with `;`, and the template's header talks about
        # the preset it writes.
        self.assertEqual(
            export_filter_from_text('; export_filter="scenes" was considered\nexport_filter="all_resources"\n'),
            "all_resources",
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
