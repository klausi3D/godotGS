#!/usr/bin/env python3
"""Every GPU job disables the third-party Vulkan layers AND proves it (#875).

`tests/ci/preflight_runner_gpu_environment.py` is the preflight. This is the guard
that makes it reach the jobs that need it, and that keeps the two halves of the
fix from drifting apart.

Why a guard and not a checklist
-------------------------------
The fix has two halves and each is useless alone:

* the **disable** -- `VK_LOADER_LAYERS_DISABLE` / `VK_LOADER_LAYERS_ENABLE`
  exported by the job. On its own it is unfalsifiable: a value the loader does
  not understand is ignored in silence and the job stays green.
* the **preflight** -- which reads the layer chain the loader actually built and
  fails on anything that is not the GPU driver's own.

A job with the variables but no preflight has a fix nobody can check. A job with
the preflight but no variables goes red. Both halves, in every GPU job, or the
gate means nothing -- so this guard checks both, in a job set it **derives**.

Deriving the job set
--------------------
The GPU pool is `runs-on:` label routing, not a list written here: every
self-hosted job in `.github/workflows/*.yml` carrying the `gpu` label. The
self-hosted classification is reused wholesale from
`tests/ci/test_release_builds_runner_trust.py`, which already models label
routing properly (a job reaches the persistent runner through its custom labels
whether or not it spells out `self-hosted`) and fails closed on any `runs-on:`
form it cannot read. #825 is the precedent for why: the previous list of
self-hosted jobs was hand-written prose, and it was already stale by the time
anyone read it.

An empty derived set is a failure, not a pass. "The sweep found no GPU jobs" and
"there are no GPU jobs" must never produce the same result.

Ordering
--------
The preflight has to run **before** the build and before anything touching the
GPU; a preflight at the end of a job reports on an environment whose damage is
already in the artifacts. So the guard requires the preflight's invocation to
precede the job's first `SCons` invocation, and requires that a `SCons`
invocation exist -- an ordering assertion with nothing to order against would
pass for the wrong reason.

Run directly (``python tests/ci/test_preflight_runner_gpu_environment.py``) or via
``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path
from typing import Dict, List, Optional, Tuple
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = ROOT / ".github" / "workflows"
README = WORKFLOW_DIR / "README.md"
FIXTURES = Path(__file__).resolve().parent / "fixtures"

sys.path.insert(0, str(Path(__file__).resolve().parent))

import preflight_runner_gpu_environment as preflight  # noqa: E402
import test_release_builds_runner_trust as trust  # noqa: E402

#: The label that marks the GPU pool inside the self-hosted runner inventory.
GPU_LABEL = "gpu"

#: Relative path of the preflight, as it appears in a workflow `run:`.
PREFLIGHT_SCRIPT = "tests/ci/preflight_runner_gpu_environment.py"

#: The build invocation the preflight must precede. Spelled with the `python -m`
#: prefix on purpose: a bare `SCons` also matches the words `SConstruct` and
#: `SCsub` in every one of these jobs' fork-guard comments, which put the
#: "build" at the top of the job and made the ordering assertion fail on
#: prose. Comment lines are skipped as well (see `first_index`).
BUILD_MARKER = "python -m SCons"

README_SECTION_HEADING = "## Self-hosted GPU runner environment"

_JOB_ENV_KEY = re.compile(r"^    env:\s*$")
_JOB_ENV_ENTRY = re.compile(r"^      ([A-Za-z_][A-Za-z0-9_]*):\s*(.*?)\s*$")


class WorkflowContractError(AssertionError):
    """A workflow shape this guard refuses to interpret."""


# --------------------------------------------------------------------------
# Derivation
# --------------------------------------------------------------------------


def workflow_paths() -> List[Path]:
    return sorted(WORKFLOW_DIR.glob("*.yml"))


def job_spans(lines: List[str]) -> Dict[str, Tuple[int, int]]:
    """Job name -> [start, end) line indices of its block.

    Mirrors `test_release_builds_runner_trust.parse_jobs`'s scoping rules: only
    the block under a column-0 `jobs:` counts, so the two-space keys under `on:`
    are never mistaken for jobs.
    """
    spans: Dict[str, Tuple[int, int]] = {}
    in_jobs = False
    current: Optional[str] = None
    start = 0
    for index, line in enumerate(lines):
        if line.rstrip() == "jobs:":
            in_jobs = True
            continue
        if in_jobs and line.strip() and not line.startswith(" "):
            if current is not None:
                spans[current] = (start, index)
                current = None
            in_jobs = False
            continue
        if not in_jobs:
            continue
        match = trust.JOB_KEY.match(line)
        if match:
            if current is not None:
                spans[current] = (start, index)
            current = match.group(1)
            start = index
    if current is not None:
        spans[current] = (start, len(lines))
    return spans


def gpu_jobs() -> Dict[Tuple[str, str], List[str]]:
    """(workflow file name, job name) -> the job's lines, for every GPU-pool job.

    Self-hosted classification is `test_release_builds_runner_trust`'s, so a job
    that reaches the persistent runner without spelling out `self-hosted` is
    still seen, and any unmodelled `runs-on:` raises instead of dropping out.
    """
    policy = trust.runner_label_policy()
    found: Dict[Tuple[str, str], List[str]] = {}
    for path in workflow_paths():
        lines = path.read_text(encoding="utf-8").splitlines()
        jobs = trust.parse_jobs(lines)
        spans = job_spans(lines)
        for name, data in jobs.items():
            labels = {str(label).lower() for label in data["runs_on"]}
            if trust.classify_runner(data["runs_on"], name, policy) != "self-hosted":
                continue
            if GPU_LABEL not in labels:
                continue
            if name not in spans:
                raise WorkflowContractError(
                    f"{path.name}: job {name!r} was parsed but its block could not be "
                    "located; the two parsers disagree, so nothing about it can be checked."
                )
            start, end = spans[name]
            found[(path.name, name)] = lines[start:end]
    return found


def job_level_env(job_lines: List[str]) -> Dict[str, str]:
    """The job's own `env:` mapping (4-space key, 6-space entries).

    Step-level `env:` blocks sit deeper (6 or 8 spaces depending on the file's
    step indentation) and are deliberately not collected: a variable set on one
    step does not protect the rest of the job.
    """
    env: Dict[str, str] = {}
    index = 0
    while index < len(job_lines):
        if _JOB_ENV_KEY.match(job_lines[index]):
            index += 1
            while index < len(job_lines):
                line = job_lines[index]
                if not line.strip() or line.strip().startswith("#"):
                    index += 1
                    continue
                entry = _JOB_ENV_ENTRY.match(line)
                if not entry:
                    break
                env[entry.group(1)] = entry.group(2).strip().strip("\"'")
                index += 1
            continue
        index += 1
    return env


def first_index(job_lines: List[str], needle: str) -> Optional[int]:
    """First line containing `needle`, ignoring YAML comments.

    Comments are skipped because a job that only *mentions* the preflight in a
    comment does not run it, and a comment naming the build tool is not a build
    step -- either would make these assertions read the wrong line.
    """
    for index, line in enumerate(job_lines):
        if line.lstrip().startswith("#"):
            continue
        if needle in line:
            return index
    return None


# --------------------------------------------------------------------------
# The workflow contract
# --------------------------------------------------------------------------


class GpuJobEnvironmentContract(unittest.TestCase):
    def setUp(self) -> None:
        self.jobs = gpu_jobs()

    def test_gpu_pool_is_not_empty(self) -> None:
        """A derivation that found nothing must fail, not pass quietly."""
        self.assertTrue(
            self.jobs,
            f"No self-hosted job in {WORKFLOW_DIR} carries the {GPU_LABEL!r} label, so every "
            "assertion below would pass over an empty set. Either the GPU pool moved to a "
            "different label -- update GPU_LABEL -- or the `runs-on:` parser stopped seeing "
            "these jobs. Do not read this as 'nothing to check'.",
        )

    def test_every_gpu_job_disables_third_party_implicit_layers(self) -> None:
        for (workflow, job), lines in sorted(self.jobs.items()):
            with self.subTest(workflow=workflow, job=job):
                env = job_level_env(lines)
                self.assertEqual(
                    env.get(preflight.LOADER_LAYERS_DISABLE_VAR),
                    preflight.LOADER_LAYERS_DISABLE_VALUE,
                    f"{workflow}: job {job!r} runs on the self-hosted GPU pool but does not "
                    f"export {preflight.LOADER_LAYERS_DISABLE_VAR}="
                    f"{preflight.LOADER_LAYERS_DISABLE_VALUE!r} at job level. Without it the "
                    "runner's third-party implicit Vulkan layers (OBS hook, Overwolf "
                    "overlay + graphics hook, and any newly installed one) inject into every "
                    "GPU process this job starts (#875).",
                )
                self.assertEqual(
                    env.get(preflight.LOADER_LAYERS_ENABLE_VAR),
                    preflight.LOADER_LAYERS_ENABLE_VALUE,
                    f"{workflow}: job {job!r} must re-enable the GPU driver's own implicit "
                    f"layers with {preflight.LOADER_LAYERS_ENABLE_VAR}="
                    f"{preflight.LOADER_LAYERS_ENABLE_VALUE!r}. `~implicit~` alone removes "
                    "those too, which changes the driver stack rather than only removing "
                    "third-party instrumentation.",
                )

    def test_every_gpu_job_sets_the_per_layer_disable_variables(self) -> None:
        """The half that actually works on this runner (#878).

        The two variables above are read through the loader's `secure_getenv`
        and are discarded outright in an elevated process -- which every job on
        this pool is, because the runner service is registered as LocalSystem.
        These per-layer opt-outs come from each layer's own manifest and are
        read unprivileged, so they are what removes the layers here. A job
        carrying only the loader-filter pair looks configured and is not.
        """
        for (workflow, job), lines in sorted(self.jobs.items()):
            env = job_level_env(lines)
            for layer, variable, value in preflight.LAYER_DISABLE_ENV:
                with self.subTest(workflow=workflow, job=job, layer=layer):
                    self.assertEqual(
                        env.get(variable),
                        value,
                        f"{workflow}: job {job!r} runs on the self-hosted GPU pool but does "
                        f"not export {variable}={value!r}, the `disable_environment` opt-out "
                        f"{layer} declares in its own manifest. Without it that layer stays "
                        "in the chain of every GPU process this job starts: the loader "
                        f"discards {preflight.LOADER_LAYERS_DISABLE_VAR} in an elevated "
                        "process, so it will not cover for this (#878).",
                    )

    def test_every_gpu_job_runs_the_preflight(self) -> None:
        for (workflow, job), lines in sorted(self.jobs.items()):
            with self.subTest(workflow=workflow, job=job):
                self.assertIsNotNone(
                    first_index(lines, PREFLIGHT_SCRIPT),
                    f"{workflow}: job {job!r} sets the layer-disable variables but never runs "
                    f"{PREFLIGHT_SCRIPT}. The variables alone are not evidence -- a value the "
                    "loader does not honour is ignored silently and this job would stay green "
                    "with all seven layers still in the chain (#875).",
                )

    def test_preflight_runs_before_the_build(self) -> None:
        for (workflow, job), lines in sorted(self.jobs.items()):
            with self.subTest(workflow=workflow, job=job):
                preflight_at = first_index(lines, PREFLIGHT_SCRIPT)
                build_at = first_index(lines, BUILD_MARKER)
                self.assertIsNotNone(
                    build_at,
                    f"{workflow}: job {job!r} has no {BUILD_MARKER!r} invocation, so this "
                    "ordering assertion has nothing to order the preflight against and would "
                    "pass for the wrong reason. If the job genuinely stopped building, give "
                    "this guard a marker it can order against instead of deleting the check.",
                )
                self.assertIsNotNone(preflight_at)
                self.assertLess(
                    preflight_at,
                    build_at,
                    f"{workflow}: job {job!r} runs {PREFLIGHT_SCRIPT} after it starts "
                    "building. The preflight decides whether everything measured afterwards "
                    "is a measurement of the renderer, so it has to run first.",
                )

    def test_readme_documents_the_gpu_runner_environment(self) -> None:
        text = README.read_text(encoding="utf-8")
        self.assertIn(
            README_SECTION_HEADING,
            text,
            f"{README} must carry a {README_SECTION_HEADING!r} section: the runner's Vulkan "
            "environment is a trust/behaviour property of the persistent runner, and "
            "`.github/workflows/AGENTS.md` requires such properties to be readable there.",
        )
        for job_key in sorted(self.jobs):
            with self.subTest(job=job_key):
                self.assertIn(
                    f"`{job_key[1]}`",
                    text,
                    f"GPU-pool job {job_key[1]!r} ({job_key[0]}) is not named anywhere in "
                    f"{README.name}; the documented picture of what runs on the persistent "
                    "runner is partial again (#825's failure mode).",
                )


class PreflightPolicyMatchesWorkflows(unittest.TestCase):
    """The allowlist the preflight gates on is the one the workflows enable.

    Two hand-maintained lists that must agree is exactly the drift #825 was
    about. If someone adds a driver layer to `VK_LOADER_LAYERS_ENABLE` without
    adding it to `EXPECTED_LAYERS`, every GPU job goes red on a layer we chose to
    keep; the reverse silently widens the allowlist. So they are compared.
    """

    def test_expected_layers_equal_the_enabled_layers(self) -> None:
        self.assertEqual(
            list(preflight.EXPECTED_LAYERS),
            [name for name in preflight.LOADER_LAYERS_ENABLE_VALUE.split(",") if name],
        )

    def test_expected_layers_are_not_empty(self) -> None:
        self.assertTrue(preflight.EXPECTED_LAYERS)

    def test_per_layer_disable_vars_are_stripped_for_the_control_run(self) -> None:
        """A control run that carries the treatment is not a control.

        If a per-layer variable is added to the job env but not to the stripped
        set, the control probe runs with that layer already disabled, "removed
        by the disable" comes back empty, and the two runs stop being
        comparable.
        """
        for layer, variable, _value in preflight.LAYER_DISABLE_ENV:
            with self.subTest(layer=layer):
                self.assertIn(variable, preflight._CONTROL_STRIPPED_VARS)

    def test_per_layer_disable_entries_are_well_formed(self) -> None:
        self.assertTrue(preflight.LAYER_DISABLE_ENV)
        for entry in preflight.LAYER_DISABLE_ENV:
            layer, variable, value = entry
            with self.subTest(layer=layer):
                self.assertTrue(layer.startswith("VK_LAYER_"))
                self.assertTrue(variable and value)
                self.assertNotIn(
                    layer,
                    preflight.EXPECTED_LAYERS,
                    f"{layer} is both disabled per-layer and allowlisted as the GPU driver's "
                    "own. One of the two is wrong.",
                )


# --------------------------------------------------------------------------
# The preflight's own logic
# --------------------------------------------------------------------------

#: Verbatim Vulkan loader output, captured off the runner rather than invented.
#:
#: Provenance: `VK_LOADER_DEBUG=layer,info C:\\Windows\\System32\\vulkaninfo.exe`
#: against `C:\\Windows\\System32\\vulkan-1.dll` 1.4.341.0 -- the chain lines from
#: an interactive-user run, the `elevated permissions` lines from the
#: System-integrity CI job (run 31603970211). Nothing was reformatted.
#:
#: A parser proven only against strings this file invented can drift from the
#: producer's real spacing, prefix or wording and stay green while matching
#: nothing, so the producer's own bytes are the contract, and every scenario
#: below is *built from this file's line shape* rather than typed out again.
LOADER_CAPTURE = FIXTURES / "vulkan_loader_layer_report_1_4_341.txt"
_CAPTURED_TEXT = LOADER_CAPTURE.read_text(encoding="utf-8")

#: The producer's line prefix, taken from the capture instead of assumed:
#: everything ahead of `Insert instance layer` on a real chain line.
_CAPTURED_PREFIX = _CAPTURED_TEXT.split("Insert instance layer", 1)[0].rsplit("\n", 1)[-1]


def loader_line(chain: str, name: str, module: str) -> str:
    """One chain line in the producer's real format.

    `chain` is `"instance"` or `"device"`. The wording and prefix come from the
    captured output, so a scenario cannot quietly assert against a format the
    loader does not emit.
    """
    phrase = "Insert instance layer" if chain == "instance" else "Inserted device layer"
    return f'{_CAPTURED_PREFIX}{phrase} "{name}" ({module})'


_OBS_DLL = "C:\\ProgramData\\obs-studio-hook\\graphics-hook64.dll"
_OW_DLL = "C:\\Program Files (x86)\\Overwolf\\0.305.0.9\\owclient.dll"
_NV_DLL = (
    "C:\\WINDOWS\\System32\\DriverStore\\FileRepository"
    "\\nv_dispig.inf_amd64_f4c7a2fd13e0f763\\nvoglv64.dll"
)

_CONTROL_TEXT = "\n".join(
    [
        loader_line("instance", "VK_LAYER_OBS_HOOK", _OBS_DLL),
        loader_line("instance", "VK_LAYER_OW_OVERLAY", _OW_DLL),
        loader_line("instance", "VK_LAYER_NV_optimus", _NV_DLL),
        loader_line("instance", "VK_LAYER_NV_present", _NV_DLL),
        loader_line("device", "VK_LAYER_OBS_HOOK", _OBS_DLL),
        loader_line("device", "VK_LAYER_NV_optimus", _NV_DLL),
        loader_line("device", "VK_LAYER_NV_present", _NV_DLL),
    ]
) + "\n"

_CLEAN_TEXT = "\n".join(
    [
        loader_line("instance", "VK_LAYER_NV_optimus", _NV_DLL),
        loader_line("instance", "VK_LAYER_NV_present", _NV_DLL),
        loader_line("device", "VK_LAYER_NV_optimus", _NV_DLL),
        loader_line("device", "VK_LAYER_NV_present", _NV_DLL),
    ]
) + "\n"

#: The driver's own layers stripped along with the third-party ones. Reachable
#: for real if a loader update changes what `~implicit~` covers, and it is the
#: shape that passes an "is anything unexpected present?" check while meaning
#: the GPU jobs now measure a different driver stack.
_NO_LAYERS_TEXT = f"{_CAPTURED_PREFIX}no layers were inserted into the chain\n"


def _probe(text: str, returncode: int = 0):
    """A fake `subprocess.run` result for the loader probe."""
    return mock.Mock(returncode=returncode, stdout=text, stderr="")


class CapturedLoaderOutputContract(unittest.TestCase):
    """The parser is proven against the producer's own bytes, not a model of them.

    Everything else in this file constructs its input. That is fine for
    scenarios, but it cannot detect the failure that matters most: the loader's
    real format differing from what we imagined, leaving a parser that matches
    nothing and a preflight that reports an empty chain -- which the gate then
    has to interpret. So the recorded output of the loader version the runner
    actually has is checked directly.
    """

    def setUp(self) -> None:
        self.layers = preflight.parse_layer_report(_CAPTURED_TEXT)

    def test_capture_is_present_and_is_loader_output(self) -> None:
        """An emptied or replaced capture must fail here, not degrade silently."""
        self.assertTrue(
            LOADER_CAPTURE.is_file(), f"the captured loader output {LOADER_CAPTURE} is missing"
        )
        self.assertIn(preflight._LOADER_DEBUG_MARKER, _CAPTURED_TEXT)
        self.assertGreaterEqual(len(_CAPTURED_TEXT.splitlines()), 20)

    def test_every_layer_the_runner_registers_is_recognised(self) -> None:
        self.assertEqual(
            sorted(self.layers),
            [
                "VK_LAYER_NV_optimus",
                "VK_LAYER_NV_present",
                "VK_LAYER_OBS_HOOK",
                "VK_LAYER_OW_OBS_HOOK",
                "VK_LAYER_OW_OVERLAY",
            ],
        )
        for name, entry in self.layers.items():
            with self.subTest(layer=name):
                self.assertEqual(entry.chains, ("device", "instance"))

    def test_real_module_paths_survive_the_parse(self) -> None:
        self.assertEqual(
            self.layers["VK_LAYER_OW_OVERLAY"].modules,
            ("C:\\Program Files (x86)\\Overwolf\\0.305.0.9\\owclient.dll",),
        )
        self.assertEqual(
            self.layers["VK_LAYER_OBS_HOOK"].modules,
            ("C:\\ProgramData\\obs-studio-hook\\graphics-hook64.dll",),
        )

    def test_elevated_permission_notices_are_read(self) -> None:
        """The line that explains why a set variable did nothing (#878)."""
        ignored = preflight.parse_ignored_env_vars(_CAPTURED_TEXT)
        self.assertIn(preflight.LOADER_LAYERS_DISABLE_VAR, ignored)
        self.assertIn(preflight.LOADER_LAYERS_ENABLE_VAR, ignored)

    def test_scenario_lines_match_the_captured_format(self) -> None:
        """The constructed scenarios below use the producer's real line shape."""
        for line in (_CONTROL_TEXT + _CLEAN_TEXT).splitlines():
            with self.subTest(line=line):
                self.assertRegex(line, preflight._LAYER_LINE)


class LoaderReportParsing(unittest.TestCase):
    def test_instance_and_device_chains_are_both_read(self) -> None:
        layers = preflight.parse_layer_report(_CONTROL_TEXT)
        self.assertEqual(
            sorted(layers),
            [
                "VK_LAYER_NV_optimus",
                "VK_LAYER_NV_present",
                "VK_LAYER_OBS_HOOK",
                "VK_LAYER_OW_OVERLAY",
            ],
        )
        self.assertEqual(layers["VK_LAYER_OBS_HOOK"].chains, ("device", "instance"))
        # Instance-chain only: a layer that never reaches the device chain is
        # still in the instance chain and still ours to report.
        self.assertEqual(layers["VK_LAYER_OW_OVERLAY"].chains, ("instance",))

    def test_module_path_containing_parentheses_is_not_truncated(self) -> None:
        """`C:\\Program Files (x86)\\...` is the common case, not an edge case.

        A non-greedy `[^)]*` truncates it to `C:\\Program Files (x86`, which
        misnames the module in the exact message a maintainer acts on.
        """
        layers = preflight.parse_layer_report(_CONTROL_TEXT)
        self.assertEqual(
            layers["VK_LAYER_OW_OVERLAY"].modules,
            ("C:\\Program Files (x86)\\Overwolf\\0.305.0.9\\owclient.dll",),
        )

    def test_text_without_layer_lines_yields_nothing(self) -> None:
        self.assertEqual(preflight.parse_layer_report("no layers here"), {})


class VulkanLayerGate(unittest.TestCase):
    def _run(self, control: str, effective: str, returncode: int = 0):
        calls = {"n": 0}

        def fake_run(command, **kwargs):
            env = kwargs.get("env") or {}
            stripped = preflight.LOADER_LAYERS_DISABLE_VAR not in env
            calls["n"] += 1
            return _probe(control if stripped else effective, returncode)

        with mock.patch.object(preflight, "resolve_probe", return_value="fake-probe"):
            with mock.patch.dict(
                preflight.os.environ,
                {preflight.LOADER_LAYERS_DISABLE_VAR: preflight.LOADER_LAYERS_DISABLE_VALUE},
            ):
                with mock.patch.object(preflight.subprocess, "run", side_effect=fake_run):
                    return preflight.check_vulkan_layers()

    def test_clean_chain_passes(self) -> None:
        ok, lines, _record = self._run(_CONTROL_TEXT, _CLEAN_TEXT)
        self.assertTrue(ok, "\n".join(lines))
        self.assertIn("VK_LAYER_OBS_HOOK", "\n".join(lines))  # reported as removed

    def test_remaining_third_party_layer_fails_and_is_named(self) -> None:
        ok, lines, _record = self._run(_CONTROL_TEXT, _CONTROL_TEXT)
        self.assertFalse(ok)
        text = "\n".join(lines)
        self.assertIn("VK_LAYER_OBS_HOOK", text)
        self.assertIn("VK_LAYER_OW_OVERLAY", text)
        self.assertIn("graphics-hook64.dll", text)

    def test_empty_control_run_fails_rather_than_passing_vacuously(self) -> None:
        """The assertion that keeps a broken parser from reading as success."""
        ok, lines, _record = self._run("[Vulkan Loader] LAYER: nothing inserted", _CLEAN_TEXT)
        self.assertFalse(ok)
        self.assertIn("cannot discriminate", "\n".join(lines))

    def test_probe_without_loader_debug_output_fails(self) -> None:
        ok, lines, _record = self._run("totally silent", "totally silent")
        self.assertFalse(ok)
        self.assertIn("no Vulkan loader debug output", "\n".join(lines))

    def test_probe_failure_fails(self) -> None:
        ok, lines, _record = self._run(_CONTROL_TEXT, _CLEAN_TEXT, returncode=3)
        self.assertFalse(ok)
        self.assertIn("exited 3", "\n".join(lines))

    def test_effective_layer_absent_from_control_fails(self) -> None:
        ok, lines, _record = self._run(_CLEAN_TEXT, _CONTROL_TEXT)
        self.assertFalse(ok)
        self.assertIn("the control run did not", "\n".join(lines))

    def test_missing_probe_fails(self) -> None:
        with mock.patch.object(preflight, "resolve_probe", return_value=None):
            ok, lines, _record = preflight.check_vulkan_layers()
        self.assertFalse(ok)
        self.assertIn("no Vulkan loader probe found", "\n".join(lines))

    def test_losing_the_driver_layers_fails_rather_than_passing_as_clean(self) -> None:
        """An empty effective chain contains nothing unexpected -- and is a failure.

        `~implicit~` without a working enable produces exactly this, and so
        would a loader update that changes what the enable covers. Checking only
        for the absence of third-party layers would call it a pass while every
        GPU job silently moved to a driver stack nothing else uses.
        """
        ok, lines, _record = self._run(_CONTROL_TEXT, _NO_LAYERS_TEXT)
        self.assertFalse(ok, "\n".join(lines))
        text = "\n".join(lines)
        self.assertIn("missing from the effective chain", text)
        self.assertIn("VK_LAYER_NV_optimus", text)
        self.assertIn("VK_LAYER_NV_present", text)

    def test_losing_one_driver_layer_fails(self) -> None:
        partial = "\n".join(
            [
                loader_line("instance", "VK_LAYER_NV_optimus", _NV_DLL),
                loader_line("device", "VK_LAYER_NV_optimus", _NV_DLL),
            ]
        )
        ok, lines, _record = self._run(_CONTROL_TEXT, partial)
        self.assertFalse(ok)
        self.assertIn("VK_LAYER_NV_present", "\n".join(lines))

    def test_control_without_any_driver_layer_fails(self) -> None:
        """Control saw layers, but none of the driver's own: nothing to check survival against."""
        third_party_only = "\n".join(
            [
                loader_line("instance", "VK_LAYER_OBS_HOOK", _OBS_DLL),
                loader_line("device", "VK_LAYER_OBS_HOOK", _OBS_DLL),
            ]
        )
        ok, lines, _record = self._run(third_party_only, _NO_LAYERS_TEXT)
        self.assertFalse(ok)
        self.assertIn("none of them were the GPU driver's own", "\n".join(lines))

    def test_elevated_notice_is_reported_next_to_the_verdict(self) -> None:
        """Why a set variable did nothing, said in the failure a maintainer reads."""
        elevated = _CONTROL_TEXT + "".join(
            f"{preflight._LOADER_DEBUG_MARKER} INFO:           Loader is running with "
            f"elevated permissions. Environment variable {name} will be ignored\n"
            for name in (
                preflight.LOADER_LAYERS_DISABLE_VAR,
                preflight.LOADER_LAYERS_ENABLE_VAR,
            )
        )
        ok, lines, record = self._run(_CONTROL_TEXT, elevated)
        self.assertFalse(ok)
        text = "\n".join(lines)
        self.assertIn("because this process is elevated", text)
        self.assertIn(preflight.LOADER_LAYERS_DISABLE_VAR, text)
        self.assertEqual(
            sorted(record["ignored_filter_env_vars"]),
            sorted([preflight.LOADER_LAYERS_DISABLE_VAR, preflight.LOADER_LAYERS_ENABLE_VAR]),
        )


class PageHeapFlagReading(unittest.TestCase):
    def test_dword_and_string_forms_are_both_understood(self) -> None:
        # gflags writes REG_SZ, other tooling writes REG_DWORD; reading only one
        # form reports "not set" for an image that is in fact under page heap.
        self.assertTrue(preflight._flag_is_set(0x02000000))
        self.assertTrue(preflight._flag_is_set("0x02000000"))
        self.assertTrue(preflight._flag_is_set("33554432"))
        self.assertTrue(preflight._flag_is_set("verifier.dll"))

    def test_zero_and_absent_forms_are_not_set(self) -> None:
        self.assertFalse(preflight._flag_is_set(0))
        self.assertFalse(preflight._flag_is_set("0x0"))
        self.assertFalse(preflight._flag_is_set("0"))
        self.assertFalse(preflight._flag_is_set(""))

    def test_ci_image_matching(self) -> None:
        self.assertTrue(preflight._is_ci_image("godot.windows.editor.dev.x86_64.exe"))
        self.assertTrue(preflight._is_ci_image("Godot_v4.7-stable_win64.exe"))
        self.assertFalse(preflight._is_ci_image("notepad.exe"))


class _FakeWinreg:
    """Enough of `winreg` to drive the IFEO sweep's error handling.

    `errors` maps a registry step to the OSError it should raise, so a read that
    fails can be told apart from a key that is not there -- which is the whole
    distinction under test.
    """

    HKEY_LOCAL_MACHINE = object()

    def __init__(self, images, errors=None):
        self.images = list(images)
        self.errors = errors or {}

    @staticmethod
    def _oserror(winerror: int) -> OSError:
        exc = OSError(f"winerror {winerror}")
        exc.winerror = winerror
        return exc

    def OpenKey(self, root, path):  # noqa: N802 - mirrors winreg's spelling
        if path in self.errors:
            raise self._oserror(self.errors[path])
        return _FakeKey(self, path)

    def EnumKey(self, key, index):  # noqa: N802
        if index >= len(self.images):
            raise self._oserror(preflight._ERROR_NO_MORE_ITEMS)
        return self.images[index]

    def QueryValueEx(self, key, value_name):  # noqa: N802
        marker = f"{key.path}!{value_name}"
        if marker in self.errors:
            raise self._oserror(self.errors[marker])
        raise self._oserror(preflight._ERROR_FILE_NOT_FOUND)


class _FakeKey:
    def __init__(self, reg, path):
        self.reg = reg
        self.path = path

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class IfeoReadsFailClosed(unittest.TestCase):
    """"Could not read" and "not set" must not produce the same verdict.

    An access-denied IFEO view that still holds a page-heap entry looks exactly
    like a clean machine to a sweep that swallows `OSError`, and Gate 2 would
    report green on a runner it never managed to inspect.
    """

    def _scan(self, images, errors):
        fake = _FakeWinreg(images, errors)
        with mock.patch.dict(sys.modules, {"winreg": fake}):
            return preflight.collect_ifeo_flags()

    def test_unreadable_ci_image_is_an_error_not_an_empty_result(self) -> None:
        scan = self._scan(
            ["godot.windows.editor.dev.x86_64.exe"],
            {"godot.windows.editor.dev.x86_64.exe": 5},  # ERROR_ACCESS_DENIED
        )
        self.assertIsNotNone(scan.error)
        self.assertIn("godot", str(scan.error))
        self.assertEqual(scan.entries, [])

    def test_unreadable_value_on_a_ci_image_is_an_error(self) -> None:
        image = "godot.windows.editor.dev.x86_64.exe"
        scan = self._scan([image], {f"{image}!GlobalFlag": 5})
        self.assertIsNotNone(scan.error)

    def test_unreadable_foreign_image_is_recorded_not_gated(self) -> None:
        """Several IFEO subkeys on this workstation are ACL'd against non-admins.

        Failing on those would mean the preflight could only run as an
        administrator -- a worse failure mode than the one being guarded -- so
        an image CI never executes is reported as unread instead.
        """
        scan = self._scan(["DefenderAgentScan.exe"], {"DefenderAgentScan.exe": 5})
        self.assertIsNone(scan.error)
        # Once per registry view: the sweep walks the native and WOW6432Node views.
        self.assertEqual(
            [entry["image"] for entry in scan.unreadable],
            ["DefenderAgentScan.exe"] * len(preflight._IFEO_PATHS),
        )
        self.assertEqual(
            {entry["view"] for entry in scan.unreadable}, set(preflight._IFEO_PATHS)
        )

    def test_absent_view_is_not_an_error(self) -> None:
        scan = self._scan([], {path: preflight._ERROR_FILE_NOT_FOUND for path in preflight._IFEO_PATHS})
        self.assertIsNone(scan.error)
        self.assertEqual(scan.entries, [])

    def test_unreadable_view_is_an_error(self) -> None:
        scan = self._scan([], {preflight._IFEO_PATHS[0]: 5})
        self.assertIsNotNone(scan.error)


if __name__ == "__main__":
    unittest.main(verbosity=2)
