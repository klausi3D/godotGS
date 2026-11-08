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

import io
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

    def generator(self, rc: int = 0, gen_sha: Optional[str] = GEN_SHA) -> FakeRunner:
        def run(cmd, **kw):
            out = Path(self.arg(cmd, "--out"))
            out.mkdir(parents=True, exist_ok=True)
            (out / lane.WORLD_FILE_NAME).write_bytes(b"\1" * 4096)
            manifest = {"schema": "godotgs-world-manifest/1"}
            if gen_sha is not None:
                manifest["gen_sha"] = gen_sha
            (out / lane.MANIFEST_NAME).write_text(json.dumps(manifest), encoding="utf-8")
            return _proc(cmd, rc)

        return FakeRunner(**{lane.GENERATOR_ENTRY.name: run})

    def prime(self, tier: str = "T0", runner: Optional[FakeRunner] = None, run_id: str = "11") -> int:
        self.install(lane.GENERATOR_ENTRY)
        return lane.prime_cache(
            self.repo, self.binary, tier, self.cache, self.out, run_id,
            runner or self.generator(), _all_tools, self.fadvise, self.lock,
        )

    def key(self, world: str = "T0") -> Path:
        return self.cache / wc.world_id(world, lane.ENCODING) / GEN_SHA[:12]

    def check(self, tier: str = "T0", ok: bool = True) -> int:
        def verify(world_file: Path) -> dict:
            self.assertTrue(world_file.is_file(), world_file)
            return {"ok": ok, "problems": [] if ok else ["chunk_sha_mismatch: 3"]}

        return lane.cache_check(self.repo, tier, self.cache, self.out, "12", self.fadvise, verify, self.lock)


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

    def prober(self) -> FakeRunner:
        def run(cmd, **kw):
            (Path(self.arg(cmd, "--out")) / lane.PROBE_SUMMARY_NAME).write_text("{}", encoding="utf-8")
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
        self.assertEqual(cache["v10"]["read_bytes"], 5120)
        self.assertEqual(cache["v10"]["status"], "NOT_EVALUATED")
        self.assertFalse(cache["cold_proven"], "V10 not evaluated is never proven cold")

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
        self.assertIn(data["host_cache"], ("present", "absent"))
        self.assertEqual(data["host_cache"], wc.host_cache_label(data["reread_over_first_read"]))

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
        cls.gpu = _job_block(cls.text, "flythrough")
        cls.codec = _job_block(cls.text, "codec-probe")
        cls.watchdog = _job_block(cls.text, "liveness-watchdog")
        cls.steps = _steps(cls.gpu)
        cls.prime_steps = _steps(cls.prime)
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
            "inputs.mode == 'watchdog') && 'watchdog' || 'lane' }}\n  cancel-in-progress: false\n",
            self.text,
        )
        # actions: read on the watchdog job only (ADR section 9.1).
        self.assertEqual(len(re.findall(r"^ +actions: read$", self.text, re.M)), 1)
        self.assertIn("      actions: read", self.watchdog)

    def test_nothing_may_swallow_a_failure(self) -> None:
        self.assertNotIn("continue-on-error", self.text)

    def test_runner_routing(self) -> None:
        self.assertIn("    runs-on: [self-hosted, Linux, X64]", self.build)
        # The cache lives on the 3080's disk: both jobs that touch it are pinned to it.
        for job in (self.prime, self.gpu):
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
        for steps in (self.steps, self.prime_steps):
            self.assertIn('--scons-args "$BVW_SCONS_ARGS"', steps[self.index_of("build-info", steps)]["run"])

    def test_the_binary_handoff_is_pinned(self) -> None:
        build_runs = "".join(s.get("run", "") for s in _steps(self.build))
        self.assertIn("linux_gpu_runner.sh stage-binary", build_runs)
        for steps in (self.steps, self.prime_steps):
            self.assertIn("linux_gpu_runner.sh install-binary", "".join(s.get("run", "") for s in steps))
        # build upload; prime, flythrough and codec-probe download
        self.assertEqual(self.text.count("name: godot-linuxbsd-editor-tests-${{ github.sha }}"), 4)

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

    def test_the_probe_runs_only_on_a_checked_cache(self) -> None:
        probe = self.index_of("probe")
        self.assertGreater(probe, self.index_of("cache-check"))
        self.assertTrue(self._reaches(probe, "cache_check"))
        self.assertTrue(self._reaches(probe, "build_info"))
        self.assertIn(f'--cache-worlds "$BVW_OUT/{lane.CACHE_WORLDS_FILE}"', self.steps[probe]["run"])

    def test_preflights_precede_the_lane(self) -> None:
        for steps, first in ((self.steps, "disk-preflight"), (self.prime_steps, "build-info")):
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
        self.assertEqual(len(uploads), 2)
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

if __name__ == "__main__":
    unittest.main()
