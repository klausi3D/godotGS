#!/usr/bin/env python3
"""Unit test for the benchmark fixture contract (#669).

The defect this pins: a benchmark lane whose splat fixture was absent did not
fail. It instantiated zero splat nodes, measured an empty scene, and reported a
*flattering* number (~16k FPS headless / ~2400 FPS windowed, score 95) together
with a passing recommendation. The failure did not look like a failure, so a
contributor reproducing published figures on a fresh clone could reasonably
conclude the published numbers were conservative.

`tests/examples/godot/test_project/tests/fixtures/test_splats.ply` is gitignored
(`.gitignore:441`) and generated, so "absent" is the DEFAULT state of a clean
checkout rather than an exotic one. Most lanes resolve to it.

The same check covers the second half of the defect: `prepare_synthetic_assets.py`
without `--godot-binary` falls back to Python generators that write that fixture
with 1024 splats instead of the canonical 10000, so even a contributor following
the docs benchmarked a 10x-smaller workload with nothing reporting the gap.

These cases pin the properties the guard's value depends on:

* an absent fixture fails, and the message names the asset and the prep command;
* an undersized fixture fails and reports both the actual and required counts;
* a fixture whose count cannot be read fails CLOSED rather than being assumed
  adequate;
* a satisfying fixture passes (so the guard still discriminates, rather than
  failing everything and being disabled).

The last case matters: this repo's dominant test failure mode is a green test
that asserts nothing, and a guard that rejects every input is the same bug wearing
a different hat.
"""

from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIME_DIR = ROOT / "tests" / "runtime"


def _load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


# run_benchmark.py imports its sibling helper by name.
sys.path.insert(0, str(RUNTIME_DIR))
_run_benchmark = _load_module("_gs_run_benchmark", RUNTIME_DIR / "run_benchmark.py")
_manifest_mod = _load_module(
    "_gs_benchmark_asset_manifest", RUNTIME_DIR / "benchmark_asset_manifest.py"
)
_prepare = _load_module(
    "_gs_prepare_synthetic_assets", RUNTIME_DIR / "prepare_synthetic_assets.py"
)

read_ply_vertex_count = _run_benchmark.read_ply_vertex_count
evaluate_fixture_contract = _run_benchmark.evaluate_fixture_contract
PLY_PREP_COMMAND = _run_benchmark.PLY_PREP_COMMAND

TEST_SPLATS_ASSET = "res://tests/fixtures/test_splats.ply"


def _write_ply(path: Path, vertex_count: int, *, header_only: bool = False) -> None:
    """Write a minimal binary PLY with a declared vertex count."""
    header = (
        "ply\n"
        "format binary_little_endian 1.0\n"
        f"element vertex {vertex_count}\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "end_header\n"
    ).encode("ascii")
    body = b"" if header_only else struct.pack("<3f", 0.0, 0.0, 0.0) * vertex_count
    path.write_bytes(header + body)


class ReadPlyVertexCountTests(unittest.TestCase):
    def test_reads_declared_vertex_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "fixture.ply"
            _write_ply(ply, 10000, header_only=True)
            self.assertEqual(read_ply_vertex_count(ply), 10000)

    def test_missing_file_returns_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(read_ply_vertex_count(Path(tmp) / "absent.ply"))

    def test_non_ply_returns_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            junk = Path(tmp) / "fixture.ply"
            junk.write_bytes(b"this is not a PLY file\n")
            self.assertIsNone(read_ply_vertex_count(junk))

    def test_truncated_header_returns_none(self):
        """A fixture truncated mid-header has no readable count."""
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "fixture.ply"
            ply.write_bytes(b"ply\nformat binary_little_endian 1.0\n")
            self.assertIsNone(read_ply_vertex_count(ply))


class FixtureContractTests(unittest.TestCase):
    """The guard proper: does a lane refuse to benchmark the wrong thing?"""

    def test_absent_fixture_fails_and_names_asset_and_prep_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            failure = evaluate_fixture_contract(
                lane_id="static_baseline",
                asset_path=TEST_SPLATS_ASSET,
                asset_file=Path(tmp) / "test_splats.ply",
                required_splats=10000,
            )
        self.assertTrue(failure, "an absent fixture must fail the lane")
        self.assertIn("MISSING", failure)
        # Naming the asset and the remedy is the point: the fixture is gitignored,
        # so a fresh clone hits this and must be told how to fix it.
        self.assertIn(TEST_SPLATS_ASSET, failure)
        self.assertIn(PLY_PREP_COMMAND, failure)
        self.assertIn("static_baseline", failure)

    def test_undersized_fixture_fails_with_both_counts(self):
        """The 1024-vs-10000 Python-fallback case from #669."""
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "test_splats.ply"
            _write_ply(ply, 1024, header_only=True)
            failure = evaluate_fixture_contract(
                lane_id="static_baseline",
                asset_path=TEST_SPLATS_ASSET,
                asset_file=ply,
                required_splats=10000,
            )
        self.assertTrue(failure, "an undersized fixture must fail the lane")
        self.assertIn("UNDERSIZED", failure)
        self.assertIn("1024", failure)
        self.assertIn("10000", failure)
        self.assertIn(PLY_PREP_COMMAND, failure)

    def test_unreadable_fixture_fails_closed(self):
        """A fixture whose count cannot be read is rejected, not assumed adequate."""
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "test_splats.ply"
            ply.write_bytes(b"not a ply at all")
            failure = evaluate_fixture_contract(
                lane_id="static_baseline",
                asset_path=TEST_SPLATS_ASSET,
                asset_file=ply,
                required_splats=10000,
            )
        self.assertTrue(failure, "an unverifiable fixture must fail closed")
        self.assertIn("UNVERIFIABLE", failure)

    def test_satisfying_fixture_passes(self):
        """The guard must still discriminate; rejecting everything is the same bug."""
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "test_splats.ply"
            _write_ply(ply, 10000, header_only=True)
            failure = evaluate_fixture_contract(
                lane_id="static_baseline",
                asset_path=TEST_SPLATS_ASSET,
                asset_file=ply,
                required_splats=10000,
            )
        self.assertEqual(failure, "", f"a satisfying fixture must pass, got: {failure}")

    def test_oversized_fixture_passes(self):
        """The contract is a floor, not an equality: a richer fixture is fine."""
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "test_splats.ply"
            _write_ply(ply, 50000, header_only=True)
            failure = evaluate_fixture_contract(
                lane_id="static_baseline",
                asset_path=TEST_SPLATS_ASSET,
                asset_file=ply,
                required_splats=10000,
            )
        self.assertEqual(failure, "")

    def test_undeclared_asset_only_checks_existence(self):
        """Assets with no declared floor still must exist, but carry no size rule."""
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "unknown.ply"
            _write_ply(ply, 1, header_only=True)
            self.assertEqual(
                evaluate_fixture_contract(
                    lane_id="some_lane",
                    asset_path="res://tests/fixtures/unknown.ply",
                    asset_file=ply,
                    required_splats=0,
                ),
                "",
            )
            self.assertIn(
                "MISSING",
                evaluate_fixture_contract(
                    lane_id="some_lane",
                    asset_path="res://tests/fixtures/unknown.ply",
                    asset_file=Path(tmp) / "absent.ply",
                    required_splats=0,
                ),
            )


class ManifestContractTests(unittest.TestCase):
    """The declared floors must actually reach the harness and be satisfiable."""

    def test_test_splats_floor_matches_cpp_generator(self):
        """test_splats.ply's floor is the canonical C++ [GeneratePLY] count.

        The C++ generator is the only producer of the published benchmark
        workload; the Python fallback writes 1024. If someone lowers this floor
        to 1024 to make a fallback fixture pass, the guard stops guarding.
        """
        self.assertEqual(
            _prepare.ASSET_MIN_SPLAT_COUNTS[TEST_SPLATS_ASSET],
            10000,
            "test_splats.ply floor must match generate_synthetic_ply_fixtures.h",
        )

    def test_committed_fixture_floors_are_satisfied_by_committed_fixtures(self):
        """A clean checkout must pass.

        The synthetic_*.ply fixtures ARE committed, at the Python-fallback sizes.
        Their floors are sourced from those sizes, so this pins that the contract
        does not fail a fresh clone (which would get the guard disabled).
        """
        project_fixtures = (
            ROOT / "tests" / "examples" / "godot" / "test_project" / "tests" / "fixtures"
        )
        checked = 0
        for asset_path, floor in _prepare.ASSET_MIN_SPLAT_COUNTS.items():
            if asset_path == TEST_SPLATS_ASSET:
                continue  # gitignored; never present in a clean checkout
            fixture = project_fixtures / Path(asset_path).name
            if not fixture.is_file():
                continue
            actual = read_ply_vertex_count(fixture)
            self.assertIsNotNone(actual, f"committed fixture unreadable: {fixture}")
            self.assertGreaterEqual(
                actual,
                floor,
                f"committed fixture {fixture.name} has {actual} splats but the "
                f"manifest declares a floor of {floor}; a clean checkout would fail",
            )
            checked += 1
        self.assertGreater(
            checked, 0, "no committed fixtures were checked - this test asserted nothing"
        )

    def test_manifest_round_trips_the_declared_floors(self):
        """The floors must survive generation -> JSON -> load into the harness."""
        manifest_path = (
            ROOT / "tests" / "examples" / "godot" / "test_project" / "tests" / "fixtures"
            / "benchmark_asset_manifest.json"
        )
        manifest = _manifest_mod.load_benchmark_asset_manifest(manifest_path)
        self.assertEqual(
            manifest.min_splat_count_for(TEST_SPLATS_ASSET),
            10000,
            "the manifest on disk must carry the fixture floors; regenerate it with "
            "prepare_synthetic_assets.py if this fails",
        )
        # An undeclared asset must report 0 (no floor), not raise.
        self.assertEqual(manifest.min_splat_count_for("res://nope.ply"), 0)

    def test_manifest_rejects_non_integer_floors(self):
        """Malformed contract data fails loudly instead of silently disabling the guard."""
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "manifest.json"
            bad.write_text(
                '{"default_asset": "res://a.ply", "lane_defaults": {}, '
                '"scene_defaults": {}, "asset_min_splat_counts": {"res://a.ply": "lots"}}',
                encoding="utf-8",
            )
            with self.assertRaises(ValueError):
                _manifest_mod.load_benchmark_asset_manifest(bad)

    def test_manifest_without_floors_loads_with_empty_contract(self):
        """Back-compat: a manifest predating the contract still loads."""
        with tempfile.TemporaryDirectory() as tmp:
            legacy = Path(tmp) / "manifest.json"
            legacy.write_text(
                '{"default_asset": "res://a.ply", "lane_defaults": {}, "scene_defaults": {}}',
                encoding="utf-8",
            )
            manifest = _manifest_mod.load_benchmark_asset_manifest(legacy)
            self.assertEqual(manifest.asset_min_splat_counts, {})
            self.assertEqual(manifest.min_splat_count_for("res://a.ply"), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
