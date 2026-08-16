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
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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


class SyntheticAssetGenerationContractTests(unittest.TestCase):
    """The lightweight producer must not destroy a fixture that meets its floor."""

    def test_python_fallback_preserves_existing_valid_canonical_asset(self):
        with tempfile.TemporaryDirectory() as raw_td:
            root = Path(raw_td)
            fixture = root / "tests" / "fixtures" / "test_splats.ply"
            fixture.parent.mkdir(parents=True)
            _write_ply(fixture, 10000)
            original = fixture.read_bytes()
            spec = _prepare.PLYSpec(
                "tests/fixtures/test_splats.ply", 1024, 1101, "sphere", 3.0
            )

            with mock.patch.object(_prepare, "CANONICAL_SPECS", (spec,)), \
                    mock.patch.object(_prepare, "_write_manifest"), \
                    mock.patch.object(_prepare, "FORBIDDEN_LEGACY_PLYS", ()), \
                    mock.patch.object(_prepare, "FORBIDDEN_LEGACY_ASSET_DIRS", ()):
                self.assertEqual(_prepare._generate(root, quiet=True), 0)

            self.assertEqual(
                fixture.read_bytes(),
                original,
                "the 1024-splat fallback must not overwrite a valid 10000-splat fixture",
            )

    def test_required_floor_mode_does_not_dirty_valid_consumer_fixture(self):
        with tempfile.TemporaryDirectory() as raw_td:
            root = Path(raw_td)
            primary = root / "tests" / "fixtures" / "synthetic_sphere.ply"
            consumer = (
                root
                / "tests"
                / "examples"
                / "godot"
                / "test_project"
                / "tests"
                / "fixtures"
                / "synthetic_sphere.ply"
            )
            primary.parent.mkdir(parents=True)
            consumer.parent.mkdir(parents=True)
            _write_ply(primary, 50000)
            _write_ply(consumer, 2048)
            original = consumer.read_bytes()
            spec = _prepare.PLYSpec(
                "tests/examples/godot/test_project/tests/fixtures/synthetic_sphere.ply",
                2048,
                3101,
                "sphere",
                4.5,
            )

            with mock.patch.object(_prepare, "CANONICAL_SPECS", (spec,)), \
                    mock.patch.object(_prepare, "_generate_via_godot", return_value=True), \
                    mock.patch.object(_prepare, "_write_manifest"), \
                    mock.patch.object(_prepare, "FORBIDDEN_LEGACY_PLYS", ()), \
                    mock.patch.object(_prepare, "FORBIDDEN_LEGACY_ASSET_DIRS", ()):
                self.assertEqual(
                    _prepare._generate(
                        root,
                        quiet=True,
                        godot_binary=Path("godot.exe"),
                        preserve_floor_valid=True,
                    ),
                    0,
                )

            self.assertEqual(consumer.read_bytes(), original)

    def test_floor_validation_rejects_the_1024_fallback(self):
        with tempfile.TemporaryDirectory() as raw_td:
            root = Path(raw_td)
            for relative in (
                Path("tests/fixtures/test_splats.ply"),
                Path("tests/examples/godot/test_project/tests/fixtures/test_splats.ply"),
            ):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                _write_ply(path, 1024)

            with mock.patch.dict(
                _prepare.ASSET_MIN_SPLAT_COUNTS,
                {TEST_SPLATS_ASSET: 10000},
                clear=True,
            ):
                failures = _prepare.asset_floor_failures(root)

        self.assertEqual(len(failures), 2)
        self.assertTrue(all("1024" in failure and "10000" in failure for failure in failures))

    def test_floor_validation_accepts_both_consumer_copies_at_the_floor(self):
        with tempfile.TemporaryDirectory() as raw_td:
            root = Path(raw_td)
            for relative in (
                Path("tests/fixtures/test_splats.ply"),
                Path("tests/examples/godot/test_project/tests/fixtures/test_splats.ply"),
            ):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                _write_ply(path, 10000)

            with mock.patch.dict(
                _prepare.ASSET_MIN_SPLAT_COUNTS,
                {TEST_SPLATS_ASSET: 10000},
                clear=True,
            ):
                failures = _prepare.asset_floor_failures(root)

        self.assertEqual(failures, [])


FIXTURE_IMPORT_RELATIVE_DIR = (
    Path("tests") / "examples" / "godot" / "test_project" / "tests" / "fixtures"
)
FIXTURE_IMPORT_DIR = ROOT / FIXTURE_IMPORT_RELATIVE_DIR


def _tracked_fixture_imports(
    *,
    root: Path = ROOT,
    tracked_paths: list[str] | None = None,
) -> list[Path]:
    """Derive fixture imports from Git, excluding ignored editor sidecars."""
    if tracked_paths is None:
        pathspec = f"{FIXTURE_IMPORT_RELATIVE_DIR.as_posix()}/*.ply.import"
        result = subprocess.run(
            ["git", "ls-files", "-z", "--", pathspec],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(
                "Could not derive committed benchmark fixture imports from Git: "
                + (result.stderr.strip() or f"git ls-files exited {result.returncode}")
            )
        tracked_paths = [path for path in result.stdout.split("\0") if path]

    fixtures = []
    for raw_path in tracked_paths:
        relative_path = Path(raw_path)
        if (
            relative_path.parent == FIXTURE_IMPORT_RELATIVE_DIR
            and relative_path.name.endswith(".ply.import")
        ):
            fixtures.append(root / relative_path)
    return sorted(fixtures)


def _parse_import_file(path: Path) -> dict:
    """Extract the [params] block and the count fields from a Godot .import file.

    Deliberately a small hand parser: .import is Godot's own ConfigFile-ish
    format with `&"key": value` metadata, and pulling in a real parser to read
    five integers would be a worse trade than twelve lines of splitting.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    params: dict[str, str] = {}
    in_params = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("["):
            in_params = stripped == "[params]"
            continue
        if in_params and "=" in stripped:
            key, _, value = stripped.partition("=")
            params[key.strip()] = value.strip()

    # The `&` prefix matters and is not decoration: Godot writes top-level
    # metadata keys as StringNames (`&"splat_count"`), while the nested
    # `loader_statistics` dictionary uses plain string keys. Both spell
    # "splat_count", and they mean DIFFERENT things — loader_statistics carries
    # the count parsed from the source PLY, the top-level key carries the count
    # actually imported. Matching without the prefix finds the source count
    # first and compares it against itself, which is how the first draft of
    # this guard passed the very file it was written to reject.
    counts: dict[str, int] = {}
    for field in ("original_splat_count", "pre_prune_splat_count", "splat_count"):
        match = re.search(rf'&"{field}"\s*:\s*(\d+)', text)
        if match:
            counts[field] = int(match.group(1))
    compression = re.search(r'&"compression_flags"\s*:\s*(\d+)', text)
    return {"params": params, "counts": counts,
            "compression_flags": int(compression.group(1)) if compression else None}


def _fixture_thinning_failure(path: Path) -> str | None:
    """Describe why one imported fixture cannot prove full source fidelity."""
    counts = _parse_import_file(path)["counts"]
    required = ("original_splat_count", "splat_count")
    missing = [f'&"{field}"' for field in required if field not in counts]
    if missing:
        return (
            f"{path.name}: missing top-level fidelity metadata: "
            + ", ".join(missing)
        )

    original = counts["original_splat_count"]
    final = counts["splat_count"]
    if original <= 0:
        return (
            f'{path.name}: &"original_splat_count" must be positive, '
            f"got {original}"
        )
    if final != original:
        return (
            f"{path.name}: imports {final} of {original} splats "
            f"({final / original:.0%}) - the benchmark measures less than it names"
        )
    return None


class FixtureImportFidelityTests(unittest.TestCase):
    """Committed benchmark fixtures must import at FULL fidelity (#790).

    Every `synthetic_*.ply.import` on master carried `quality/preset="mobile"`,
    `density_multiplier=0.4` and all four `compression/quantize_*=true`. The
    consequence was recorded in the files' own metadata: synthetic_flower_field
    declared `original_splat_count: 30000` and shipped `splat_count: 12000`,
    with `compression_flags: 15`. Every benchmark backed by those fixtures was
    publishing a number for 40% of the workload it named, through the quantized
    (unlit) render path rather than the one being measured.

    Nothing generates that state — `ultra` is preset index 0, i.e. the
    importer's own default for a fresh import (gaussian_import_preset.cpp). The
    files were stale editor artifacts (importer_version 6 against today's 11,
    import_time three months old) that nobody re-checked.

    The load-bearing assertion here is the COUNT EQUALITY, not the preset name:
    it catches thinning no matter which option caused it, including options
    that do not exist yet.
    """

    QUANTIZE_OPTIONS = (
        "compression/quantize_positions",
        "compression/quantize_colors",
        "compression/quantize_scales",
        "compression/quantize_rotations",
    )

    def _fixtures(
        self,
        *,
        root: Path = ROOT,
        tracked_paths: list[str] | None = None,
    ) -> list[Path]:
        # Derived from the Git index, never a hand-maintained list: a new
        # committed fixture is covered immediately, while ignored editor
        # sidecars cannot make identical revisions produce different results.
        return _tracked_fixture_imports(root=root, tracked_paths=tracked_paths)

    def test_there_are_fixtures_to_check(self):
        """Guard against the guard silently covering nothing."""
        self.assertTrue(
            self._fixtures(),
            f"No committed .ply.import fixtures found under {FIXTURE_IMPORT_DIR}. "
            "If the fixtures moved, this guard is now inert - point it at the new path.",
        )

    def test_fixture_discovery_ignores_untracked_editor_sidecars(self):
        with tempfile.TemporaryDirectory() as raw_td:
            root = Path(raw_td)
            fixture_dir = root / FIXTURE_IMPORT_RELATIVE_DIR
            fixture_dir.mkdir(parents=True)
            tracked = fixture_dir / "synthetic_tracked.ply.import"
            ignored = fixture_dir / "test_splats.ply.import"
            tracked.write_text("", encoding="utf-8")
            ignored.write_text("", encoding="utf-8")

            fixtures = self._fixtures(
                root=root,
                tracked_paths=[tracked.relative_to(root).as_posix()],
            )

        self.assertEqual([path.name for path in fixtures], [tracked.name])
        self.assertNotIn(ignored.name, [path.name for path in fixtures])

    def test_no_fixture_is_thinned_at_import(self):
        """The invariant that actually matters: what was imported == what exists."""
        offenders = []
        for path in self._fixtures():
            failure = _fixture_thinning_failure(path)
            if failure is not None:
                offenders.append(failure)
        self.assertEqual(offenders, [], "Thinned benchmark fixtures:\n  " + "\n  ".join(offenders))

    def test_missing_top_level_fidelity_metadata_fails_closed(self):
        """Removing either load-bearing count must not skip the fixture.

        Keep the nested plain-string splat_count decoy in both probes: matching
        it would recreate the parser bug that once compared the source count to
        itself and passed the 40%-density fixture.
        """
        cases = {
            "original_splat_count": '&"splat_count": 30000,\n',
            "splat_count": '&"original_splat_count": 30000,\n',
        }
        with tempfile.TemporaryDirectory() as raw_td:
            for missing_field, present_metadata in cases.items():
                with self.subTest(missing_field=missing_field):
                    probe = Path(raw_td) / f"missing_{missing_field}.ply.import"
                    probe.write_text(
                        '[remap]\n\nmetadata={\n'
                        '&"loader_statistics": {\n'
                        '"splat_count": 30000\n'
                        '},\n'
                        + present_metadata
                        + '}\n\n[params]\n',
                        encoding="utf-8",
                    )
                    self.assertEqual(
                        _fixture_thinning_failure(probe),
                        f'{probe.name}: missing top-level fidelity metadata: &"{missing_field}"',
                    )

    def test_zero_source_count_cannot_make_the_fidelity_check_vacuous(self):
        with tempfile.TemporaryDirectory() as raw_td:
            probe = Path(raw_td) / "zero_counts.ply.import"
            probe.write_text(
                '[remap]\n\nmetadata={\n'
                '&"original_splat_count": 0,\n'
                '&"splat_count": 0\n'
                '}\n\n[params]\n',
                encoding="utf-8",
            )
            self.assertEqual(
                _fixture_thinning_failure(probe),
                'zero_counts.ply.import: &"original_splat_count" must be positive, got 0',
            )

    def test_no_fixture_is_quantized(self):
        """Quantized assets render through a different (unlit) path, so a
        quantized fixture does not merely measure fewer splats - it measures a
        different renderer."""
        offenders = []
        for path in self._fixtures():
            parsed = _parse_import_file(path)
            enabled = [opt for opt in self.QUANTIZE_OPTIONS if parsed["params"].get(opt) == "true"]
            flags = parsed["compression_flags"]
            if enabled or (flags is not None and flags != 0):
                offenders.append(f"{path.name}: quantize={enabled or '-'} compression_flags={flags}")
        self.assertEqual(offenders, [], "Quantized benchmark fixtures:\n  " + "\n  ".join(offenders))

    def test_no_fixture_declares_a_reducing_import_option(self):
        """Belt and braces on the two options that can thin a fixture, so the
        cause is named in the failure rather than only the symptom."""
        offenders = []
        for path in self._fixtures():
            params = _parse_import_file(path)["params"]
            density = params.get("quality/density_multiplier")
            max_splats = params.get("quality/max_splats")
            if density is not None and float(density) < 1.0:
                offenders.append(f"{path.name}: quality/density_multiplier={density} < 1.0")
            if max_splats is not None and int(max_splats) != 0:
                offenders.append(f"{path.name}: quality/max_splats={max_splats} (0 means unlimited)")
        self.assertEqual(offenders, [], "Reducing import options:\n  " + "\n  ".join(offenders))

    def test_the_thinning_check_still_discriminates(self):
        """A guard that cannot fail is worse than no guard. Feed it the exact
        shape master shipped and require a rejection."""
        with tempfile.TemporaryDirectory() as raw_td:
            probe = Path(raw_td) / "thinned.ply.import"
            # Shaped exactly like the real file, INCLUDING the decoy
            # `loader_statistics.splat_count` that carries the source count with
            # no `&` prefix. The first draft of this parser matched that one and
            # compared 30000 against 30000, so the probe must contain it or the
            # guard's own regression cannot be caught.
            probe.write_text(
                '[remap]\n\nmetadata={\n'
                '&"compression_flags": 15,\n'
                '&"loader_statistics": {\n'
                '"splat_count": 30000\n'
                '},\n'
                '&"original_splat_count": 30000,\n'
                '&"splat_count": 12000\n'
                '}\n\n[params]\n\nquality/preset="mobile"\n'
                'quality/density_multiplier=0.4\nquality/max_splats=250000\n'
                'compression/quantize_positions=true\n',
                encoding="utf-8",
            )
            parsed = _parse_import_file(probe)
            self.assertEqual(parsed["counts"]["original_splat_count"], 30000)
            self.assertEqual(
                parsed["counts"]["splat_count"],
                12000,
                "Must read the top-level &\"splat_count\" (imported), not "
                "loader_statistics.splat_count (source).",
            )
            self.assertEqual(parsed["compression_flags"], 15)
            self.assertEqual(parsed["params"]["quality/preset"], '"mobile"')
            self.assertEqual(parsed["params"]["quality/density_multiplier"], "0.4")
            self.assertEqual(parsed["params"]["compression/quantize_positions"], "true")
            self.assertEqual(
                _fixture_thinning_failure(probe),
                "thinned.ply.import: imports 12000 of 30000 splats (40%) - "
                "the benchmark measures less than it names",
                "The discrimination probe must exercise the guard decision, not only its parser.",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
