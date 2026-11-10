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
* ``generate`` and ``probe`` call the BV-05 generator and the BV-07 probe.
  Should an entry point be absent, the step exits 3 with
  ``NOT_RUN: ... not available yet`` -- red in the run, never green. When the
  entry point exists, a zero exit that left no manifest / summary behind is a
  failure, not a pass.
* ``vulkan-limits`` fails when ``vulkaninfo`` does not report
  ``maxStorageBufferRange`` and ``maxBufferSize`` (ADR K8: the limits must be
  logged before the first > 2 GiB single buffer runs).
* ``cleanup`` deletes only a world root that carries this lane's marker, and
  fails if anything is left behind.

Exit codes follow ADR section 9.1: 0 OK, 1 FAIL, 2 INVALID, 3 NOT_RUN, 75 VOID.

BV-06b (the full workflow) adds the world cache of section 9.2 and the D-14
cold-cache method; the cache helpers live in ``world_cache.py``:

* ``prime-cache`` (dispatch only, job ``prime-world-cache``) sweeps orphans,
  clears the quarantine, deletes the old key of each world delete-first, refuses
  ``DID NOT RUN: disk`` below the world's budget plus the margin, generates,
  then fdatasync + ``POSIX_FADV_DONTNEED`` on every file and writes
  ``.complete`` last.
* ``cache-check`` (every fly-through) refuses ``NOT_RUN: cache absent`` /
  ``cache_stale``, verifies every digest (V2 against the world's own manifest;
  a mismatch moves the key to ``quarantine/`` and is INVALID), then drops the
  world from the page cache again.
* ``probe --cache-worlds`` measures ``mincore`` residency first: above 1 % the
  run is INVALID ``cache_warm`` and the probe does not start. It runs the probe
  under a shared cache lock, records the probe tree's block-read bytes, the path
  oracle's expected unique stored bytes of every probed world (BV-13a-2b, #1376
  item 1) and the ``host_cache`` label of the disk step: the D-14 record that
  the gate verdict's V10 entry judges.
* ``summarize`` maps the step records to one lane verdict (VOID > FAIL >
  INVALID > NOT_RUN > PASS; a missing record is NOT_RUN, never PASS), writes
  ``verdict.json`` for the liveness watchdog and exits with the section 9.1
  code. This is the lane's liveness verdict, report-only. It then runs BV-09's
  gate verdict (``flythrough_verdict.py``) against the frozen thresholds,
  writes ``gate_verdict.json`` and copies its verdict into ``verdict.json``
  under ``gate_verdict``. Until BV-14 makes gates hard that verdict is
  report-only: it never changes the lane's exit code.

Entry-point contract of the generator and the probe (a change to either must
change this file and its tests in the same PR):

* BV-05: ``python3 tests/runtime/flythrough/generate_world.py --godot-binary B
  --tier {T0,A} --encoding {plain_v3_uncompressed,plain_v3_gsq} --out DIR`` writes
  the world and ``DIR/world.manifest.json`` (BV-C7: ``plain_v3_gsq`` is plain v3 +
  bit 7, paged GSQ v1 chunks, and its manifest carries the ``codec`` block).

Encoding (BV-C7, ADR D-12/D-13): ``--encoding`` of ``generate``, ``prime-cache`` and
``cache-check`` selects Tier A's encoding; T0 always stays uncompressed, because the
BV-07 F/k sweep regenerates T0 subsets uncompressed and one sweep must not mix codecs.
The encoding is part of the world id (``A-plain_v3_gsq``), and the codec block is part
of ``gen_sha``, so the two encodings never share a cache key. A prime of compressed
Tier A deletes the uncompressed M1 world (``A-plain_v3_uncompressed``) once the
compressed key is complete (D-13: "the file is deleted when compressed Tier A is
primed").
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
sys.path.insert(0, str(Path(__file__).resolve().parent))

import flythrough_controls as fc  # noqa: E402
import flythrough_verdict as fv  # noqa: E402
import world_cache as wc  # noqa: E402

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
#: BV-C7: Tier A as plain v3 + paged GSQ v1 chunks (ADR D-12), from M1c on.
ENCODING_GSQ = "plain_v3_gsq"
ENCODINGS = (ENCODING, ENCODING_GSQ)
#: The writer codec id each encoding's manifest must name (`writer.codec.codec_id`).
CODEC_ID_FOR_ENCODING: Dict[str, int] = {ENCODING: 0, ENCODING_GSQ: 1}
#: ADR section 5.3 runner table, "Steady from M1c: A plain GSQ ... 7.6" (decimal GB).
WORLD_BUDGET_GSQ_BYTES: Dict[str, int] = {
    "A": int(7.6 * GB),
}

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

#: The fly-through job's steps (BV-06b). Generation moved to the prime job.
STEP_NAMES = (
    "build_info",
    # BV-13a-2b: ADR 8.5 doctest proofs on the release-tests binary, in every run mode.
    "doctest_proofs",
    "disk_preflight",
    "disk_throughput",
    "vulkan_limits",
    "cache_check",
    "probe",
    "controls",
    "contention",
)

#: Run modes of the fly-through job (#1390). `controls_only` (workflow input) runs the
#: ADR 8.5 controls without the main fly-through: the probe step is skipped on purpose.
#: Only the steps a mode lists are EXCLUDED when they wrote no record; any other step
#: without a record stays MISSING, and a normal run excludes nothing.
RUN_MODES: Dict[str, Tuple[str, ...]] = {
    "full": (),
    "controls_only": ("probe",),
}
EXCLUDED = "EXCLUDED"

#: Section 9.1 exit mapping and the lane verdict's precedence (first wins).
VERDICT_EXIT = {"PASS": 0, "FAIL": 1, "INVALID": 2, "NOT_RUN": 3, "VOID": 75}
VERDICT_PRECEDENCE = ("VOID", "FAIL", "INVALID", "NOT_RUN")
VERDICT_SCHEMA = "godotgs-beyond-vram-lane-verdict/1"
VERDICT_FILE = "verdict.json"
CACHE_WORLDS_FILE = "cache_worlds.json"
WORLD_FILE_NAME = "world.gsplatworld"

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


def world_encoding(world: str, encoding: str) -> str:
    """The encoding `world` is generated in: the requested one for Tier A, T0 always flat."""
    if encoding not in ENCODINGS:
        raise LaneError(f"unknown encoding {encoding!r}; expected one of {list(ENCODINGS)}")
    return encoding if world == "A" else ENCODING


def world_budget(world: str, encoding: str) -> int:
    if world_encoding(world, encoding) == ENCODING_GSQ:
        return WORLD_BUDGET_GSQ_BYTES[world]
    return WORLD_BUDGET_BYTES[world]


def _manifest_codec_id(manifest: object) -> object:
    try:
        return manifest["writer"]["codec"]["codec_id"]  # type: ignore[index]
    except (KeyError, TypeError):
        return None


def required_free_bytes(tier: str, probe_gib: int, include_worlds: bool = True) -> Tuple[int, Dict[str, int]]:
    """`include_worlds=False`: the worlds already sit in the world cache (BV-06b), so
    only the disk-probe file and the margin are new bytes for this run."""
    if tier not in TIER_WORLDS:
        raise LaneError(f"unknown tier {tier!r}; expected one of {sorted(TIER_WORLDS)}")
    parts = {f"world_{w}": WORLD_BUDGET_BYTES[w] for w in TIER_WORLDS[tier]} if include_worlds else {}
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
    include_worlds: bool = True,
) -> int:
    """Create the world root, then refuse unless df leaves the ADR budget free."""
    step = "disk_preflight"
    data: Dict[str, object] = {"tier": tier, "world_root": str(world_root), "worlds_cached": not include_worlds}
    try:
        _validate_probe_gib(probe_gib)
        required, parts = required_free_bytes(tier, probe_gib, include_worlds)
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
                f"({'worlds ' + ', '.join(TIER_WORLDS[tier]) + ' + ' if include_worlds else ''}"
                f"{probe_gib} GiB disk probe + "
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
        # D-14 item 4: the label every cold result of this run carries.
        # The probe file was written by this step, so absence is unprovable here.
        data["host_cache"] = wc.host_cache_label(data["reread_over_first_read"], first_read_follows_own_write=True)
        data["host_cache_note"] = (
            "Both reads bypass the guest page cache (O_DIRECT). A re-read much faster "
            "than the first read means a host-side cache serves the VM disk (ADR D-14 "
            "item 4). The first read follows this step's own write, which a write-back host "
            "cache may still hold, so the label is always `present`: a cold result never "
            "claims more than guest-cold. A label, not a gate."
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
    encoding: str = ENCODING,
) -> int:
    step = "generate"
    data: Dict[str, object] = {"tier": tier, "encoding": encoding, "worlds": {}}
    try:
        if tier not in TIER_WORLDS:
            raise LaneError(f"unknown tier {tier!r}")
        entry = _entry_point(repo, GENERATOR_ENTRY, GENERATOR_PACKAGE, "generator")
        _require_world_root(world_root)
        for world in TIER_WORLDS[tier]:
            world_dir = world_root / world
            cmd = [
                sys.executable, str(entry), "--godot-binary", str(godot_binary),
                "--tier", world, "--encoding", world_encoding(world, encoding), "--out", str(world_dir),
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
            budget = world_budget(world, encoding)
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
        reason = f"generated {', '.join(TIER_WORLDS[tier])} (Tier A encoding {encoding})"
        code = EXIT_OK
    except LaneError as exc:
        reason, code = str(exc), exc.code
    except (OSError, subprocess.TimeoutExpired) as exc:
        reason, code = f"generator could not run: {exc}", EXIT_FAIL
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


_children_read_bytes = wc.children_read_bytes


def _host_cache_from_disk_step(out: Path) -> str:
    try:
        record = json.loads((out / "steps" / "disk_throughput.json").read_text(encoding="utf-8"))
        return str(record["data"]["host_cache"])
    except (OSError, ValueError, KeyError, TypeError):
        return wc.host_cache_label(None)


def _cached_worlds(tier: str, cache_worlds: Path) -> Tuple[Dict[str, Path], Path]:
    try:
        mapping = json.loads(cache_worlds.read_text(encoding="utf-8"))
        worlds = {k: Path(v) for k, v in mapping["worlds"].items()}
        root = Path(mapping["cache_root"])
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        raise LaneError(
            f"NOT_RUN: no usable {cache_worlds.name} ({exc}); cache-check did not admit a world",
            EXIT_NOT_RUN,
        ) from exc
    for world in TIER_WORLDS[tier]:
        if world not in worlds or not (worlds[world] / MANIFEST_NAME).is_file():
            raise LaneError(
                f"NOT_RUN: cache-check admitted no world {world}; there is nothing to probe",
                EXIT_NOT_RUN,
            )
    return worlds, root


def probe(
    repo: Path,
    godot_binary: Path,
    tier: str,
    world_root: Path,
    out: Path,
    runner: Runner = subprocess.run,
    cache_worlds: Optional[Path] = None,
    residency_probe: Optional[Callable[[Path], Tuple[int, int]]] = None,
    lock: Callable[..., object] = wc.cache_lock,
    read_bytes: Callable[[], Optional[int]] = _children_read_bytes,
) -> int:
    """Run the BV-07 probe. With `cache_worlds` (BV-06b): on the cached worlds, cold.

    D-14 item 2 is checked first: above the residency limit the run is INVALID
    `cache_warm` and the probe does not start.
    """
    step = "probe"
    data: Dict[str, object] = {"tier": tier}
    try:
        if tier not in TIER_WORLDS:
            raise LaneError(f"unknown tier {tier!r}")
        entry = _entry_point(repo, PROBE_ENTRY, PROBE_PACKAGE, "probe")
        cache_root: Optional[Path] = None
        if cache_worlds is not None:
            dirs, cache_root = _cached_worlds(tier, cache_worlds)
            dirs = {w: dirs[w] for w in TIER_WORLDS[tier]}
            files = [f for d in dirs.values() for f in wc.world_files(d)]
            host_cache = _host_cache_from_disk_step(out)
            cache: Dict[str, object] = {"host_cache": host_cache, "cold_proven": False}
            data["cache"] = cache
            res = wc.residency(files, residency_probe)
            cache["residency"] = {k: v for k, v in res.items() if k != "per_file"}
            if not res["cold"]:
                cache["invalid_reason"] = "cache_warm"
                raise LaneError(
                    f"INVALID: cache_warm -- {res['resident_fraction']:.2%} of the world pages are "
                    f"resident before the cold pass, limit {wc.RESIDENCY_MAX_FRACTION:.0%} (ADR D-14 "
                    "item 2); the probe did not start",
                    EXIT_INVALID,
                )
        else:
            _require_world_root(world_root)
            dirs = {}
            for world in TIER_WORLDS[tier]:
                if not (world_root / world / MANIFEST_NAME).is_file():
                    raise LaneError(
                        f"NOT_RUN: world {world} was not generated (no {MANIFEST_NAME} under "
                        f"{world_root / world}); there is nothing to probe",
                        EXIT_NOT_RUN,
                    )
                dirs[world] = world_root / world
        worlds: List[str] = []
        for world, directory in dirs.items():
            worlds += ["--world", f"{world}={directory}"]
        probe_out = out / "probe"
        probe_out.mkdir(parents=True, exist_ok=True)
        cmd = [sys.executable, str(entry), "--godot-binary", str(godot_binary), *worlds, "--out", str(probe_out)]
        if cache_root is not None:
            with lock(cache_root, False):  # type: ignore[operator]
                before = read_bytes()
                rc = _run_logged(cmd, out / "logs" / "probe.log", runner, None)
                after = read_bytes()
            delta = after - before if before is not None and after is not None else None
            # BV-13a-2b (#1376 item 1): the path oracle's expected unique stored bytes
            # (ADR 6.4, published per world in the summary) over every probed world. A
            # summary without them leaves V10 NOT_EVALUATED, never OK.
            summary_doc, _ = fv._read_json(probe_out / PROBE_SUMMARY_NAME)  # noqa: SLF001
            # #1403 (D-14: account cold segments only): V10 judges the reads of each world's
            # FIRST path pass, sampled by that process before its repeat pass; the sweep runs
            # later. `delta` spans the whole probe tree (repeat pass, F/k sweep incl. the
            # whole-world T0 overview) and is kept as warm evidence only, never judged.
            cold_read, read_source = wc.cold_pass_read_bytes(summary_doc, list(dirs))
            data["cache"] = cache = wc.cold_cache_record(res, cold_read, summary_doc, list(dirs), host_cache,
                                                         read_source=read_source, tree_read_bytes=delta)
        else:
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
    except (LaneError, wc.CacheError) as exc:
        reason, code = str(exc), exc.code
    except (OSError, subprocess.TimeoutExpired) as exc:
        reason, code = f"probe could not run: {exc}", EXIT_FAIL
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


def controls(
    repo: Path,
    godot_binary: Path,
    out: Path,
    cache_worlds: Path,
    runner: Runner = subprocess.run,
    only: Optional[Sequence[str]] = None,
    lock: Callable[..., object] = wc.cache_lock,
    sha: Optional[str] = None,
) -> int:
    """BV-13a: the ADR 8.5 forcing controls on the cached T0, after the cold probe.

    They run after the main probe, not before it as ADR 7.3 lists them: a control
    reads T0 and would warm its pages, and D-14 item 2 then refuses the cold pass.
    The order changes nothing in the verdict: any control whose tuple differs makes
    the lane INVALID. Exit: 0 every control fired as expected, 2 INVALID, 3 NOT_RUN.
    """
    step = "controls"
    data: Dict[str, object] = {}
    try:
        entry = _entry_point(repo, PROBE_ENTRY, PROBE_PACKAGE, "probe")
        dirs, cache_root = _cached_worlds(fc.CONTROL_WORLD, cache_worlds)
        base = fv.load_inputs(out, sha)
        with lock(cache_root, False):  # type: ignore[operator]
            rep = fc.run_controls(entry, godot_binary, dirs[fc.CONTROL_WORLD], out / "controls", base,
                                  runner=runner, only=only)
        data = {
            "verdict": rep["verdict"],
            "controls": [{k: r.get(k) for k in ("control", "expected", "observed", "status", "reason")}
                         for r in rep["controls"]],
            "absent": sorted(rep["absent"]),
            "doctest": sorted(rep["doctest"]),
            "file": f"controls/{fc.CONTROLS_FILE}",
        }
        reason = f"controls {rep['verdict']}: {rep['reason']}"
        code = {"PASS": EXIT_OK, fv.NOT_RUN: EXIT_NOT_RUN}.get(rep["verdict"], EXIT_INVALID)
        print(fc.markdown(rep))
    except (LaneError, wc.CacheError) as exc:
        reason, code = str(exc), exc.code
    except (OSError, subprocess.TimeoutExpired) as exc:
        reason, code = f"controls could not run: {exc}", EXIT_INVALID
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


def doctest_proofs(godot_binary: Path, out: Path, runner: Runner = subprocess.run, timeout_s: float = 900.0) -> int:
    """BV-13a-2b: the ADR 8.5 doctest proofs (`fc.DOCTEST`) on the release-tests binary.

    The binary is the one this job installed and `build-info` asserted (D-10
    producer, `optimize=speed_trace dev_build=no tests=yes`). Each proof runs its
    one case by exact name; it passes only when doctest ran exactly that case with
    assertions and nothing failed or skipped. Exit: 0 PASS, 1 a proof failed,
    2 a proof did not run (INVALID), 3 no binary (NOT_RUN). The proofs still
    absent (`fc.DOCTEST_ABSENT`) are listed in the record, never counted.
    """
    step = "doctest_proofs"
    data: Dict[str, object] = {"absent": dict(fc.DOCTEST_ABSENT), "binary": str(godot_binary)}
    try:
        if not godot_binary.is_file():
            raise LaneError(f"NOT_RUN: no release-tests binary at {godot_binary}; the doctest proofs did not run",
                            EXIT_NOT_RUN)
        records = []
        for proof, spec in fc.DOCTEST.items():
            cmd = [str(godot_binary), "--headless", "--test", f"--test-case={spec['case']}"]
            log = out / "logs" / f"doctest_{proof}.log"
            try:
                rc: Optional[int] = _run_logged(cmd, log, runner, timeout_s)
            except (OSError, subprocess.TimeoutExpired) as exc:
                rc = None
                log.parent.mkdir(parents=True, exist_ok=True)
                with log.open("a", encoding="utf-8") as handle:
                    handle.write(f"\nharness: {type(exc).__name__}: {exc}\n")
            output = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
            record = fc.judge_doctest(proof, spec["case"], rc, output)
            record["log"] = log.relative_to(out).as_posix()
            records.append(record)
        verdict, why = fc.combine_doctests(records)
        data.update(verdict=verdict, proofs=records)
        reason = f"doctest proofs {verdict}: {why}"
        code = {"PASS": EXIT_OK, fv.FAIL: EXIT_FAIL, fv.NOT_RUN: EXIT_NOT_RUN}.get(verdict, EXIT_INVALID)
    except LaneError as exc:
        reason, code = str(exc), exc.code
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


# --------------------------------------------------------------------------
# world cache (BV-06b, ADR section 9.2 and D-14)
# --------------------------------------------------------------------------


def _fsync_write(path: Path, text: str) -> None:
    with path.open("w", encoding="utf-8") as handle:
        handle.write(text)
        handle.flush()
        os.fsync(handle.fileno())


def prime_cache(
    repo: Path,
    godot_binary: Path,
    tier: str,
    cache_root: Path,
    out: Path,
    run_id: str,
    runner: Runner = subprocess.run,
    which: Which = shutil.which,
    fadvise: Optional[wc.Fadvise] = None,
    lock: Callable[..., object] = wc.cache_lock,
    encoding: str = ENCODING,
) -> int:
    """Section 9.1 job 1: sweep orphans, delete the old key, generate, fadvise, `.complete`.

    With Tier A compressed (BV-C7), the uncompressed M1 world is deleted after the
    compressed key is complete, never before: a failed prime keeps it.
    """
    step = "prime_cache"
    data: Dict[str, object] = {"tier": tier, "encoding": encoding, "cache_root": str(cache_root), "worlds": {}}
    try:
        if tier not in TIER_WORLDS:
            raise LaneError(f"unknown tier {tier!r}")
        world_encoding("A", encoding)  # refuses an unknown encoding before anything is touched
        if not cache_root.is_absolute():
            raise LaneError(f"cache root {cache_root} is not an absolute path")
        entry = _entry_point(repo, GENERATOR_ENTRY, GENERATOR_PACKAGE, "generator")
        if which("df") is None:
            raise LaneError("df is not on PATH; free space is unmeasured, so nothing may be generated")
        fingerprint = wc.source_fingerprint(repo)
        data["source_fingerprint"] = fingerprint
        with lock(cache_root, True):  # type: ignore[operator]
            wc.ensure_format(cache_root, create=True)
            data["swept_orphans"] = wc.sweep_orphans(cache_root)
            data["quarantine_deleted"] = wc.clear_quarantine(cache_root)
            for world in TIER_WORLDS[tier]:
                enc = world_encoding(world, encoding)
                wid = wc.world_id(world, enc)
                base = cache_root / wid
                base.mkdir(parents=True, exist_ok=True)
                # Delete-first (section 5.3): the lane reports NOT_RUN: cache absent meanwhile.
                deleted = []
                for old in sorted(p for p in base.iterdir() if p.is_dir()):
                    shutil.rmtree(old)
                    deleted.append(old.name)
                df = run_df(cache_root, runner)
                budget = world_budget(world, encoding)
                need = budget + FREE_MARGIN_BYTES
                if int(df["avail_bytes"]) < need:
                    raise LaneError(
                        f"DID NOT RUN: disk -- df reports {int(df['avail_bytes']) / GB:.1f} GB free "
                        f"on {df['mounted_on']}, world {world} needs {budget / GB:.1f} GB "
                        f"+ the {FREE_MARGIN_BYTES // GIB} GiB enforced free margin (ADR section 5.3)",
                        EXIT_NOT_RUN,
                    )
                incoming = base / f"{wc.INCOMING_PREFIX}{run_id}"
                cmd = [
                    sys.executable, str(entry), "--godot-binary", str(godot_binary),
                    "--tier", world, "--encoding", enc, "--out", str(incoming),
                ]
                t_gen = time.monotonic()
                rc = _run_logged(cmd, out / "logs" / f"prime_{world}.log", runner, None)
                generate_s = round(time.monotonic() - t_gen, 1)
                if rc != 0:
                    raise LaneError(f"generator for {world} exited {rc}; the partial key stays an orphan")
                try:
                    manifest = json.loads((incoming / MANIFEST_NAME).read_text(encoding="utf-8"))
                    gen_sha = str(manifest["gen_sha"])
                except (OSError, ValueError, KeyError, TypeError) as exc:
                    raise LaneError(
                        f"generator for {world} exited 0 without a readable gen_sha in {MANIFEST_NAME}: {exc}"
                    ) from exc
                if not re.fullmatch(r"[0-9a-f]{64}", gen_sha):
                    raise LaneError(f"{MANIFEST_NAME} of {world} has a malformed gen_sha {gen_sha!r}")
                if _manifest_codec_id(manifest) != CODEC_ID_FOR_ENCODING[enc]:
                    raise LaneError(
                        f"{MANIFEST_NAME} of {world} names writer codec {_manifest_codec_id(manifest)!r}, "
                        f"not the {CODEC_ID_FOR_ENCODING[enc]} of {enc}; the generator ignored the encoding"
                    )
                meta = {
                    "schema": wc.KEY_SCHEMA,
                    "world": world,
                    "encoding": enc,
                    "gen_sha": gen_sha,
                    "source_fingerprint": fingerprint,
                    "run_id": run_id,
                }
                _fsync_write(incoming / wc.KEY_META_FILE, json.dumps(meta, indent=2, sort_keys=True) + "\n")
                key = base / gen_sha[:12]
                os.replace(incoming, key)
                # D-14 item 1: after generation (the wrapper already verified every digest).
                dropped = wc.drop_from_page_cache(wc.world_files(key), fadvise)
                _fsync_write(key / wc.COMPLETE_FILE, f"run_id={run_id}\n")
                (out / "worlds").mkdir(parents=True, exist_ok=True)
                shutil.copyfile(key / MANIFEST_NAME, out / "worlds" / f"{world}.{MANIFEST_NAME}")
                world_bytes = (key / WORLD_FILE_NAME).stat().st_size
                splats = int(manifest.get("world", {}).get("splat_count") or 0)
                data["worlds"][world] = {  # type: ignore[index]
                    "key": str(key.relative_to(cache_root)),
                    "encoding": enc,
                    "deleted_keys": deleted,
                    "bytes_dropped_from_page_cache": dropped,
                    "world_bytes": world_bytes,
                    "splat_count": splats,
                    "bytes_per_splat": round(world_bytes / splats, 3) if splats else None,
                    "codec": manifest.get("codec"),
                    "generate_s": generate_s,
                }
                if world == "A" and enc == ENCODING_GSQ:
                    # D-13: the M1 file goes once compressed Tier A is primed.
                    m1 = cache_root / wc.world_id("A", ENCODING)
                    removed = sorted(p.name for p in m1.iterdir()) if m1.is_dir() else []
                    if m1.is_dir():
                        shutil.rmtree(m1)
                    data["deleted_m1_world"] = {"world_id": m1.name, "entries": removed}
        reason = f"primed {', '.join(TIER_WORLDS[tier])} (Tier A encoding {encoding}) into {cache_root}"
        code = EXIT_OK
    except (LaneError, wc.CacheError) as exc:
        reason, code = str(exc), exc.code
    except (OSError, subprocess.TimeoutExpired) as exc:
        reason, code = f"prime could not complete: {exc}", EXIT_FAIL
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


def _verify_world(world_file: Path) -> dict:
    import verify_world_manifest  # noqa: PLC0415 - sibling module, imported on use

    return verify_world_manifest.verify(world_file)


def cache_check(
    repo: Path,
    tier: str,
    cache_root: Path,
    out: Path,
    run_id: str,
    fadvise: Optional[wc.Fadvise] = None,
    verify: Callable[[Path], dict] = _verify_world,
    lock: Callable[..., object] = wc.cache_lock,
    encoding: str = ENCODING,
) -> int:
    """Per-run verify (V2 against the world's own manifest), quarantine, fadvise."""
    step = "cache_check"
    data: Dict[str, object] = {"tier": tier, "encoding": encoding, "cache_root": str(cache_root), "worlds": {}}
    try:
        if tier not in TIER_WORLDS:
            raise LaneError(f"unknown tier {tier!r}")
        fingerprint = wc.source_fingerprint(repo)
        data["source_fingerprint"] = fingerprint
        wc.ensure_format(cache_root, create=False)
        resolved: Dict[str, str] = {}
        with lock(cache_root, True):  # type: ignore[operator]
            for world in TIER_WORLDS[tier]:
                enc = world_encoding(world, encoding)
                wid = wc.world_id(world, enc)
                keys = wc.complete_keys(cache_root, wid)
                if not keys:
                    raise wc.CacheError(
                        f"NOT_RUN: cache absent -- no complete key for {wid}; dispatch this "
                        "workflow with prime_world_cache=true",
                        EXIT_NOT_RUN,
                        "cache_absent",
                    )
                if len(keys) > 1:
                    raise LaneError(
                        f"{wid} has {len(keys)} complete keys ({[k.name for k in keys]}); the prime "
                        "deletes the old key first, so something else wrote this cache"
                    )
                key = keys[0]
                meta = wc.read_key_meta(key)
                if meta.get("encoding") != enc:
                    raise wc.CacheError(
                        f"NOT_RUN: cache_stale -- {wid}/{key.name} records encoding "
                        f"{meta.get('encoding')!r}, not {enc}; dispatch with prime_world_cache=true",
                        EXIT_NOT_RUN,
                        "cache_stale",
                    )
                if meta.get("source_fingerprint") != fingerprint:
                    raise wc.CacheError(
                        f"NOT_RUN: cache_stale -- {wid}/{key.name} was generated from other "
                        "generator sources than this checkout; dispatch with prime_world_cache=true",
                        EXIT_NOT_RUN,
                        "cache_stale",
                    )
                result = verify(key / WORLD_FILE_NAME)
                entry: Dict[str, object] = {"key": key.name, "gen_sha": meta.get("gen_sha"), "verify_ok": bool(result.get("ok"))}
                data["worlds"][world] = entry  # type: ignore[index]
                if not result.get("ok"):
                    problems = list(result.get("problems") or [])[:20]
                    entry["problems"] = problems
                    moved = wc.quarantine(cache_root, key, run_id)
                    entry["quarantined_to"] = str(moved)
                    raise wc.CacheError(
                        f"INVALID: V2 -- {wid}/{key.name} does not match its manifest ({problems[:3]}); "
                        f"moved to {moved}, which only a prime deletes",
                        EXIT_INVALID,
                        "manifest_mismatch",
                    )
                # D-14 item 1: after every per-run verify.
                entry["bytes_dropped_from_page_cache"] = wc.drop_from_page_cache(wc.world_files(key), fadvise)
                resolved[world] = str(key)
        (out / CACHE_WORLDS_FILE).write_text(
            json.dumps({"cache_root": str(cache_root), "worlds": resolved}, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        reason = f"cache verified and dropped from the page cache: {resolved}"
        code = EXIT_OK
    except wc.CacheError as exc:
        reason, code = str(exc), exc.code
        if exc.reason_code:
            data["reason_code"] = exc.reason_code
    except LaneError as exc:
        reason, code = str(exc), exc.code
    except OSError as exc:
        reason, code = f"cache check could not complete: {exc}", EXIT_FAIL
    write_record(out, step, code, reason, data)
    _announce(code, reason)
    return code


# --------------------------------------------------------------------------
# summarize / cleanup
# --------------------------------------------------------------------------


def lane_verdict(statuses: Sequence[str]) -> str:
    """VOID > FAIL > INVALID > NOT_RUN > PASS. MISSING counts as NOT_RUN, anything unknown as FAIL."""
    mapped = []
    for status in statuses:
        if status == "OK":
            mapped.append("PASS")
        elif status == "MISSING":
            mapped.append("NOT_RUN")
        elif status in VERDICT_EXIT:
            mapped.append(status)
        else:
            mapped.append("FAIL")
    if not mapped:
        return "NOT_RUN"
    for verdict in VERDICT_PRECEDENCE:
        if verdict in mapped:
            return verdict
    return "PASS"


def gate_verdict(out: Path, tier: Optional[str], sha: Optional[str]) -> dict:
    """BV-09's gate verdict for this run; a crash or a missing tier is a verdict, never a pass."""
    if not tier:
        result: dict = {"verdict": "NOT_RUN", "reason": "summarize got no --tier", "enforced_verdict": "NOT_RUN"}
    else:
        try:
            return fv.evaluate_out_dir(out, tier, "flat", sha)
        except Exception as exc:  # noqa: BLE001 -- the lane verdict must still be written
            result = {"verdict": "INVALID", "reason": f"gate verdict crashed: {type(exc).__name__}: {exc}",
                      "enforced_verdict": "INVALID"}
    (out / fv.GATE_VERDICT_FILE).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def summarize(
    out: Path,
    step_summary: Optional[Path] = None,
    contention_outcome: Optional[str] = None,
    meta: Optional[Dict[str, str]] = None,
    mode: str = "full",
) -> int:
    """Collect every step record into one lane verdict. A step without one is MISSING, never OK.

    `contention_outcome` is the GitHub outcome of the contention postflight
    step, which exits 75 to void a run and writes no record of its own:
    success -> OK, failure -> VOID, anything else (skipped, cancelled, absent)
    -> no record -> MISSING.

    `mode` (#1390, `RUN_MODES`): a step the mode skips on purpose and that wrote no
    record is EXCLUDED, not MISSING, and does not count toward the verdict. A record
    it did write is judged as usual. At least one counted step must remain.
    """
    if mode not in RUN_MODES:
        raise ValueError(f"unknown run mode {mode!r}")
    excluded_steps = RUN_MODES[mode]
    if contention_outcome == "success":
        write_record(out, "contention", EXIT_OK, "contention postflight CLEAN")
    elif contention_outcome == "failure":
        write_record(out, "contention", EXIT_VOID, "contention postflight failed: the run is VOID (#875)")
    rows = []
    cache: object = None
    for step in STEP_NAMES:
        path = out / "steps" / f"{step}.json"
        if step in excluded_steps and not os.path.lexists(path):
            # Only a genuinely absent record is excluded. A record the step did write is
            # judged; an unreadable or truncated one stays MISSING (Codex on #1391).
            rows.append((step, EXCLUDED, f"skipped on purpose by run mode {mode}"))
            continue
        try:
            record = json.loads(path.read_text(encoding="utf-8"))
            rows.append((step, str(record.get("status", "UNREADABLE")), str(record.get("reason", ""))))
            if step == "probe" and isinstance(record.get("data"), dict):
                cache = record["data"].get("cache")
        except (OSError, ValueError):
            rows.append((step, "MISSING", "the step wrote no record (skipped, cancelled or crashed)"))
    counted = [status for _, status, _ in rows if status != EXCLUDED]
    verdict = lane_verdict(counted)
    complete = bool(counted) and all(status == "OK" for status in counted)
    summary = {
        "schema": "godotgs-beyond-vram-dispatch/2",
        "report_only": True,
        "mode": mode,
        "overall": "COMPLETE" if complete else "INCOMPLETE",
        "verdict": verdict,
        "steps": [{"step": s, "status": st, "reason": r} for s, st, r in rows],
    }
    out.mkdir(parents=True, exist_ok=True)
    (out / "dispatch_summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    verdict_record = {
        "schema": VERDICT_SCHEMA,
        "verdict": verdict,
        "exit_code": VERDICT_EXIT[verdict],
        "report_only": True,
        "mode": mode,
        "cache": cache,
        "steps": summary["steps"],
    }
    verdict_record.update(meta or {})
    gates = gate_verdict(out, (meta or {}).get("tier"), (meta or {}).get("sha"))
    verdict_record["gate_verdict"] = {
        "verdict": gates.get("verdict"),
        "reason": gates.get("reason"),
        "enforced_verdict": gates.get("enforced_verdict"),
        "report_only": True,
        "file": fv.GATE_VERDICT_FILE,
    }
    (out / VERDICT_FILE).write_text(json.dumps(verdict_record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    lines = [
        "## Beyond-VRAM fly-through (BV-06b, report-only)",
        "",
        f"Lane verdict: **{verdict}** (overall {summary['overall']}, mode {mode})",
        "",
        "| Step | Status | Detail |",
        "| --- | --- | --- |",
    ]
    lines += [f"| `{s}` | {st} | {r.replace('|', '/')} |" for s, st, r in rows]
    text = "\n".join(lines) + "\n"
    if "entries" in gates:
        text += "\n" + fv.markdown(gates)
    else:
        text += f"\nGate verdict (BV-09, report-only): **{gates.get('verdict')}** ({gates.get('reason')})\n"
    print(text)
    if step_summary is not None:
        with step_summary.open("a", encoding="utf-8") as handle:
            handle.write(text)
    return VERDICT_EXIT[verdict]


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
    p.add_argument("--worlds-cached", action="store_true",
                   help="the worlds sit in the world cache; budget only the disk probe and the margin")

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
        if name == "generate":
            p.add_argument("--encoding", choices=ENCODINGS, default=ENCODING, help="Tier A's encoding (T0 stays flat)")
        if name == "probe":
            p.add_argument("--cache-worlds", type=Path,
                           help=f"the {CACHE_WORLDS_FILE} cache-check wrote: probe the cached worlds, cold")

    p = sub.add_parser("controls")
    common(p, world=False)
    p.add_argument("--godot-binary", type=Path, required=True)
    p.add_argument("--cache-worlds", type=Path, required=True,
                   help=f"the {CACHE_WORLDS_FILE} cache-check wrote; the controls run on its T0")
    p.add_argument("--repo", type=Path, default=ROOT)
    p.add_argument("--only", action="append", choices=sorted(fc.CONTROLS), default=None,
                   help="run only this control (repeatable; default: every control)")

    p = sub.add_parser("doctest-proofs")
    common(p, world=False)
    p.add_argument("--godot-binary", type=Path, required=True,
                   help="the release-tests binary build-info asserted (empty or absent: NOT_RUN)")

    p = sub.add_parser("prime-cache")
    common(p, world=False)
    p.add_argument("--godot-binary", type=Path, required=True)
    p.add_argument("--tier", required=True, choices=sorted(TIER_WORLDS))
    p.add_argument("--cache-root", type=Path, required=True)
    p.add_argument("--repo", type=Path, default=ROOT)
    p.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", "local"))
    p.add_argument("--encoding", choices=ENCODINGS, default=ENCODING, help="Tier A's encoding (T0 stays flat)")

    p = sub.add_parser("cache-check")
    common(p, world=False)
    p.add_argument("--tier", required=True, choices=sorted(TIER_WORLDS))
    p.add_argument("--cache-root", type=Path, required=True)
    p.add_argument("--repo", type=Path, default=ROOT)
    p.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", "local"))
    p.add_argument("--encoding", choices=ENCODINGS, default=ENCODING, help="Tier A's encoding (T0 stays flat)")

    p = sub.add_parser("summarize")
    common(p, world=False)
    p.add_argument("--contention-outcome", default=None,
                   help="outcome of the contention postflight step (success, failure, ...)")
    p.add_argument("--tier", default=None)
    p.add_argument("--mode", choices=sorted(RUN_MODES), default="full",
                   help="run mode: controls_only skips the probe on purpose (EXCLUDED, not MISSING)")

    p = sub.add_parser("cleanup")
    common(p, out=False)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    if getattr(args, "out", None) is not None:
        args.out.mkdir(parents=True, exist_ok=True)
    if args.command == "disk-preflight":
        return disk_preflight(
            args.tier, args.world_root, args.out, args.probe_gib, args.run_id,
            include_worlds=not args.worlds_cached,
        )
    if args.command == "prime-cache":
        return prime_cache(args.repo, args.godot_binary, args.tier, args.cache_root, args.out, args.run_id,
                           encoding=args.encoding)
    if args.command == "cache-check":
        return cache_check(args.repo, args.tier, args.cache_root, args.out, args.run_id, encoding=args.encoding)
    if args.command == "disk-throughput":
        return disk_throughput(args.world_root, args.out, args.probe_gib)
    if args.command == "vulkan-limits":
        return vulkan_limits(args.out)
    if args.command == "build-info":
        return build_info(args.godot_binary, args.provenance, args.scons_args, args.out)
    if args.command == "generate":
        return generate(args.repo, args.godot_binary, args.tier, args.world_root, args.out, encoding=args.encoding)
    if args.command == "probe":
        return probe(args.repo, args.godot_binary, args.tier, args.world_root, args.out,
                     cache_worlds=args.cache_worlds)
    if args.command == "controls":
        return controls(args.repo, args.godot_binary, args.out, args.cache_worlds, only=args.only,
                        sha=os.environ.get("GITHUB_SHA"))
    if args.command == "doctest-proofs":
        return doctest_proofs(args.godot_binary, args.out)
    if args.command == "summarize":
        env = os.environ.get("GITHUB_STEP_SUMMARY")
        meta = {
            key: os.environ[name]
            for key, name in (
                ("run_id", "GITHUB_RUN_ID"), ("run_attempt", "GITHUB_RUN_ATTEMPT"),
                ("event", "GITHUB_EVENT_NAME"), ("sha", "GITHUB_SHA"), ("ref", "GITHUB_REF"),
            )
            if os.environ.get(name)
        }
        if args.tier:
            meta["tier"] = args.tier
        return summarize(args.out, Path(env) if env else None, args.contention_outcome, meta, mode=args.mode)
    if args.command == "cleanup":
        return cleanup(args.world_root)
    raise AssertionError(args.command)


if __name__ == "__main__":
    sys.exit(main())
