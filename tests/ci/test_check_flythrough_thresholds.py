#!/usr/bin/env python3
"""Self-tests of the beyond-VRAM thresholds guard (BV-09, ADR section 8.4).

Every ADR mutation proof is a case here, against the committed thresholds
file: raise a value, delete a key, demote a class, change an arm, edit the
drawn budget down without a waiver, use a bogus base. The wiring mutation
(delete the guard from `run_module_tests.py`) is pinned by
`tests/runtime/flythrough/test_flythrough_verdict.py`, which a different
wire runs. The legal routes are tested too (tighten, promote, raise the drawn
budget, re-prime with a record, waive with G11q and Q-codec hard), so the
guard cannot pass these cases by refusing every edit.

The base comparison runs through real git: a single-commit fixture cannot tell
HEAD from the base (`docs/governance/evidence-integrity.md`, "fixture cannot
represent the property"), so `GitBase` builds a two-commit repository.

Run directly or through ``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_flythrough_thresholds", ROOT / "tests" / "ci" / "check_flythrough_thresholds.py")
guard = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(guard)

COMMITTED = json.loads((ROOT / guard.THRESHOLDS_REL).read_text(encoding="utf-8"))


class _Case(unittest.TestCase):
    def setUp(self) -> None:
        self.base = copy.deepcopy(COMMITTED)
        self.head = copy.deepcopy(COMMITTED)

    def fired(self, *needles: str) -> list:
        problems = guard.compare(self.base, self.head)
        self.assertTrue(problems, "the guard stayed green")
        for needle in needles:
            self.assertTrue(any(needle in p for p in problems), f"{needle!r} not in {problems}")
        return problems

    def clean(self) -> None:
        self.assertEqual(guard.compare(self.base, self.head), [])
        self.assertEqual(guard.validate(self.head), [])


class CommittedFile(_Case):
    def test_the_committed_file_is_valid_and_equal_to_itself(self) -> None:
        self.clean()

    def test_every_entry_is_report_only_until_bv14(self) -> None:
        modes = {e["mode"] for s in guard.SECTIONS for e in COMMITTED[s].values()}
        self.assertEqual(modes, {"report_only"})


class AdrMutations(_Case):
    """ADR 8.4: 'Each must turn the guard red.'"""

    def test_raise_a_value(self) -> None:
        self.head["gates"]["G4"]["tiers"]["A"] = 12.5
        self.fired("gates.G4.tiers.A", "loosens")

    def test_lower_a_floor(self) -> None:
        self.head["validity"]["V9-evictions"]["tiers"]["A"] = 999
        self.fired("V9-evictions.tiers.A")

    def test_change_an_equality_value(self) -> None:
        self.head["validity"]["V5"]["tiers"]["A"] = 1
        self.fired("V5.tiers.A")

    def test_delete_a_key(self) -> None:
        del self.head["gates"]["G6"]
        self.fired("gates.G6: removed")
        self.setUp()
        del self.head["gates"]["G6"]["tiers"]["AB"]
        self.fired("gates.G6.tiers.AB: removed")
        self.setUp()
        del self.head["gates"]["G6"]["window"]
        self.fired("gates.G6.window: removed")
        self.setUp()
        del self.head["globals"]["settings"]["visible_cap"]
        self.fired("settings.visible_cap: removed")

    def test_demote_a_class_or_a_mode(self) -> None:
        self.head["gates"]["G4"]["class"] = "statistical"
        self.fired("class 'correctness' -> 'statistical'")
        self.setUp()
        self.base["gates"]["G4"]["mode"] = "hard"
        self.head["gates"]["G4"]["mode"] = "report_only"
        self.fired("hard -> 'report_only'")

    def test_change_an_arm(self) -> None:
        self.head["gates"]["G2a"]["arm"] = "hlod"
        self.fired("gates.G2a.arm")

    def test_change_op_window_exclusions_method_or_source(self) -> None:
        for key, value in (("op", ">="), ("window", "x"), ("exclusions", "x"), ("percentile_method", "p50"),
                           ("source_field", "x"), ("checkpoints", ["C1"])):
            self.setUp()
            self.head["gates"]["G6"][key] = value
            self.fired(f"gates.G6.{key}")

    def test_drawn_budget_down_without_a_waiver(self) -> None:
        self.base["globals"]["settings"]["drawn_budget_splats"] = 8_000_000
        self.head["globals"]["settings"]["drawn_budget_splats"] = 6_000_000
        self.fired("drawn_budget_splats", "reduces work")

    def test_drawn_budget_down_with_a_waiver_but_g11q_not_hard(self) -> None:
        self.base["globals"]["settings"]["drawn_budget_splats"] = 8_000_000
        self.head["globals"]["settings"]["drawn_budget_splats"] = 6_000_000
        self.head["waivers"].append({"setting": "settings.drawn_budget_splats", "maintainer": "m", "date": "2026-11-08",
                                     "reason": "r", "evidence_url": "https://example.invalid/run"})
        self.fired("waiver present, gates not hard")

    def test_min_vram_ratio_lowered(self) -> None:
        self.head["globals"]["min_vram_ratio"] = 2.4
        self.fired("min_vram_ratio")

    def test_world_digest_changed_without_reprime(self) -> None:
        self.head["globals"]["worlds"]["A"]["path_sha256"] = "a" * 64
        self.fired("globals.worlds.A")
        self.setUp()
        self.head["globals"]["worlds"]["T0"]["generator_params_sha"] = "b" * 64
        self.fired("globals.worlds.T0")

    def test_frozen_setting_edited(self) -> None:
        key = "rendering/gaussian_splatting/streaming/max_chunks_in_vram"
        self.head["globals"]["settings"]["tuned"][key] = 512
        self.fired("frozen")
        self.setUp()
        self.head["globals"]["settings"]["far_m"] = 600.0
        self.fired("settings.far_m")

    def test_codec_bits_down(self) -> None:
        self.base["globals"]["settings"]["codec"]["sh_bits"] = [8, 6, 4]
        self.head["globals"]["settings"]["codec"]["sh_bits"] = [8, 6, 3]
        self.fired("codec.sh_bits[2]")

    def test_device_binary_and_history(self) -> None:
        self.head["globals"]["devices"]["rtx3080"]["vram_bytes"] = 12 * 1024**3
        self.fired("devices.rtx3080")
        self.setUp()
        self.head["globals"]["binary"]["required_scons_args"].remove("dev_build=no")
        self.fired("binary.required_scons_args")
        self.setUp()
        record = {"world": "A", "run_id": "1", "date": "d", "calibration_record": "c"}
        self.base["reprime_records"].append(record)
        self.fired("reprime_records")
        self.setUp()
        self.base["waivers"].append({"setting": "s"})
        self.fired("waivers")

    def test_non_finite_values(self) -> None:
        with self.assertRaises(guard.GuardError):
            guard.parse('{"x": NaN}', "t")
        with self.assertRaises(guard.GuardError):
            guard.parse('{"x": Infinity}', "t")
        self.head["gates"]["G4"]["tiers"]["A"] = float("inf")
        self.assertTrue(any("non-finite" in p for p in guard.validate(self.head)))

    def test_standalone_validation(self) -> None:
        self.head["gates"]["G4"]["op"] = "<"
        self.assertTrue(guard.validate(self.head))
        self.setUp()
        self.head["provenance"]["source_run"]["run_id"] = ""
        self.assertTrue(any("run_id" in p for p in guard.validate(self.head)))
        self.setUp()
        self.head["validity"]["V1"]["class"] = "correctness"
        self.assertTrue(guard.validate(self.head))


class LegalRoutes(_Case):
    def test_tighten_values(self) -> None:
        self.head["gates"]["G6"]["tiers"]["A"] = 2.5
        self.head["validity"]["V9-evictions"]["tiers"]["A"] = 2000
        self.head["globals"]["min_vram_ratio"] = 3.0
        self.clean()

    def test_promote(self) -> None:
        self.head["gates"]["G4"]["mode"] = "hard"
        self.head["gates"]["G6"]["class"] = "correctness"
        self.clean()

    def test_add_entries_tiers_and_settings(self) -> None:
        self.head["gates"]["G15"] = dict(self.head["gates"]["G6"], tiers={"A": 1.0})
        self.head["gates"]["G4"]["tiers"]["T9"] = 1.0
        self.head["globals"]["settings"]["drawn_budget_splats"] = 8_000_000
        self.clean()

    def test_more_work_needs_no_waiver(self) -> None:
        self.base["globals"]["settings"]["drawn_budget_splats"] = 6_000_000
        self.head["globals"]["settings"]["drawn_budget_splats"] = 8_000_000
        self.base["globals"]["settings"]["tau"] = 2.0
        self.head["globals"]["settings"]["tau"] = 1.5
        self.clean()

    def test_waived_work_reduction_with_hard_g11q_and_q_codec(self) -> None:
        for gid in guard.WAIVER_GATES:
            entry = dict(self.base["gates"]["G6"], mode="hard")
            self.base["gates"][gid] = entry
            self.head["gates"][gid] = dict(entry)
        self.base["globals"]["settings"]["drawn_budget_splats"] = 8_000_000
        self.head["globals"]["settings"]["drawn_budget_splats"] = 6_000_000
        self.head["waivers"].append({"setting": "settings.drawn_budget_splats", "maintainer": "m", "date": "2026-11-08",
                                     "reason": "r", "evidence_url": "https://example.invalid/run"})
        self.clean()

    def test_reprime_with_a_record(self) -> None:
        self.head["globals"]["worlds"]["A"]["file_sha256"] = "c" * 64
        self.head["globals"]["worlds"]["A"]["gen_sha"] = "d" * 64
        self.head["reprime_records"].append({"world": "A", "file_sha256": "c" * 64, "gen_sha": "d" * 64,
                                             "run_id": "123", "date": "2026-11-08", "calibration_record": "G6, G6t: run 123"})
        self.clean()
        self.head["reprime_records"][-1]["file_sha256"] = "e" * 64
        self.fired("globals.worlds.A")


def _git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, text=True).stdout.strip()


class GitBase(unittest.TestCase):
    """The base comes from git history, never from HEAD; a bad base cannot run."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        _git(self.repo, "init", "-q")
        _git(self.repo, "config", "user.email", "t@example.invalid")
        _git(self.repo, "config", "user.name", "t")
        _git(self.repo, "config", "commit.gpgsign", "false")
        (self.repo / "README").write_text("x\n", encoding="utf-8")
        _git(self.repo, "add", "README")
        _git(self.repo, "commit", "-q", "-m", "before the thresholds file")
        self.pre = _git(self.repo, "rev-parse", "HEAD")
        self.path = self.repo / guard.THRESHOLDS_REL
        self.path.parent.mkdir(parents=True)
        self.write(COMMITTED)
        _git(self.repo, "add", guard.THRESHOLDS_REL)
        _git(self.repo, "commit", "-q", "-m", "thresholds")
        self.base = _git(self.repo, "rev-parse", "HEAD")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, doc: dict) -> None:
        self.path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")

    def run_guard(self, base_ref: str) -> tuple:
        try:
            return guard.run(self.repo, base_ref)
        except guard.GuardError as exc:
            return guard.EXIT_CANNOT_RUN, [str(exc)]

    def test_a_loosened_head_against_a_real_base(self) -> None:
        doc = copy.deepcopy(COMMITTED)
        doc["gates"]["G4"]["tiers"]["A"] = 13.0
        self.write(doc)
        _git(self.repo, "commit", "-q", "-am", "loosen")
        code, lines = self.run_guard(self.base)
        self.assertEqual(code, guard.EXIT_FIRED, lines)
        # Against itself the same change would be invisible: that is why HEAD is never the base.
        code, lines = self.run_guard("HEAD")
        self.assertEqual(code, guard.EXIT_CLEAN, lines)
        self.assertTrue(any("informational" in line for line in lines), lines)

    def test_a_bogus_base_cannot_run(self) -> None:
        self.assertEqual(guard.main(["--repo", str(self.repo), "--base-ref", "0" * 40]), guard.EXIT_CANNOT_RUN)
        self.assertEqual(guard.main(["--repo", str(self.repo), "--base-ref", "no-such-branch"]), guard.EXIT_CANNOT_RUN)

    def test_the_introducing_change_is_validated_on_its_own(self) -> None:
        code, lines = self.run_guard(self.pre)
        self.assertEqual(code, guard.EXIT_CLEAN, lines)
        self.assertTrue(any("introduced here" in line for line in lines), lines)
        doc = copy.deepcopy(COMMITTED)
        doc["gates"]["G4"]["op"] = "<"
        self.write(doc)
        self.assertEqual(self.run_guard(self.pre)[0], guard.EXIT_FIRED)

    def test_a_deleted_file_cannot_pass(self) -> None:
        self.path.unlink()
        self.assertEqual(guard.main(["--repo", str(self.repo), "--base-ref", self.base]), guard.EXIT_CANNOT_RUN)

    def test_unparsable_head(self) -> None:
        self.path.write_text('{"schema": NaN}', encoding="utf-8")
        self.assertEqual(guard.main(["--repo", str(self.repo), "--base-ref", self.base]), guard.EXIT_CANNOT_RUN)


if __name__ == "__main__":
    unittest.main()
