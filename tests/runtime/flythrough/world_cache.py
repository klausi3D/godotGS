#!/usr/bin/env python3
"""World cache on the GPU runner and the D-14 cold-cache method (BV-06b).

ADR: docs/architecture/adr-beyond-vram-worlds.md, section 9.2 (cache layout),
D-14 (cold-cache method) and V2/V10 of section 8.2. The steps that use these
helpers are ``prime-cache``, ``cache-check`` and ``probe`` of
``tests/runtime/flythrough/dispatch_lane.py``; the helpers raise
:class:`CacheError` with the exit code the step reports.

Layout (section 9.2)::

    <root>/CACHE_FORMAT                      "godotgs-world-cache/1"
    <root>/.lock                             flock: prime exclusive, check exclusive, probe shared
    <root>/<world_id>/<gen_sha12>/           one key per world id; prime deletes the old one first
        world.gsplatworld, world.manifest.json
        .key.json                            gen_sha, encoding, generator source fingerprint, run id
        .complete                            written last, after verify + fdatasync + fadvise
    <root>/<world_id>/.incoming-<run_id>/    a prime in progress (an orphan if no prime runs)
    <root>/quarantine/<world_id>-<key>-<run_id>/   moved here on a V2 mismatch; deleted only by prime

A key directory without ``.complete`` is an orphan of an interrupted prime and
is never read; the next prime sweeps it.

Staleness: the manifest's ``gen_sha`` does not cover the generator's source
(``gen_sha_inputs`` carries a generator *version* integer). So every key also
records a sha256 over the generator and writer sources in this checkout
(:data:`GENERATOR_SOURCES`); a checkout whose sources differ refuses the cached
world as ``cache_stale`` instead of measuring a world the current code would
not write.

Cold-cache method (D-14): after the prime's verify and after every per-run
verify, every world file is ``fdatasync``-ed and dropped with
``POSIX_FADV_DONTNEED``. Before the probe, ``mincore()`` (through libc via
ctypes; no root needed) must show at most :data:`RESIDENCY_MAX_FRACTION` of the
world pages resident, or the run is INVALID with reason ``cache_warm``.

Standard library only; Linux for fadvise, mincore and flock. On any other
platform those raise :class:`CacheError` -- they never pretend to have worked.
"""

from __future__ import annotations

import contextlib
import ctypes
import ctypes.util
import hashlib
import json
import mmap
import os
import shutil
import sys
from pathlib import Path
from typing import Callable, Dict, Iterator, List, Optional, Sequence, Tuple

try:  # Linux runner; absent on Windows, where the lock refuses.
    import fcntl  # type: ignore[import-not-found]
except ImportError:  # pragma: no cover - platform dependent
    fcntl = None  # type: ignore[assignment]

EXIT_FAIL = 1
EXIT_INVALID = 2
EXIT_NOT_RUN = 3

DEFAULT_CACHE_ROOT = Path("/opt/actions-runner/godotgs-world-cache")
CACHE_FORMAT_FILE = "CACHE_FORMAT"
CACHE_FORMAT = "godotgs-world-cache/1"
LOCK_FILE = ".lock"
COMPLETE_FILE = ".complete"
KEY_META_FILE = ".key.json"
QUARANTINE_DIR = "quarantine"
INCOMING_PREFIX = ".incoming-"
KEY_SCHEMA = "godotgs-world-cache-key/1"

#: D-14 item 2: at most 1 % of the world pages may be resident before a cold pass.
RESIDENCY_MAX_FRACTION = 0.01
#: D-14 item 3 / V10: read_bytes >= 0.9 x the expected unique stored bytes.
V10_MIN_READ_FRACTION = 0.9
#: D-14 item 4 labelling rule (not a gate): an O_DIRECT re-read at least this
#: much faster than the first read means a host cache serves the VM disk. A
#: missing or unreadable measurement is labelled ``present`` as well, so "cold"
#: is never claimed beyond guest-cold without evidence.
HOST_CACHE_REREAD_RATIO = 1.25

#: Inputs of the world bytes that `gen_sha` does not cover: the generator, the
#: writer it delegates serialization to (gs_world_writer::Writer) and the format
#: and codec sources the writer encodes with. A change to any of them makes every
#: cached world stale. A missing file fails closed.
GENERATOR_SOURCES: Tuple[str, ...] = (
    "modules/gaussian_splatting/io/gaussian_splat_world_format.h",
    "modules/gaussian_splatting/io/gs_world_writer.cpp",
    "modules/gaussian_splatting/io/gs_world_writer.h",
    "modules/gaussian_splatting/io/gsq/gsq_codec.cpp",
    "modules/gaussian_splatting/io/gsq/gsq_codec.h",
    # BV-C7: the GSQ writer delegates page building (grouping, order, page cuts) here.
    "modules/gaussian_splatting/io/gsq/gsq_page_builder.cpp",
    "modules/gaussian_splatting/io/gsq/gsq_page_builder.h",
    "modules/gaussian_splatting/io/gs_world_generator.cpp",
    "modules/gaussian_splatting/io/gs_world_generator.h",
    "modules/gaussian_splatting/io/gaussian_splat_world_generator.cpp",
    "modules/gaussian_splatting/io/gaussian_splat_world_generator.h",
    "tests/runtime/flythrough/generate_world.py",
    "tests/runtime/flythrough/generate_world.gd",
)

#: The auto-tree producer has a separate identity: changing these sources must
#: not invalidate the expensive plain-world generator cache. Keep explicit paths
#: so a removed source fails closed instead of vanishing from a glob's digest.
TREE_SOURCES: Tuple[str, ...] = (
    "modules/gaussian_splatting/core/gaussian_splat_hlod_auto_tree.cpp",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_auto_tree.h",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_bake.cpp",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_bake.h",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_bake_admission.cpp",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_bake_admission.h",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_bake_bounds.cpp",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_bake_bounds.h",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_job.h",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_merge.cpp",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_merge.h",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_tiled_assemble.cpp",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_tiled_bake.cpp",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_tiled_bake.h",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_tiled_internal.h",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_tiled_workers.cpp",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_tree.cpp",
    "modules/gaussian_splatting/core/gaussian_splat_hlod_tree.h",
    "modules/gaussian_splatting/io/gaussian_splat_world_paged.cpp",
    "modules/gaussian_splatting/io/gaussian_splat_world_paged.h",
    "modules/gaussian_splatting/io/gs_auto_tree_source.cpp",
    "modules/gaussian_splatting/io/gs_auto_tree_source.h",
    "modules/gaussian_splatting/io/gs_world_writer_v4.cpp",
    "modules/gaussian_splatting/io/gs_world_writer_v4.h",
    "modules/gaussian_splatting/io/gsq/gsq_writer_v4_memory.cpp",
    "tests/runtime/flythrough/build_auto_tree.py",
    "tests/runtime/flythrough/build_auto_tree.gd",
)

Fadvise = Callable[[int, int, int, int], None]


class CacheError(Exception):
    """The cache cannot give this step its signal. Carries the exit code to use."""

    def __init__(self, message: str, code: int = EXIT_FAIL, reason_code: str = "") -> None:
        super().__init__(message)
        self.code = code
        self.reason_code = reason_code


# --------------------------------------------------------------------------
# Layout
# --------------------------------------------------------------------------


def world_id(world: str, encoding: str) -> str:
    return f"{world}-{encoding}"


def source_fingerprint(repo: Path, sources: Sequence[str] = GENERATOR_SOURCES) -> str:
    """sha256 over (path, sha256(bytes)) of every generator source, sorted by path."""
    digest = hashlib.sha256()
    for relative in sorted(sources):
        path = repo / relative
        if not path.is_file():
            raise CacheError(
                f"generator source {relative} is missing from the checkout; the cache key "
                "cannot be derived, so no cached world can be trusted"
            )
        digest.update(relative.encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).hexdigest().encode("ascii") + b"\n")
    return digest.hexdigest()


def ensure_format(root: Path, create: bool) -> None:
    """Refuse a cache root of another format; create it only when `create` (prime)."""
    marker = root / CACHE_FORMAT_FILE
    if not marker.is_file():
        if not create:
            raise CacheError(
                f"NOT_RUN: cache absent -- {marker} does not exist; dispatch this workflow "
                "with prime_world_cache=true",
                EXIT_NOT_RUN,
                "cache_absent",
            )
        root.mkdir(parents=True, exist_ok=True)
        marker.write_text(CACHE_FORMAT + "\n", encoding="utf-8")
        return
    found = marker.read_text(encoding="utf-8").strip()
    if found != CACHE_FORMAT:
        raise CacheError(
            f"{marker} says {found!r}, this lane reads {CACHE_FORMAT!r}; a cache of another "
            "format is neither read nor rewritten (delete it by hand after checking)"
        )


@contextlib.contextmanager
def cache_lock(root: Path, exclusive: bool) -> Iterator[None]:
    """flock on `<root>/.lock` (section 9.1: prime and fly-through serialise on it)."""
    if fcntl is None:
        raise CacheError("flock is unavailable on this platform; the cache is not touched unlocked")
    root.mkdir(parents=True, exist_ok=True)
    handle = (root / LOCK_FILE).open("a+")
    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH)
        yield
    finally:
        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        finally:
            handle.close()


def complete_keys(root: Path, wid: str) -> List[Path]:
    base = root / wid
    if not base.is_dir():
        return []
    return sorted(
        p for p in base.iterdir()
        if p.is_dir() and not p.name.startswith(".") and (p / COMPLETE_FILE).is_file()
    )


def sweep_orphans(root: Path) -> List[str]:
    """Delete key and incoming directories without `.complete` (interrupted primes)."""
    removed: List[str] = []
    if not root.is_dir():
        return removed
    for world_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        if world_dir.name in (QUARANTINE_DIR,) or world_dir.name.startswith("."):
            continue
        for key in sorted(p for p in world_dir.iterdir() if p.is_dir()):
            if not (key / COMPLETE_FILE).is_file():
                shutil.rmtree(key)
                removed.append(str(key.relative_to(root)))
            else:
                for scratch in (".scratch", ".tmp"):
                    if (key / scratch).exists():
                        shutil.rmtree(key / scratch)
                        removed.append(str((key / scratch).relative_to(root)))
    return removed


def clear_quarantine(root: Path) -> List[str]:
    """Section 9.2: quarantined keys are deleted only by a prime."""
    base = root / QUARANTINE_DIR
    if not base.is_dir():
        return []
    removed = [p.name for p in sorted(base.iterdir())]
    shutil.rmtree(base)
    return removed


def quarantine(root: Path, key: Path, run_id: str) -> Path:
    """V2 mismatch: move the key out of reach of every later run; never delete it."""
    target = root / QUARANTINE_DIR / f"{key.parent.name}-{key.name}-{run_id}"
    target.parent.mkdir(parents=True, exist_ok=True)
    os.replace(key, target)
    return target


def read_key_meta(key: Path) -> Dict[str, object]:
    try:
        meta = json.loads((key / KEY_META_FILE).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise CacheError(f"{key / KEY_META_FILE} is unreadable: {exc}") from exc
    if not isinstance(meta, dict) or meta.get("schema") != KEY_SCHEMA:
        raise CacheError(f"{key / KEY_META_FILE} is not a {KEY_SCHEMA} record")
    return meta


def world_files(directory: Path) -> List[Path]:
    return sorted(p for p in directory.rglob("*") if p.is_file() and not p.name.startswith("."))


# --------------------------------------------------------------------------
# D-14: fdatasync + POSIX_FADV_DONTNEED, mincore
# --------------------------------------------------------------------------


def _platform_fadvise() -> Fadvise:
    fadvise = getattr(os, "posix_fadvise", None)
    if fadvise is None:
        raise CacheError("posix_fadvise is unavailable on this platform; the world cannot be dropped from the page cache")
    return fadvise


def drop_from_page_cache(paths: Sequence[Path], fadvise: Optional[Fadvise] = None) -> int:
    """D-14 item 1 on every file. Returns the bytes covered."""
    advise = fadvise or _platform_fadvise()
    dontneed = getattr(os, "POSIX_FADV_DONTNEED", 4)
    total = 0
    for path in paths:
        fd = os.open(str(path), os.O_RDONLY)
        try:
            if hasattr(os, "fdatasync"):
                os.fdatasync(fd)
            advise(fd, 0, 0, dontneed)
            total += os.fstat(fd).st_size
        finally:
            os.close(fd)
    return total


def _libc_mincore() -> Callable[[Path], Tuple[int, int]]:
    if not sys.platform.startswith("linux"):
        raise CacheError("mincore residency is only implemented for Linux; residency is unmeasured")
    libc = ctypes.CDLL(ctypes.util.find_library("c") or "libc.so.6", use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
    libc.munmap.restype = ctypes.c_int
    libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    libc.mincore.restype = ctypes.c_int
    libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_ubyte)]
    page = mmap.PAGESIZE
    map_failed = ctypes.c_void_p(-1).value

    def residency(path: Path) -> Tuple[int, int]:
        size = path.stat().st_size
        if size == 0:
            return 0, 0
        pages = (size + page - 1) // page
        fd = os.open(str(path), os.O_RDONLY)
        try:
            addr = libc.mmap(None, size, mmap.PROT_READ, mmap.MAP_SHARED, fd, 0)
            if addr is None or addr == map_failed:
                err = ctypes.get_errno()
                raise CacheError(f"mmap {path} failed: {os.strerror(err)}")
            try:
                vec = (ctypes.c_ubyte * pages)()
                if libc.mincore(addr, size, vec) != 0:
                    err = ctypes.get_errno()
                    raise CacheError(f"mincore {path} failed: {os.strerror(err)}")
                resident = sum(1 for b in vec if b & 1)
            finally:
                libc.munmap(addr, size)
        finally:
            os.close(fd)
        return resident, pages

    return residency


def residency(paths: Sequence[Path], probe: Optional[Callable[[Path], Tuple[int, int]]] = None) -> Dict[str, object]:
    """D-14 item 2: resident / total pages over every world file."""
    measure = probe or _libc_mincore()
    resident = total = 0
    per_file: Dict[str, Dict[str, int]] = {}
    for path in paths:
        r, t = measure(path)
        resident += r
        total += t
        per_file[str(path)] = {"resident_pages": r, "pages": t}
    if total == 0:
        raise CacheError("the world has no pages to measure; residency is not evidence of anything")
    fraction = resident / total
    return {
        "resident_pages": resident,
        "pages": total,
        "resident_fraction": round(fraction, 6),
        "max_fraction": RESIDENCY_MAX_FRACTION,
        "cold": fraction <= RESIDENCY_MAX_FRACTION,
        "per_file": per_file,
    }


def host_cache_label(reread_over_first_read: object, first_read_follows_own_write: bool = False) -> str:
    """D-14 item 4. `present` unless a measurement shows the re-read is not served faster.

    When the "never-read" region was written by the same step just before, a
    write-back host cache holds it and serves the first read too: both reads are
    fast, the ratio is about 1, and absence is unprovable. Measured on the RTX
    3080 runner (run 42482681830): 5.59 GB/s first read, 5.30 GB/s re-read over
    1 GiB. Such a measurement is always labelled `present`.
    """
    if first_read_follows_own_write:
        return "present"
    if isinstance(reread_over_first_read, (int, float)) and not isinstance(reread_over_first_read, bool):
        if 0 < reread_over_first_read < HOST_CACHE_REREAD_RATIO:
            return "absent"
        if reread_over_first_read >= HOST_CACHE_REREAD_RATIO:
            return "present"
    return "present"


def v10(read_bytes: Optional[int], expected_unique_stored_bytes: Optional[int]) -> Dict[str, object]:
    """D-14 item 3. Never evaluates to a pass without both numbers."""
    result: Dict[str, object] = {
        "read_bytes": read_bytes,
        "expected_unique_stored_bytes": expected_unique_stored_bytes,
        "min_fraction": V10_MIN_READ_FRACTION,
    }
    if read_bytes is None or not expected_unique_stored_bytes:
        result["status"] = "NOT_EVALUATED"
        result["why"] = (
            "read_bytes unmeasured" if read_bytes is None
            else "the manifest has no expected unique stored bytes (the path oracle, ADR section 6.4)"
        )
        return result
    fraction = read_bytes / expected_unique_stored_bytes
    result["fraction"] = round(fraction, 4)
    result["status"] = "OK" if fraction >= V10_MIN_READ_FRACTION else "INVALID"
    return result


# --------------------------------------------------------------------------
# V10 inputs (BV-13a-2b, #1376 item 1): read bytes, the path oracle, the record
# --------------------------------------------------------------------------


def children_read_bytes() -> Optional[int]:
    """Block-input bytes of every reaped descendant (Linux `ru_inblock` is in 512 B units).

    The probe's Godot process is a grandchild that run_probe.py waits for, so its
    reads accumulate here. On Linux `ru_inblock` is the task's io-accounting
    `read_bytes` (the `/proc/<pid>/io read_bytes` of D-14 item 3) summed over the
    waited-for descendants. Page-cache hits are not block input, which is what
    makes this the V10 signal. It counts every block the probe tree read (the
    binary, the worlds, anything else), so it bounds the world reads from above:
    a warm world passes V10 only if something else read about as much. None where
    `resource` does not exist.
    """
    try:
        import resource  # noqa: PLC0415 - POSIX only
    except ImportError:
        return None
    return int(resource.getrusage(resource.RUSAGE_CHILDREN).ru_inblock) * 512


def warm_page_cache(paths: Sequence[Path], block: int = 8 * 1024 * 1024) -> int:
    """Read every file once through the page cache and leave it there: the per-run verify
    without the fadvise that follows it (ADR 8.5 C-warm, "fadvise skipped"). Returns bytes read."""
    total = 0
    for path in paths:
        with Path(path).open("rb") as handle:
            while True:
                chunk = handle.read(block)
                if not chunk:
                    break
                total += len(chunk)
    return total


def expected_unique_stored_bytes(summary: object, worlds: Sequence[str]) -> Tuple[Optional[int], str]:
    """The path oracle's expected unique stored bytes (ADR 6.4), summed over every probed world.

    The read-byte signal covers the whole probe tree, so the oracle side covers
    every world the probe flew. One world without its oracle leaves the sum
    unknown (None): V10 is then not evaluated, never judged on a partial sum.
    """
    if not isinstance(summary, dict) or not isinstance(summary.get("worlds"), dict):
        return None, "no probe summary with worlds"
    if not worlds:
        return None, "no world was probed"
    total = 0
    for world in worlds:
        ws = summary["worlds"].get(world)
        fl = ws.get("flythrough") if isinstance(ws, dict) else None
        oracle = fl.get("expected_unique_upload_flat") if isinstance(fl, dict) else None
        stored = oracle.get("stored_bytes") if isinstance(oracle, dict) else None
        if isinstance(stored, bool) or not isinstance(stored, int) or stored <= 0:
            return None, f"worlds.{world}.flythrough.expected_unique_upload_flat.stored_bytes absent or not positive"
        total += stored
    return total, f"path oracle stored bytes of {', '.join(worlds)}"


def cold_pass_read_bytes(summary: object, worlds: Sequence[str]) -> Tuple[Optional[int], str]:
    """Block reads of the COLD path pass only (#1403, D-14: account cold segments only), summed per world.

    Each world's path process records its own `/proc/self/io read_bytes` at the end
    of its first pass (run_probe.cold_pass_io, `worlds.<w>.cold_pass_io`). The
    repeat pass and the F/k sweep (with its whole-world overview) come after that
    sample and never enter it. One world without the sample leaves the sum unknown
    (None): V10 is then not evaluated, never judged on a partial sum or on the
    whole probe tree's reads.
    """
    if not isinstance(summary, dict) or not isinstance(summary.get("worlds"), dict):
        return None, "no probe summary with worlds"
    if not worlds:
        return None, "no world was probed"
    total = 0
    for world in worlds:
        ws = summary["worlds"].get(world)
        io = ws.get("cold_pass_io") if isinstance(ws, dict) else None
        value = io.get("cold_read_bytes") if isinstance(io, dict) else None
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            why = io.get("why") if isinstance(io, dict) else None
            return None, f"worlds.{world}.cold_pass_io.cold_read_bytes absent ({why or 'no cold-pass sample'})"
        total += value
    return total, f"first-pass /proc/self/io read_bytes of {', '.join(worlds)}"


def cold_cache_record(
    residency_record: Dict[str, object],
    read_bytes: Optional[int],
    summary: object,
    worlds: Sequence[str],
    host_cache: str,
    read_source: str = "the probe process tree",
    tree_read_bytes: Optional[int] = None,
) -> Dict[str, object]:
    """The D-14 record of one probe process: residency before it, cold read bytes, the oracle.

    The lane's probe step and every BV-13a control write this record; the verdict
    module's V10 entry judges it against the frozen thresholds. `cold_proven` is
    this module's own reading with its constants, for the step record only.
    `read_bytes` is what V10 judges; `tree_read_bytes` (every pass and the sweep)
    is warm-pass evidence, recorded and never judged.
    """
    expected, source = expected_unique_stored_bytes(summary, worlds)
    result = v10(read_bytes, expected)
    result["expected_source"] = source
    result["read_source"] = read_source
    if read_bytes is None:
        result["why"] = f"read_bytes unmeasured: {read_source}"
    result["worlds"] = list(worlds)
    res = {k: v for k, v in residency_record.items() if k != "per_file"}
    record = {
        "host_cache": host_cache,
        "residency": res,
        "v10": result,
        "cold_proven": bool(res.get("cold")) and result["status"] == "OK",
    }
    if tree_read_bytes is not None:
        record["probe_tree_read_bytes"] = {
            "value": tree_read_bytes,
            "note": "every probe process (cold pass, repeat pass, F/k sweep): warm evidence, not judged by V10",
        }
    return record
