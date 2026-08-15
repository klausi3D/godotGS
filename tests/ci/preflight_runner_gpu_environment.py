#!/usr/bin/env python3
"""Preflight (#875): the self-hosted GPU runner's environment is *measured*, not assumed.

The persistent self-hosted GPU runner is also the maintainer's workstation, so
its environment carries developer/consumer software that silently rewrites what
every GPU gate means:

* **Vulkan implicit layers.** Seven third-party implicit layers were registered
  under ``HKLM\\SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers`` (RenderDoc, Steam
  overlay, Steam Fossilize, Epic Online Services overlay, OBS studio hook,
  Overwolf overlay + Overwolf graphics hook). A layer in the chain changes
  allocation behaviour, command-buffer handling, pipeline creation and timing --
  even when it is not actively capturing. One of them is already in our logs:
  PR #852's run 31487454038 shows the loader failing to resolve
  ``vkGetInstanceProcAddr`` in Overwolf's ``ow-graphics-vulkan.dll`` while
  building the device chain, in the same job that then failed to create a
  ``RenderingDevice``.
* **Page-heap / Application Verifier IFEO flags.** Full page heap on our own
  binaries makes every allocation slow and every heap layout different, which
  turns a timing gate into a measurement of the debugger. The two entries found
  on this runner have been removed; this preflight asserts they stay removed.

Both are *invisible variables*: nothing in CI reported them, so months of GPU
results were read as measurements of the renderer. This script turns them into
recorded, gating ones.

Why this is not "we set an environment variable and CI went green"
------------------------------------------------------------------
This is not hypothetical here. The first version of this change set
``VK_LOADER_LAYERS_DISABLE=~implicit~`` on every GPU job, verified as the
interactive user that the loader honoured it, and would have shipped -- except
the preflight measured the CI job and found the chain unchanged. The runner
service is registered as ``LocalSystem``, so job steps run at System integrity,
and the loader reads its own filter variables through ``loader_secure_getenv``,
which discards them in an elevated process. The variable was set, the log showed
it set, and it did nothing (#878). What works there is each layer's own
``disable_environment`` variable, which the loader reads unprivileged.

So: setting ``VK_LOADER_LAYERS_DISABLE`` proves nothing on its own -- an
unsupported value, a misspelled one, or a correct one in a process the loader
declines to read it in are all silently ignored and the job stays green.
So this preflight never reads the environment variable, and never reads the
registry. It asks **the loader itself, at runtime**, which layers it inserted
into the instance and device chains, by running a probe process under
``VK_LOADER_DEBUG=layer`` and parsing the loader's own
``Insert instance layer "X"`` / ``Inserted device layer "X"`` messages.

It runs the probe **twice**:

``control``
    with the disable variables stripped from the environment -- what the loader
    would do if we did nothing.
``effective``
    with the environment exactly as this job has it -- what our GPU work
    actually gets.

The control run is what keeps the check from being vacuous. A parser that has
stopped matching the loader's output, a probe that failed to start, or a loader
that emitted nothing would all produce "no layers found" -- indistinguishable
from success. So an empty *control* set is a **failure**, not a pass: the probe
must demonstrate it can see layers before its silence on the effective run means
anything.

Verdict
-------
* Gate 1 fails if the effective chain contains any layer outside
  :data:`EXPECTED_LAYERS` (the GPU driver's own implicit layers), naming each
  unexpected layer and the module the loader loaded for it.
* Gate 1 also fails if a driver layer the control run put on a chain is missing
  from that **same chain** in the effective run. The comparison is per chain, not
  per layer name: the loader builds an instance chain and a device chain and
  reports them separately, so a layer that survives on one and disappears from
  the other keeps its *name* in the effective set while the device stack the GPU
  jobs measure on has changed.
* Gate 1 also fails if the probe could not be validated (missing probe binary,
  non-zero exit, unparseable output, empty control set, a control run that
  never observed one of the two chains, or an effective set that is not a
  subset of the control set).
* Gate 2 fails if page-heap/Application Verifier flags are set for any image we
  execute, or system-wide.

A surprise here is a finding to report, not to add to the allowlist. Widening
:data:`EXPECTED_LAYERS` says "this layer is part of the GPU driver and cannot be
removed", which is a claim about the machine that a human has to make.

Usage::

    python tests/ci/preflight_runner_gpu_environment.py [--json out.json]

Exit codes: ``0`` pass, ``1`` gate failure, ``2`` usage error.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, NamedTuple, Optional, Sequence, Tuple

# --------------------------------------------------------------------------
# Policy
# --------------------------------------------------------------------------

#: Implicit layers that may legitimately remain in the chain: the GPU driver's
#: own. They ship with the NVIDIA display driver, are reinstalled by every driver
#: update, and are part of the vendor's Vulkan implementation rather than
#: third-party instrumentation -- so they are kept rather than stripped, and the
#: workflows re-enable them explicitly (see LOADER_LAYERS_ENABLE below).
#:
#: Nothing else belongs here. This tuple is the allowlist Gate 1 measures the
#: runtime chain against, and `tests/ci/test_preflight_runner_gpu_environment.py`
#: cross-checks it against the value the workflows actually export, so the two
#: cannot drift apart.
EXPECTED_LAYERS: Tuple[str, ...] = (
    "VK_LAYER_NV_optimus",
    "VK_LAYER_NV_present",
)

#: The loader-side filter variables. Measured against Vulkan loader 1.4.341.0
#: (`C:\\Windows\\System32\\vulkan-1.dll`) **as the interactive user**: with
#: `VK_LOADER_LAYERS_DISABLE=~implicit~` alone the loader inserts *no* implicit
#: layers at all, including the driver's own; adding `VK_LOADER_LAYERS_ENABLE`
#: brings back exactly the named ones, so enable takes precedence over disable.
#:
#: **These do nothing in the GPU jobs.** The self-hosted runner service is
#: registered as `LocalSystem`, so every job step runs at System integrity
#: (`S-1-16-16384`), and the loader reads its own filter variables through
#: `loader_secure_getenv`, which drops them when the process is elevated. Run
#: 31603970211 on the runner has the loader saying so in as many words::
#:
#:     [Vulkan Loader] INFO: Loader is running with elevated permissions.
#:                           Environment variable VK_LOADER_LAYERS_DISABLE will be ignored
#:
#: In that run `~implicit~`, `~all~`, and `~implicit~` + enable all produced the
#: byte-identical chain the bare baseline did. They are still exported by the
#: jobs because they are the only mechanism that covers a layer we have not
#: enumerated, and they *are* honoured on any runner whose service does not run
#: elevated -- but what actually strips the layers on this runner is
#: :data:`LAYER_DISABLE_ENV` below. `VK_LOADER_DEBUG` is read with the plain
#: getenv and keeps working while elevated, which is why the preflight can see
#: any of this at all.
LOADER_LAYERS_DISABLE_VAR = "VK_LOADER_LAYERS_DISABLE"
LOADER_LAYERS_DISABLE_VALUE = "~implicit~"
LOADER_LAYERS_ENABLE_VAR = "VK_LOADER_LAYERS_ENABLE"
LOADER_LAYERS_ENABLE_VALUE = ",".join(EXPECTED_LAYERS)

#: The per-layer opt-out each third-party layer declares in its **own** manifest
#: under the `disable_environment` key, read off the runner's registered layer
#: JSONs. The loader reads these with the ordinary getenv, not the secure one,
#: so unlike the filter variables above they are honoured in an elevated
#: process -- measured in run 31603970211, where this set and nothing else
#: reduced the System-integrity chain to the driver's own two layers.
#:
#: `(layer name, variable, value)`. The layer name is not used to build the
#: environment; it records which manifest each variable came from, so a reader
#: can check the claim against the JSON on disk.
#:
#: This list is machine-specific by construction and *will* go stale when the
#: runner gains or updates instrumentation. That is survivable only because it
#: is not the guarantee: Gate 1 measures the chain the loader actually built, so
#: a layer this list does not cover fails the job instead of passing quietly.
LAYER_DISABLE_ENV: Tuple[Tuple[str, str, str], ...] = (
    # C:\ProgramData\obs-studio-hook\obs-vulkan64.json
    ("VK_LAYER_OBS_HOOK", "DISABLE_VULKAN_OBS_CAPTURE", "1"),
    # C:\Program Files (x86)\Overwolf\<version>\ow-graphics-vulkan64.json
    ("VK_LAYER_OW_OBS_HOOK", "DISABLE_VULKAN_OW_OBS_CAPTURE", "1"),
    # C:\Program Files (x86)\Overwolf\<version>\ow-vulkan-overlay64.json
    ("VK_LAYER_OW_OVERLAY", "DISABLE_VULKAN_OW_OVERLAY_LAYER", "1"),
)

#: Environment variables stripped for the control run: everything the jobs set
#: to influence the chain. `VK_INSTANCE_LAYERS` is the legacy force-enable
#: variable and is included so a machine-level setting cannot make the control
#: run look like the effective one. Missing one of the per-layer variables here
#: would leave the control run disabled too, and a control that carries the
#: treatment cannot show what the treatment removed.
_CONTROL_STRIPPED_VARS = (
    LOADER_LAYERS_DISABLE_VAR,
    LOADER_LAYERS_ENABLE_VAR,
    "VK_INSTANCE_LAYERS",
) + tuple(variable for _layer, variable, _value in LAYER_DISABLE_ENV)

#: Substring match, lowercased, against an Image File Execution Options subkey
#: name. These are the images CI executes; page-heap flags on any of them taint
#: our measurements. Other images on this dual-use workstation are recorded but
#: do not gate -- page heap on someone's unrelated tool is not our variable.
CI_IMAGE_NAME_MARKERS = ("godot",)

#: IFEO values that turn on page heap / Application Verifier for an image.
PAGE_HEAP_VALUE_NAMES = ("GlobalFlag", "PageHeapFlags", "VerifierDlls")

#: Override for the loader probe binary (mainly for the guard's own tests).
PROBE_ENV_VAR = "GS_CI_VULKAN_PROBE"

#: The loader's own debug messages, one per layer it inserts into a chain.
#: `Insert instance layer "VK_LAYER_X" (C:\\path\\to.dll)` and the device-chain
#: form `Inserted device layer "..."`. The module path is optional so a loader
#: that stops printing it degrades to a missing path, not a missing layer.
#:
#: The module group is greedy and anchored to end-of-line because the real paths
#: contain parentheses -- `C:\\Program Files (x86)\\Overwolf\\...` truncated to
#: `C:\\Program Files (x86` under a non-greedy `[^)]*`, which would name the
#: offending layer's module wrongly in the very message a maintainer acts on.
_LAYER_LINE = re.compile(
    r'(?P<chain>Insert instance layer|Inserted device layer)\s+"(?P<name>[^"]+)"'
    r'(?:\s*\((?P<module>.*)\))?\s*$',
    re.MULTILINE,
)

#: The loader's INFO message for a filter variable it refused to read because
#: the process is elevated. Captured verbatim from run 31603970211:
#: `Loader is running with elevated permissions. Environment variable
#: VK_LOADER_LAYERS_DISABLE will be ignored`. Parsed so a maintainer reading a
#: failure is told *why* the disable had no effect rather than having to
#: rediscover it -- this is exactly the message that explains a control run and
#: an effective run coming back identical.
_IGNORED_ENV_LINE = re.compile(
    r"running with elevated permissions\.\s*"
    r"Environment variable\s+(?P<name>[A-Za-z0-9_]+)\s+will be ignored"
)

#: What the probe asks the loader to report. `layer` yields the chain messages;
#: `info` is what carries the elevated-permissions notices above, and without it
#: the loader silently declines our variables with nothing in the log.
_PROBE_LOADER_DEBUG = "layer,info"

#: Any line the Vulkan loader's debug channel emits. Used only to tell
#: "the loader said nothing" apart from "the loader said nothing about layers".
_LOADER_DEBUG_MARKER = "[Vulkan Loader]"

PROBE_TIMEOUT_SEC = 300


class LayerRecord(NamedTuple):
    name: str
    modules: Tuple[str, ...]
    chains: Tuple[str, ...]


class ProbeResult(NamedTuple):
    ok: bool
    returncode: Optional[int]
    layers: Dict[str, LayerRecord]
    loader_debug_lines: int
    error: Optional[str]
    command: Tuple[str, ...]
    #: Filter variables the loader told us it discarded because the process is
    #: elevated. Empty on a non-elevated runner.
    ignored_env_vars: Tuple[str, ...] = ()


# --------------------------------------------------------------------------
# Gate 1 -- the Vulkan layer chain, as the loader reports it
# --------------------------------------------------------------------------


def resolve_probe() -> Optional[str]:
    """The binary used to make the loader build a real instance + device chain.

    `vulkaninfo` ships with the Vulkan runtime (it is in `System32` on the
    runner, not only in the SDK) and creates both an instance and a device, so
    the loader emits both chains. Absence is a failure, not a skip: an
    unverifiable environment is exactly what this preflight exists to stop.
    """
    override = os.environ.get(PROBE_ENV_VAR)
    if override:
        return override if Path(override).is_file() else None
    found = shutil.which("vulkaninfo")
    if found:
        return found
    fallback = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "vulkaninfo.exe"
    return str(fallback) if fallback.is_file() else None


def _probe_env(strip_disable: bool) -> Dict[str, str]:
    env = dict(os.environ)
    if strip_disable:
        for name in _CONTROL_STRIPPED_VARS:
            env.pop(name, None)
    # The loader only reports its layer decisions when asked to.
    env["VK_LOADER_DEBUG"] = _PROBE_LOADER_DEBUG
    return env


def parse_ignored_env_vars(text: str) -> Tuple[str, ...]:
    """Filter variables the loader discarded because the process is elevated."""
    return tuple(sorted({match.group("name") for match in _IGNORED_ENV_LINE.finditer(text)}))


def parse_layer_report(text: str) -> Dict[str, LayerRecord]:
    """Layer name -> modules/chains, from the loader's `VK_LOADER_DEBUG=layer` output."""
    collected: Dict[str, Tuple[List[str], List[str]]] = {}
    for match in _LAYER_LINE.finditer(text):
        name = match.group("name")
        module = (match.group("module") or "").strip()
        chain = "instance" if match.group("chain").startswith("Insert instance") else "device"
        modules, chains = collected.setdefault(name, ([], []))
        if module and module not in modules:
            modules.append(module)
        if chain not in chains:
            chains.append(chain)
    return {
        name: LayerRecord(name, tuple(modules), tuple(sorted(chains)))
        for name, (modules, chains) in collected.items()
    }


def run_probe(probe: str, strip_disable: bool) -> ProbeResult:
    command = (probe, "--summary")
    try:
        completed = subprocess.run(
            list(command),
            capture_output=True,
            text=True,
            errors="replace",
            env=_probe_env(strip_disable),
            timeout=PROBE_TIMEOUT_SEC,
        )
    except OSError as exc:
        return ProbeResult(False, None, {}, 0, f"could not execute probe: {exc}", command)
    except subprocess.TimeoutExpired:
        return ProbeResult(
            False, None, {}, 0, f"probe timed out after {PROBE_TIMEOUT_SEC}s", command
        )

    text = (completed.stdout or "") + (completed.stderr or "")
    debug_lines = text.count(_LOADER_DEBUG_MARKER)
    if completed.returncode != 0:
        return ProbeResult(
            False,
            completed.returncode,
            {},
            debug_lines,
            f"probe exited {completed.returncode}",
            command,
        )
    if debug_lines == 0:
        # The loader never spoke. Every layer set derived from this run would be
        # empty for a reason that has nothing to do with layers being absent.
        return ProbeResult(
            False,
            completed.returncode,
            {},
            0,
            f"probe produced no Vulkan loader debug output; VK_LOADER_DEBUG={_PROBE_LOADER_DEBUG} "
            "was ignored or the loader's message format changed, so an empty layer list "
            "would mean nothing",
            command,
        )
    return ProbeResult(
        True,
        completed.returncode,
        parse_layer_report(text),
        debug_lines,
        None,
        command,
        parse_ignored_env_vars(text),
    )


def check_vulkan_layers() -> Tuple[bool, List[str], Dict[str, object]]:
    """Gate 1. Returns (passed, message lines, record)."""
    lines: List[str] = ["GATE 1 -- Vulkan implicit layer chain (as the loader reports it)"]
    record: Dict[str, object] = {
        "expected_layers": list(EXPECTED_LAYERS),
        "probe": None,
        "control": None,
        "effective": None,
    }

    probe = resolve_probe()
    record["probe"] = probe
    if not probe:
        lines.append(
            "  FAIL: no Vulkan loader probe found. Looked for `vulkaninfo` on PATH and in "
            "%SystemRoot%\\System32. Without it the active layer chain cannot be read, and "
            "an unread environment is the defect this preflight exists to prevent. Install "
            f"the Vulkan runtime on the runner or point {PROBE_ENV_VAR} at a probe binary."
        )
        return False, lines, record
    lines.append(f"  probe: {probe}")

    control = run_probe(probe, strip_disable=True)
    effective = run_probe(probe, strip_disable=False)
    record["control"] = _probe_record(control)
    record["effective"] = _probe_record(effective)

    if not control.ok:
        lines.append(f"  FAIL: control probe unusable -- {control.error}")
        return False, lines, record
    if not effective.ok:
        lines.append(f"  FAIL: effective probe unusable -- {effective.error}")
        return False, lines, record

    control_names = set(control.layers)
    effective_names = set(effective.layers)
    lines.append(f"  control   (disable stripped): {_fmt_layers(control_names)}")
    lines.append(f"  effective (this job's env)  : {_fmt_layers(effective_names)}")

    # Not a verdict -- the explanation a maintainer would otherwise have to go
    # find. The loader drops its own filter variables in an elevated process, so
    # on a runner service registered as LocalSystem the control and effective
    # runs come back identical no matter what those variables say (#878).
    ignored = sorted(set(effective.ignored_env_vars) & set(_CONTROL_STRIPPED_VARS))
    record["ignored_filter_env_vars"] = ignored
    if ignored:
        lines.append(
            "  NOTE: the loader reports it discarded "
            f"{', '.join(ignored)} because this process is elevated, so those variables "
            "did not shape the chain above. What did is each layer's own "
            "`disable_environment` variable, which the loader reads unprivileged."
        )

    if not control_names:
        # See the module docstring: this is the assertion that keeps a silent
        # effective run from being read as success.
        lines.append(
            "  FAIL: the control probe reported no inserted layers at all, so this check "
            "cannot discriminate. Either the loader inserts nothing even without the "
            "disable (in which case there is nothing for this gate to prove and the "
            "runner's Vulkan install should be inspected), or the loader's debug format "
            "changed and the parser is matching nothing. Do not relax this assertion to "
            "make the job green."
        )
        return False, lines, record

    # A nonempty control set proves the parser saw *layers*; it does not prove
    # it saw both chains. The loader reports the instance chain and the device
    # chain with different line formats, and every comparison below diffs
    # against the control run's own chain memberships -- so a format change
    # that hides one chain's lines blinds the control and effective runs
    # *symmetrically*: nothing is "lost", nothing is "unexpected", and a
    # third-party layer living only on the hidden chain is invisible while the
    # gate passes. The control run must therefore demonstrate it observed each
    # chain before its silence about that chain means anything (#878 review,
    # thread on line 418).
    control_chain_types = {
        chain for entry in control.layers.values() for chain in entry.chains
    }
    record["control_chain_types"] = sorted(control_chain_types)
    missing_chain_types = sorted({"device", "instance"} - control_chain_types)
    if missing_chain_types:
        lines.append(
            "  FAIL: the control run reported layers, but not one of them on the "
            f"{' or the '.join(missing_chain_types)} chain, so this probe never observed "
            f"that chain at all. Either the loader's debug format for "
            f"{'/'.join(missing_chain_types)}-chain insertions changed and the parser is "
            "matching nothing there, or the probe never built that chain -- and a "
            "control that is blind to a chain cannot certify it. This means 'the gate "
            "could not look', not 'that chain is clean'. Do not relax this assertion "
            "to make the job green."
        )
        return False, lines, record

    outside_control = sorted(effective_names - control_names)
    if outside_control:
        lines.append(
            "  FAIL: the effective chain contains layer(s) the control run did not: "
            f"{', '.join(outside_control)}. The two runs differ only by "
            f"{'/'.join(_CONTROL_STRIPPED_VARS)}, so this is not a difference this guard "
            "can explain."
        )
        return False, lines, record

    removed = sorted(control_names - effective_names)
    lines.append(f"  removed by the disable: {_fmt_layers(set(removed))}")

    # An empty effective chain satisfies "nothing unexpected is present" while
    # meaning the opposite of what this gate is for: the driver's own layers
    # were stripped too, and every GPU job downstream is measuring a different
    # driver stack. So the gate asserts survival, not just absence.
    #
    # The bar is what the *control* run saw, not the whole of EXPECTED_LAYERS:
    # the control is this machine's ground truth for which of the driver's
    # layers exist at all, and demanding a layer the driver never registered
    # would be this guard asserting facts about hardware instead of measuring
    # it.
    expected_in_control = control_names & set(EXPECTED_LAYERS)
    if not expected_in_control:
        lines.append(
            "  FAIL: the control run inserted layers, but none of them were the GPU "
            f"driver's own ({', '.join(EXPECTED_LAYERS)}). Either the driver's implicit "
            "layers are no longer registered on this runner, or EXPECTED_LAYERS names "
            "layers this machine does not have -- and until that is resolved, "
            "'the driver's layers survived' cannot be checked at all."
        )
        return False, lines, record

    # Survival is asserted **per chain**, not per layer name.
    #
    # The loader builds two chains and reports them separately, and a layer can
    # be in one and not the other. Collapsing both to a name throws away the only
    # fact that distinguishes "the driver's layer survived" from "the driver's
    # layer survived on the instance chain and vanished from the device chain" --
    # and the second is a changed device stack, which is the thing this gate is
    # for. A name-set difference is empty in that case, so the gate reported PASS
    # on output it could not tell apart from a healthy run, byte for byte.
    #
    # The comparison is against the *control* run's chain membership for the same
    # reason the layer set is: the control is this machine's ground truth. A
    # driver layer the loader only ever puts on the instance chain is not
    # required to appear on the device chain.
    lost_chains = [
        (name, chain)
        for name in sorted(expected_in_control)
        for chain in sorted(
            set(control.layers[name].chains)
            - set(effective.layers[name].chains if name in effective.layers else ())
        )
    ]
    if lost_chains:
        gone_entirely = sorted(
            {name for name, _chain in lost_chains if name not in effective.layers}
        )
        lines.append(
            f"  FAIL: {len(lost_chains)} chain membership(s) of the GPU driver's own layers "
            "present in the control run are missing from the effective chain:"
        )
        for name, chain in lost_chains:
            still_on = _fmt_layers(
                effective.layers[name].chains if name in effective.layers else ()
            )
            lines.append(
                f"    - {name}: gone from the {chain} chain "
                f"(control had {'+'.join(control.layers[name].chains)}; "
                f"effective has {still_on})"
            )
        if gone_entirely:
            lines.append(
                f"  {len(gone_entirely)} of them left the chain altogether: "
                f"{', '.join(gone_entirely)}."
            )
        lines.append(
            "  The disable is over-reaching: GPU jobs would run on a driver stack that no "
            "measurement outside CI uses. A layer lost from the device chain alone is the "
            "same defect and just as invisible in the layer *names* -- narrow the disable "
            "rather than accepting the loss, and do not compare names instead of chains to "
            "make this green."
        )
        return False, lines, record

    lines.append(
        "  driver layers retained: "
        + ", ".join(
            f"{name} [{'+'.join(effective.layers[name].chains)}]"
            for name in sorted(expected_in_control)
        )
    )

    unexpected = sorted(effective_names - set(EXPECTED_LAYERS))
    if unexpected:
        lines.append(
            f"  FAIL: {len(unexpected)} layer(s) are still in the runtime chain that are not "
            "the GPU driver's own:"
        )
        for name in unexpected:
            entry = effective.layers[name]
            modules = ", ".join(entry.modules) or "<module path not reported>"
            lines.append(f"    - {name}  [{'+'.join(entry.chains)} chain]  {modules}")
        lines.append(
            "  Each of these injects into every GPU process on this runner. Disable it for "
            "CI (per-layer `disable_environment` variable from the layer's own JSON, or a "
            "wider `VK_LOADER_LAYERS_DISABLE` value) -- do not add it to EXPECTED_LAYERS "
            "unless it is genuinely part of the GPU driver."
        )
        return False, lines, record

    lines.append("  PASS: only the GPU driver's own implicit layers are in the chain.")
    return True, lines, record


def _probe_record(result: ProbeResult) -> Dict[str, object]:
    return {
        "command": list(result.command),
        "ok": result.ok,
        "returncode": result.returncode,
        "loader_debug_lines": result.loader_debug_lines,
        "error": result.error,
        "ignored_env_vars": list(result.ignored_env_vars),
        "layers": {
            name: {"modules": list(entry.modules), "chains": list(entry.chains)}
            for name, entry in sorted(result.layers.items())
        },
    }


def _fmt_layers(names: Sequence[str]) -> str:
    ordered = sorted(names)
    return ", ".join(ordered) if ordered else "(none)"


# --------------------------------------------------------------------------
# Gate 2 -- page heap / Application Verifier
# --------------------------------------------------------------------------

_IFEO_PATHS = (
    r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options",
    r"SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Image File Execution Options",
)
_SESSION_MANAGER_PATH = r"SYSTEM\CurrentControlSet\Control\Session Manager"


def _is_ci_image(image_name: str) -> bool:
    lowered = image_name.lower()
    return any(marker in lowered for marker in CI_IMAGE_NAME_MARKERS)


def _flag_is_set(value: object) -> bool:
    """A GlobalFlag/PageHeapFlags value that actually enables something.

    IFEO writes `GlobalFlag` as REG_DWORD or as a REG_SZ hex/decimal string
    depending on which tool set it, so both spellings have to be understood --
    reading only the DWORD form would report "not set" for a gflags-written entry.
    """
    if isinstance(value, int):
        return value != 0
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return False
        try:
            return int(text, 16 if text.lower().startswith("0x") else 10) != 0
        except ValueError:
            return True  # unparseable but present: treat as set rather than ignore
    return bool(value)


#: `GlobalFlag` is not a boolean. It is an NT bitfield of ~30 unrelated debug
#: flags, and only two of them are what this gate is about. Treating any nonzero
#: value as page heap means a runner carrying, say, `FLG_SHOW_LDR_SNAPS` (0x2)
#: or `FLG_STOP_ON_EXCEPTION` (0x1) fails every GPU job under a "full page heap"
#: diagnosis that is simply untrue -- and a misleading gate is worse than none,
#: because the next person disables it rather than reading it.
#:
#: `PageHeapFlags` and `VerifierDlls` keep the generic "present and nonzero"
#: test: those values exist only to configure page heap / Application Verifier,
#: so any set value there *is* this gate's business.
FLG_APPLICATION_VERIFIER = 0x00000100  # gflags `vrf`
FLG_HEAP_PAGE_ALLOCS = 0x02000000  # gflags `hpa` -- full page heap
GLOBAL_FLAG_GATED_BITS = FLG_APPLICATION_VERIFIER | FLG_HEAP_PAGE_ALLOCS

_GLOBAL_FLAG_BIT_NAMES = {
    FLG_APPLICATION_VERIFIER: "FLG_APPLICATION_VERIFIER",
    FLG_HEAP_PAGE_ALLOCS: "FLG_HEAP_PAGE_ALLOCS",
}


def _parse_flag_value(value: object) -> Tuple[int, bool]:
    """`(number, parsed)` for a registry flag written as REG_DWORD or REG_SZ."""
    if isinstance(value, bool):
        return int(value), True
    if isinstance(value, int):
        return value, True
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return 0, True
        try:
            return int(text, 16 if text.lower().startswith("0x") else 10), True
        except ValueError:
            return 0, False
    return 0, False


class GlobalFlagVerdict(NamedTuple):
    """What a `GlobalFlag` value actually turns on, split by what we gate on."""

    #: True when the page-heap/verifier bits are set, or when the value could
    #: not be parsed at all.
    gating: bool
    #: The gated bits that are set (0 when unparseable).
    gated_bits: int
    #: Bits that are set but are none of this gate's business. Reported, never
    #: gated -- so "we saw a nonzero GlobalFlag" is still visible in the log.
    other_bits: int
    #: False when the value could not be read as a number.
    parsed: bool


def classify_global_flag(value: object) -> GlobalFlagVerdict:
    """Split a `GlobalFlag` value into the bits this gate owns and the rest.

    Fails closed on an unparseable value: we cannot show the page-heap bits are
    clear, and "could not read it" must not be reported as "it is off".
    """
    number, parsed = _parse_flag_value(value)
    if not parsed:
        return GlobalFlagVerdict(True, 0, 0, False)
    gated = number & GLOBAL_FLAG_GATED_BITS
    return GlobalFlagVerdict(bool(gated), gated, number & ~GLOBAL_FLAG_GATED_BITS, True)


def describe_global_flag_bits(bits: int) -> str:
    named = [name for bit, name in sorted(_GLOBAL_FLAG_BIT_NAMES.items()) if bits & bit]
    return ", ".join(named) if named else f"0x{bits:08x}"


def _value_gates(value_name: str, data: object) -> bool:
    """Does this IFEO value turn on page heap / Application Verifier?"""
    if value_name == "GlobalFlag":
        return classify_global_flag(data).gating
    return _flag_is_set(data)


#: `ERROR_FILE_NOT_FOUND` -- the only registry error that means "this is not
#: configured". Everything else (access denied, the key marked for deletion, a
#: corrupt hive) means "we could not look", which is not the same answer and
#: must not be reported as one.
_ERROR_FILE_NOT_FOUND = 2
#: `ERROR_NO_MORE_ITEMS` -- the normal end of a subkey enumeration.
_ERROR_NO_MORE_ITEMS = 259


def _registry_error_is_absence(exc: OSError) -> bool:
    return getattr(exc, "winerror", None) == _ERROR_FILE_NOT_FOUND


class IfeoScan(NamedTuple):
    """What the IFEO sweep managed to learn, and what it could not."""

    #: Images found carrying a page-heap/verifier value.
    entries: List[Dict[str, object]]
    #: Images whose subkey could not be read at all. Recorded so "no flags
    #: found" is never silently standing in for "not looked at".
    unreadable: List[Dict[str, object]]
    #: Set when the sweep could not be completed and its result means nothing.
    error: Optional[str]


def collect_ifeo_flags() -> IfeoScan:
    """Every IFEO image carrying a page-heap/verifier value, from both registry views.

    Fails closed, scoped to what Gate 2 actually judges. A read that does not
    succeed is never folded into the empty result -- an access-denied view that
    still holds a page-heap entry would otherwise be indistinguishable from a
    clean machine. Only `ERROR_FILE_NOT_FOUND` is read as "not configured".

    The three failures that make the whole sweep meaningless -- a view that
    cannot be opened, an enumeration that cannot be walked -- return an
    ``error`` and fail the gate: without the list of image names, whether a
    binary CI executes carries flags is simply unknown.

    A single *image subkey* that cannot be read is narrower than that, because
    enumeration already told us its name. If it is one of the images CI runs it
    is returned as an error for the same reason as above. If it is not, it is
    recorded in ``unreadable`` and reported but does not gate: this box is a
    workstation, several images under IFEO are ACL'd against non-administrators
    (`DefenderAgentScan.exe` among them), and page heap on software we never
    execute is not a variable in our measurements. Gating on those would mean
    the preflight could only ever run as an administrator, which is a worse
    failure mode than the one it would be guarding.
    """
    try:
        import winreg  # noqa: PLC0415 -- Windows-only, imported where it is used
    except ImportError as exc:  # pragma: no cover - non-Windows
        return IfeoScan([], [], f"winreg unavailable: {exc}")

    entries: List[Dict[str, object]] = []
    unreadable: List[Dict[str, object]] = []
    for path in _IFEO_PATHS:
        try:
            root = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path)
        except OSError as exc:
            if _registry_error_is_absence(exc):
                continue  # this registry view does not exist on this machine
            return IfeoScan([], [], f"HKLM\\{path} could not be opened: {exc}")
        with root:
            index = 0
            while True:
                try:
                    image = winreg.EnumKey(root, index)
                except OSError as exc:
                    if getattr(exc, "winerror", None) == _ERROR_NO_MORE_ITEMS:
                        break
                    return IfeoScan(
                        [], [], f"HKLM\\{path} could not be enumerated at index {index}: {exc}"
                    )
                index += 1
                flags, noted, read_error = _read_ifeo_image_flags(winreg, root, image)
                if read_error is not None:
                    if _is_ci_image(image):
                        return IfeoScan(
                            [],
                            [],
                            f"HKLM\\{path}\\{image} is an image CI executes and could not be "
                            f"read: {read_error}. Page-heap flags on it cannot be ruled out.",
                        )
                    unreadable.append({"image": image, "view": path, "error": read_error})
                    continue
                if flags or noted:
                    entries.append(
                        {"image": image, "view": path, "flags": flags, "noted": noted}
                    )
    return IfeoScan(entries, unreadable, None)


def _read_ifeo_image_flags(
    winreg, root, image: str
) -> Tuple[Dict[str, object], Dict[str, object], Optional[str]]:
    """Page-heap/verifier values on one IFEO image, or why they could not be read.

    Returns `(gating, noted, error)`. `noted` carries values that are present and
    nonzero but set none of the bits this gate owns -- recorded so the log never
    implies we did not look, gated on so nothing fails under the wrong diagnosis.
    """
    flags: Dict[str, object] = {}
    noted: Dict[str, object] = {}
    try:
        image_key = winreg.OpenKey(root, image)
    except OSError as exc:
        if _registry_error_is_absence(exc):
            return flags, noted, None  # deleted between enumeration and open
        return flags, noted, str(exc)
    with image_key:
        for value_name in PAGE_HEAP_VALUE_NAMES:
            try:
                data, _kind = winreg.QueryValueEx(image_key, value_name)
            except OSError as exc:
                if _registry_error_is_absence(exc):
                    continue  # this image does not carry that value
                return {}, {}, f"{value_name}: {exc}"
            if _value_gates(value_name, data):
                flags[value_name] = data
            elif _flag_is_set(data):
                noted[value_name] = data
    return flags, noted, None


def collect_systemwide_global_flag() -> Tuple[Optional[object], Optional[str]]:
    try:
        import winreg  # noqa: PLC0415
    except ImportError as exc:  # pragma: no cover - non-Windows
        return None, f"winreg unavailable: {exc}"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, _SESSION_MANAGER_PATH) as key:
            data, _kind = winreg.QueryValueEx(key, "GlobalFlag")
    except OSError as exc:
        # Same fail-closed rule as collect_ifeo_flags(): "no such value" is an
        # answer, "could not read" is not.
        if _registry_error_is_absence(exc):
            return None, None
        return None, f"HKLM\\{_SESSION_MANAGER_PATH}\\GlobalFlag could not be read: {exc}"
    return data, None


def check_page_heap() -> Tuple[bool, List[str], Dict[str, object]]:
    """Gate 2. Returns (passed, message lines, record)."""
    lines = ["GATE 2 -- page heap / Application Verifier (IFEO)"]
    record: Dict[str, object] = {"platform": sys.platform}

    if os.name != "nt":
        lines.append(
            "  N/A: Image File Execution Options is a Windows registry mechanism and this "
            "host is not Windows. The GPU jobs this preflight guards are Windows-only."
        )
        record["status"] = "not-applicable"
        return True, lines, record

    scan = collect_ifeo_flags()
    if scan.error is not None:
        lines.append(f"  FAIL: could not read Image File Execution Options -- {scan.error}")
        record["status"] = "unreadable"
        record["error"] = scan.error
        return False, lines, record

    entries = scan.entries
    record["entries_with_flags"] = entries
    record["unreadable_images"] = scan.unreadable
    for skipped in scan.unreadable:
        lines.append(
            f"    unread (not an image CI executes): {skipped['image']} -- {skipped['error']}"
        )
    # Only an entry with gating `flags` can fail the job. An entry that carries
    # nothing but `noted` bits (a nonzero GlobalFlag that is not page heap and
    # not Application Verifier) is printed and recorded, never gated.
    ours = [
        entry
        for entry in entries
        if _is_ci_image(str(entry["image"])) and entry["flags"]
    ]
    others = [entry for entry in entries if entry not in ours]
    record["ci_images_flagged"] = ours

    systemwide, sys_error = collect_systemwide_global_flag()
    record["systemwide_global_flag"] = systemwide
    if sys_error is not None:
        # Unreadable is not unset. Reporting green here would claim the runner
        # has no system-wide page heap on the strength of a read that failed.
        lines.append(f"  FAIL: could not read the system-wide GlobalFlag -- {sys_error}")
        record["status"] = "unreadable"
        record["error"] = sys_error
        return False, lines, record
    systemwide_verdict = (
        classify_global_flag(systemwide) if systemwide is not None else None
    )
    systemwide_set = systemwide_verdict is not None and systemwide_verdict.gating
    if systemwide_verdict is not None:
        record["systemwide_global_flag_gated_bits"] = systemwide_verdict.gated_bits
        record["systemwide_global_flag_other_bits"] = systemwide_verdict.other_bits
        if not systemwide_verdict.gating and systemwide_verdict.other_bits:
            # Nonzero, but none of it ours. Say so rather than pass in silence:
            # the reader must be able to tell "we looked and it was clean of page
            # heap" from "there was nothing there".
            lines.append(
                f"  NOTE: system-wide GlobalFlag is {systemwide!r}, which sets "
                f"0x{systemwide_verdict.other_bits:08x} but none of "
                f"{describe_global_flag_bits(GLOBAL_FLAG_GATED_BITS)}. Not gated."
            )

    lines.append(f"  IFEO images carrying page-heap/verifier values: {len(entries)}")
    lines.append(
        f"  of which CI images (matching {'/'.join(CI_IMAGE_NAME_MARKERS)}): {len(ours)}"
    )
    for entry in others:
        # Recorded, not gated, for two different reasons: page heap on unrelated
        # software is not a variable in our measurements, and a CI image whose
        # GlobalFlag sets only bits this gate does not own is not page heap.
        shown = sorted(entry["flags"]) or sorted(entry.get("noted", {}))
        why = "" if entry["flags"] else " (no page-heap/verifier bits)"
        lines.append(f"    record-only: {entry['image']} -> {shown}{why}")

    if ours or systemwide_set:
        for entry in ours:
            lines.append(
                f"  FAIL: {entry['image']} has {entry['flags']} under {entry['view']}. "
                "Full page heap changes every allocation's cost and layout, so any timing "
                "or heap-corruption result from this runner is a measurement of the "
                "debugger."
            )
        if systemwide_set:
            lines.append(
                f"  FAIL: system-wide GlobalFlag is {systemwide!r} under "
                f"HKLM\\{_SESSION_MANAGER_PATH}, setting "
                f"{describe_global_flag_bits(systemwide_verdict.gated_bits)}"
                f"{'' if systemwide_verdict.parsed else ' (value unparseable -- failing closed)'}"
                "; it applies to every process on the runner."
            )
        record["status"] = "failed"
        return False, lines, record

    lines.append("  PASS: no page-heap/verifier flags on any image CI executes, none system-wide.")
    record["status"] = "passed"
    return True, lines, record


# --------------------------------------------------------------------------
# Record -- GPU contention (never gates)
# --------------------------------------------------------------------------


#: One row, from the driver: how busy the GPU is and how much of it is already
#: spoken for. Deliberately *not* `--query-compute-apps`: under WDDM that lists
#: every desktop application touching the GPU (60+ rows of browser, shell and
#: tray processes on this workstation) and reports `[N/A]` for their memory, so
#: it is volume without attribution -- a record nobody can read is not a record.
_GPU_OCCUPANCY_QUERY = "utilization.gpu,memory.used,memory.total,name"


def record_gpu_occupancy() -> Tuple[List[str], Dict[str, object]]:
    """How loaded the GPU is at job start. Report-only, never fails.

    This is a **record, not a gate**. #875's third observation is that the sole
    GPU runner is a shared workstation whose GPU is contended by work CI knows
    nothing about; writing the occupancy into the log turns that into something a
    surprising result can afterwards be read against. It deliberately does not
    fail the job -- how much foreign GPU load is tolerable is a policy question
    about what the machine is for, and a threshold guessed here would be a gate
    that fires on the wrong thing.
    """
    lines = ["RECORD -- GPU occupancy at job start (report-only, never fails)"]
    record: Dict[str, object] = {}
    smi = shutil.which("nvidia-smi")
    if not smi:
        lines.append("  nvidia-smi not on PATH; GPU occupancy not recorded.")
        record["status"] = "unavailable"
        return lines, record
    try:
        completed = subprocess.run(
            [smi, f"--query-gpu={_GPU_OCCUPANCY_QUERY}", "--format=csv,noheader"],
            capture_output=True,
            text=True,
            errors="replace",
            timeout=60,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        lines.append(f"  nvidia-smi failed: {exc}")
        record["status"] = "error"
        record["error"] = str(exc)
        return lines, record
    rows = [row.strip() for row in (completed.stdout or "").splitlines() if row.strip()]
    record["returncode"] = completed.returncode
    record["query"] = _GPU_OCCUPANCY_QUERY
    record["rows"] = rows
    if completed.returncode != 0:
        # `nvidia-smi` on PATH is not the same as `nvidia-smi answered`. It exits
        # nonzero when the driver is unavailable or the query is rejected, and
        # recording that as "ok" would encode a *missing* occupancy measurement
        # as a successful one -- exactly the reading this record exists to let a
        # surprising result be checked against.
        stderr = (completed.stderr or "").strip()
        record["status"] = "error"
        record["error"] = (
            f"nvidia-smi exited {completed.returncode}" + (f": {stderr}" if stderr else "")
        )
        lines.append(f"  nvidia-smi failed: {record['error']}")
        lines.append("  GPU occupancy not recorded.")
        return lines, record
    if not rows:
        # A clean exit that names no GPU is still not an occupancy measurement.
        # `status: "ok"` with `rows: []` says "we asked and the machine is idle";
        # what actually happened is "we asked and learned nothing". Same defect
        # as the nonzero-exit path above, reached by a different route.
        record["status"] = "unavailable"
        record["error"] = (
            f"nvidia-smi exited {completed.returncode} but reported no GPU rows"
        )
        lines.append(f"  {record['error']}.")
        lines.append("  GPU occupancy not recorded.")
        return lines, record
    record["status"] = "ok"
    for row in rows:
        lines.append(f"    {_GPU_OCCUPANCY_QUERY} = {row}")
    return lines, record


# --------------------------------------------------------------------------


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--json",
        type=Path,
        default=None,
        help="write the full environment record to this path (also printed to stdout)",
    )
    args = parser.parse_args(argv)

    layers_ok, layer_lines, layer_record = check_vulkan_layers()
    heap_ok, heap_lines, heap_record = check_page_heap()
    gpu_lines, gpu_record = record_gpu_occupancy()

    for block in (layer_lines, heap_lines, gpu_lines):
        for line in block:
            print(line)
        print()

    passed = layers_ok and heap_ok
    record = {
        "passed": passed,
        "vulkan_layers": layer_record,
        "page_heap": heap_record,
        "gpu_occupancy": gpu_record,
    }
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(record, indent=2, sort_keys=True), encoding="utf-8")
        print(f"environment record written to {args.json}")

    if passed:
        print("Runner GPU environment preflight PASSED.")
        return 0
    print("Runner GPU environment preflight FAILED (see the FAIL lines above).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
