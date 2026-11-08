#!/usr/bin/env python3
"""BV-06b world cache helpers and the D-14 cold-cache method.

Pure logic runs everywhere. The three Linux primitives -- POSIX_FADV_DONTNEED,
mincore through libc and flock -- run for real on Linux (the guard lane in CI)
and are skipped elsewhere with the reason printed; on a non-Linux platform the
helpers must refuse, never pretend, and that is tested everywhere.

The residency test discriminates: the same file measured warm (just read)
must be above the 1 % limit, and below it after the drop. A mincore that
always answered 0 would fail the warm half; an fadvise that did nothing would
fail the cold half.

Run directly or through ``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import world_cache as wc  # noqa: E402

LINUX = sys.platform.startswith("linux")


def _on_tmpfs(path: Path) -> bool:
    """tmpfs pages cannot be dropped (they are the file); the cold half needs a disk."""
    try:
        mounts = Path("/proc/mounts").read_text(encoding="utf-8").splitlines()
    except OSError:
        return False
    best, fstype = "", ""
    resolved = str(path.resolve())
    for line in mounts:
        fields = line.split()
        if len(fields) >= 3 and resolved.startswith(fields[1]) and len(fields[1]) > len(best):
            best, fstype = fields[1], fields[2]
    return fstype in ("tmpfs", "ramfs")


class _Tmp(unittest.TestCase):
    def setUp(self) -> None:
        # Next to this file, not /tmp: the cold half needs a disk-backed filesystem.
        self._tmp = tempfile.TemporaryDirectory(dir=HERE if LINUX else None)
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()


class Layout(_Tmp):
    def test_fingerprint_covers_every_source_and_fails_closed(self) -> None:
        repo = self.tmp
        for rel in wc.GENERATOR_SOURCES:
            (repo / rel).parent.mkdir(parents=True, exist_ok=True)
            (repo / rel).write_text(rel, encoding="utf-8")
        first = wc.source_fingerprint(repo)
        for rel in wc.GENERATOR_SOURCES:
            with self.subTest(rel=rel):
                original = (repo / rel).read_text(encoding="utf-8")
                (repo / rel).write_text(original + " ", encoding="utf-8")
                self.assertNotEqual(wc.source_fingerprint(repo), first)
                (repo / rel).write_text(original, encoding="utf-8")
        self.assertEqual(wc.source_fingerprint(repo), first)
        (repo / wc.GENERATOR_SOURCES[-1]).unlink()
        with self.assertRaises(wc.CacheError):
            wc.source_fingerprint(repo)

    def test_the_real_generator_sources_exist(self) -> None:
        self.assertEqual(len(wc.source_fingerprint(HERE.parents[2])), 64)

    def test_format_is_created_only_by_prime_and_never_overwritten(self) -> None:
        root = self.tmp / "cache"
        with self.assertRaises(wc.CacheError) as ctx:
            wc.ensure_format(root, create=False)
        self.assertEqual(ctx.exception.code, wc.EXIT_NOT_RUN)
        self.assertFalse(root.exists())
        wc.ensure_format(root, create=True)
        wc.ensure_format(root, create=False)
        (root / wc.CACHE_FORMAT_FILE).write_text("other/2\n", encoding="utf-8")
        with self.assertRaises(wc.CacheError) as ctx:
            wc.ensure_format(root, create=True)
        self.assertEqual(ctx.exception.code, wc.EXIT_FAIL)

    def test_sweep_removes_only_incomplete_keys_and_scratch(self) -> None:
        root = self.tmp
        done = root / "A-x" / "aaaaaaaaaaaa"
        (done / ".tmp").mkdir(parents=True)
        (done / wc.COMPLETE_FILE).write_text("", encoding="utf-8")
        (done / "world.gsplatworld").write_bytes(b"1")
        half = root / "A-x" / ".incoming-9"
        half.mkdir()
        quarantined = root / wc.QUARANTINE_DIR / "A-x-bbbbbbbbbbbb-1"
        quarantined.mkdir(parents=True)
        removed = wc.sweep_orphans(root)
        self.assertTrue((done / "world.gsplatworld").is_file())
        self.assertFalse((done / ".tmp").exists())
        self.assertFalse(half.exists())
        self.assertTrue(quarantined.exists(), "the sweep never deletes quarantine; only a prime clears it")
        self.assertEqual(len(removed), 2)

    def test_quarantine_moves_and_keeps_the_bytes(self) -> None:
        key = self.tmp / "T0-x" / "cccccccccccc"
        key.mkdir(parents=True)
        (key / "world.gsplatworld").write_bytes(b"data")
        moved = wc.quarantine(self.tmp, key, "77")
        self.assertFalse(key.exists())
        self.assertEqual((moved / "world.gsplatworld").read_bytes(), b"data")
        self.assertEqual(moved.parent.name, wc.QUARANTINE_DIR)
        self.assertEqual(wc.clear_quarantine(self.tmp), [moved.name])


class Labels(unittest.TestCase):
    def test_residency_limit_is_inclusive_one_percent(self) -> None:
        paths = [Path("a")]
        self.assertTrue(wc.residency(paths, lambda p: (1, 100))["cold"])
        self.assertFalse(wc.residency(paths, lambda p: (2, 100))["cold"])
        with self.assertRaises(wc.CacheError):
            wc.residency(paths, lambda p: (0, 0))

    def test_host_cache_is_absent_only_on_evidence(self) -> None:
        self.assertEqual(wc.host_cache_label(1.0), "absent")
        self.assertEqual(wc.host_cache_label(1.249), "absent")
        self.assertEqual(wc.host_cache_label(1.25), "present")
        self.assertEqual(wc.host_cache_label(8.0), "present")
        for unknown in (None, 0, -1.0, "fast", True):
            self.assertEqual(wc.host_cache_label(unknown), "present", unknown)

    def test_v10_never_passes_without_both_numbers(self) -> None:
        self.assertEqual(wc.v10(None, 100)["status"], "NOT_EVALUATED")
        self.assertEqual(wc.v10(100, None)["status"], "NOT_EVALUATED")
        self.assertEqual(wc.v10(100, 0)["status"], "NOT_EVALUATED")
        self.assertEqual(wc.v10(90, 100)["status"], "OK")
        self.assertEqual(wc.v10(89, 100)["status"], "INVALID")


@unittest.skipIf(LINUX, "the refusal path is for platforms without the Linux primitives")
class NonLinuxRefuses(_Tmp):
    def test_mincore_and_lock_refuse(self) -> None:
        f = self.tmp / "f"
        f.write_bytes(b"x" * 10)
        with self.assertRaises(wc.CacheError):
            wc.residency([f])
        if wc.fcntl is None:
            with self.assertRaises(wc.CacheError):
                with wc.cache_lock(self.tmp, True):
                    pass


@unittest.skipUnless(LINUX, "fadvise, mincore and flock are Linux; covered by the CI guard lane")
class LinuxPrimitives(_Tmp):
    SIZE = 32 * 1024 * 1024

    def test_dropped_pages_are_cold_and_read_pages_are_warm(self) -> None:
        path = self.tmp / "world.gsplatworld"
        with path.open("wb") as handle:
            handle.write(os.urandom(self.SIZE))
        if _on_tmpfs(path):
            self.skipTest(f"{path} is on tmpfs; pages cannot be dropped there")
        with path.open("rb") as handle:
            while handle.read(1 << 20):
                pass
        warm = wc.residency([path])
        self.assertGreater(warm["resident_fraction"], 0.5, warm)
        self.assertFalse(warm["cold"])
        self.assertEqual(wc.drop_from_page_cache([path]), self.SIZE)
        cold = wc.residency([path])
        self.assertTrue(cold["cold"], cold)
        self.assertEqual(cold["pages"], warm["pages"])

    def test_exclusive_lock_blocks_a_shared_one(self) -> None:
        order = []
        with wc.cache_lock(self.tmp, True):
            def reader() -> None:
                with wc.cache_lock(self.tmp, False):
                    order.append("shared")

            thread = threading.Thread(target=reader)
            thread.start()
            time.sleep(0.3)
            order.append("exclusive-released")
        thread.join(5)
        self.assertEqual(order, ["exclusive-released", "shared"])


if __name__ == "__main__":
    unittest.main()
