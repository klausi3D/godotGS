#!/usr/bin/env python3
"""Steps of the minimal beyond-VRAM dispatch workflow (BV-06a).

ADR: docs/architecture/adr-beyond-vram-worlds.md, section 9.1 ("BV-06a is a
minimal dispatch-only version (build, `df`, `dd` with `O_DIRECT`, vulkaninfo,
generate, probe, upload) so that M1 does not wait for the full workflow"), the
runner disk budget in section 5.3, the throughput measurement in section 5.4 and
the host-cache comparison in D-14 item 4. The workflow is
`.github/workflows/beyond_vram_flythrough.yml`; every step of its GPU job that
measures, generates or probes is one subcommand here, so the fail-closed logic is
unit-tested (`tests/runtime/flythrough/test_dispatch_lane.py`) instead of living
in untestable inline shell.

Report-only. Nothing here gates a merge. But no step turns an absent signal
into a pass:

* ``disk-preflight`` refuses with ``DID NOT RUN: disk`` (exit 3) when the free
  space ``df`` reports for the world filesystem is below the worlds this run
  will generate, plus the disk-probe file, plus the ADR's enforced 25 GiB free
  margin. An unreadable ``df`` fails (exit 1); it is never read as "enough".
* ``generate`` and ``probe`` call the BV-05 generator and the BV-07 probe. Both
  are not merged yet. While their entry point is absent the step exits 3 with
  ``NOT_RUN: ... not available yet`` -- red in the run, never green. When the
  entry point exists, a zero exit that left no manifest / summary behind is a
  failure, not a pass.
* ``vulkan-limits`` fails when ``vulkaninfo`` does not report
  ``maxStorageBufferRange`` and ``maxBufferSize`` (ADR K8: the limits must be
  logged before the first > 2 GiB single buffer runs).
* ``cleanup`` deletes only a world root that carries this lane's marker, and
  fails if anything is left behind.

Exit codes follow ADR section 9.1: 0 OK, 1 FAIL, 3 NOT_RUN.

Entry-point contract for the packages that are not merged yet (they may change
it, but must change this file and its tests in the same PR):

* BV-05: ``python3 tests/runtime/flythrough/generate_world.py --godot-binary B
  --tier {T0,A} --encoding plain_v3_uncompressed --out DIR`` writes the world
  and ``DIR/world.manifest.json``.
* BV-07: ``python3 tests/runtime/flythrough/run_probe.py --godot-binary B
  --world TIER=DIR [--world ...] --out DIR`` writes ``DIR/summary.json``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[3]

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_INVALID = 2
EXIT_NOT_RUN = 3
EXIT_VOID = 75

GIB = 1024**3
GB = 1000**3

#: ADR section 5.3, runner table: "Enforced free margin (25 GiB)".
FREE_MARGIN_BYTES = 25 * GIB

#: Bytes each world occupies on disk, uncompressed plain v3 (the only encoding
#: BV-06a generates; ADR D-13). The figures are the ADR section 5.3 runner
#: table's "During M1: A uncompressed + T0 | 58.8 + 1.5" (decimal GB; 58.8 GB is
#: the [D] 54.8 GiB of "A plain v3 uncompressed (M1 only)").
WORLD_BUDGET_BYTES: Dict[str, int] = {
    "T0": int(1.5 * GB),
    "A": int(58.8 * GB),
}

#: What one dispatch generates per `tier` input. Tier A is the M1 probe; the
#: BV-07 probe also runs its F/k drawn sweep on T0, so A brings T0 with it.
TIER_WORLDS: Dict[str, Tuple[str, ...]] = {
    "T0": ("T0",),
    "A": ("T0", "A"),
}

ENCODING = "plain_v3_uncompressed"

GENERATOR_ENTRY = Path("tests/runtime/flythrough/generate_world.py")
PROBE_ENTRY = Path("tests/runtime/flythrough/run_probe.py")
GENERATOR_PACKAGE = "BV-05"
PROBE_PACKAGE = "BV-07"

MANIFEST_NAME = "world.manifest.json"
PROBE_SUMMARY_NAME = "summary.json"

#: Written by disk-preflight into the world root it creates; cleanup deletes
#: nothing that does not carry it.
WORLD_ROOT_MARKER = ".bvw-dispatch-world-root"
WORLD_ROOT_MAGIC = "godotgs-beyond-vram-dispatch-world-root/1"

DISK_PROBE_FILE = ".bvw-disk-probe.bin"
DISK_PROBE_MIN_GIB = 1
DISK_PROBE_MAX_GIB = 64
DD_BLOCK = "16M"
FILL_BLOCK_BYTES = 64 * 1024 * 1024

#: ADR D-10: the producer binary is `target=editor optimize=speed_trace
#: dev_build=no tests=yes`, labelled `binary_kind: editor_tests`.
REQUIRED_SCONS_ARGS = ("target=editor", "optimize=speed_trace", "dev_build=no", "tests=yes")
PRODUCER_BINARY_NAME = "godot.linuxbsd.editor.x86_64"
BINARY_KIND = "editor_tests"

VULKAN_LIMIT_KEYS = ("maxStorageBufferRange", "maxBufferSize")
#: ADR K8: the first > 2 GiB single buffer is the 3.5 GiB atlas.
ATLAS_SINGLE_BUFFER_BYTES = int(3.5 * GIB)

#: Directories whose size replaces the ADR's [E] "OS, tools, runner" and cache
#: lines with a measurement (section 5.3). Missing ones are reported as absent.
MEASURED_CACHE_DIRS = (
    "/opt/actions-runner/godotgs-scons-cache",
    "/opt/actions-runner/godotgs-scons-cache-opt",
)

STEP_NAMES = (
    "build_info",
    "disk_preflight",
    "disk_throughput",
    "vulkan_limits",
    "generate",
    "probe",
)

Runner = Callable[..., subprocess.CompletedProcess]
Which = Callable[[str], Optional[str]]


class LaneError(Exception):
    """A step cannot produce its signal. Carries the exit code to use."""

    def __init__(self, message: str, code: int = EXIT_FAIL) -> None:
        super().__init__(message)
        self.code = code


# --------------------------------------------------------------------------
# Records
# --------------------------------------------------------------------------


def _status_for(code: int) -> str:
    return {
        EXIT_OK: "OK",
        EXIT_INVALID: "INVALID",
        EXIT_NOT_RUN: "NOT_RUN",
        EXIT_VOID: "VOID",
    }.get(code, "FAIL")


def write_record(out: Path, step: str, code: int, reason: str, data: Optional[dict] = None) -> Path:
    steps = out / "steps"
    steps.mkdir(parents=True, exist_ok=True)
    record = {"step": step, "status": _status_for(code), "exit_code": code, "reason": reason}
    if data:
        record["data"] = data
    path = steps / f"{step}.json"
    path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def _announce(code: int, reason: str) -> None:
    if code == EXIT_OK:
        print(reason)
    elif code == EXIT_NOT_RUN:
        print(f"::error title=NOT RUN::{reason}")
    else:
        print(f"::error::{reason}")


# --------------------------------------------------------------------------
# df
# --------------------------------------------------------------------------


def parse_df(text: str) -> Dict[str, object]:
    """Parse `df -P -B1 PATH` output. Raises LaneError on anything else.

    POSIX output format: one header line, then one line per filesystem with
    `Filesystem 1-blocks Used Available Capacity Mounted-on`. Exactly one data
    line is accepted: the path names one filesystem.
    """
    lines = [line for line in text.splitlines() if line.strip()]
    if len(lines) != 2 or not lines[0].split() or lines[0].split()[0] != "Filesystem":
        raise LaneError(f"df output is not one header plus one filesystem line: {text!r}")
    fields = lines[1].split()
    if len(fields) < 6:
        raise LaneError(f"df filesystem line has {len(fields)} fields, expected 6: {lines[1]!r}")
    try:
        total, used, avail = (int(fields[1]), int(fields[2]), int(fields[3]))
    except ValueError as exc:
        raise LaneError(f"df sizes are not byte counts (was -B1 honoured?): {lines[1]!r}") from exc
    if total <= 0 or avail < 0 or used < 0:
        raise LaneError(f"df reports an impossible size: {lines[1]!r}")
    return {
        "filesystem": fields[0],
        "total_bytes": total,
        "used_bytes": used,
        "avail_bytes": avail,
        "mounted_on": " ".join(fields[5:]),
    }


def run_df(path: Path, runner: Runner = subprocess.run) -> Dict[str, object]:
    proc = runner(["df", "-P", "-B1", str(path)], capture_output=True, text=True, timeout=60)
    if proc.returncode != 0:
        raise LaneError(f"df {path} exited {proc.returncode}: {proc.stderr.strip()}")
    parsed = parse_df(proc.stdout)
    parsed["raw"] = proc.stdout
    return parsed


def required_free_bytes(tier: str, probe_gib: int) -> Tuple[int, Dict[str, int]]:
    if tier not in TIER_WORLDS:
        raise LaneError(f"unknown tier {tier!r}; expected one of {sorted(TIER_WORLDS)}")
    parts = {f"world_{w}": WORLD_BUDGET_BYTES[w] for w in TIER_WORLDS[tier]}
    parts["disk_probe_file"] = probe_gib * GIB
    parts["free_margin"] = FREE_MARGIN_BYTES
    return sum(parts.values()), parts


def _validate_probe_gib(value: int) -> int:
    if not DISK_PROBE_MIN_GIB <= value <= DISK_PROBE_MAX_GIB:
        raise LaneError(
            f"disk probe size {value} GiB is outside {DISK_PROBE_MIN_GIB}..{DISK_PROBE_MAX_GIB}"
        )
    return value


def _du_bytes(path: str, runner: Runner) -> Dict[str, object]:
    if not os.path.isdir(path):
        return {"path": path, "present": False}
    try:
        proc = runner(["du", "-s", "-B1", path], capture_output=True, text=True, timeout=600)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"path": path, "present": True, "error": str(exc)}
    if proc.returncode != 0 or not proc.stdout.split():
        return {"path": path, "present": True, "error": proc.stderr.strip() or "du failed"}
    try:
        return {"path": path, "present": True, "bytes": int(proc.stdout.split()[0])}
    except ValueError:
        return {"path": path, "present": True, "error": f"unparseable du output {proc.stdout!r}"}


def disk_preflight(
    tier: str,
    world_root: Path,
    out: Path,
    probe_gib: int,
    run_id: str,
    runner: Runner = subprocess.run,
    which: Which = shutil.which,
) -> int:
    """Create the world root, then refuse unless df leaves the ADR budget free."""
    step = "disk_preflight"
    data: Dict[str, object] = {"tier": tier, "world_root": str(world_root)}
    try:
        _validate_probe_gib(probe_gib)
        required, parts = required_free_bytes(tier, probe_gib)
        data["required_free_bytes"] = required
        data["required_parts_bytes"] = parts
        if which("df") is None:
            raise LaneError("df is not on PATH; free space is unmeasured, so nothing may run")
        if not world_root.is_absolute():
            raise LaneError(f"world root {world_root} is not an absolute path")
        if world_root.exists() and any(world_root.iterdir()):
            raise LaneError(
                f"world root {world_root} already exists and is not empty; a previous "
                "run did not clean up, and its bytes are not in this budget"
            )
        world_root.mkdir(parents=True, exist_ok=True)
        (world_root / WORLD_ROOT_MARKER).write_text(
            f"{WORLD_ROOT_MAGIC}\nrun_id={run_id}\n", encoding="utf-8"
        )
        df = run_df(world_root, runner)
        data["df_world_fs"] = df
        try:
            data["df_root_fs"] = run_df(Path("/"), runner)
        except LaneError as exc:  # informative only; the decision is the world fs
            data["df_root_fs"] = {"error": str(exc)}
        data["measured_dirs"] = [_du_bytes(path, runner) for path in MEASURED_CACHE_DIRS]
        avail = int(df["avail_bytes"])
        data["avail_bytes"] = avail
        if avail < required:
            # Nothing was generated yet; leave no world root for cleanup to find.
            shutil.rmtree(world_root, ignore_errors=True)
            raise LaneError(
                f"DID NOT RUN: disk -- df reports {avail / GB:.1f} GB free on "
                f"{df['mounted_on']} ({world_root}), this run needs {required / GB:.1f} GB "
                f"(worlds {', '.join(TIER_WORLDS[tier])} + {probe_gib} GiB disk probe + "
                f"{FREE_MARGIN_BYTES // GIB} GiB enforced free margin, ADR section 5.3)",
                EXIT_NOT_RUN,
            )
        reason = (
            f"disk OK: {avail / GB:.1f} GB free on {df['mounted_on']}, "
            f"{required / GB:.1f} GB required"
        )
        code = EXIT_OK
    except LaneError as exc:
        reason, code = str(exc), exc.code
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


# --------------------------------------------------------------------------
# dd / fio
# --------------------------------------------------------------------------


def _require_world_root(world_root: Path) -> None:
    marker = world_root / WORLD_ROOT_MARKER
    if not marker.is_file():
        raise LaneError(
            f"{marker} is missing: disk-preflight did not admit this world root, so "
            "nothing may be written to it",
            EXIT_NOT_RUN,
        )


def _fill_probe_file(path: Path, size_bytes: int) -> float:
    """Write `size_bytes` of incompressible data, fdatasync, drop it from the guest cache.

    Random bytes, not /dev/zero: a thin-provisioned or zero-detecting virtual
    disk can serve zero blocks without reading them, and that would turn the
    read measurement into a fiction.
    """
    block = os.urandom(FILL_BLOCK_BYTES)
    start = time.perf_counter()
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        written = 0
        while written < size_bytes:
            chunk = block[: min(FILL_BLOCK_BYTES, size_bytes - written)]
            written += os.write(fd, chunk)
        os.fdatasync(fd)
        if hasattr(os, "posix_fadvise"):
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    finally:
        os.close(fd)
    return time.perf_counter() - start


def _timed_dd_read(path: Path, size_bytes: int, runner: Runner) -> Dict[str, object]:
    cmd = ["dd", f"if={path}", "of=/dev/null", f"bs={DD_BLOCK}", "iflag=direct"]
    start = time.perf_counter()
    proc = runner(cmd, capture_output=True, text=True, timeout=1800)
    seconds = time.perf_counter() - start
    if proc.returncode != 0:
        raise LaneError(f"{' '.join(cmd)} exited {proc.returncode}: {proc.stderr.strip()}")
    if seconds <= 0:
        raise LaneError(f"{' '.join(cmd)} took no measurable time")
    return {
        "command": " ".join(cmd),
        "bytes": size_bytes,
        "seconds": round(seconds, 4),
        "mb_per_s": round(size_bytes / seconds / 1e6, 1),
        "dd_stderr": proc.stderr.strip(),
    }


def _run_fio(path: Path, out: Path, runner: Runner) -> Dict[str, object]:
    jobs = {
        "seq_read_1m": ["--rw=read", "--bs=1M"],
        "rand_read_128k": ["--rw=randread", "--bs=128k"],
    }
    results: Dict[str, object] = {}
    for name, extra in jobs.items():
        report = out / f"fio_{name}.json"
        cmd = [
            "fio", f"--name={name}", f"--filename={path}", "--direct=1", "--ioengine=psync",
            "--readonly", "--runtime=30", "--time_based", "--output-format=json",
            f"--output={report}", *extra,
        ]
        proc = runner(cmd, capture_output=True, text=True, timeout=300)
        if proc.returncode != 0:
            raise LaneError(
                f"fio is installed but {name} exited {proc.returncode}: {proc.stderr.strip()}"
            )
        try:
            parsed = json.loads(report.read_text(encoding="utf-8"))
            read = parsed["jobs"][0]["read"]
            results[name] = {
                "bw_bytes_per_s": read["bw_bytes"],
                "iops": read["iops"],
                "report": report.name,
            }
        except (OSError, ValueError, KeyError, IndexError) as exc:
            raise LaneError(f"fio {name} wrote no readable JSON report: {exc}") from exc
    return results


def disk_throughput(
    world_root: Path,
    out: Path,
    probe_gib: int,
    runner: Runner = subprocess.run,
    which: Which = shutil.which,
) -> int:
    """ADR section 5.4 / D-14 item 4: O_DIRECT read of a never-read region, then a re-read."""
    step = "disk_throughput"
    data: Dict[str, object] = {}
    probe = world_root / DISK_PROBE_FILE
    try:
        _validate_probe_gib(probe_gib)
        _require_world_root(world_root)
        if which("dd") is None:
            raise LaneError("dd is not on PATH")
        size = probe_gib * GIB
        fill_seconds = _fill_probe_file(probe, size)
        data["fill"] = {
            "bytes": size,
            "seconds": round(fill_seconds, 4),
            "mb_per_s": round(size / fill_seconds / 1e6, 1) if fill_seconds > 0 else None,
            "method": "buffered write of a repeated 64 MiB random block + fdatasync + "
            "POSIX_FADV_DONTNEED",
        }
        first = _timed_dd_read(probe, size, runner)
        second = _timed_dd_read(probe, size, runner)
        data["dd_o_direct_first_read"] = first
        data["dd_o_direct_reread"] = second
        data["reread_over_first_read"] = round(
            float(second["mb_per_s"]) / float(first["mb_per_s"]), 3
        ) if first["mb_per_s"] else None
        data["host_cache_note"] = (
            "Both reads bypass the guest page cache (O_DIRECT). A re-read much faster "
            "than the first read means a host-side cache serves the VM disk (ADR D-14 "
            "item 4); report-only, no threshold is applied here. The first read follows "
            "this step's own write, which a write-back host cache may still hold."
        )
        if which("fio") is None:
            data["fio"] = {"present": False, "note": "fio is optional (ADR section 5.4)"}
        else:
            data["fio"] = {"present": True, "jobs": _run_fio(probe, out, runner)}
        reason = (
            f"dd O_DIRECT read {first['mb_per_s']} MB/s, re-read {second['mb_per_s']} MB/s "
            f"over {probe_gib} GiB"
        )
        code = EXIT_OK
    except LaneError as exc:
        reason, code = str(exc), exc.code
    except OSError as exc:
        reason, code = f"disk probe I/O failed: {exc}", EXIT_FAIL
    finally:
        try:
            probe.unlink()
        except FileNotFoundError:
            pass
        except OSError as exc:
            print(f"::warning::could not delete {probe}: {exc}")
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


# --------------------------------------------------------------------------
# vulkaninfo
# --------------------------------------------------------------------------

_LIMIT_LINE = re.compile(
    r"^\s*(?P<key>" + "|".join(VULKAN_LIMIT_KEYS) + r")\s*[=:]\s*(?P<value>0x[0-9A-Fa-f]+|[0-9]+)\b",
    re.MULTILINE,
)


def parse_vulkan_limits(text: str) -> Dict[str, List[int]]:
    """Every reported value of each key, in output order (one per GPU)."""
    found: Dict[str, List[int]] = {key: [] for key in VULKAN_LIMIT_KEYS}
    for match in _LIMIT_LINE.finditer(text):
        value = match.group("value")
        found[match.group("key")].append(int(value, 16) if value.lower().startswith("0x") else int(value))
    return found


def vulkan_limits(out: Path, runner: Runner = subprocess.run, which: Which = shutil.which) -> int:
    step = "vulkan_limits"
    data: Dict[str, object] = {}
    try:
        if which("vulkaninfo") is None:
            raise LaneError("vulkaninfo is not on PATH (package vulkan-tools)")
        full = runner(["vulkaninfo"], capture_output=True, text=True, timeout=120)
        (out / "vulkaninfo.txt").write_text(full.stdout + full.stderr, encoding="utf-8")
        if full.returncode != 0:
            raise LaneError(f"vulkaninfo exited {full.returncode}")
        summary = runner(["vulkaninfo", "--summary"], capture_output=True, text=True, timeout=120)
        (out / "vulkaninfo_summary.txt").write_text(summary.stdout + summary.stderr, encoding="utf-8")
        limits = parse_vulkan_limits(full.stdout)
        data["limits"] = limits
        missing = [key for key, values in limits.items() if not values]
        if missing:
            raise LaneError(
                f"vulkaninfo reported no {', '.join(missing)}; the driver limits the first "
                "> 2 GiB single buffer depends on are unknown (ADR K8)"
            )
        smallest = min(min(limits["maxStorageBufferRange"]), min(limits["maxBufferSize"]))
        data["smallest_limit_bytes"] = smallest
        data["atlas_3_5_gib_within_limits"] = ATLAS_SINGLE_BUFFER_BYTES <= smallest
        reason = (
            f"maxStorageBufferRange {limits['maxStorageBufferRange']}, "
            f"maxBufferSize {limits['maxBufferSize']}"
        )
        code = EXIT_OK
    except LaneError as exc:
        reason, code = str(exc), exc.code
    except (OSError, subprocess.TimeoutExpired) as exc:
        reason, code = f"vulkaninfo could not run: {exc}", EXIT_FAIL
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


# --------------------------------------------------------------------------
# build_info
# --------------------------------------------------------------------------


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _provenance(path: Path) -> Dict[str, str]:
    values: Dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            if key in values:
                raise LaneError(f"{path} repeats {key!r}")
            values[key] = value
    for key in ("commit", "run_id", "binary", "sha256"):
        if key not in values:
            raise LaneError(f"{path} has no {key!r}")
    return values


def build_info(
    godot_binary: Path,
    provenance: Path,
    scons_args: str,
    out: Path,
    runner: Runner = subprocess.run,
) -> int:
    """The ADR's `build_info` assert: this is the optimized editor_tests producer binary."""
    step = "build_info"
    data: Dict[str, object] = {"binary_kind": BINARY_KIND, "scons_args": scons_args}
    try:
        args = scons_args.split()
        missing = [arg for arg in REQUIRED_SCONS_ARGS if arg not in args]
        if missing:
            raise LaneError(
                f"the SCons arguments lack {missing}; BV-06a measures the optimized "
                f"producer binary (ADR D-10: {' '.join(REQUIRED_SCONS_ARGS)})"
            )
        if godot_binary.name != PRODUCER_BINARY_NAME:
            raise LaneError(
                f"resolved binary {godot_binary.name!r} is not {PRODUCER_BINARY_NAME!r}; "
                "a .dev binary is a dev_build and is not what this lane measures"
            )
        if not godot_binary.is_file():
            raise LaneError(f"{godot_binary} does not exist")
        prov = _provenance(provenance)
        data["provenance"] = prov
        if prov["binary"] != godot_binary.name:
            raise LaneError(f"provenance names {prov['binary']!r}, resolved {godot_binary.name!r}")
        actual = _sha256(godot_binary)
        data["sha256"] = actual
        if actual != prov["sha256"]:
            raise LaneError(f"{godot_binary} hashes to {actual}, provenance says {prov['sha256']}")
        proc = runner(
            [str(godot_binary), "--headless", "--version"],
            capture_output=True, text=True, timeout=120,
        )
        version = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else ""
        data["version"] = version
        if proc.returncode != 0 or not version:
            raise LaneError(f"{godot_binary} --version exited {proc.returncode} with {version!r}")
        reason = f"{BINARY_KIND} {version} sha256 {actual}"
        code = EXIT_OK
    except LaneError as exc:
        reason, code = str(exc), exc.code
    except (OSError, subprocess.TimeoutExpired) as exc:
        reason, code = f"build_info could not inspect the binary: {exc}", EXIT_FAIL
    out.mkdir(parents=True, exist_ok=True)
    (out / "build_info.json").write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


# --------------------------------------------------------------------------
# generate / probe
# --------------------------------------------------------------------------


def _entry_point(repo: Path, relative: Path, package: str, what: str) -> Path:
    entry = repo / relative
    if not entry.is_file():
        raise LaneError(
            f"NOT_RUN: {what} entry point {relative.as_posix()} is not available yet "
            f"({package} has not landed); this lane has no {what} result",
            EXIT_NOT_RUN,
        )
    return entry


def _run_logged(cmd: Sequence[str], log: Path, runner: Runner, timeout: Optional[float]) -> int:
    log.parent.mkdir(parents=True, exist_ok=True)
    print("+ " + " ".join(cmd), flush=True)
    with log.open("w", encoding="utf-8") as handle:
        proc = runner(list(cmd), stdout=handle, stderr=subprocess.STDOUT, text=True, timeout=timeout)
    tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-40:]
    for line in tail:
        print(line)
    return proc.returncode


def _tree_bytes(path: Path) -> int:
    return sum(p.stat().st_size for p in path.rglob("*") if p.is_file())


def generate(
    repo: Path,
    godot_binary: Path,
    tier: str,
    world_root: Path,
    out: Path,
    runner: Runner = subprocess.run,
) -> int:
    step = "generate"
    data: Dict[str, object] = {"tier": tier, "encoding": ENCODING, "worlds": {}}
    try:
        if tier not in TIER_WORLDS:
            raise LaneError(f"unknown tier {tier!r}")
        entry = _entry_point(repo, GENERATOR_ENTRY, GENERATOR_PACKAGE, "generator")
        _require_world_root(world_root)
        for world in TIER_WORLDS[tier]:
            world_dir = world_root / world
            cmd = [
                sys.executable, str(entry), "--godot-binary", str(godot_binary),
                "--tier", world, "--encoding", ENCODING, "--out", str(world_dir),
            ]
            rc = _run_logged(cmd, out / "logs" / f"generate_{world}.log", runner, None)
            if rc != 0:
                raise LaneError(f"generator for {world} exited {rc}")
            manifest = world_dir / MANIFEST_NAME
            try:
                parsed = json.loads(manifest.read_text(encoding="utf-8"))
            except (OSError, ValueError) as exc:
                raise LaneError(
                    f"generator for {world} exited 0 but left no readable {MANIFEST_NAME}: {exc}"
                ) from exc
            if not isinstance(parsed, dict) or not parsed:
                raise LaneError(f"{manifest} is not a non-empty JSON object")
            (out / "worlds").mkdir(parents=True, exist_ok=True)
            shutil.copyfile(manifest, out / "worlds" / f"{world}.{MANIFEST_NAME}")
            on_disk = _tree_bytes(world_dir)
            budget = WORLD_BUDGET_BYTES[world]
            data["worlds"][world] = {
                "bytes_on_disk": on_disk,
                "budget_bytes": budget,
                "exceeds_budget": on_disk > budget,
            }
            if on_disk > budget:
                print(
                    f"::warning::world {world} occupies {on_disk / GB:.2f} GB, above the ADR "
                    f"section 5.3 budget of {budget / GB:.2f} GB the disk preflight reserved"
                )
        reason = f"generated {', '.join(TIER_WORLDS[tier])} ({ENCODING})"
        code = EXIT_OK
    except LaneError as exc:
        reason, code = str(exc), exc.code
    except (OSError, subprocess.TimeoutExpired) as exc:
        reason, code = f"generator could not run: {exc}", EXIT_FAIL
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


def probe(
    repo: Path,
    godot_binary: Path,
    tier: str,
    world_root: Path,
    out: Path,
    runner: Runner = subprocess.run,
) -> int:
    step = "probe"
    data: Dict[str, object] = {"tier": tier}
    try:
        if tier not in TIER_WORLDS:
            raise LaneError(f"unknown tier {tier!r}")
        entry = _entry_point(repo, PROBE_ENTRY, PROBE_PACKAGE, "probe")
        _require_world_root(world_root)
        worlds: List[str] = []
        for world in TIER_WORLDS[tier]:
            if not (world_root / world / MANIFEST_NAME).is_file():
                raise LaneError(
                    f"NOT_RUN: world {world} was not generated (no {MANIFEST_NAME} under "
                    f"{world_root / world}); there is nothing to probe",
                    EXIT_NOT_RUN,
                )
            worlds += ["--world", f"{world}={world_root / world}"]
        probe_out = out / "probe"
        probe_out.mkdir(parents=True, exist_ok=True)
        cmd = [sys.executable, str(entry), "--godot-binary", str(godot_binary), *worlds, "--out", str(probe_out)]
        rc = _run_logged(cmd, out / "logs" / "probe.log", runner, None)
        data["probe_exit_code"] = rc
        summary = probe_out / PROBE_SUMMARY_NAME
        data["summary_written"] = summary.is_file()
        if rc != 0:
            # The probe's own verdict code (ADR section 9.1 mapping) is passed on.
            raise LaneError(f"probe exited {rc}", rc if 0 < rc < 256 else EXIT_FAIL)
        if not summary.is_file():
            raise LaneError(f"probe exited 0 but wrote no {PROBE_SUMMARY_NAME}; a silent probe is not a pass")
        reason = f"probe finished; summary at {summary.relative_to(out).as_posix()}"
        code = EXIT_OK
    except LaneError as exc:
        reason, code = str(exc), exc.code
    except (OSError, subprocess.TimeoutExpired) as exc:
        reason, code = f"probe could not run: {exc}", EXIT_FAIL
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


# --------------------------------------------------------------------------
# summarize / cleanup
# --------------------------------------------------------------------------


def summarize(out: Path, step_summary: Optional[Path] = None) -> int:
    """Collect every step record. A step without one is MISSING, never OK."""
    rows = []
    for step in STEP_NAMES:
        path = out / "steps" / f"{step}.json"
        try:
            record = json.loads(path.read_text(encoding="utf-8"))
            rows.append((step, str(record.get("status", "UNREADABLE")), str(record.get("reason", ""))))
        except (OSError, ValueError):
            rows.append((step, "MISSING", "the step wrote no record (skipped, cancelled or crashed)"))
    complete = all(status == "OK" for _, status, _ in rows)
    summary = {
        "schema": "godotgs-beyond-vram-dispatch/1",
        "report_only": True,
        "overall": "COMPLETE" if complete else "INCOMPLETE",
        "steps": [{"step": s, "status": st, "reason": r} for s, st, r in rows],
    }
    out.mkdir(parents=True, exist_ok=True)
    (out / "dispatch_summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    lines = [
        "## Beyond-VRAM dispatch (BV-06a, report-only)",
        "",
        f"Overall: **{summary['overall']}**",
        "",
        "| Step | Status | Detail |",
        "| --- | --- | --- |",
    ]
    lines += [f"| `{s}` | {st} | {r.replace('|', '/')} |" for s, st, r in rows]
    text = "\n".join(lines) + "\n"
    print(text)
    if step_summary is not None:
        with step_summary.open("a", encoding="utf-8") as handle:
            handle.write(text)
    return EXIT_OK


def cleanup(world_root: Path) -> int:
    """Delete the generated worlds. Refuses a directory this lane did not create."""
    if not world_root.exists():
        print(f"cleanup: {world_root} does not exist; nothing was generated or it is already gone")
        return EXIT_OK
    marker = world_root / WORLD_ROOT_MARKER
    try:
        magic = marker.read_text(encoding="utf-8").splitlines()[0]
    except (OSError, IndexError):
        magic = ""
    if not world_root.is_absolute() or magic != WORLD_ROOT_MAGIC:
        _announce(EXIT_FAIL, f"cleanup refuses {world_root}: it carries no {WORLD_ROOT_MARKER} marker")
        return EXIT_FAIL
    freed = _tree_bytes(world_root)
    shutil.rmtree(world_root, ignore_errors=True)
    if world_root.exists():
        _announce(EXIT_FAIL, f"cleanup could not delete {world_root}; generated worlds remain on disk")
        return EXIT_FAIL
    print(f"cleanup: deleted {world_root} ({freed / GB:.2f} GB)")
    return EXIT_OK


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def common(p: argparse.ArgumentParser, world: bool = True, out: bool = True) -> None:
        if world:
            p.add_argument("--world-root", type=Path, required=True)
        if out:
            p.add_argument("--out", type=Path, required=True)

    p = sub.add_parser("disk-preflight")
    common(p)
    p.add_argument("--tier", required=True, choices=sorted(TIER_WORLDS))
    p.add_argument("--probe-gib", type=int, required=True)
    p.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", "local"))

    p = sub.add_parser("disk-throughput")
    common(p)
    p.add_argument("--probe-gib", type=int, required=True)

    p = sub.add_parser("vulkan-limits")
    common(p, world=False)

    p = sub.add_parser("build-info")
    common(p, world=False)
    p.add_argument("--godot-binary", type=Path, required=True)
    p.add_argument("--provenance", type=Path, required=True)
    p.add_argument("--scons-args", required=True)

    for name in ("generate", "probe"):
        p = sub.add_parser(name)
        common(p)
        p.add_argument("--godot-binary", type=Path, required=True)
        p.add_argument("--tier", required=True, choices=sorted(TIER_WORLDS))
        p.add_argument("--repo", type=Path, default=ROOT)

    p = sub.add_parser("summarize")
    common(p, world=False)

    p = sub.add_parser("cleanup")
    common(p, out=False)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    if getattr(args, "out", None) is not None:
        args.out.mkdir(parents=True, exist_ok=True)
    if args.command == "disk-preflight":
        return disk_preflight(args.tier, args.world_root, args.out, args.probe_gib, args.run_id)
    if args.command == "disk-throughput":
        return disk_throughput(args.world_root, args.out, args.probe_gib)
    if args.command == "vulkan-limits":
        return vulkan_limits(args.out)
    if args.command == "build-info":
        return build_info(args.godot_binary, args.provenance, args.scons_args, args.out)
    if args.command == "generate":
        return generate(args.repo, args.godot_binary, args.tier, args.world_root, args.out)
    if args.command == "probe":
        return probe(args.repo, args.godot_binary, args.tier, args.world_root, args.out)
    if args.command == "summarize":
        env = os.environ.get("GITHUB_STEP_SUMMARY")
        return summarize(args.out, Path(env) if env else None)
    if args.command == "cleanup":
        return cleanup(args.world_root)
    raise AssertionError(args.command)


if __name__ == "__main__":
    sys.exit(main())
