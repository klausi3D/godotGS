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
Setting ``VK_LOADER_LAYERS_DISABLE`` proves nothing on its own -- an unsupported
or misspelled value is silently ignored by the loader and the job stays green.
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
* Gate 1 also fails if the probe could not be validated (missing probe binary,
  non-zero exit, unparseable output, empty control set, or an effective set that
  is not a subset of the control set).
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

#: The loader-side disable mechanism the GPU jobs use. Measured against Vulkan
#: loader 1.4.341.0 (`C:\\Windows\\System32\\vulkan-1.dll`) on the runner: with
#: `VK_LOADER_LAYERS_DISABLE=~implicit~` alone the loader inserts *no* implicit
#: layers at all, including the driver's own; adding `VK_LOADER_LAYERS_ENABLE`
#: brings back exactly the named ones, so enable takes precedence over disable.
LOADER_LAYERS_DISABLE_VAR = "VK_LOADER_LAYERS_DISABLE"
LOADER_LAYERS_DISABLE_VALUE = "~implicit~"
LOADER_LAYERS_ENABLE_VAR = "VK_LOADER_LAYERS_ENABLE"
LOADER_LAYERS_ENABLE_VALUE = ",".join(EXPECTED_LAYERS)

#: Environment variables stripped for the control run. `VK_INSTANCE_LAYERS` is
#: the legacy force-enable variable and is included so a machine-level setting
#: cannot make the control run look like the effective one.
_CONTROL_STRIPPED_VARS = (
    LOADER_LAYERS_DISABLE_VAR,
    LOADER_LAYERS_ENABLE_VAR,
    "VK_INSTANCE_LAYERS",
)

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
    env["VK_LOADER_DEBUG"] = "layer"
    return env


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
            "probe produced no Vulkan loader debug output; VK_LOADER_DEBUG=layer was "
            "ignored or the loader's message format changed, so an empty layer list "
            "would mean nothing",
            command,
        )
    return ProbeResult(True, completed.returncode, parse_layer_report(text), debug_lines, None, command)


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


def collect_ifeo_flags() -> Tuple[Optional[List[Dict[str, object]]], Optional[str]]:
    """Every IFEO image carrying a page-heap/verifier value, from both registry views."""
    try:
        import winreg  # noqa: PLC0415 -- Windows-only, imported where it is used
    except ImportError as exc:  # pragma: no cover - non-Windows
        return None, f"winreg unavailable: {exc}"

    entries: List[Dict[str, object]] = []
    for path in _IFEO_PATHS:
        try:
            root = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path)
        except OSError:
            continue
        with root:
            index = 0
            while True:
                try:
                    image = winreg.EnumKey(root, index)
                except OSError:
                    break
                index += 1
                flags: Dict[str, object] = {}
                try:
                    with winreg.OpenKey(root, image) as image_key:
                        for value_name in PAGE_HEAP_VALUE_NAMES:
                            try:
                                data, _kind = winreg.QueryValueEx(image_key, value_name)
                            except OSError:
                                continue
                            if _flag_is_set(data):
                                flags[value_name] = data
                except OSError:
                    continue
                if flags:
                    entries.append({"image": image, "view": path, "flags": flags})
    return entries, None


def collect_systemwide_global_flag() -> Tuple[Optional[object], Optional[str]]:
    try:
        import winreg  # noqa: PLC0415
    except ImportError as exc:  # pragma: no cover - non-Windows
        return None, f"winreg unavailable: {exc}"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, _SESSION_MANAGER_PATH) as key:
            data, _kind = winreg.QueryValueEx(key, "GlobalFlag")
    except OSError:
        return None, None
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

    entries, error = collect_ifeo_flags()
    if error is not None or entries is None:
        lines.append(f"  FAIL: could not read Image File Execution Options -- {error}")
        record["status"] = "unreadable"
        record["error"] = error
        return False, lines, record

    record["entries_with_flags"] = entries
    ours = [entry for entry in entries if _is_ci_image(str(entry["image"]))]
    others = [entry for entry in entries if entry not in ours]
    record["ci_images_flagged"] = ours

    systemwide, sys_error = collect_systemwide_global_flag()
    record["systemwide_global_flag"] = systemwide
    systemwide_set = sys_error is None and systemwide is not None and _flag_is_set(systemwide)

    lines.append(f"  IFEO images carrying page-heap/verifier values: {len(entries)}")
    lines.append(
        f"  of which CI images (matching {'/'.join(CI_IMAGE_NAME_MARKERS)}): {len(ours)}"
    )
    for entry in others:
        # Recorded, not gated: this box is also a workstation, and page heap on
        # unrelated software is not a variable in our measurements.
        lines.append(f"    record-only: {entry['image']} -> {sorted(entry['flags'])}")

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
                f"HKLM\\{_SESSION_MANAGER_PATH}; it applies to every process on the runner."
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
    record["status"] = "ok"
    record["returncode"] = completed.returncode
    record["query"] = _GPU_OCCUPANCY_QUERY
    record["rows"] = rows
    if not rows:
        lines.append(f"  nvidia-smi reported no GPU rows (exit {completed.returncode}).")
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
