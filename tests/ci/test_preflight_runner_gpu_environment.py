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


# --------------------------------------------------------------------------
# The preflight's own logic
# --------------------------------------------------------------------------

_CONTROL_TEXT = """\
[Vulkan Loader] INFO | LAYER:   Insert instance layer "VK_LAYER_OBS_HOOK" (C:\\ProgramData\\obs-studio-hook\\graphics-hook64.dll)
[Vulkan Loader] INFO | LAYER:   Insert instance layer "VK_LAYER_OW_OVERLAY" (C:\\Program Files (x86)\\Overwolf\\0.305.0.9\\owclient.dll)
[Vulkan Loader] INFO | LAYER:   Insert instance layer "VK_LAYER_NV_optimus" (C:\\WINDOWS\\System32\\nvoglv64.dll)
[Vulkan Loader] INFO | LAYER:   Insert instance layer "VK_LAYER_NV_present" (C:\\WINDOWS\\System32\\nvoglv64.dll)
[Vulkan Loader] INFO | LAYER:   Inserted device layer "VK_LAYER_OBS_HOOK" (C:\\ProgramData\\obs-studio-hook\\graphics-hook64.dll)
[Vulkan Loader] INFO | LAYER:   Inserted device layer "VK_LAYER_NV_optimus" (C:\\WINDOWS\\System32\\nvoglv64.dll)
[Vulkan Loader] INFO | LAYER:   Inserted device layer "VK_LAYER_NV_present" (C:\\WINDOWS\\System32\\nvoglv64.dll)
"""

_CLEAN_TEXT = """\
[Vulkan Loader] INFO | LAYER:   Insert instance layer "VK_LAYER_NV_optimus" (C:\\WINDOWS\\System32\\nvoglv64.dll)
[Vulkan Loader] INFO | LAYER:   Insert instance layer "VK_LAYER_NV_present" (C:\\WINDOWS\\System32\\nvoglv64.dll)
[Vulkan Loader] INFO | LAYER:   Inserted device layer "VK_LAYER_NV_optimus" (C:\\WINDOWS\\System32\\nvoglv64.dll)
[Vulkan Loader] INFO | LAYER:   Inserted device layer "VK_LAYER_NV_present" (C:\\WINDOWS\\System32\\nvoglv64.dll)
"""


def _probe(text: str, returncode: int = 0):
    """A fake `subprocess.run` result for the loader probe."""
    return mock.Mock(returncode=returncode, stdout=text, stderr="")


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


if __name__ == "__main__":
    unittest.main(verbosity=2)
