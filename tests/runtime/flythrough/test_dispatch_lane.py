#!/usr/bin/env python3
"""BV-06a dispatch lane: every step fails closed, and the workflow wires them so.

Two halves, both GPU-free and binary-free:

* ``dispatch_lane.py`` behaviour, with ``df``/``dd``/``fio``/``vulkaninfo``/the
  generator/the probe replaced by fakes: the disk preflight refuses below the
  ADR budget (and at one byte below, passes at exactly the budget), an
  unreadable ``df`` is a failure and never "enough", an absent generator or
  probe is NOT_RUN (exit 3) and never OK, a zero exit that leaves no manifest or
  summary is a failure, cleanup deletes only a marked world root.
* ``.github/workflows/beyond_vram_flythrough.yml`` structure, read as text (the
  guard lane has no PyYAML): dispatch-only, no continue-on-error, the disk
  preflight precedes and gates every step that writes to the world root, the
  upload and cleanup always run and cleanup runs after the upload, and the
  build is the optimized D-10 producer build.

Run directly or through ``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import ast
import io
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Dict, List, Optional, Tuple
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))

import dispatch_lane as lane  # noqa: E402
import lane_watchdog as watchdog  # noqa: E402
import run_probe as rp  # noqa: E402
import world_cache as wc  # noqa: E402

WORKFLOW = ROOT / ".github" / "workflows" / "beyond_vram_flythrough.yml"
ADR = ROOT / "docs" / "architecture" / "adr-beyond-vram-worlds.md"
GIB = lane.GIB


def _proc(args, returncode=0, stdout="", stderr=""):
    return subprocess.CompletedProcess(args, returncode, stdout, stderr)


def _df_text(avail: int, total: Optional[int] = None) -> str:
    total = max(228 * 10**9, avail) if total is None else total
    return (
        "Filesystem     1-blocks         Used    Available Capacity Mounted on\n"
        f"/dev/vda1   {total} {total - avail} {avail}      50% /\n"
    )


class FakeRunner:
    """Answers df/du/dd/fio/vulkaninfo/binary/script invocations from a table."""

    def __init__(self, avail: int = 10**15, **overrides) -> None:
        self.avail = avail
        self.calls: List[List[str]] = []
        self.overrides = overrides

    def __call__(self, cmd, **kwargs):
        cmd = [str(c) for c in cmd]
        self.calls.append(cmd)
        tool = Path(cmd[0]).name
        if cmd[0] == sys.executable and len(cmd) > 1:
            tool = Path(cmd[1]).name
        handler = self.overrides.get(tool)
        if handler is not None:
            return handler(cmd, **kwargs)
        if tool == "df":
            return _proc(cmd, 0, _df_text(self.avail))
        if tool == "du":
            return _proc(cmd, 0, f"1234\t{cmd[-1]}\n")
        if tool == "dd":
            return _proc(cmd, 0, "", "1 records in\n")
        raise AssertionError(f"unexpected command {cmd}")


def _no_tools(_name: str) -> Optional[str]:
    return None


def _all_tools(name: str) -> Optional[str]:
    return f"/usr/bin/{name}"


def _tools_except(*absent: str):
    return lambda name: None if name in absent else f"/usr/bin/{name}"


class _TempCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.world_root = self.tmp / "worlds"
        self.out = self.tmp / "out"
        self.out.mkdir()
        self._stdout = mock.patch("sys.stdout", new_callable=io.StringIO)
        self._stdout.start()

    def tearDown(self) -> None:
        self._stdout.stop()
        self._tmp.cleanup()

    def record(self, step: str) -> dict:
        return json.loads((self.out / "steps" / f"{step}.json").read_text(encoding="utf-8"))

    def admit(self, tier: str = "T0") -> None:
        code = lane.disk_preflight(tier, self.world_root, self.out, 1, "7", FakeRunner(), _all_tools)
        self.assertEqual(code, lane.EXIT_OK)


# --------------------------------------------------------------------------
# disk preflight
# --------------------------------------------------------------------------


class DiskBudgetMatchesTheAdr(unittest.TestCase):
    """The constants are the ADR section 5.3 runner table, not numbers chosen here."""

    def setUp(self) -> None:
        self.adr = ADR.read_text(encoding="utf-8")

    def test_margin_is_the_enforced_free_margin(self) -> None:
        self.assertIn("Enforced free margin (25 GiB)", self.adr)
        self.assertEqual(lane.FREE_MARGIN_BYTES, 25 * GIB)

    def test_world_budgets_are_the_m1_line(self) -> None:
        self.assertIn("**During M1:** A uncompressed + T0 | 58.8 + 1.5", self.adr)
        self.assertEqual(lane.WORLD_BUDGET_BYTES["A"], int(58.8 * 10**9))
        self.assertEqual(lane.WORLD_BUDGET_BYTES["T0"], int(1.5 * 10**9))
        self.assertIn("| A plain v3 uncompressed (M1 only) | 54.8 GiB |", self.adr)
        self.assertAlmostEqual(lane.WORLD_BUDGET_BYTES["A"] / GIB, 54.8, delta=0.05)

    def test_tier_a_reserves_t0_too(self) -> None:
        required, parts = lane.required_free_bytes("A", 8)
        self.assertEqual(
            required,
            lane.WORLD_BUDGET_BYTES["A"] + lane.WORLD_BUDGET_BYTES["T0"] + 8 * GIB + 25 * GIB,
        )
        self.assertEqual(set(parts), {"world_T0", "world_A", "disk_probe_file", "free_margin"})

    def test_unknown_tier_is_refused(self) -> None:
        with self.assertRaises(lane.LaneError):
            lane.required_free_bytes("AB", 8)


class DiskPreflight(_TempCase):
    def run_preflight(self, avail: int, tier: str = "A", which=_all_tools, runner=None) -> int:
        return lane.disk_preflight(
            tier, self.world_root, self.out, 8, "42", runner or FakeRunner(avail), which
        )

    def test_refuses_below_the_budget_with_did_not_run_disk(self) -> None:
        required, _ = lane.required_free_bytes("A", 8)
        code = self.run_preflight(required - 1)
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        record = self.record("disk_preflight")
        self.assertEqual(record["status"], "NOT_RUN")
        self.assertTrue(record["reason"].startswith("DID NOT RUN: disk"), record["reason"])
        self.assertFalse(self.world_root.exists(), "a refused run must leave no world root")

    def test_admits_at_exactly_the_budget(self) -> None:
        required, _ = lane.required_free_bytes("A", 8)
        self.assertEqual(self.run_preflight(required), lane.EXIT_OK)
        self.assertEqual(self.record("disk_preflight")["status"], "OK")
        marker = self.world_root / lane.WORLD_ROOT_MARKER
        self.assertTrue(marker.is_file())
        self.assertIn("run_id=42", marker.read_text(encoding="utf-8"))

    def test_the_decision_reads_the_world_filesystem(self) -> None:
        runner = FakeRunner(10**15)
        self.run_preflight(0, runner=runner)
        df_paths = [call[-1] for call in runner.calls if call[0] == "df"]
        self.assertEqual(df_paths[0], str(self.world_root))

    def test_t0_needs_less_than_a(self) -> None:
        required_a, _ = lane.required_free_bytes("A", 8)
        self.assertEqual(self.run_preflight(required_a - 1, tier="T0"), lane.EXIT_OK)

    def test_unreadable_df_fails_and_is_not_enough(self) -> None:
        runner = FakeRunner(df=lambda cmd, **kw: _proc(cmd, 0, "garbage\n"))
        code = self.run_preflight(0, runner=runner)
        self.assertEqual(code, lane.EXIT_FAIL)
        self.assertEqual(self.record("disk_preflight")["status"], "FAIL")

    def test_df_error_fails(self) -> None:
        runner = FakeRunner(df=lambda cmd, **kw: _proc(cmd, 1, "", "boom"))
        self.assertEqual(self.run_preflight(0, runner=runner), lane.EXIT_FAIL)

    def test_df_absent_fails_before_creating_anything(self) -> None:
        self.assertEqual(self.run_preflight(10**15, which=_no_tools), lane.EXIT_FAIL)
        self.assertFalse(self.world_root.exists())

    def test_a_leftover_world_root_is_refused(self) -> None:
        self.world_root.mkdir()
        (self.world_root / "old.bin").write_bytes(b"x")
        self.assertEqual(self.run_preflight(10**15), lane.EXIT_FAIL)
        self.assertTrue((self.world_root / "old.bin").exists(), "preflight deletes nothing")

    def test_relative_world_root_is_refused(self) -> None:
        code = lane.disk_preflight("T0", Path("rel"), self.out, 8, "1", FakeRunner(), _all_tools)
        self.assertEqual(code, lane.EXIT_FAIL)

    def test_probe_size_is_bounded(self) -> None:
        code = lane.disk_preflight("T0", self.world_root, self.out, 0, "1", FakeRunner(), _all_tools)
        self.assertEqual(code, lane.EXIT_FAIL)

    def test_df_parser_rejects_multiple_filesystems(self) -> None:
        text = _df_text(5) + "/dev/vdb1 10 5 5 50% /data\n"
        with self.assertRaises(lane.LaneError):
            lane.parse_df(text)

    def test_df_parser_rejects_human_sizes(self) -> None:
        with self.assertRaises(lane.LaneError):
            lane.parse_df("Filesystem Size Used Avail Use% Mounted on\n/dev/vda1 228G 100G 128G 44% /\n")


# --------------------------------------------------------------------------
# disk throughput
# --------------------------------------------------------------------------


def _fake_fill(path: Path, size: int) -> float:
    path.write_bytes(b"\0" * 16)
    return 2.0


class DiskThroughput(_TempCase):
    def setUp(self) -> None:
        super().setUp()
        self._fill = mock.patch.object(lane, "_fill_probe_file", side_effect=_fake_fill)
        self._fill.start()

    def tearDown(self) -> None:
        self._fill.stop()
        super().tearDown()

    def test_dd_reads_twice_with_o_direct_and_fio_absent_is_recorded(self) -> None:
        self.admit()
        runner = FakeRunner()
        code = lane.disk_throughput(self.world_root, self.out, 1, runner, _tools_except("fio"))
        self.assertEqual(code, lane.EXIT_OK)
        dd_calls = [c for c in runner.calls if c[0] == "dd"]
        self.assertEqual(len(dd_calls), 2)
        for call in dd_calls:
            self.assertIn("iflag=direct", call)
        data = self.record("disk_throughput")["data"]
        self.assertEqual(data["fio"]["present"], False)
        self.assertIn("mb_per_s", data["dd_o_direct_first_read"])
        self.assertIn("mb_per_s", data["dd_o_direct_reread"])
        self.assertFalse((self.world_root / lane.DISK_PROBE_FILE).exists(), "probe file must be deleted")

    def test_dd_absent_fails(self) -> None:
        self.admit()
        code = lane.disk_throughput(self.world_root, self.out, 1, FakeRunner(), _tools_except("dd"))
        self.assertEqual(code, lane.EXIT_FAIL)

    def test_dd_error_fails_and_still_deletes_the_probe_file(self) -> None:
        self.admit()
        runner = FakeRunner(dd=lambda cmd, **kw: _proc(cmd, 1, "", "EINVAL"))
        code = lane.disk_throughput(self.world_root, self.out, 1, runner, _tools_except("fio"))
        self.assertEqual(code, lane.EXIT_FAIL)
        self.assertFalse((self.world_root / lane.DISK_PROBE_FILE).exists())

    def test_installed_fio_that_fails_is_a_failure_not_absence(self) -> None:
        self.admit()
        runner = FakeRunner(fio=lambda cmd, **kw: _proc(cmd, 1, "", "bad"))
        code = lane.disk_throughput(self.world_root, self.out, 1, runner, _all_tools)
        self.assertEqual(code, lane.EXIT_FAIL)

    def test_installed_fio_report_is_read(self) -> None:
        self.admit()

        def fio(cmd, **kw):
            output = next(arg.split("=", 1)[1] for arg in cmd if arg.startswith("--output="))
            self.assertIn("--direct=1", cmd)
            Path(output).write_text(
                json.dumps({"jobs": [{"read": {"bw_bytes": 900000000, "iops": 880.0}}]}),
                encoding="utf-8",
            )
            return _proc(cmd)

        code = lane.disk_throughput(self.world_root, self.out, 1, FakeRunner(fio=fio), _all_tools)
        self.assertEqual(code, lane.EXIT_OK)
        fio_data = self.record("disk_throughput")["data"]["fio"]
        self.assertTrue(fio_data["present"])
        self.assertEqual(fio_data["jobs"]["seq_read_1m"]["bw_bytes_per_s"], 900000000)

    def test_without_the_preflight_marker_nothing_is_written(self) -> None:
        self.world_root.mkdir()
        code = lane.disk_throughput(self.world_root, self.out, 1, FakeRunner(), _all_tools)
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        self.assertEqual(list(self.world_root.iterdir()), [])


# --------------------------------------------------------------------------
# vulkaninfo
# --------------------------------------------------------------------------

VULKANINFO_SAMPLE = """\
VkPhysicalDeviceLimits:
-----------------------
\tmaxStorageBufferRange                           = 4294967295
\tmaxPushConstantsSize                            = 256
VkPhysicalDeviceMaintenance4Properties:
---------------------------------------
\tmaxBufferSize = 0x10000000000
"""


class VulkanLimits(_TempCase):
    def runner(self, text: str, code: int = 0) -> FakeRunner:
        return FakeRunner(vulkaninfo=lambda cmd, **kw: _proc(cmd, code, text))

    def test_decimal_and_hex_limits_are_parsed(self) -> None:
        self.assertEqual(
            lane.parse_vulkan_limits(VULKANINFO_SAMPLE),
            {"maxStorageBufferRange": [4294967295], "maxBufferSize": [0x10000000000]},
        )

    def test_both_limits_present_is_ok_and_logged(self) -> None:
        code = lane.vulkan_limits(self.out, self.runner(VULKANINFO_SAMPLE), _all_tools)
        self.assertEqual(code, lane.EXIT_OK)
        data = self.record("vulkan_limits")["data"]
        self.assertTrue(data["atlas_3_5_gib_within_limits"])
        self.assertTrue((self.out / "vulkaninfo.txt").is_file())

    def test_a_missing_limit_fails(self) -> None:
        text = VULKANINFO_SAMPLE.replace("maxBufferSize", "maxSomethingElse")
        self.assertEqual(lane.vulkan_limits(self.out, self.runner(text), _all_tools), lane.EXIT_FAIL)

    def test_vulkaninfo_error_fails(self) -> None:
        code = lane.vulkan_limits(self.out, self.runner(VULKANINFO_SAMPLE, 1), _all_tools)
        self.assertEqual(code, lane.EXIT_FAIL)

    def test_vulkaninfo_absent_fails(self) -> None:
        self.assertEqual(lane.vulkan_limits(self.out, FakeRunner(), _no_tools), lane.EXIT_FAIL)


# --------------------------------------------------------------------------
# build_info
# --------------------------------------------------------------------------

GOOD_ARGS = (
    "platform=linuxbsd target=editor optimize=speed_trace dev_build=no tests=yes "
    "gs_native_arch=no cache_limit=15 -j16"
)


class BuildInfo(_TempCase):
    def make_binary(self, name: str = lane.PRODUCER_BINARY_NAME, sha: Optional[str] = None) -> Tuple[Path, Path]:
        binary = self.tmp / name
        binary.write_bytes(b"\x7fELF fake")
        provenance = self.tmp / "build-provenance.txt"
        provenance.write_text(
            f"commit=abc\nrun_id=1\nbinary={name}\nsha256={sha or lane._sha256(binary)}\n",
            encoding="utf-8",
        )
        return binary, provenance

    def runner(self) -> FakeRunner:
        return FakeRunner(**{lane.PRODUCER_BINARY_NAME: lambda cmd, **kw: _proc(cmd, 0, "4.7.dev.custom\n")})

    def test_optimized_producer_binary_passes(self) -> None:
        binary, provenance = self.make_binary()
        code = lane.build_info(binary, provenance, GOOD_ARGS, self.out, self.runner())
        self.assertEqual(code, lane.EXIT_OK)
        info = json.loads((self.out / "build_info.json").read_text(encoding="utf-8"))
        self.assertEqual(info["binary_kind"], "editor_tests")
        self.assertEqual(info["version"], "4.7.dev.custom")

    def test_dev_build_args_fail(self) -> None:
        binary, provenance = self.make_binary()
        args = GOOD_ARGS.replace("dev_build=no", "dev_build=yes")
        self.assertEqual(lane.build_info(binary, provenance, args, self.out, self.runner()), lane.EXIT_FAIL)

    def test_missing_optimize_fails(self) -> None:
        binary, provenance = self.make_binary()
        args = GOOD_ARGS.replace("optimize=speed_trace ", "")
        self.assertEqual(lane.build_info(binary, provenance, args, self.out, self.runner()), lane.EXIT_FAIL)

    def test_dev_binary_name_fails(self) -> None:
        binary, provenance = self.make_binary("godot.linuxbsd.editor.dev.x86_64")
        self.assertEqual(lane.build_info(binary, provenance, GOOD_ARGS, self.out, self.runner()), lane.EXIT_FAIL)

    def test_hash_mismatch_fails(self) -> None:
        binary, provenance = self.make_binary(sha="0" * 64)
        self.assertEqual(lane.build_info(binary, provenance, GOOD_ARGS, self.out, self.runner()), lane.EXIT_FAIL)

    def test_a_binary_that_does_not_start_fails(self) -> None:
        binary, provenance = self.make_binary()
        runner = FakeRunner(**{lane.PRODUCER_BINARY_NAME: lambda cmd, **kw: _proc(cmd, 1, "")})
        self.assertEqual(lane.build_info(binary, provenance, GOOD_ARGS, self.out, runner), lane.EXIT_FAIL)


# --------------------------------------------------------------------------
# generate / probe
# --------------------------------------------------------------------------


class _EntryCase(_TempCase):
    def setUp(self) -> None:
        super().setUp()
        self.repo = self.tmp / "repo"
        self.repo.mkdir()
        self.binary = self.tmp / lane.PRODUCER_BINARY_NAME

    def install(self, relative: Path) -> None:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("# stand-in entry point\n", encoding="utf-8")

    @staticmethod
    def arg(cmd: List[str], flag: str) -> str:
        return cmd[cmd.index(flag) + 1]


class Generate(_EntryCase):
    def generator(self, rc: int = 0, manifest: Optional[str] = '{"total_splats": 4194304}'):
        def run(cmd, **kw):
            out = Path(self.arg(cmd, "--out"))
            out.mkdir(parents=True, exist_ok=True)
            (out / "world.bin").write_bytes(b"\0" * 100)
            if manifest is not None:
                (out / lane.MANIFEST_NAME).write_text(manifest, encoding="utf-8")
            return _proc(cmd, rc)

        return FakeRunner(**{lane.GENERATOR_ENTRY.name: run})

    def test_absent_generator_is_not_run_never_ok(self) -> None:
        self.admit()
        code = lane.generate(self.repo, self.binary, "T0", self.world_root, self.out, self.generator())
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        record = self.record("generate")
        self.assertEqual(record["status"], "NOT_RUN")
        self.assertIn("not available yet", record["reason"])
        self.assertIn(lane.GENERATOR_PACKAGE, record["reason"])

    def test_generator_without_the_disk_preflight_is_not_run(self) -> None:
        self.install(lane.GENERATOR_ENTRY)
        runner = self.generator()
        code = lane.generate(self.repo, self.binary, "T0", self.world_root, self.out, runner)
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        self.assertEqual(runner.calls, [], "nothing may be generated without an admitted world root")

    def test_generated_world_is_ok_and_its_manifest_uploaded(self) -> None:
        self.admit("A")
        self.install(lane.GENERATOR_ENTRY)
        runner = self.generator()
        code = lane.generate(self.repo, self.binary, "A", self.world_root, self.out, runner)
        self.assertEqual(code, lane.EXIT_OK)
        self.assertEqual([self.arg(c, "--tier") for c in runner.calls], ["T0", "A"])
        for call in runner.calls:
            self.assertEqual(self.arg(call, "--encoding"), "plain_v3_uncompressed")
        self.assertTrue((self.out / "worlds" / f"A.{lane.MANIFEST_NAME}").is_file())
        self.assertEqual(self.record("generate")["data"]["worlds"]["A"]["bytes_on_disk"], 100 + len('{"total_splats": 4194304}'))

    def test_zero_exit_without_manifest_fails(self) -> None:
        self.admit()
        self.install(lane.GENERATOR_ENTRY)
        code = lane.generate(self.repo, self.binary, "T0", self.world_root, self.out, self.generator(manifest=None))
        self.assertEqual(code, lane.EXIT_FAIL)

    def test_empty_manifest_fails(self) -> None:
        self.admit()
        self.install(lane.GENERATOR_ENTRY)
        code = lane.generate(self.repo, self.binary, "T0", self.world_root, self.out, self.generator(manifest="{}"))
        self.assertEqual(code, lane.EXIT_FAIL)

    def test_generator_error_fails(self) -> None:
        self.admit()
        self.install(lane.GENERATOR_ENTRY)
        code = lane.generate(self.repo, self.binary, "T0", self.world_root, self.out, self.generator(rc=1))
        self.assertEqual(code, lane.EXIT_FAIL)


class Probe(_EntryCase):
    def world(self, name: str) -> None:
        (self.world_root / name).mkdir(parents=True, exist_ok=True)
        (self.world_root / name / lane.MANIFEST_NAME).write_text('{"a": 1}', encoding="utf-8")

    def prober(self, rc: int = 0, summary: bool = True) -> FakeRunner:
        def run(cmd, **kw):
            if summary:
                (Path(self.arg(cmd, "--out")) / lane.PROBE_SUMMARY_NAME).write_text("{}", encoding="utf-8")
            return _proc(cmd, rc)

        return FakeRunner(**{lane.PROBE_ENTRY.name: run})

    def test_absent_probe_is_not_run_never_ok(self) -> None:
        self.admit()
        self.world("T0")
        code = lane.probe(self.repo, self.binary, "T0", self.world_root, self.out, self.prober())
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        self.assertIn("not available yet", self.record("probe")["reason"])
        self.assertIn(lane.PROBE_PACKAGE, self.record("probe")["reason"])

    def test_probe_without_a_generated_world_is_not_run(self) -> None:
        self.admit()
        self.install(lane.PROBE_ENTRY)
        runner = self.prober()
        code = lane.probe(self.repo, self.binary, "T0", self.world_root, self.out, runner)
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        self.assertEqual(runner.calls, [])

    def test_probe_gets_every_world_of_the_tier(self) -> None:
        self.admit("A")
        self.install(lane.PROBE_ENTRY)
        self.world("T0")
        self.world("A")
        runner = self.prober()
        self.assertEqual(lane.probe(self.repo, self.binary, "A", self.world_root, self.out, runner), lane.EXIT_OK)
        worlds = [runner.calls[0][i + 1] for i, a in enumerate(runner.calls[0]) if a == "--world"]
        self.assertEqual([w.split("=", 1)[0] for w in worlds], ["T0", "A"])

    def test_zero_exit_without_summary_fails(self) -> None:
        self.admit()
        self.install(lane.PROBE_ENTRY)
        self.world("T0")
        code = lane.probe(self.repo, self.binary, "T0", self.world_root, self.out, self.prober(summary=False))
        self.assertEqual(code, lane.EXIT_FAIL)

    def test_probe_verdict_code_is_passed_on(self) -> None:
        self.admit()
        self.install(lane.PROBE_ENTRY)
        self.world("T0")
        code = lane.probe(self.repo, self.binary, "T0", self.world_root, self.out, self.prober(rc=2))
        self.assertEqual(code, lane.EXIT_INVALID)
        self.assertEqual(self.record("probe")["status"], "INVALID")


# --------------------------------------------------------------------------
# summarize / cleanup
# --------------------------------------------------------------------------


class Summarize(_TempCase):
    def test_a_step_without_a_record_is_missing_and_the_run_incomplete(self) -> None:
        lane.write_record(self.out, "disk_preflight", lane.EXIT_OK, "ok")
        lane.summarize(self.out)
        summary = json.loads((self.out / "dispatch_summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["overall"], "INCOMPLETE")
        statuses = {row["step"]: row["status"] for row in summary["steps"]}
        self.assertEqual(statuses["disk_preflight"], "OK")
        self.assertEqual(statuses["cache_check"], "MISSING")
        self.assertEqual(set(statuses), set(lane.STEP_NAMES))

    def test_complete_only_when_every_step_is_ok(self) -> None:
        for step in lane.STEP_NAMES:
            lane.write_record(self.out, step, lane.EXIT_OK, "ok")
        lane.summarize(self.out)
        summary = json.loads((self.out / "dispatch_summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["overall"], "COMPLETE")
        lane.write_record(self.out, "probe", lane.EXIT_NOT_RUN, "NOT_RUN: x")
        lane.summarize(self.out)
        summary = json.loads((self.out / "dispatch_summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["overall"], "INCOMPLETE")


class Cleanup(_TempCase):
    def test_deletes_an_admitted_world_root(self) -> None:
        self.admit()
        (self.world_root / "T0").mkdir()
        (self.world_root / "T0" / "world.bin").write_bytes(b"\0" * 10)
        self.assertEqual(lane.cleanup(self.world_root), lane.EXIT_OK)
        self.assertFalse(self.world_root.exists())

    def test_refuses_a_directory_without_the_marker(self) -> None:
        self.world_root.mkdir()
        (self.world_root / "precious").write_bytes(b"x")
        self.assertEqual(lane.cleanup(self.world_root), lane.EXIT_FAIL)
        self.assertTrue((self.world_root / "precious").exists())

    def test_refuses_a_forged_marker(self) -> None:
        self.world_root.mkdir()
        (self.world_root / lane.WORLD_ROOT_MARKER).write_text("something else\n", encoding="utf-8")
        self.assertEqual(lane.cleanup(self.world_root), lane.EXIT_FAIL)
        self.assertTrue(self.world_root.exists())

    def test_absent_world_root_is_nothing_to_do(self) -> None:
        self.assertEqual(lane.cleanup(self.world_root), lane.EXIT_OK)

    def test_leftovers_fail(self) -> None:
        self.admit()
        with mock.patch.object(lane.shutil, "rmtree"):
            self.assertEqual(lane.cleanup(self.world_root), lane.EXIT_FAIL)


# --------------------------------------------------------------------------
# World cache steps (BV-06b)
# --------------------------------------------------------------------------

GEN_SHA = "ab" * 32


class _Lock:
    """Records lock use; the real flock is tested on Linux in test_world_cache.py."""

    def __init__(self) -> None:
        self.calls: List[Tuple[Path, bool]] = []

    def __call__(self, root: Path, exclusive: bool):
        self.calls.append((root, exclusive))
        import contextlib

        return contextlib.nullcontext()


def _no_fadvise(fd: int, offset: int, length: int, advice: int) -> None:
    pass


class _CacheCase(_EntryCase):
    def setUp(self) -> None:
        super().setUp()
        for relative in wc.GENERATOR_SOURCES:
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"// {relative}\n", encoding="utf-8")
        self.cache = self.tmp / "cache"
        self.lock = _Lock()
        self.fadvised: List[int] = []

    def fadvise(self, fd: int, offset: int, length: int, advice: int) -> None:
        self.fadvised.append(fd)

    def generator(self, rc: int = 0, gen_sha: Optional[str] = GEN_SHA, honour_encoding: bool = True,
                  fail_tier: Optional[str] = None) -> FakeRunner:
        def run(cmd, **kw):
            out = Path(self.arg(cmd, "--out"))
            out.mkdir(parents=True, exist_ok=True)
            (out / lane.WORLD_FILE_NAME).write_bytes(b"\1" * 4096)
            codec_id = lane.CODEC_ID_FOR_ENCODING[self.arg(cmd, "--encoding")] if honour_encoding else 0
            manifest = {"schema": "godotgs-world-manifest/1", "writer": {"codec": {"codec_id": codec_id}},
                        "world": {"splat_count": 1024}}
            if gen_sha is not None:
                manifest["gen_sha"] = gen_sha
            (out / lane.MANIFEST_NAME).write_text(json.dumps(manifest), encoding="utf-8")
            return _proc(cmd, 1 if self.arg(cmd, "--tier") == fail_tier else rc)

        return FakeRunner(**{lane.GENERATOR_ENTRY.name: run})

    def prime(self, tier: str = "T0", runner: Optional[FakeRunner] = None, run_id: str = "11",
              encoding: str = lane.ENCODING) -> int:
        self.install(lane.GENERATOR_ENTRY)
        return lane.prime_cache(
            self.repo, self.binary, tier, self.cache, self.out, run_id,
            runner or self.generator(), _all_tools, self.fadvise, self.lock, encoding=encoding,
        )

    def key(self, world: str = "T0", encoding: str = lane.ENCODING) -> Path:
        return self.cache / wc.world_id(world, encoding) / GEN_SHA[:12]

    def check(self, tier: str = "T0", ok: bool = True, encoding: str = lane.ENCODING) -> int:
        def verify(world_file: Path) -> dict:
            self.assertTrue(world_file.is_file(), world_file)
            return {"ok": ok, "problems": [] if ok else ["chunk_sha_mismatch: 3"]}

        return lane.cache_check(self.repo, tier, self.cache, self.out, "12", self.fadvise, verify, self.lock,
                                encoding=encoding)


class PrimeCache(_CacheCase):
    def test_prime_writes_the_section_9_2_layout_complete_last(self) -> None:
        self.assertEqual(self.prime("A"), lane.EXIT_OK, self.record("prime_cache"))
        self.assertEqual((self.cache / wc.CACHE_FORMAT_FILE).read_text(encoding="utf-8").strip(), wc.CACHE_FORMAT)
        for world in ("T0", "A"):
            key = self.key(world)
            self.assertTrue((key / wc.COMPLETE_FILE).is_file())
            meta = wc.read_key_meta(key)
            self.assertEqual(meta["gen_sha"], GEN_SHA)
            self.assertEqual(meta["source_fingerprint"], wc.source_fingerprint(self.repo))
        self.assertEqual(self.lock.calls, [(self.cache, True)])
        # D-14 item 1: every world file (world + manifest) of both worlds was dropped.
        self.assertEqual(len(self.fadvised), 4)

    def test_old_keys_orphans_and_quarantine_are_deleted_first(self) -> None:
        stale = self.cache / wc.world_id("T0", lane.ENCODING) / "000000000000"
        stale.mkdir(parents=True)
        (stale / wc.COMPLETE_FILE).write_text("x", encoding="utf-8")
        orphan = self.cache / wc.world_id("T0", lane.ENCODING) / ".incoming-3"
        orphan.mkdir()
        (self.cache / wc.QUARANTINE_DIR / "T0-x-1").mkdir(parents=True)
        self.assertEqual(self.prime(), lane.EXIT_OK)
        self.assertFalse(stale.exists())
        self.assertFalse(orphan.exists())
        self.assertFalse((self.cache / wc.QUARANTINE_DIR).exists())
        data = self.record("prime_cache")["data"]
        self.assertEqual(data["worlds"]["T0"]["deleted_keys"], ["000000000000"])

    def test_disk_below_world_plus_margin_is_did_not_run_and_generates_nothing(self) -> None:
        need = lane.WORLD_BUDGET_BYTES["T0"] + lane.FREE_MARGIN_BYTES
        runner = self.generator()
        runner.avail = need - 1
        self.assertEqual(self.prime(runner=runner), lane.EXIT_NOT_RUN)
        self.assertIn("DID NOT RUN: disk", self.record("prime_cache")["reason"])
        self.assertFalse(any(Path(c[1]).name == lane.GENERATOR_ENTRY.name for c in runner.calls if len(c) > 1))
        runner = self.generator()
        runner.avail = need
        self.assertEqual(self.prime(runner=runner), lane.EXIT_OK)

    def test_a_failed_generation_leaves_no_complete_key(self) -> None:
        self.assertEqual(self.prime(runner=self.generator(rc=1)), lane.EXIT_FAIL)
        self.assertEqual(wc.complete_keys(self.cache, wc.world_id("T0", lane.ENCODING)), [])
        self.assertEqual(self.check(), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("cache_check")["data"]["reason_code"], "cache_absent")

    def test_a_manifest_without_gen_sha_fails(self) -> None:
        self.assertEqual(self.prime(runner=self.generator(gen_sha=None)), lane.EXIT_FAIL)
        self.assertEqual(wc.complete_keys(self.cache, wc.world_id("T0", lane.ENCODING)), [])

    def test_a_cache_of_another_format_is_refused(self) -> None:
        self.cache.mkdir()
        (self.cache / wc.CACHE_FORMAT_FILE).write_text("other/9\n", encoding="utf-8")
        self.assertEqual(self.prime(), lane.EXIT_FAIL)


class CompressedTierA(_CacheCase):
    """BV-C7 (ADR D-12/D-13, section 5.3): Tier A primed as plain GSQ, the M1 file deleted."""

    def test_gsq_budget_is_the_steady_m1c_line(self) -> None:
        adr = ADR.read_text(encoding="utf-8")
        self.assertIn("**Steady from M1c:** A plain GSQ + A V4 GSQ + T0 + T-ref transient + spill | 7.6 + ", adr)
        self.assertEqual(lane.WORLD_BUDGET_GSQ_BYTES["A"], int(7.6 * 10**9))
        self.assertEqual(lane.world_budget("A", lane.ENCODING_GSQ), int(7.6 * 10**9))
        self.assertEqual(lane.world_budget("T0", lane.ENCODING_GSQ), lane.WORLD_BUDGET_BYTES["T0"])

    def test_only_tier_a_is_compressed(self) -> None:
        runner = self.generator()
        self.assertEqual(self.prime("A", runner, encoding=lane.ENCODING_GSQ), lane.EXIT_OK, self.record("prime_cache"))
        encodings = {self.arg(c, "--tier"): self.arg(c, "--encoding") for c in runner.calls if "--tier" in c}
        self.assertEqual(encodings, {"T0": lane.ENCODING, "A": lane.ENCODING_GSQ})
        self.assertTrue((self.key("A", lane.ENCODING_GSQ) / wc.COMPLETE_FILE).is_file())
        self.assertTrue((self.key("T0") / wc.COMPLETE_FILE).is_file())
        self.assertEqual(wc.read_key_meta(self.key("A", lane.ENCODING_GSQ))["encoding"], lane.ENCODING_GSQ)
        self.assertEqual(self.check("A", encoding=lane.ENCODING_GSQ), lane.EXIT_OK, self.record("cache_check"))
        mapping = json.loads((self.out / lane.CACHE_WORLDS_FILE).read_text(encoding="utf-8"))
        self.assertEqual(Path(mapping["worlds"]["A"]), self.key("A", lane.ENCODING_GSQ))

    def test_the_m1_world_is_deleted_once_compressed_a_is_complete(self) -> None:
        self.assertEqual(self.prime("A"), lane.EXIT_OK)
        m1 = self.key("A").parent
        self.assertTrue(m1.is_dir())
        self.assertEqual(self.prime("A", encoding=lane.ENCODING_GSQ), lane.EXIT_OK, self.record("prime_cache"))
        self.assertFalse(m1.exists())
        data = self.record("prime_cache")["data"]
        self.assertEqual(data["deleted_m1_world"], {"world_id": m1.name, "entries": [GEN_SHA[:12]]})
        self.assertEqual(data["worlds"]["A"]["bytes_per_splat"], 4.0)
        # The flat Tier A check now finds nothing: NOT_RUN, never a probe of the deleted world.
        self.assertEqual(self.check("A"), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("cache_check")["data"]["reason_code"], "cache_absent")

    def test_a_failed_compressed_prime_keeps_the_m1_world(self) -> None:
        self.assertEqual(self.prime("A"), lane.EXIT_OK)
        self.assertEqual(self.prime("A", self.generator(fail_tier="A"), encoding=lane.ENCODING_GSQ), lane.EXIT_FAIL)
        self.assertIn("generator for A exited 1", self.record("prime_cache")["reason"])
        self.assertTrue((self.key("A") / wc.COMPLETE_FILE).is_file())

    def test_a_generator_that_ignores_the_encoding_fails(self) -> None:
        runner = self.generator(honour_encoding=False)
        self.assertEqual(self.prime("A", runner, encoding=lane.ENCODING_GSQ), lane.EXIT_FAIL)
        self.assertIn("ignored the encoding", self.record("prime_cache")["reason"])
        self.assertEqual(wc.complete_keys(self.cache, wc.world_id("A", lane.ENCODING_GSQ)), [])

    def test_a_key_recording_another_encoding_is_stale(self) -> None:
        self.assertEqual(self.prime("A", encoding=lane.ENCODING_GSQ), lane.EXIT_OK)
        meta_path = self.key("A", lane.ENCODING_GSQ) / wc.KEY_META_FILE
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        meta["encoding"] = lane.ENCODING
        meta_path.write_text(json.dumps(meta), encoding="utf-8")
        self.assertEqual(self.check("A", encoding=lane.ENCODING_GSQ), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("cache_check")["data"]["reason_code"], "cache_stale")

    def test_an_unknown_encoding_touches_nothing(self) -> None:
        self.assertEqual(self.prime("A", encoding="spz"), lane.EXIT_FAIL)
        self.assertFalse(self.cache.exists())


class CacheCheck(_CacheCase):
    def test_no_cache_is_not_run_cache_absent(self) -> None:
        self.assertEqual(self.check(), lane.EXIT_NOT_RUN)
        self.assertIn("cache absent", self.record("cache_check")["reason"])
        self.assertFalse(self.cache.exists(), "a check must not create the cache")

    def test_a_verified_cache_is_admitted_and_dropped_again(self) -> None:
        self.assertEqual(self.prime("A"), lane.EXIT_OK)
        self.fadvised.clear()
        self.assertEqual(self.check("A"), lane.EXIT_OK, self.record("cache_check"))
        mapping = json.loads((self.out / lane.CACHE_WORLDS_FILE).read_text(encoding="utf-8"))
        self.assertEqual(set(mapping["worlds"]), {"T0", "A"})
        self.assertEqual(Path(mapping["worlds"]["A"]), self.key("A"))
        self.assertEqual(len(self.fadvised), 4, "D-14 item 1: after every per-run verify")

    def test_a_digest_mismatch_quarantines_the_key_and_is_invalid(self) -> None:
        self.assertEqual(self.prime(), lane.EXIT_OK)
        self.assertEqual(self.check(ok=False), lane.EXIT_INVALID)
        record = self.record("cache_check")
        self.assertEqual(record["data"]["reason_code"], "manifest_mismatch")
        self.assertFalse(self.key().exists())
        moved = Path(record["data"]["worlds"]["T0"]["quarantined_to"])
        self.assertTrue((moved / lane.WORLD_FILE_NAME).is_file(), "quarantine keeps the bytes")
        self.assertFalse((self.out / lane.CACHE_WORLDS_FILE).exists())
        # The next run finds no key: NOT_RUN, never a probe of a bad world.
        self.assertEqual(self.check(), lane.EXIT_NOT_RUN)

    def test_changed_generator_sources_make_the_cache_stale(self) -> None:
        self.assertEqual(self.prime(), lane.EXIT_OK)
        (self.repo / wc.GENERATOR_SOURCES[0]).write_text("// changed\n", encoding="utf-8")
        self.assertEqual(self.check(), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("cache_check")["data"]["reason_code"], "cache_stale")

    def test_two_complete_keys_fail(self) -> None:
        self.assertEqual(self.prime(), lane.EXIT_OK)
        other = self.key().parent / "ffffffffffff"
        shutil.copytree(self.key(), other)
        self.assertEqual(self.check(), lane.EXIT_FAIL)

    def test_an_incomplete_key_is_never_read(self) -> None:
        self.assertEqual(self.prime(), lane.EXIT_OK)
        (self.key() / wc.COMPLETE_FILE).unlink()
        self.assertEqual(self.check(), lane.EXIT_NOT_RUN)


class ProbeCold(_CacheCase):
    def setUp(self) -> None:
        super().setUp()
        self.assertEqual(self.prime(), lane.EXIT_OK)
        self.assertEqual(self.check(), lane.EXIT_OK)
        self.install(lane.PROBE_ENTRY)
        lane.write_record(self.out, "disk_throughput", lane.EXIT_OK, "ok", {"host_cache": "absent"})

    def prober(self, summary: Optional[dict] = None) -> FakeRunner:
        def run(cmd, **kw):
            (Path(self.arg(cmd, "--out")) / lane.PROBE_SUMMARY_NAME).write_text(json.dumps(summary or {}),
                                                                                 encoding="utf-8")
            return _proc(cmd, 0)

        return FakeRunner(**{lane.PROBE_ENTRY.name: run})

    def run_probe(self, resident: Tuple[int, int], runner: Optional[FakeRunner] = None) -> Tuple[int, FakeRunner]:
        runner = runner or self.prober()
        reads = iter([1000, 1000 + 512 * 10])
        code = lane.probe(
            self.repo, self.binary, "T0", self.world_root, self.out, runner,
            cache_worlds=self.out / lane.CACHE_WORLDS_FILE,
            residency_probe=lambda path: resident,
            lock=self.lock,
            read_bytes=lambda: next(reads),
        )
        return code, runner

    def test_a_cold_cache_runs_the_probe_under_a_shared_lock(self) -> None:
        self.lock.calls.clear()
        code, runner = self.run_probe((1, 100))  # exactly 1 %
        self.assertEqual(code, lane.EXIT_OK, self.record("probe"))
        self.assertEqual(self.lock.calls, [(self.cache, False)])
        self.assertIn(f"T0={self.key()}", runner.calls[0])
        cache = self.record("probe")["data"]["cache"]
        self.assertEqual(cache["host_cache"], "absent")
        self.assertTrue(cache["residency"]["cold"])
        self.assertEqual(cache["probe_tree_read_bytes"]["value"], 5120, "the whole tree is recorded, as evidence")
        self.assertIsNone(cache["v10"]["read_bytes"], "a summary without a cold-pass sample is not a measurement")
        self.assertIn("read_bytes unmeasured", cache["v10"]["why"])
        self.assertEqual(cache["v10"]["status"], "NOT_EVALUATED")
        self.assertFalse(cache["cold_proven"], "V10 not evaluated is never proven cold")

    @staticmethod
    def oracle_summary(stored: int, cold: Optional[int] = 5120, warm: int = 0) -> dict:
        """The oracle plus the cold-pass record run_probe derives from the driver's pass_io samples."""
        passes = [{"pass_index": 0, "read_bytes_start": 0 if cold is None else min(cold, 512),
                   "read_bytes_end": cold}]
        if cold is not None:
            passes.append({"pass_index": 1, "read_bytes_start": cold, "read_bytes_end": cold + warm})
        driver = {"pass_io": {"source": "/proc/self/io", "passes": passes}}
        return {"worlds": {"T0": {"flythrough": {"expected_unique_upload_flat": {"stored_bytes": stored}},
                                  "cold_pass_io": rp.cold_pass_io(driver)}}}

    def test_v10_reads_the_path_oracle_of_the_summary(self) -> None:
        """#1376 item 1: read bytes against the oracle's expected unique stored bytes."""
        for stored, status in ((5000, "OK"), (5120, "OK"), (5689, "INVALID"), (100000, "INVALID")):
            code, _ = self.run_probe((0, 100), self.prober(self.oracle_summary(stored)))
            self.assertEqual(code, lane.EXIT_OK)
            cache = self.record("probe")["data"]["cache"]
            self.assertEqual(cache["v10"]["expected_unique_stored_bytes"], stored)
            self.assertEqual(cache["v10"]["worlds"], ["T0"])
            self.assertEqual(cache["v10"]["status"], status, (stored, cache))
            self.assertEqual(cache["cold_proven"], status == "OK")
            self.assertTrue(cache["residency"]["cold"])

    def test_v10_judges_the_cold_pass_alone_never_the_repeat_pass_or_the_sweep(self) -> None:
        """#1403 / D-14: cold-pass reads at 0.5 x the oracle stay INVALID however much the rest of the
        probe read. The probe tree (repeat pass, F/k sweep with its whole-world overview) read 1.0 x
        here; judging that, as the step did before, certified the run cold."""
        stored = 5120
        code, _ = self.run_probe((0, 100), self.prober(self.oracle_summary(stored, cold=stored // 2,
                                                                              warm=stored // 2)))
        self.assertEqual(code, lane.EXIT_OK)
        cache = self.record("probe")["data"]["cache"]
        self.assertEqual(cache["probe_tree_read_bytes"]["value"], stored, "the tree read >= 0.9 x the oracle")
        self.assertEqual(cache["v10"]["read_bytes"], stored // 2)
        self.assertEqual(cache["v10"]["status"], "INVALID", cache)
        self.assertFalse(cache["cold_proven"])

    def test_a_cold_pass_without_its_samples_is_not_evaluated(self) -> None:
        for passes in (None, [], [{"pass_index": 0, "read_bytes_start": -1, "read_bytes_end": 9000}],
                       [{"pass_index": 0, "read_bytes_start": 100}],
                       [{"pass_index": 0, "read_bytes_start": 9000, "read_bytes_end": 100}],
                       [{"pass_index": 1, "read_bytes_start": 0, "read_bytes_end": 9000}]):
            summary = self.oracle_summary(5000)
            driver = {} if passes is None else {"pass_io": {"source": "/proc/self/io", "passes": passes}}
            summary["worlds"]["T0"]["cold_pass_io"] = rp.cold_pass_io(driver)
            self.run_probe((0, 100), self.prober(summary))
            cache = self.record("probe")["data"]["cache"]
            self.assertEqual(cache["v10"]["status"], "NOT_EVALUATED", passes)
            self.assertIsNone(cache["v10"]["read_bytes"], passes)
            self.assertFalse(cache["cold_proven"])

    def test_v10_never_sums_a_partial_oracle(self) -> None:
        summary = self.oracle_summary(5000)
        summary["worlds"]["T0"]["flythrough"]["expected_unique_upload_flat"]["stored_bytes"] = None
        self.run_probe((0, 100), self.prober(summary))
        cache = self.record("probe")["data"]["cache"]
        self.assertEqual(cache["v10"]["status"], "NOT_EVALUATED")
        self.assertIn("T0", cache["v10"]["expected_source"])
        self.assertFalse(cache["cold_proven"])

    def test_a_warm_cache_is_invalid_and_the_probe_never_starts(self) -> None:
        code, runner = self.run_probe((2, 100))  # 2 % > 1 %
        self.assertEqual(code, lane.EXIT_INVALID)
        self.assertEqual(runner.calls, [])
        record = self.record("probe")
        self.assertIn("cache_warm", record["reason"])
        self.assertEqual(record["data"]["cache"]["invalid_reason"], "cache_warm")

    def test_without_a_cache_check_there_is_nothing_to_probe(self) -> None:
        (self.out / lane.CACHE_WORLDS_FILE).unlink()
        code, runner = self.run_probe((0, 100))
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        self.assertEqual(runner.calls, [])

    def test_unmeasurable_residency_is_never_cold(self) -> None:
        def broken(path: Path) -> Tuple[int, int]:
            raise wc.CacheError("mincore residency is only implemented for Linux")

        runner = self.prober()
        code = lane.probe(
            self.repo, self.binary, "T0", self.world_root, self.out, runner,
            cache_worlds=self.out / lane.CACHE_WORLDS_FILE, residency_probe=broken, lock=self.lock,
        )
        self.assertEqual(code, lane.EXIT_FAIL)
        self.assertEqual(runner.calls, [])


class LaneVerdict(_TempCase):
    def all_ok(self) -> None:
        for step in lane.STEP_NAMES:
            if step != "contention":
                lane.write_record(self.out, step, lane.EXIT_OK, "ok")

    def verdict(self) -> dict:
        return json.loads((self.out / lane.VERDICT_FILE).read_text(encoding="utf-8"))

    def test_pass_only_when_every_step_including_contention_is_ok(self) -> None:
        self.all_ok()
        self.assertEqual(lane.summarize(self.out, contention_outcome="success", meta={"run_id": "5"}), 0)
        self.assertEqual(self.verdict()["verdict"], "PASS")
        self.assertEqual(self.verdict()["run_id"], "5")

    def test_a_missing_contention_record_is_not_run(self) -> None:
        self.all_ok()
        self.assertEqual(lane.summarize(self.out, contention_outcome="skipped"), lane.EXIT_NOT_RUN)
        self.assertEqual(self.verdict()["verdict"], "NOT_RUN")

    def test_a_failed_postflight_voids_the_run(self) -> None:
        self.all_ok()
        lane.write_record(self.out, "probe", lane.EXIT_FAIL, "probe exited 1")
        self.assertEqual(lane.summarize(self.out, contention_outcome="failure"), lane.EXIT_VOID)
        self.assertEqual(self.verdict()["verdict"], "VOID")

    def test_controls_only_excludes_the_skipped_probe(self) -> None:
        """#1390 P2: the probe is skipped on purpose in controls_only; that is EXCLUDED, not MISSING."""
        for step in lane.STEP_NAMES:
            if step not in ("contention", "probe"):
                lane.write_record(self.out, step, lane.EXIT_OK, "ok")
        self.assertEqual(lane.summarize(self.out, contention_outcome="success", mode="controls_only"), 0)
        steps = {s["step"]: s["status"] for s in self.verdict()["steps"]}
        self.assertEqual(steps["probe"], lane.EXCLUDED)
        self.assertEqual(self.verdict()["mode"], "controls_only")
        summary = json.loads((self.out / "dispatch_summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["overall"], "COMPLETE")

    def test_a_missing_probe_in_a_full_run_stays_not_run(self) -> None:
        for step in lane.STEP_NAMES:
            if step not in ("contention", "probe"):
                lane.write_record(self.out, step, lane.EXIT_OK, "ok")
        self.assertEqual(lane.summarize(self.out, contention_outcome="success"), lane.EXIT_NOT_RUN)
        self.assertEqual({s["step"]: s["status"] for s in self.verdict()["steps"]}["probe"], "MISSING")

    def test_controls_only_excludes_nothing_else(self) -> None:
        for step in lane.STEP_NAMES:
            if step not in ("contention", "probe", "controls"):
                lane.write_record(self.out, step, lane.EXIT_OK, "ok")
        self.assertEqual(lane.summarize(self.out, contention_outcome="success", mode="controls_only"),
                         lane.EXIT_NOT_RUN)
        steps = {s["step"]: s["status"] for s in self.verdict()["steps"]}
        self.assertEqual(steps["controls"], "MISSING")
        # A record the excluded step did write is still judged.
        lane.write_record(self.out, "controls", lane.EXIT_OK, "ok")
        lane.write_record(self.out, "probe", lane.EXIT_FAIL, "probe exited 1")
        self.assertEqual(lane.summarize(self.out, contention_outcome="success", mode="controls_only"),
                         lane.EXIT_FAIL)
        # A truncated record of the excluded step is not excluded (Codex on #1391).
        (self.out / "steps" / "probe.json").write_text('{"status": "OK"', encoding="utf-8")
        self.assertEqual(lane.summarize(self.out, contention_outcome="success", mode="controls_only"),
                         lane.EXIT_NOT_RUN)
        self.assertEqual({s["step"]: s["status"] for s in self.verdict()["steps"]}["probe"], "MISSING")
        with self.assertRaises(ValueError):
            lane.summarize(self.out, mode="probe_only")

    def test_the_workflow_passes_the_run_mode(self) -> None:
        text = (ROOT / ".github" / "workflows" / "beyond_vram_flythrough.yml").read_text(encoding="utf-8")
        line = next(ln for ln in text.splitlines() if "dispatch_lane.py summarize " in ln)
        self.assertIn("--mode \"${{ inputs.controls_only && 'controls_only' || 'full' }}\"", line)

    def test_precedence(self) -> None:
        self.assertEqual(lane.lane_verdict(["OK", "NOT_RUN", "INVALID"]), "INVALID")
        self.assertEqual(lane.lane_verdict(["OK", "FAIL", "INVALID"]), "FAIL")
        self.assertEqual(lane.lane_verdict(["OK", "MISSING"]), "NOT_RUN")
        self.assertEqual(lane.lane_verdict(["OK", "UNREADABLE"]), "FAIL")
        self.assertEqual(lane.lane_verdict([]), "NOT_RUN")
        self.assertEqual(lane.lane_verdict(["OK", "OK"]), "PASS")

    def test_the_probe_cache_block_reaches_the_verdict(self) -> None:
        self.all_ok()
        lane.write_record(self.out, "probe", lane.EXIT_OK, "ok", {"cache": {"host_cache": "present"}})
        lane.summarize(self.out, contention_outcome="success")
        self.assertEqual(self.verdict()["cache"], {"host_cache": "present"})


class HostCacheLabel(_TempCase):
    def test_disk_throughput_records_the_label(self) -> None:
        self.admit()
        with mock.patch.object(lane, "_fill_probe_file", _fake_fill):
            lane.disk_throughput(self.world_root, self.out, 1, FakeRunner(), _tools_except("fio"))
        data = self.record("disk_throughput")["data"]
        # The probe file was just written by this step: absence is unprovable.
        self.assertEqual(data["host_cache"], "present")

    def test_cached_worlds_budget_only_the_probe_and_the_margin(self) -> None:
        required, parts = lane.required_free_bytes("A", 8, include_worlds=False)
        self.assertEqual(parts, {"disk_probe_file": 8 * GIB, "free_margin": lane.FREE_MARGIN_BYTES})
        self.assertEqual(required, 8 * GIB + lane.FREE_MARGIN_BYTES)


# --------------------------------------------------------------------------
# The workflow
# --------------------------------------------------------------------------

_STEP_START = "      - "
_STEP_FIELD = re.compile(r"^        ([A-Za-z-]+):\s*(.*)$")
_LANE = "tests/runtime/flythrough/dispatch_lane.py"


def _job_block(text: str, job: str) -> List[str]:
    lines = text.splitlines()
    start = lines.index(f"  {job}:")
    end = next(
        (i for i in range(start + 1, len(lines)) if re.match(r"^  [A-Za-z0-9_-]+:\s*$", lines[i])),
        len(lines),
    )
    return lines[start:end]


def _steps(job_lines: List[str]) -> List[Dict[str, str]]:
    """name/id/if/uses/run of each step; fails on a shape it does not model."""
    steps_at = job_lines.index("    steps:")
    steps: List[Dict[str, str]] = []
    current: Optional[Dict[str, str]] = None
    block_key: Optional[str] = None
    for line in job_lines[steps_at + 1 :]:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.startswith(_STEP_START):
            current = {}
            steps.append(current)
            line = "        " + line[len(_STEP_START):]
            block_key = None
        if current is None:
            raise AssertionError(f"content before the first step: {line!r}")
        field = _STEP_FIELD.match(line)
        if field:
            key, value = field.groups()
            if key in current:
                raise AssertionError(f"duplicate {key} in step {current.get('name')!r}")
            if value in ("|", ">"):
                current[key] = ""
                block_key = key
            else:
                current[key] = value.strip().strip("'\"") if key != "if" else value.strip()
                block_key = None
        elif block_key is not None and line.startswith("          "):
            current[block_key] += line.strip() + "\n"
        elif not line.startswith("          "):
            raise AssertionError(f"unmodelled step line {line!r}")
    return steps


WATCHDOG_CRON = "0 6 * * 1"
LANE_CRON = "0 22 * * 6"


class WorkflowContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.build = _job_block(cls.text, "build-linux-editor")
        cls.prime = _job_block(cls.text, "prime-world-cache")
        cls.auto_tree = _job_block(cls.text, "prime-auto-tree")
        cls.gpu = _job_block(cls.text, "flythrough")
        cls.codec = _job_block(cls.text, "codec-probe")
        cls.watchdog = _job_block(cls.text, "liveness-watchdog")
        cls.steps = _steps(cls.gpu)
        cls.prime_steps = _steps(cls.prime)
        cls.auto_tree_steps = _steps(cls.auto_tree)
        cls.by_id = {s["id"]: i for i, s in enumerate(cls.steps) if "id" in s}

    @staticmethod
    def job_if(job_lines: List[str]) -> str:
        hits = [line.split("if:", 1)[1].strip() for line in job_lines if line.startswith("    if:")]
        assert len(hits) == 1, hits
        return hits[0]

    def index_of(self, subcommand: str, steps: Optional[List[Dict[str, str]]] = None) -> int:
        steps = self.steps if steps is None else steps
        hits = [
            i for i, s in enumerate(steps)
            if re.search(rf"^python3 {re.escape(_LANE)} {subcommand}\b", s.get("run", ""), re.M)
        ]
        self.assertEqual(len(hits), 1, f"expected one step running {subcommand}, found {hits}")
        return hits[0]

    def test_triggers_are_the_weekly_lane_the_watchdog_and_dispatch(self) -> None:
        on_block = self.text.split("\non:\n", 1)[1].split("\nrun-name:", 1)[0]
        events = re.findall(r"^  ([a-z_]+):", on_block, re.M)
        self.assertEqual(events, ["schedule", "workflow_dispatch"])
        self.assertEqual(re.findall(r'^    - cron: "([^"]+)"$', on_block, re.M), [LANE_CRON, WATCHDOG_CRON])

    def test_job_env_does_not_reference_runner_context(self) -> None:
        # runner is available to steps, but not jobs.<job_id>.env.
        jobs = re.findall(r"^  ([A-Za-z0-9_-]+):\s*$", self.text.split("\njobs:\n", 1)[1], re.M)
        self.assertTrue(jobs)
        for name in jobs:
            in_env = False
            env_lines = []
            for line in _job_block(self.text, name):
                if line.startswith("    env:"):
                    in_env = True
                elif re.match(r"^    \S", line):
                    in_env = False
                if in_env:
                    env_lines.append(line)
            for expression in re.findall(r"\$\{\{(.*?)\}\}", "\n".join(env_lines), re.S):
                self.assertNotRegex(expression, r"\brunner\s*(?:\.|\[)", name)

    def test_every_watchdog_cron_copy_matches_the_schedule(self) -> None:
        """A drifted copy would run the GPU lane on Mondays or never run the watchdog."""
        copies = re.findall(r"github\.event\.schedule (?:==|!=) '([^']+)'", self.text)
        self.assertGreaterEqual(len(copies), 5)
        self.assertEqual(set(copies), {WATCHDOG_CRON})

    def test_lane_jobs_never_run_for_the_watchdog_and_the_watchdog_only_for_it(self) -> None:
        exclude = f"github.event.schedule != '{WATCHDOG_CRON}' && inputs.mode != 'watchdog'"
        for job in (self.build, self.gpu):
            self.assertIn(exclude, self.job_if(job))
        for job in (self.prime, self.codec):
            condition = self.job_if(job)
            self.assertIn("github.event_name == 'workflow_dispatch'", condition)
            self.assertIn("inputs.mode != 'watchdog'", condition)
        self.assertIn("inputs.prime_world_cache", self.job_if(self.prime))
        self.assertEqual(
            self.job_if(self.watchdog),
            f"${{{{ github.event.schedule == '{WATCHDOG_CRON}' || inputs.mode == 'watchdog' }}}}",
        )

    def test_least_privilege_and_never_cancelled(self) -> None:
        self.assertIn("\npermissions:\n  contents: read\n\n", self.text)
        self.assertIn(
            "  group: bvw-${{ github.event_name }}-${{ (github.event.schedule == '0 6 * * 1' || "
            "inputs.mode == 'watchdog') && 'watchdog' || inputs.mode == 'auto_tree' && 'auto-tree' || 'lane' }}\n  cancel-in-progress: false\n",
            self.text,
        )
        # actions: read on the watchdog job only (ADR section 9.1).
        self.assertEqual(len(re.findall(r"^ +actions: read$", self.text, re.M)), 1)
        self.assertIn("      actions: read", self.watchdog)

    def test_nothing_may_swallow_a_failure(self) -> None:
        self.assertNotIn("continue-on-error", self.text)

    def test_runner_routing(self) -> None:
        self.assertIn("    runs-on: [self-hosted, Linux, X64]", self.build)
        # The cache lives on the 3080's disk: every job that touches it is pinned to it.
        for job in (self.prime, self.auto_tree, self.gpu):
            self.assertIn("    runs-on: [self-hosted, Linux, X64, gpu, rtx3080]", job)
        self.assertIn("    runs-on: ubuntu-latest", self.watchdog)
        self.assertIn("    needs: [build-linux-editor, prime-world-cache]", self.gpu)
        gpu_if = self.job_if(self.gpu)
        self.assertIn("needs.build-linux-editor.result == 'success'", gpu_if)
        self.assertIn(
            "(needs.prime-world-cache.result == 'success' || needs.prime-world-cache.result == 'skipped')",
            gpu_if,
        )

    def test_the_build_is_the_optimized_producer_build(self) -> None:
        match = re.search(r'^  BVW_SCONS_ARGS: "([^"]+)"$', self.text, re.M)
        self.assertIsNotNone(match)
        args = match.group(1).split()
        for required in lane.REQUIRED_SCONS_ARGS:
            self.assertIn(required, args)
        self.assertNotIn("dev_build=yes", args)
        self.assertIn("cache_path=/opt/actions-runner/godotgs-scons-cache-opt", args)
        build_runs = "".join(s.get("run", "") for s in _steps(self.build))
        self.assertIn("python -m SCons $BVW_SCONS_ARGS\n", build_runs)
        for steps in (self.steps, self.prime_steps, self.auto_tree_steps):
            self.assertIn('--scons-args "$BVW_SCONS_ARGS"', steps[self.index_of("build-info", steps)]["run"])

    def test_the_binary_handoff_is_pinned(self) -> None:
        build_runs = "".join(s.get("run", "") for s in _steps(self.build))
        self.assertIn("linux_gpu_runner.sh stage-binary", build_runs)
        for steps in (self.steps, self.prime_steps, self.auto_tree_steps):
            self.assertIn("linux_gpu_runner.sh install-binary", "".join(s.get("run", "") for s in steps))
        # Build upload; both primes, flythrough and codec-probe download.
        self.assertEqual(self.text.count("name: godot-linuxbsd-editor-tests-${{ github.sha }}"), 5)

    def test_the_cache_root_is_the_section_9_2_path(self) -> None:
        self.assertIn(f"  BVW_CACHE_ROOT: {wc.DEFAULT_CACHE_ROOT.as_posix()}\n", self.text)
        prime = self.prime_steps[self.index_of("prime-cache", self.prime_steps)]
        self.assertIn('--cache-root "$BVW_CACHE_ROOT"', prime["run"])
        self.assertIn('--cache-root "$BVW_CACHE_ROOT"', self.steps[self.index_of("cache-check")]["run"])

    def test_every_lane_step_is_wired(self) -> None:
        for subcommand in (
            "disk-preflight", "disk-throughput", "vulkan-limits", "build-info",
            "cache-check", "probe", "summarize", "cleanup",
        ):
            with self.subTest(subcommand=subcommand):
                self.index_of(subcommand)
        # Generation is the prime job's alone: the fly-through never writes a world.
        self.assertFalse(
            any(re.search(rf"{re.escape(_LANE)} (generate|prime-cache)\b", s.get("run", "")) for s in self.steps)
        )

    def _reaches(self, index: int, gate: str) -> bool:
        """True when step `index` can only run after step id `gate` succeeded."""
        target = self.by_id[gate]
        seen = set()
        while index not in seen:
            seen.add(index)
            if index == target:
                return True
            condition = self.steps[index].get("if")
            if condition is None:
                # Implicit success(): runs only if every earlier step succeeded.
                return index > target
            if re.search(r"\b(always|failure)\s*\(", condition):
                return False
            ids = re.findall(r"steps\.([a-z_]+)\.outcome == 'success'", condition)
            if not ids:
                return False
            if gate in ids:
                return True
            index = self.by_id[ids[0]]
        return False

    def test_the_disk_preflight_gates_everything_that_writes_or_measures(self) -> None:
        preflight = self.index_of("disk-preflight")
        self.assertIsNone(self.steps[preflight].get("if"), "the refusal step itself is unconditional")
        self.assertIn("--worlds-cached", self.steps[preflight]["run"])
        for subcommand in ("disk-throughput", "vulkan-limits", "build-info", "cache-check", "probe"):
            with self.subTest(subcommand=subcommand):
                index = self.index_of(subcommand)
                self.assertGreater(index, preflight)
                self.assertTrue(
                    self._reaches(index, "disk_preflight"),
                    f"{subcommand} can run without a successful disk preflight",
                )

    def test_every_flythrough_test_file_is_wired_into_a_lane(self) -> None:
        """A test no lane runs is decoration; use the runner's guard and binary registrations."""
        sys.path.insert(0, str(ROOT / "tests" / "ci"))
        import run_module_tests  # noqa: PLC0415

        wired = {p.resolve() for p in run_module_tests.BEYOND_VRAM_LANE_TEST_SCRIPTS}
        wired.add(run_module_tests.BEYOND_VRAM_PROBE_LANE_TEST_SCRIPT.resolve())
        wired.add(run_module_tests.WORLD_MANIFEST_VERIFIER_TEST_SCRIPT.resolve())
        wired.add(run_module_tests.AUTO_TREE_BINARY_TEST_SCRIPT.resolve())
        present = {p.resolve() for p in HERE.glob("test_*.py")}
        self.assertEqual(present - wired, set(), "a flythrough test file runs in no lane")

    def test_the_probe_runs_only_on_a_checked_cache(self) -> None:
        probe = self.index_of("probe")
        self.assertGreater(probe, self.index_of("cache-check"))
        self.assertTrue(self._reaches(probe, "cache_check"))
        self.assertTrue(self._reaches(probe, "build_info"))
        self.assertIn(f'--cache-worlds "$BVW_OUT/{lane.CACHE_WORLDS_FILE}"', self.steps[probe]["run"])

    def test_preflights_precede_the_lane(self) -> None:
        for steps, first in ((self.steps, "disk-preflight"), (self.prime_steps, "build-info"),
                             (self.auto_tree_steps, "build-info")):
            first_lane = self.index_of(first, steps)
            runs = [s.get("run", "") for s in steps]
            for script in ("tests/ci/runner_gpu_contention.py preflight", "tests/ci/preflight_runner_gpu_environment.py"):
                index = next(i for i, r in enumerate(runs) if script in r)
                self.assertLess(index, first_lane)
        self.assertLess(
            self.index_of("build-info", self.prime_steps), self.index_of("prime-cache", self.prime_steps)
        )

    def test_postflight_verdict_upload_then_cleanup_always_run(self) -> None:
        postflight = self.by_id["postflight"]
        summarize = self.index_of("summarize")
        uploads = [i for i, s in enumerate(self.steps) if s.get("uses", "").startswith("actions/upload-artifact")]
        cleanup = self.index_of("cleanup")
        for index in (postflight, summarize, *uploads, cleanup):
            self.assertEqual(self.steps[index].get("if"), "always()")
        self.assertIn("tests/ci/runner_gpu_contention.py postflight", self.steps[postflight]["run"])
        self.assertIn('--contention-outcome "${{ steps.postflight.outcome }}"', self.steps[summarize]["run"])
        # Outputs, the probe screenshots (gate repair) and the verdict the watchdog reads.
        self.assertEqual(len(uploads), 3)
        self.assertLess(postflight, summarize, "the verdict must see the contention outcome")
        self.assertLess(summarize, min(uploads))
        self.assertLess(max(uploads), cleanup)
        self.assertEqual(cleanup, max(i for i, s in enumerate(self.steps) if _LANE in s.get("run", "")))

    def test_the_verdict_artifact_is_what_the_watchdog_reads(self) -> None:
        block = self.text.split("- name: Upload lane verdict", 1)[1].split("- name:", 1)[0]
        self.assertIn(
            f"name: {watchdog.VERDICT_ARTIFACT_PREFIX}${{{{ github.run_id }}}}-${{{{ github.run_attempt }}}}", block
        )
        self.assertIn(f"path: ${{{{ runner.temp }}}}/bvw-out/{lane.VERDICT_FILE}", block)
        self.assertIn("if-no-files-found: error", block)
        self.assertIn(f"&& '{watchdog.WATCHDOG_TITLE}' ||", self.text, "the watchdog skips its own runs by run-name")

    def test_the_watchdog_job_runs_the_watchdog(self) -> None:
        runs = "".join(s.get("run", "") for s in _steps(self.watchdog))
        self.assertIn('python3 tests/runtime/flythrough/lane_watchdog.py --repo "$GITHUB_REPOSITORY"', runs)
        self.assertIn("--workflow beyond_vram_flythrough.yml", runs)
        self.assertIn("      BVW_WATCHDOG_MODE: pre-m2", self.watchdog)
        self.assertIn("      GH_TOKEN: ${{ github.token }}", self.watchdog)

    def test_the_scratch_root_and_outputs_are_job_temp_paths(self) -> None:
        runs = "".join(s.get("run", "") for s in self.steps)
        self.assertIn('echo "BVW_WORLD_ROOT=$RUNNER_TEMP/bvw-scratch" >> "$GITHUB_ENV"', runs)
        self.assertIn('echo "BVW_OUT=$RUNNER_TEMP/bvw-out" >> "$GITHUB_ENV"', runs)
        self.assertIn("path: ${{ runner.temp }}/bvw-out/", self.text)

    def test_the_schedule_runs_tier_a(self) -> None:
        self.assertIn("      BVW_TIER: ${{ inputs.tier || 'A' }}", self.gpu)

    def test_tier_choices_match_the_lane(self) -> None:
        tier_block = self.text.split("      tier:\n", 1)[1].split("      prime_world_cache:", 1)[0]
        options = re.findall(r'^          - "([^"]+)"$', tier_block, re.M)
        self.assertEqual(sorted(options), sorted(lane.TIER_WORLDS))

    def test_encoding_choices_match_the_lane_and_reach_prime_and_check(self) -> None:
        # BV-C7: Tier A's encoding; the schedule (no inputs) and the default are compressed (D-13).
        block = self.text.split("      encoding:\n", 1)[1].split("      disk_probe_gib:", 1)[0]
        options = re.findall(r'^          - "([^"]+)"$', block, re.M)
        self.assertEqual(sorted(options), sorted(lane.ENCODINGS))
        self.assertIn('        default: "plain_v3_gsq"', block)
        self.assertEqual(self.text.count("BVW_ENCODING: ${{ inputs.encoding || 'plain_v3_gsq' }}"), 2)
        self.assertIn('prime-cache --tier "$BVW_TIER" --encoding "$BVW_ENCODING"', self.text)
        self.assertIn('cache-check --tier "$BVW_TIER" --encoding "$BVW_ENCODING"', self.text)

class D25AutoTreePreflight(_TempCase):
    """Captured /proc/meminfo shape; values are boundary-test substitutions."""

    REQUIRED_DISK = 2 * 10_200_000_000 + 12 * GIB

    def meminfo(self, available=20 * GIB):
        path = self.tmp / "meminfo"
        path.write_text(
            f"MemTotal:       {32 * GIB // 1024} kB\n"
            "MemFree:          1048576 kB\n"
            f"MemAvailable:   {available // 1024} kB\n"
            "Buffers:           123456 kB\nCached:          2345678 kB\n"
            "SwapTotal:             0 kB\n", encoding="ascii",
        )
        return path

    def preflight(self, path, free=None):
        runner = FakeRunner(avail=self.REQUIRED_DISK if free is None else free)
        code = lane.auto_tree_preflight(
            self.tmp, self.out, runner=runner, which=_all_tools, meminfo_path=path,
        )
        return code, self.record("auto_tree_preflight")

    def test_memory_below_20_gib_is_not_run(self):
        code, record = self.preflight(self.meminfo(20 * GIB - 1024))
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        self.assertEqual(record["status"], "NOT_RUN")
        self.assertEqual(record["data"]["reason_code"], "ram_insufficient")
        estimate = record["data"]["builder_ram_estimate"]
        self.assertFalse(estimate["enforced"])
        self.assertEqual(estimate["tiled_permanent_bytes"], 389_184_099_456)
        self.assertTrue(estimate["exceeds_job_limit"])

    def test_disk_below_two_outputs_plus_scratch_is_not_run(self):
        code, record = self.preflight(self.meminfo(), self.REQUIRED_DISK - 1)
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        self.assertEqual(record["data"]["reason_code"], "disk_insufficient")

    def test_missing_meminfo_is_not_run(self):
        code, record = self.preflight(self.tmp / "missing-meminfo")
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        self.assertEqual(record["data"]["reason_code"], "meminfo_unavailable")

    def test_unreadable_meminfo_is_not_run(self):
        path = self.meminfo()
        original = Path.read_text

        def read(candidate, *args, **kwargs):
            if candidate == path:
                raise PermissionError("denied")
            return original(candidate, *args, **kwargs)

        with mock.patch.object(Path, "read_text", read):
            code, record = self.preflight(path)
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        self.assertEqual(record["data"]["reason_code"], "meminfo_unavailable")

    def test_malformed_or_missing_available_memory_is_not_run(self):
        for content in ("", "MemTotal: 33554432 kB\n", "MemAvailable: nope kB\n",
                        "MemTotal: 33554432 kB\nMemAvailable: 23068672 MB\n",
                        "MemTotal: 33554432 kB\nMemAvailable: -1 kB\n"):
            with self.subTest(content=content):
                path = self.meminfo()
                path.write_text(content, encoding="ascii")
                code, record = self.preflight(path)
                self.assertEqual(code, lane.EXIT_NOT_RUN)
                self.assertEqual(record["data"]["reason_code"], "meminfo_invalid")

    def test_exact_memory_and_disk_boundaries_record_measured_numbers(self):
        code, record = self.preflight(self.meminfo())
        self.assertEqual(code, lane.EXIT_OK)
        self.assertEqual(record["status"], "OK")
        data = record["data"]
        self.assertEqual(data["mem_available_bytes"], 20 * GIB)
        self.assertEqual(data["mem_total_bytes"], 32 * GIB)
        self.assertEqual(data["df"]["avail_bytes"], self.REQUIRED_DISK)
        self.assertEqual(data["df"]["total_bytes"], 228 * 10**9)
        self.assertEqual(data["df"]["used_bytes"], 228 * 10**9 - self.REQUIRED_DISK)
        self.assertEqual(data["required_disk_bytes"], self.REQUIRED_DISK)
        self.assertTrue(data["builder_ram_estimate"]["exceeds_job_limit"],
                        "the estimate must not refuse admission by itself")

    def test_swap_measurements_are_printed_and_recorded_in_bytes(self):
        for total, free in ((8 * GIB, 3 * GIB), (0, 0)):
            with self.subTest(total=total, free=free):
                path = self.meminfo()
                path.write_text(path.read_text().replace("SwapTotal:             0 kB\n",
                                f"SwapTotal: {total // 1024} kB\nSwapFree: {free // 1024} kB\n"))
                output = io.StringIO()
                with mock.patch("sys.stdout", output):
                    code, record = self.preflight(path)
                self.assertEqual(code, lane.EXIT_OK)
                for name, field, value in (("MemAvailable", "mem_available_bytes", 20 * GIB),
                                           ("MemTotal", "mem_total_bytes", 32 * GIB),
                                           ("SwapTotal", "swap_total_bytes", total),
                                           ("SwapFree", "swap_free_bytes", free)):
                    self.assertEqual(record["data"][field], value)
                    self.assertIn(f"{name}={value} bytes", output.getvalue())

    def test_missing_memory_fields_are_absent_with_reason(self):
        for name, field in (("MemTotal", "mem_total_bytes"), ("SwapTotal", "swap_total_bytes"),
                            ("SwapFree", "swap_free_bytes")):
            with self.subTest(name=name):
                path = self.meminfo()
                path.write_text("\n".join(line for line in path.read_text().splitlines()
                                           if not line.startswith(name + ":")))
                output = io.StringIO()
                with mock.patch("sys.stdout", output):
                    code, record = self.preflight(path)
                self.assertEqual(code, lane.EXIT_NOT_RUN if name == "MemTotal" else lane.EXIT_OK)
                self.assertIsNone(record["data"][field])
                self.assertIn(name, record["data"]["measurement_absence"][field])
                self.assertIn(f"{name}=absent (", output.getvalue())
                self.assertEqual(record["data"]["mem_available_bytes"], 20 * GIB)

    def test_unavailable_meminfo_records_absence_for_all_memory_measurements(self):
        code, record = self.preflight(self.tmp / "missing")
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        for field in ("mem_total_bytes", "mem_available_bytes", "swap_total_bytes", "swap_free_bytes"):
            self.assertIsNone(record["data"][field])
            self.assertTrue(record["data"]["measurement_absence"][field])

    def test_failed_tier_a_preflight_preserves_t0_evidence_and_records_not_run(self):
        self.out.mkdir(parents=True, exist_ok=True)
        names = ("T0.build.json", "T0.verify.json", "T0.oracle.json", "real_scan_identity.json")
        # Orchestration sentinels, not producer reports.
        for name in names:
            (self.out / name).write_text('{"status": "OK", "reason": "completed earlier"}\n')
        before = {name: (self.out / name).read_bytes() for name in names}
        preflight = lane.auto_tree_preflight
        with mock.patch.object(lane, "auto_tree_preflight", side_effect=lambda root, out:
                               preflight(root, out, FakeRunner(), _all_tools, self.meminfo(20 * GIB - 1024))):
            code = lane.main(["auto-tree-preflight", "--cache-root", str(self.tmp), "--out", str(self.out)])
        self.assertEqual(code, lane.EXIT_NOT_RUN)
        self.assertNotEqual(code, 0, "the workflow must stay red on refused Tier A admission")
        self.assertEqual(before, {name: (self.out / name).read_bytes() for name in names})
        for name in ("A.first.build", "A.second.build", "A.repeat_identity", "A.cache_key"):
            record = json.loads((self.out / (name + ".json")).read_text())
            self.assertEqual(record["status"], "NOT_RUN")
            self.assertIn("MemAvailable", record["reason"])


class D25AutoTreeCache(_CacheCase):
    """Orchestration doubles only: arbitrary file bytes are NOT a V4 fixture."""

    def sources(self):
        for relative in wc.TREE_SOURCES:
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"// tree source {relative}\n", encoding="utf-8")

    def snapshot(self, path):
        return {p.relative_to(path).as_posix(): p.read_bytes()
                for p in path.rglob("*") if p.is_file()}

    def ready_plain(self):
        self.assertEqual(self.prime("A", encoding=lane.ENCODING_GSQ), lane.EXIT_OK)
        self.sources()
        return self.key("A", lane.ENCODING_GSQ)

    def tree_fixture(self):
        from test_verify_world_manifest import build_v4_structure_world
        from verify_world_manifest import compute_tree_sha

        plain = self.ready_plain()
        fixture_dir = self.tmp / "tree-structure"
        fixture_dir.mkdir()
        fixture = build_v4_structure_world(fixture_dir)
        sidecar = json.loads(fixture.with_suffix(".manifest.json").read_text(encoding="utf-8"))
        sidecar["source_fingerprint"] = wc.source_fingerprint(self.repo, wc.TREE_SOURCES)
        sidecar["input_file_sha256"] = hashlib.sha256((plain / lane.WORLD_FILE_NAME).read_bytes()).hexdigest()
        self.tree_sha = sidecar["tree_sha"] = compute_tree_sha(sidecar)
        tree = self.cache / "A-v4_gsq_auto" / f"{GEN_SHA[:12]}-{self.tree_sha[:12]}"
        tree.mkdir(parents=True)
        (tree / lane.WORLD_FILE_NAME).write_bytes(fixture.read_bytes())
        # The sidecar must bind the same inputs as .key.json. An empty sidecar
        # would conceal a missing cross-check while the verifier is doubled.
        (tree / lane.MANIFEST_NAME).write_text(json.dumps(sidecar), encoding="utf-8")
        meta = {"schema": wc.KEY_SCHEMA, "world": "A", "encoding": "v4_gsq_auto",
                "gen_sha": GEN_SHA, "tree_sha": self.tree_sha, "run_id": "25",
                "input_file_sha256": hashlib.sha256((plain / lane.WORLD_FILE_NAME).read_bytes()).hexdigest(),
                "tree_source_fingerprint": wc.source_fingerprint(self.repo, wc.TREE_SOURCES),
                "source_fingerprint": wc.source_fingerprint(self.repo)}
        (tree / wc.KEY_META_FILE).write_text(json.dumps(meta), encoding="utf-8")
        (tree / wc.COMPLETE_FILE).write_text("run_id=25\n", encoding="utf-8")
        return plain, tree

    def tree_check(self):
        return lane.cache_check(
            self.repo, "A", self.cache, self.out, "26", self.fadvise,
            lambda path: {"ok": True, "problems": [], "tree_sha": self.tree_sha},
            self.lock, encoding=lane.ENCODING_GSQ, auto_tree=True,
        )

    def test_tree_source_edit_stales_only_tree_key(self):
        plain, tree = self.tree_fixture()
        self.assertEqual(self.tree_check(), lane.EXIT_OK)
        before = self.snapshot(plain)
        (self.repo / wc.TREE_SOURCES[0]).write_text("// edited tree\n", encoding="utf-8")
        self.assertEqual(self.check("A", encoding=lane.ENCODING_GSQ), lane.EXIT_OK)
        self.assertEqual(self.tree_check(), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("cache_check")["data"]["reason_code"], "cache_stale")
        self.assertEqual(self.snapshot(plain), before)
        self.assertTrue((tree / wc.COMPLETE_FILE).exists())

    def test_generator_source_edit_stales_both_keys(self):
        plain, tree = self.tree_fixture()
        self.assertEqual(self.tree_check(), lane.EXIT_OK)
        before = self.snapshot(self.cache)
        # Mutation is confined to the TEMPORARY checkout, never the real source.
        (self.repo / wc.GENERATOR_SOURCES[0]).write_text("// edited generator\n", encoding="utf-8")
        self.assertEqual(self.check("A", encoding=lane.ENCODING_GSQ), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("cache_check")["data"]["reason_code"], "cache_stale")
        self.assertEqual(self.tree_check(), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("cache_check")["data"]["reason_code"], "cache_stale")
        self.assertEqual(self.snapshot(self.cache), before)

    def test_recorded_input_digest_mismatch_is_stale_without_rebuild(self):
        plain, tree = self.tree_fixture()
        self.assertEqual(self.tree_check(), lane.EXIT_OK)
        path = tree / wc.KEY_META_FILE
        meta = json.loads(path.read_text(encoding="utf-8"))
        meta["input_file_sha256"] = "00" * 32
        path.write_text(json.dumps(meta), encoding="utf-8")
        before = self.snapshot(self.cache)
        self.assertEqual(self.tree_check(), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("cache_check")["data"]["reason_code"], "cache_stale")
        self.assertEqual(self.snapshot(self.cache), before)

    def test_auto_tree_check_requires_both_complete_markers(self):
        plain, tree = self.tree_fixture()
        self.assertEqual(self.tree_check(), lane.EXIT_OK)
        for key in (plain, tree):
            with self.subTest(key=key.name):
                marker = key / wc.COMPLETE_FILE
                saved = marker.read_bytes()
                marker.unlink()
                self.assertEqual(self.tree_check(), lane.EXIT_NOT_RUN)
                self.assertEqual(self.record("cache_check")["data"]["reason_code"], "cache_absent")
                marker.write_bytes(saved)

    def test_sidecar_identity_cannot_be_laundered_by_updating_key_metadata(self):
        plain, tree = self.tree_fixture()
        self.assertEqual(self.tree_check(), lane.EXIT_OK)
        path = tree / wc.KEY_META_FILE
        meta = json.loads(path.read_text(encoding="utf-8"))
        (self.repo / wc.TREE_SOURCES[0]).write_text("// new tree source\n", encoding="utf-8")
        meta["tree_source_fingerprint"] = wc.source_fingerprint(self.repo, wc.TREE_SOURCES)
        path.write_text(json.dumps(meta), encoding="utf-8")
        self.assertEqual(self.tree_check(), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("cache_check")["data"]["reason_code"], "cache_stale")

    def test_sidecar_input_cannot_be_laundered_by_updating_key_metadata(self):
        plain, tree = self.tree_fixture()
        self.assertEqual(self.tree_check(), lane.EXIT_OK)
        source = plain / lane.WORLD_FILE_NAME
        source.write_bytes(source.read_bytes() + b"changed input")
        path = tree / wc.KEY_META_FILE
        meta = json.loads(path.read_text(encoding="utf-8"))
        meta["input_file_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        path.write_text(json.dumps(meta), encoding="utf-8")
        self.assertEqual(self.tree_check(), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("cache_check")["data"]["reason_code"], "cache_stale")

    def prime_tree(self, runner):
        # Injectable OS/process boundaries match the existing prime-cache tests.
        meminfo = self.tmp / "meminfo"
        meminfo.write_text("MemTotal: 33554432 kB\nMemAvailable: 25165824 kB\n", encoding="ascii")
        return lane.prime_auto_tree(
            self.repo, self.binary, self.cache, self.out, "25", runner=runner,
            which=_all_tools, fadvise=self.fadvise, lock=self.lock,
            verify=lambda path: {"ok": True, "problems": []}, meminfo_path=meminfo,
        )

    def test_prime_absent_plain_is_not_run_and_builds_nothing(self):
        self.sources()
        runner = FakeRunner()
        self.assertEqual(self.prime_tree(runner), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("prime_auto_tree")["data"]["reason_code"], "cache_absent")
        self.assertFalse(any("build_auto_tree.py" in " ".join(c) for c in runner.calls))
        self.assertFalse((self.cache / "A-v4_gsq_auto").exists())
        self.assertTrue(self.record("prime_auto_tree")["data"]["builder_ram_estimate"]["exceeds_job_limit"])

    def test_prime_stale_plain_is_not_run_and_builds_nothing(self):
        plain = self.ready_plain()
        before = self.snapshot(plain)
        (self.repo / wc.GENERATOR_SOURCES[0]).write_text("// stale\n", encoding="utf-8")
        runner = FakeRunner()
        self.assertEqual(self.prime_tree(runner), lane.EXIT_NOT_RUN)
        self.assertEqual(self.record("prime_auto_tree")["data"]["reason_code"], "cache_stale")
        self.assertFalse(any("build_auto_tree.py" in " ".join(c) for c in runner.calls))
        self.assertEqual(self.snapshot(plain), before)

    def test_failed_build_preserves_plain_and_publishes_no_tree(self):
        plain = self.ready_plain()
        before = self.snapshot(plain)
        calls = []

        def fail(cmd, **kwargs):
            calls.append(cmd)
            return _proc(cmd, 1)

        runner = FakeRunner(**{"build_auto_tree.py": fail})
        self.assertEqual(self.prime_tree(runner), lane.EXIT_FAIL)
        self.assertEqual(len(calls), 1)
        self.assertEqual(self.snapshot(plain), before)
        self.assertEqual(wc.complete_keys(self.cache, "A-v4_gsq_auto"), [])
        estimate = self.record("prime_auto_tree")["data"]["builder_ram_estimate"]
        self.assertEqual(estimate["writer_payload_bytes_range"], [69_155_364_864, 78_083_364_864])
        self.assertEqual(estimate["full_worker_batch_subtotal_bytes"], 422_464_315_008)
        self.assertIn("builder", estimate["reason"])

    def test_old_tree_keys_are_deleted_before_build_and_plain_is_preserved(self):
        plain, tree = self.tree_fixture()
        before = self.snapshot(plain)

        def fail(cmd, **kwargs):
            self.assertFalse(tree.exists(), "delete old tree before building its replacement")
            return _proc(cmd, 1)

        runner = FakeRunner(**{"build_auto_tree.py": fail})
        self.assertEqual(self.prime_tree(runner), lane.EXIT_FAIL)
        self.assertIn(tree.name, self.record("prime_auto_tree")["data"]["deleted_keys"])
        self.assertEqual(self.snapshot(plain), before)

    def test_successful_plain_reprime_removes_the_derived_tree(self):
        plain, tree = self.tree_fixture()
        self.assertEqual(self.prime("A", self.generator(rc=1), encoding=lane.ENCODING_GSQ), lane.EXIT_FAIL)
        self.assertTrue((tree / wc.COMPLETE_FILE).exists())
        self.assertEqual(self.prime("A", encoding=lane.ENCODING_GSQ), lane.EXIT_OK)
        self.assertFalse(tree.exists())
        self.assertEqual(self.record("prime_cache")["data"]["deleted_auto_tree_keys"], [tree.name])

    def test_redirected_tree_directory_is_refused_without_deleting_plain(self):
        plain = self.ready_plain()
        before = self.snapshot(plain)
        base = self.cache / "A-v4_gsq_auto"
        original = Path.is_symlink
        # Windows need not grant symlink privileges to exercise the refusal.
        with mock.patch.object(Path, "is_symlink", lambda p: p == base or original(p)):
            self.assertEqual(self.prime_tree(FakeRunner()), lane.EXIT_FAIL)
        self.assertIn("redirected tree", self.record("prime_auto_tree")["reason"])
        self.assertEqual(self.snapshot(plain), before)

    def repeat_runner(self, changed=None, *, tier="A"):
        import build_auto_tree as driver
        from test_verify_world_manifest import build_v4_structure_world

        outputs, identities = [], []

        def build(cmd, **kwargs):
            output = Path(self.arg(cmd, "--out"))
            self.assertNotIn(output, outputs, "repeat build must use a different scratch path")
            self.assertFalse((output.parent / wc.COMPLETE_FILE).exists())
            if not outputs and tier == "A":
                self.assertEqual(output.parent, self.cache / "A-v4_gsq_auto" / ".incoming-25")
            if tier == "T0":
                self.assertFalse(output.is_relative_to(self.cache), "T0 must not write a cache key")
            outputs.append(output)
            output.parent.mkdir(parents=True, exist_ok=True)
            # Share the explicitly structural fixture, not a captured producer
            # golden. Both external producer and verifier are doubles here.
            fixture = build_v4_structure_world(output.parent)
            manifest = json.loads(fixture.with_suffix(".manifest.json").read_text(encoding="utf-8"))
            blob = fixture.read_bytes()
            if len(outputs) == 2 and changed == "output_file_sha256":
                blob = blob[:-1] + bytes([blob[-1] ^ 1])
            output.write_bytes(blob)
            manifest["input_file_sha256"] = hashlib.sha256(Path(self.arg(cmd, "--input")).read_bytes()).hexdigest()
            manifest["source_fingerprint"] = wc.source_fingerprint(self.repo, wc.TREE_SOURCES)
            # Honor the submitted job instead of silently using the tiny V4
            # structure fixture's tile limit. Real builder reports include W13
            # and selected_splats; the lane must not accept their absence.
            job = json.loads(Path(self.arg(cmd, "--job-manifest")).read_text(encoding="utf-8"))
            manifest["build_params"]["max_tile_splats"] = job["max_tile_splats"]
            manifest["build_params"]["worker_count"] = job["worker_count"]
            key = {k: manifest[k] for k in ("source_fingerprint", "build_params", "codec", "input_file_sha256")}
            key["build_params"] = {k: v for k, v in key["build_params"].items() if k != "worker_count"}
            manifest["tree_sha"] = hashlib.sha256(json.dumps(key, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            manifest["output_file_sha256"] = hashlib.sha256(blob).hexdigest()
            if len(outputs) == 2 and changed == "content_generation":
                manifest["content_generation"] = str(int(manifest["content_generation"]) + 1)
            output.with_suffix(".manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            report = driver.new_report()
            report.update(status="complete", reason="", wall_time_sec=1.5,
                          peak_rss_bytes={"available": True, "value": 1024, "source": "process_rss"},
                          output_bytes=len(blob), node_count=3, height=1, levels=manifest["levels"],
                          w13={"available": True, "passed": True, "nodes_verified": 3, "node_count": 3,
                               "source": "test double of builder W13"},
                          oracle_complete_all_poses=True, budget_frozen=False,
                          drawn_budget_splats=1_000_000,
                          drawn_budget_source="ADR HLOD arm N_frozen <= 1.0M; BV-26a not merged",
                          oracle={"poses": [{"checkpoint": f"C{i}", "status": "COMPLETE",
                                             "selected_splats": 3, "drawn_le_budget": True} for i in range(8)]},
                          leaves_only={"poses": [{"checkpoint": f"C{i}", "status": "COMPLETE",
                                                  "selected_splats": 2, "drawn_le_budget": True} for i in range(8)]})
            for field in ("tree_sha", "input_file_sha256", "source_fingerprint", "content_generation", "output_file_sha256"):
                report[field] = manifest[field]
            report_path = Path(self.arg(cmd, "--report"))
            report_path.parent.mkdir(parents=True, exist_ok=True)
            report_path.write_text(json.dumps(report), encoding="utf-8")
            identities.append({k: report[k] for k in ("content_generation", "output_file_sha256")})
            return _proc(cmd)

        return FakeRunner(**{"build_auto_tree.py": build}), outputs, identities

    def test_missing_w13_evidence_prevents_publication(self):
        plain = self.ready_plain()
        before = self.snapshot(plain)
        runner, outputs, identities = self.repeat_runner()
        build = runner.overrides["build_auto_tree.py"]

        def without_w13(cmd, **kwargs):
            result = build(cmd, **kwargs)
            path = Path(self.arg(cmd, "--report"))
            report = json.loads(path.read_text(encoding="utf-8"))
            report.pop("w13")
            path.write_text(json.dumps(report), encoding="utf-8")
            return result

        runner.overrides["build_auto_tree.py"] = without_w13
        self.assertEqual(self.prime_tree(runner), lane.EXIT_FAIL)
        self.assertEqual(len(outputs), 1)
        self.assertIn("W13", self.record("prime_auto_tree")["reason"])
        self.assertEqual(wc.complete_keys(self.cache, "A-v4_gsq_auto"), [])
        self.assertEqual(self.snapshot(plain), before)

    def test_t0_generates_plain_gsq_in_scratch_and_records_evidence(self):
        self.ready_plain()
        before = self.snapshot(self.cache)
        runner, outputs, identities = self.repeat_runner(tier="T0")
        runner.overrides[lane.GENERATOR_ENTRY.name] = self.generator().overrides[lane.GENERATOR_ENTRY.name]
        with mock.patch.object(lane, "auto_tree_preflight", return_value=lane.EXIT_NOT_RUN) as tier_a_admission:
            self.assertEqual(lane.auto_tree_t0(self.repo, self.binary, self.cache, self.out, "25", runner,
                                             verify=lambda p: {"ok": True}), lane.EXIT_OK)
        tier_a_admission.assert_not_called()
        generates = [c for c in runner.calls if len(c) > 1 and Path(c[1]).name == lane.GENERATOR_ENTRY.name]
        self.assertEqual(len(generates), 1)
        self.assertEqual(self.arg(generates[0], "--tier"), "T0")
        self.assertEqual(self.arg(generates[0], "--encoding"), "plain_v3_gsq")
        self.assertFalse(Path(self.arg(generates[0], "--out")).exists(), "scratch must be removed")
        self.assertEqual(self.snapshot(self.cache), before)
        self.assertEqual(len(outputs), 1)
        self.assertFalse(outputs[0].exists())
        report = json.loads((self.out / "T0.build.json").read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "OK")
        self.assertEqual(report["wall_time_sec"], 1.5)
        self.assertEqual(report["peak_rss_bytes"]["value"], 1024)
        self.assertEqual(report["content_generation"], identities[0]["content_generation"])
        self.assertEqual(len(report["oracle"]["poses"]), 8)
        self.assertEqual(len(report["leaves_only"]["poses"]), 8)
        self.assertFalse(report["scratch_bytes"]["available"])
        self.assertTrue(report["scratch_bytes"]["reason"])
        self.assertFalse(any(c[0] == "df" for c in runner.calls), "T0 uses its own job limits")

    def test_real_scan_is_not_run_without_bound_in_core_digests(self):
        scans = self.tmp / "real-scans"
        scans.mkdir()
        result = lane.real_scan_identity(self.out, str(scans))
        self.assertEqual(result["status"], "NOT_RUN")
        self.assertEqual(result["reason_code"], "in_core_per_node_digest_api_unavailable")
        self.assertIsNone(result["leaf_count"])
        self.assertIsNone(result["in_core_digests"])
        self.assertTrue(result["scan_dir_present"])

    def test_job_limits_are_design_q4_and_t0_is_smaller(self):
        job, small = lane.auto_tree_job("A"), lane.auto_tree_job("T0")
        self.assertEqual(job["worker_count"], 16)
        self.assertEqual(job["limits"], {"max_ram_bytes": 20 * GIB, "max_scratch_bytes": 12 * GIB,
                                         "max_output_bytes": 12 * GIB, "deadline_sec": 5400})
        self.assertEqual(job["drawn_budget_splats"], 1_000_000)
        self.assertEqual(job["drawn_budget_source"], "ADR HLOD arm N_frozen <= 1.0M; BV-26a not merged")
        for field in job["limits"]:
            self.assertLess(small["limits"][field], job["limits"][field])
        self.assertEqual(small["limits"]["max_ram_bytes"], 16 * GIB)
        self.assertEqual(small["worker_count"], 4)
        self.assertEqual(small["max_tile_splats"], 262144)
        self.assertEqual(small["codec"], "gsq")
        self.assertNotIn("source_fingerprint", job)

    def test_repeat_identity_match_publishes_key_and_removes_scratch(self):
        plain = self.ready_plain()
        before = self.snapshot(plain)
        runner, outputs, identities = self.repeat_runner()
        self.assertEqual(self.prime_tree(runner), lane.EXIT_OK)
        self.assertEqual(len(outputs), 2)
        keys = wc.complete_keys(self.cache, "A-v4_gsq_auto")
        self.assertEqual(len(keys), 1)
        meta = wc.read_key_meta(keys[0])
        self.assertEqual(keys[0].name, f"{GEN_SHA[:12]}-{meta['tree_sha'][:12]}")
        self.assertEqual(meta["gen_sha"], GEN_SHA)
        self.assertEqual(meta["input_file_sha256"], hashlib.sha256((plain / lane.WORLD_FILE_NAME).read_bytes()).hexdigest())
        self.assertEqual(meta["tree_source_fingerprint"], wc.source_fingerprint(self.repo, wc.TREE_SOURCES))
        self.assertEqual(meta["source_fingerprint"], wc.source_fingerprint(self.repo))
        self.assertEqual(meta["run_id"], "25")
        repeat = self.record("prime_auto_tree")["data"]["repeat_identity"]
        self.assertEqual(repeat["status"], "OK")
        self.assertEqual(repeat["first"], identities[0])
        self.assertEqual(repeat["second"], identities[1])
        self.assertFalse(outputs[1].exists())
        self.assertEqual(self.snapshot(plain), before)

    def test_repeat_identity_mismatch_fails_for_either_identity_field(self):
        for changed in ("content_generation", "output_file_sha256"):
            with self.subTest(changed=changed):
                plain = self.ready_plain()
                before = self.snapshot(plain)
                runner, outputs, identities = self.repeat_runner(changed)
                self.assertEqual(self.prime_tree(runner), lane.EXIT_FAIL)
                self.assertEqual(len(outputs), 2, "a failure before the repeat comparison is not evidence")
                result = self.record("prime_auto_tree")["data"]["repeat_identity"]
                self.assertEqual(result["status"], "FAIL")
                self.assertEqual(result["reason_code"], "repeat_identity_mismatch")
                self.assertEqual(result["first"], identities[0])
                self.assertEqual(result["second"], identities[1])
                self.assertEqual(wc.complete_keys(self.cache, "A-v4_gsq_auto"), [])
                self.assertFalse(outputs[1].exists())
                self.assertEqual(self.snapshot(plain), before)

    def test_prime_subcommand_and_cache_check_flag_are_wired(self):
        with mock.patch.object(lane, "prime_auto_tree", return_value=lane.EXIT_NOT_RUN) as prime:
            self.assertEqual(lane.main(["prime-auto-tree", "--repo", str(self.repo),
                                       "--godot-binary", str(self.binary), "--cache-root", str(self.cache),
                                       "--out", str(self.out), "--run-id", "25"]), lane.EXIT_NOT_RUN)
        prime.assert_called_once()
        with mock.patch.object(lane, "cache_check", return_value=lane.EXIT_NOT_RUN) as check:
            self.assertEqual(lane.main(["cache-check", "--auto-tree", "--tier", "A",
                                       "--cache-root", str(self.cache), "--out", str(self.out)]), lane.EXIT_NOT_RUN)
        self.assertIs(check.call_args.kwargs["auto_tree"], True)


class D25AutoTreeWorkflow(unittest.TestCase):
    def setUp(self):
        self.text = WORKFLOW.read_text(encoding="utf-8")

    def enabled(self, condition, *, mode="auto_tree", fork=False):
        """Evaluate only the boolean/comparison subset used by these job guards."""
        values = {"github.event_name": "pull_request" if fork else "workflow_dispatch",
                  "github.event.schedule": "", "github.repository": "owner/repo",
                  "github.event.pull_request.head.repo.full_name": "fork/repo" if fork else "owner/repo",
                  "inputs.mode": mode, "inputs.prime_world_cache": True,
                  "needs.build-linux-editor.result": "success",
                  "needs.prime-world-cache.result": "skipped"}
        expression = condition.removeprefix("${{").removesuffix("}}").strip()
        for key in sorted(values, key=len, reverse=True):
            expression = expression.replace(key, repr(values[key]))
        expression = expression.replace("cancelled()", "False")
        expression = expression.replace("&&", " and ").replace("||", " or ")
        expression = re.sub(r"!(?!=)", " not ", expression)
        expression = re.sub(r"\btrue\b", "True", expression)
        expression = re.sub(r"\bfalse\b", "False", expression)
        parsed = ast.parse(expression.strip(), mode="eval")
        allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.UnaryOp, ast.Not,
                   ast.Compare, ast.Eq, ast.NotEq, ast.Constant)
        self.assertTrue(all(isinstance(node, allowed) for node in ast.walk(parsed)), condition)
        return bool(eval(compile(parsed, "<workflow condition>", "eval"), {"__builtins__": {}}))

    def test_dispatch_mode_and_distinct_run_name_and_concurrency(self):
        mode = self.text.split("      mode:\n", 1)[1].split("      tier:", 1)[0]
        self.assertIn('          - "auto_tree"', mode)
        for line in self.text.splitlines():
            if line.startswith(("run-name:", "  group: bvw-")):
                self.assertIn("inputs.mode == 'auto_tree'", line)

    def test_only_build_and_auto_tree_job_run_in_auto_tree_mode(self):
        job_names = re.findall(r"^  ([a-z][a-z0-9-]*):$", self.text.split("\njobs:\n", 1)[1], re.M)
        self.assertIn("prime-auto-tree", job_names)
        enabled = {name for name in job_names
                   if self.enabled(WorkflowContract.job_if(_job_block(self.text, name)))}
        self.assertEqual(enabled, {"build-linux-editor", "prime-auto-tree"})
        for name in job_names:
            condition = WorkflowContract.job_if(_job_block(self.text, name))
            if name == "prime-auto-tree":
                self.assertIn("github.event_name == 'workflow_dispatch'", condition)
                self.assertIn("inputs.mode == 'auto_tree'", condition)
                self.assertIn("github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository", condition)
                self.assertFalse(self.enabled(condition, fork=True))
                self.assertFalse(self.enabled(condition, mode="flythrough"))
                self.assertFalse(self.enabled(condition, mode="watchdog"))
            elif name == "build-linux-editor":
                self.assertNotIn("inputs.mode != 'auto_tree'", condition)
            elif name == "liveness-watchdog":
                self.assertEqual(condition, f"${{{{ github.event.schedule == '{WATCHDOG_CRON}' || inputs.mode == 'watchdog' }}}}")
            else:
                self.assertIn("inputs.mode != 'auto_tree'", condition, name)

    def test_auto_tree_runner_binary_handoff_and_prepostflight(self):
        job = _job_block(self.text, "prime-auto-tree")
        self.assertIn("    runs-on: [self-hosted, Linux, X64, gpu, rtx3080]", job)
        self.assertIn("    needs: build-linux-editor", job)
        timeout = next(line for line in job if line.startswith("    timeout-minutes:"))
        self.assertGreater(int(timeout.split(":", 1)[1]), 180, "two 90 minute builds plus T0 and overhead")
        steps = _steps(job)
        runs = "\n".join(s.get("run", "") for s in steps)
        for required in ("linux_gpu_runner.sh install-binary", "runner_gpu_contention.py preflight",
                         "preflight_runner_gpu_environment.py", "runner_gpu_contention.py postflight"):
            self.assertIn(required, runs)
        self.assertIn("name: godot-linuxbsd-editor-tests-${{ github.sha }}", "\n".join(job))
        post = next(s for s in steps if "runner_gpu_contention.py postflight" in s.get("run", ""))
        self.assertEqual(post.get("if"), "always()")

    def test_auto_tree_step_order_gates_builds_and_uploads_reports_only(self):
        steps = _steps(_job_block(self.text, "prime-auto-tree"))
        indices = []
        for command in ("auto-tree-t0", "auto-tree-preflight", "prime-auto-tree", "cache-check"):
            hits = [i for i, s in enumerate(steps)
                    if re.search(rf"{re.escape(_LANE)} {command}\b", s.get("run", ""))]
            self.assertEqual(len(hits), 1, command)
            indices.extend(hits)
        self.assertEqual(indices, sorted(indices))
        self.assertIn("--auto-tree", steps[indices[-1]]["run"])
        for index in indices:
            self.assertIsNone(steps[index].get("if"), "default success() gates the ordered steps")
        self.assertNotIn("continue-on-error:", "\n".join(_job_block(self.text, "prime-auto-tree")))
        uploads = [i for i, s in enumerate(steps) if s.get("uses", "").startswith("actions/upload-artifact@")]
        self.assertTrue(uploads)
        self.assertGreater(min(uploads), indices[-1])
        for index in uploads:
            self.assertEqual(steps[index].get("if"), "always()")
        block = "\n".join(_job_block(self.text, "prime-auto-tree"))
        upload_blocks = re.findall(r"uses: actions/upload-artifact@[^\n]+\n(.*?)(?=\n      - |\Z)", block, re.S)
        for upload in upload_blocks:
            self.assertNotIn("BVW_CACHE_ROOT", upload)
            self.assertNotIn(".gsplatworld", upload)
            self.assertNotIn(".incoming-", upload)
            path_field = re.search(r"^          path: (.+)(?:\n((?:            .+\n?)*))?", upload, re.M)
            self.assertIsNotNone(path_field, "upload must explicitly select report files")
            paths = (path_field[2] or "").splitlines() if path_field[1] == "|" else [path_field[1]]
            self.assertTrue(paths)
            for path in paths:
                path = path.strip().strip('"\'')
                self.assertTrue(path.startswith("${{ runner.temp }}/bvw-auto-tree-out/"), path)
                self.assertTrue(path.endswith((".json", ".log")), "upload reports only: " + path)
                self.assertNotIn("..", path)

    def test_auto_tree_contention_path_is_exported_before_preflight(self):
        job = _job_block(self.text, "prime-auto-tree")
        steps = _steps(job)
        paths = next(i for i, step in enumerate(steps) if step.get("name") == "Set lane paths")
        self.assertIn(
            'echo "GS_CI_GPU_CONTENTION_DIR=$RUNNER_TEMP/bvw-auto-tree-out/contention" >> "$GITHUB_ENV"',
            steps[paths]["run"],
        )
        # Both phases inherit the same exported directory, which is uploaded.
        self.assertEqual("\n".join(job).count("GS_CI_GPU_CONTENTION_DIR"), 1)
        pre = next(i for i, step in enumerate(steps)
                   if "runner_gpu_contention.py preflight" in step.get("run", ""))
        post = next(i for i, step in enumerate(steps)
                    if "runner_gpu_contention.py postflight" in step.get("run", ""))
        self.assertLess(paths, pre)
        self.assertLess(pre, post)


if __name__ == "__main__":
    unittest.main()
