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

import ast
import contextlib
import importlib.util
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import types
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
_provenance = _load_module("_gs_fixture_provenance", RUNTIME_DIR / "fixture_provenance.py")


@contextlib.contextmanager
def _producer_record(produced):
    """Record `{path: variant}` the way a generation run does, and point the reader at it.

    Uses the real writer and the real reader, so these tests exercise the
    record contract rather than a stand-in for it. Only the LOCATION is
    redirected: a fixture built in a temp directory is not in the workspace
    corpus, so without this the positive cases would assert against whatever
    record the developer's own tree happens to hold.
    """
    with tempfile.TemporaryDirectory() as tmp:
        fixtures_dir = Path(tmp)
        _provenance.record_producer_output(
            fixtures_dir, {Path(path): variant for path, variant in dict(produced).items()}
        )
        with mock.patch.object(_run_benchmark, "_fixtures_dir", return_value=fixtures_dir):
            yield

read_ply_vertex_count = _run_benchmark.read_ply_vertex_count
evaluate_fixture_contract = _run_benchmark.evaluate_fixture_contract
PLY_PREP_COMMAND = _run_benchmark.PLY_PREP_COMMAND

TEST_SPLATS_ASSET = "res://tests/fixtures/test_splats.ply"


#: The property set a real Gaussian fixture declares. The helper writes this by
#: default so tests exercise the header shape the generators actually produce;
#: `gaussian=False` writes a bare point cloud -- what a fixture swapped for an
#: unrelated file looks like.
_GAUSSIAN_PLY_PROPERTIES = ("x","y","z","f_dc_0","f_dc_1","f_dc_2","opacity","scale_0","scale_1","scale_2","rot_0","rot_1","rot_2","rot_3")


def _write_ply_with_properties(path: Path, vertex_count: int, props: tuple) -> None:
    """Header-only PLY with an EXACT property list, for malformed/partial shapes."""
    NL = chr(10)
    header = (
        "ply" + NL
        + "format binary_little_endian 1.0" + NL
        + f"element vertex {vertex_count}" + NL
        + "".join(f"property float {name}" + NL for name in props)
        + "end_header" + NL
    ).encode("ascii")
    path.write_bytes(header)


def _write_ply(
    path: Path,
    vertex_count: int,
    *,
    header_only: bool = False,
    gaussian: bool = True,
    rich_sh: bool = False,
) -> None:
    """Write a binary PLY with a declared vertex count.

    `rich_sh` mirrors the C++ generator, which emits f_rest_0..44
    (synthetic_ply_writer.cpp:48); the Python fallback emits none. Tests that
    mean "a fixture the C++ generator wrote" must set it, or they are asserting
    against a fallback-shaped file wearing a rich vertex count.
    """
    props = _GAUSSIAN_PLY_PROPERTIES if gaussian else ("x", "y", "z")
    if rich_sh:
        props = props + tuple(f"f_rest_{i}" for i in range(45))
    header = (
        "ply\n"
        "format binary_little_endian 1.0\n"
        f"element vertex {vertex_count}\n"
        + "".join(f"property float {name}\n" for name in props)
        + "end_header\n"
    ).encode("ascii")
    body = (
        b""
        if header_only
        else struct.pack(f"<{len(props)}f", *([0.0] * len(props))) * vertex_count
    )
    path.write_bytes(header + body)


class PlyProvenanceIsNotCountAloneTests(unittest.TestCase):
    """A declared count must not be sufficient to identify a fixture (#790 review).

    Provenance was inferred from vertex count alone, so any PLY carrying a
    declared producer's count was labelled as that producer -- and
    `--require-asset-variant` then treated the label as fidelity evidence.
    Substituting an unrelated point cloud of the right size satisfied it.
    """

    def test_an_unrelated_point_cloud_of_the_right_size_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "fixture.ply"
            _write_ply(ply, 10000, header_only=True, gaussian=False)
            self.assertIsNone(
                _run_benchmark.read_ply_vertex_count(ply),
                "a bare xyz cloud was accepted as a Gaussian fixture on count alone",
            )

    def test_a_real_gaussian_fixture_of_the_same_size_is_still_read(self):
        """Non-vacuity: rejecting everything would be the same defect."""
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "fixture.ply"
            _write_ply(ply, 10000, header_only=True)
            self.assertEqual(_run_benchmark.read_ply_vertex_count(ply), 10000)


class ProducerHeaderShapeIsDerivedTests(unittest.TestCase):
    """The rich shape the tests assert against must come from the WRITER (#790 review).

    The positive rich-fixture tests hand-author the producer's header. A locally
    invented shape can only confirm what the author already believed: if
    `synthetic_ply_writer.cpp` changed its header, those tests would stay green
    while describing a file the producer no longer writes.

    This is derivation from source, and it is NOT the capture-from-a-real-run the
    review asked for -- that needs the module build. What it does establish is
    the coupling: the writer changing its header changes the derived list, and
    these assertions fail rather than drifting quietly.
    """

    def test_the_writer_source_is_parseable_at_all(self):
        """Non-vacuity: an unparseable writer would make every check below empty."""
        props = _prepare.parse_cpp_writer_properties()
        self.assertTrue(props, "no properties derived - the writer parse produced nothing")
        self.assertIn("x", props)
        self.assertIn("opacity", props)

    def test_the_required_rich_sh_block_matches_the_writer(self):
        props = _prepare.parse_cpp_writer_properties()
        emitted = {p.encode() for p in props if p.startswith("f_rest_")}
        self.assertEqual(
            emitted,
            set(_run_benchmark._RICH_SH_PROPERTIES),
            "the f_rest block this guard requires is not the block the writer emits",
        )

    def test_every_required_property_is_one_the_writer_emits(self):
        props = {p.encode() for p in _prepare.parse_cpp_writer_properties()}
        missing = sorted(p for p in _run_benchmark._REQUIRED_PLY_PROPERTIES if p not in props)
        self.assertEqual(
            missing, [],
            "the guard demands properties the C++ writer never emits, so a genuine "
            "producer fixture would be rejected",
        )

    def test_the_test_helpers_rich_shape_matches_the_writer(self):
        """The hand-authored helper must agree with the producer it stands in for."""
        props = set(_prepare.parse_cpp_writer_properties())
        invented = set(_GAUSSIAN_PLY_PROPERTIES) | {f"f_rest_{i}" for i in range(45)}
        stray = sorted(invented - props)
        self.assertEqual(
            stray, [],
            "the test helper writes properties the real producer does not emit",
        )


class RichVariantRequiresRichShTests(unittest.TestCase):
    """A rich label must mean the rich PRODUCER, not merely the rich COUNT (#790 review).

    `synthetic_ply_writer.cpp:48` emits f_rest_0..44 whenever p_write_sh1 is set,
    and every generator call site sets it; the Python fallback emits none. So a
    fallback-shaped file carrying a rich vertex count is not a rich fixture, and
    labelling it cpp_rich would let `--require-asset-variant cpp_rich` pass on a
    workload nothing rich produced.
    """

    VARIANTS = {"python_fallback": 2048, "cpp_rich": 50000}

    def test_a_fallback_shaped_ply_with_a_rich_count_is_not_cpp_rich(self):
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "synthetic_sphere.ply"
            _write_ply(ply, 50000, header_only=True)          # no f_rest_*
            self.assertFalse(_run_benchmark.ply_header_declares_rich_sh(ply))
            self.assertEqual(
                _run_benchmark.classify_fixture_variant(
                    50000, self.VARIANTS, False, recorded_variant="cpp_rich"
                ),
                _run_benchmark.VARIANT_UNRECOGNIZED,
                "a fallback-shaped file wearing a rich count was labelled cpp_rich",
            )

    def test_a_partial_f_rest_block_is_not_rich(self):
        """One f_rest property is not the producer's block (#790 review).

        `synthetic_ply_writer.cpp:46-48` declares all 45 slots unconditionally,
        so a partial set is not something the C++ writer can emit. Accepting any
        single `f_rest_*` let a header carrying only `f_rest_44` claim cpp_rich
        and publish benchmark numbers.
        """
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "synthetic_sphere.ply"
            _write_ply_with_properties(
                ply, 50000, _GAUSSIAN_PLY_PROPERTIES + ("f_rest_44",)
            )
            self.assertFalse(
                _run_benchmark.ply_header_declares_rich_sh(ply),
                "a single f_rest property was accepted as the complete rich block",
            )

    def test_an_ascii_ply_with_the_rich_block_is_not_the_cpp_producer(self):
        """Encoding is provenance: the C++ writer never emits ascii.

        A `format ascii 1.0` PLY carrying the generic Gaussian fields and the
        full `f_rest_0..44` block used to satisfy `cpp_rich`, so an unrelated
        standard 3DGS asset of that shape could be published under the wrong
        producer -- which is precisely what `--require-asset-variant cpp_rich`
        exists to prevent.
        """
        props = _prepare.parse_cpp_writer_properties()
        self.assertTrue(props, "writer properties could not be derived from source")
        expected = _run_benchmark.cpp_writer_ply_format()
        self.assertEqual(
            expected,
            b"binary_little_endian 1.0",
            "the derived producer format changed; this test's premise needs revisiting",
        )

        NL = chr(10)
        header = (
            "ply" + NL
            + "format ascii 1.0" + NL
            + "element vertex 50000" + NL
            + "".join("property float " + name + NL for name in props)
            + "end_header" + NL
        ).encode("ascii")

        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "impostor.ply"
            ply.write_bytes(header)
            self.assertFalse(
                _run_benchmark.ply_header_declares_rich_sh(ply),
                "an ascii PLY with the producer's exact property block was accepted "
                "as cpp_rich; the property block alone is not provenance",
            )

        # Discrimination: the same property block in the producer's OWN encoding
        # must still be accepted, or the check above would be satisfied by a
        # classifier that rejects everything.
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "genuine.ply"
            _write_ply_with_properties(ply, 50000, props)
            self.assertTrue(
                _run_benchmark.ply_header_declares_rich_sh(ply),
                "the producer's own header shape was rejected",
            )

    def test_a_real_rich_fixture_is_still_labelled_cpp_rich(self):
        """Non-vacuity: rejecting the real producer too would be the same defect.

        The header is DERIVED from the writer via parse_cpp_writer_properties()
        rather than hand-authored. A locally invented shape can only confirm what
        the author already believed: if synthetic_ply_writer.cpp changed its
        header, an invented positive would stay green while describing a file the
        producer no longer writes. (Derivation from source is still not the same
        as a fixture CAPTURED from a real producer run -- that needs the module
        build -- but it establishes the coupling, so the asserted shape cannot
        drift away from the emitted one.)
        """
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "synthetic_sphere.ply"
            props = _prepare.parse_cpp_writer_properties()
            self.assertTrue(props, "writer properties could not be derived from source")
            self.assertTrue(
                any(name.startswith("f_rest_") for name in props),
                "derived producer header carries no f_rest_* block; the positive "
                "case below would be vacuous",
            )
            _write_ply_with_properties(ply, 50000, props)
            self.assertTrue(_run_benchmark.ply_header_declares_rich_sh(ply))
            self.assertEqual(
                _run_benchmark.classify_fixture_variant(
                    50000, self.VARIANTS, True, recorded_variant="cpp_rich"
                ),
                "cpp_rich",
            )

    def test_the_fallback_producer_is_unaffected(self):
        self.assertEqual(
            _run_benchmark.classify_fixture_variant(
                2048, self.VARIANTS, False, recorded_variant="python_fallback"
            ),
            "python_fallback",
        )


class ProducerRecordIsProvenanceTests(unittest.TestCase):
    """#790 review round 3: a header shape is not provenance.

    `ply_header_declares_rich_sh()` authenticates the producer's encoding and its
    property block, both derived from `synthetic_ply_writer.cpp`. Both describe a
    FILE SHAPE, and a shape can be assembled: a `binary_little_endian` PLY built
    outside the generator with a declared producer count and the full
    `f_rest_0..44` block satisfies every one of those checks, is labelled
    `cpp_rich`, and can therefore satisfy `--require-asset-variant cpp_rich` and
    publish numbers under a producer that never wrote it.

    So the producer records what it wrote and the label requires that record to
    name these exact bytes. This is not a tamper-proof signature -- anyone who can
    write the fixtures can write the record beside them, and human review remains
    the control under an adversarial model. It closes the case the review named:
    a file the producer never wrote being accepted as its output.
    """

    SPHERE = "res://tests/fixtures/synthetic_sphere.ply"

    def _variants(self) -> dict[str, int]:
        return dict(_prepare.ASSET_EXPECTED_SPLAT_COUNTS[self.SPHERE])

    def _impostor(self, directory: Path, name: str = "synthetic_sphere.ply") -> Path:
        """A PLY with the producer's exact header shape, written by nothing."""
        props = _prepare.parse_cpp_writer_properties()
        self.assertTrue(props, "writer properties could not be derived from source")
        ply = directory / name
        _write_ply_with_properties(ply, self._variants()["cpp_rich"], props)
        # Premise check: it clears every SHAPE test, which is why the record is
        # needed. If this ever fails, the impostor is being rejected for the wrong
        # reason and the assertions below would pass vacuously.
        self.assertTrue(
            _run_benchmark.ply_header_declares_rich_sh(ply),
            "the impostor no longer matches the producer's header shape",
        )
        return ply

    def _contract(self, ply: Path) -> str:
        return evaluate_fixture_contract(
            lane_id="synthetic_sphere",
            asset_path=self.SPHERE,
            asset_file=ply,
            required_splats=_prepare.ASSET_MIN_SPLAT_COUNTS[self.SPHERE],
            expected_variants=self._variants(),
        )

    def test_a_shape_perfect_impostor_without_a_record_is_not_cpp_rich(self):
        with tempfile.TemporaryDirectory() as tmp:
            ply = self._impostor(Path(tmp))
            with _producer_record({}):
                failure = self._contract(ply)
                variant = _run_benchmark.classify_fixture_variant(
                    self._variants()["cpp_rich"],
                    self._variants(),
                    True,
                    recorded_variant=_run_benchmark.recorded_fixture_producer(ply),
                )
        self.assertNotEqual(
            variant,
            "cpp_rich",
            "a file no generation run wrote was labelled as the C++ producer's output",
        )
        self.assertIn("UNRECOGNIZED", failure)
        self.assertIn("a generation run recorded this file as: nothing", failure)

    def test_the_same_bytes_with_a_producer_record_are_accepted(self):
        """Discrimination: the real producer's output must still pass.

        Without this half, the assertion above is satisfied by refusing every
        fixture -- which would make `--require-asset-variant cpp_rich` unusable
        and is the same defect wearing a different hat.
        """
        with tempfile.TemporaryDirectory() as tmp:
            ply = self._impostor(Path(tmp))
            with _producer_record({ply: "cpp_rich"}):
                self.assertEqual(
                    _run_benchmark.classify_fixture_variant(
                        self._variants()["cpp_rich"],
                        self._variants(),
                        True,
                        recorded_variant=_run_benchmark.recorded_fixture_producer(ply),
                    ),
                    "cpp_rich",
                )
                self.assertEqual(self._contract(ply), "")

    def test_a_record_authenticates_bytes_and_not_a_name(self):
        """Digest-keyed, so substituting a file at a recorded name proves nothing.

        A path-keyed record would still be satisfied by writing a different file
        at a recorded path, which is the substitution this check exists to catch.
        """
        with tempfile.TemporaryDirectory() as tmp:
            genuine = self._impostor(Path(tmp), "genuine.ply")
            swapped = Path(tmp) / "synthetic_sphere.ply"
            swapped.write_bytes(genuine.read_bytes() + b"\x00" * 16)
            self.assertEqual(
                _run_benchmark.read_ply_vertex_count(swapped),
                self._variants()["cpp_rich"],
                "the swapped file no longer declares the producer count; the case is moot",
            )
            with _producer_record({genuine: "cpp_rich"}):
                self.assertIsNone(
                    _run_benchmark.recorded_fixture_producer(swapped),
                    "a different file was authenticated by another file's record",
                )
                self.assertIn("UNRECOGNIZED", self._contract(swapped))

    def test_a_copy_of_recorded_output_authenticates_from_the_same_entry(self):
        """The consumer project's copy is byte-identical, and must not need its own entry.

        `prepare_synthetic_assets.py` copies each primary fixture into the test
        project, and lanes resolve `res://` paths to that copy. A record keyed by
        location would leave every lane's actual fixture unauthenticated.
        """
        with tempfile.TemporaryDirectory() as tmp:
            # The same basename in two directories: that is what copy2 into the
            # consumer project produces, and what the lanes resolve res:// to.
            primary_dir = Path(tmp) / "primary"
            primary_dir.mkdir()
            primary = self._impostor(primary_dir)
            consumer = Path(tmp) / "consumer"
            consumer.mkdir()
            copy = consumer / "synthetic_sphere.ply"
            copy.write_bytes(primary.read_bytes())
            with _producer_record({primary: "cpp_rich"}):
                self.assertEqual(
                    _run_benchmark.recorded_fixture_producer(copy),
                    "cpp_rich",
                    "the consumer copy of recorded producer output was not authenticated",
                )

    def test_recorded_bytes_cannot_be_published_under_another_fixtures_name(self):
        """#969 review round 4: the name is part of the claim.

        sphere, cube, plane and torus are all declared as 50,000-splat `cpp_rich`
        outputs, so the count cannot tell them apart and the header shape is
        identical. A digest-only record therefore accepted recorded sphere bytes
        copied over `synthetic_cube.ply`: the cube lane measured a sphere and
        published it as the cube.
        """
        cube_path = "res://tests/fixtures/synthetic_cube.ply"
        cube_variants = dict(_prepare.ASSET_EXPECTED_SPLAT_COUNTS[cube_path])
        self.assertEqual(
            cube_variants["cpp_rich"],
            self._variants()["cpp_rich"],
            "sphere and cube no longer share a producer count; this case is moot",
        )
        with tempfile.TemporaryDirectory() as tmp:
            sphere = self._impostor(Path(tmp))
            cube = Path(tmp) / "synthetic_cube.ply"
            cube.write_bytes(sphere.read_bytes())
            with _producer_record({sphere: "cpp_rich"}):
                self.assertIsNone(
                    _run_benchmark.recorded_fixture_producer(cube),
                    "one fixture's recorded bytes authenticated another fixture's name",
                )
                failure = evaluate_fixture_contract(
                    lane_id="synthetic_cube",
                    asset_path=cube_path,
                    asset_file=cube,
                    required_splats=_prepare.ASSET_MIN_SPLAT_COUNTS[cube_path],
                    expected_variants=cube_variants,
                )
                self.assertIn("UNRECOGNIZED", failure)
                # Discrimination: the file the record actually names still passes.
                self.assertEqual(self._contract(sphere), "")

    def test_an_unrecorded_fallback_fixture_is_not_the_fallback_producer(self):
        """#969 review round 4: `python_fallback` is advertised as provenance too.

        The record only removed variants in RICH_SH_VARIANTS, so a fallback label
        rested on the vertex count alone -- and the counts are shared across
        fixtures, so the 2,048-splat fallback cube placed at the sphere path was
        reported as the sphere producer's output and satisfied
        `--require-asset-variant python_fallback`.
        """
        variants = self._variants()
        fallback_count = variants["python_fallback"]
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "synthetic_sphere.ply"
            _write_ply(ply, fallback_count, header_only=True)
            with _producer_record({}):
                self.assertEqual(
                    _run_benchmark.classify_fixture_variant(
                        fallback_count,
                        variants,
                        False,
                        recorded_variant=_run_benchmark.recorded_fixture_producer(ply),
                    ),
                    "unrecognized",
                    "an unrecorded fixture was labelled as the Python producer's output",
                )
                self.assertIn("UNRECOGNIZED", self._contract(ply))

            # Discrimination: the fallback producer's own recorded output passes,
            # and is labelled as the fallback rather than as nothing.
            with _producer_record({ply: "python_fallback"}):
                self.assertEqual(
                    _run_benchmark.classify_fixture_variant(
                        fallback_count,
                        variants,
                        False,
                        recorded_variant=_run_benchmark.recorded_fixture_producer(ply),
                    ),
                    "python_fallback",
                )
                self.assertEqual(self._contract(ply), "")

    def test_a_record_that_disagrees_with_the_count_is_not_authentic(self):
        """Non-vacuity for the record itself: it cannot outvote the evidence.

        A recorded fixture that was later thinned still carries its entry only
        until its bytes change -- but a record naming a producer whose count this
        file does not have must not be accepted either, or the record would become
        the single point of trust the count checks exist to back up.
        """
        variants = self._variants()
        self.assertEqual(
            _run_benchmark.classify_fixture_variant(
                variants["cpp_rich"], variants, True, recorded_variant="python_fallback"
            ),
            "unrecognized",
            "a record naming a producer that writes a different count was believed",
        )

    def test_prep_fails_when_the_provenance_record_cannot_be_written(self):
        """#969 review round 4: unrecorded fixtures are fixtures nothing can use.

        `--require-asset-variant` reads that record, so a prep that exits 0 having
        failed to write it hands the next step a corpus it must reject, with the
        cause a job and several minutes behind the failure.
        """
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.object(
                _prepare.fixture_provenance, "record_producer_output", return_value=False
            ):
                code = _prepare._generate(Path(tmp), quiet=True)
        self.assertEqual(
            code, 1, "prep reported success for a corpus whose provenance was not recorded"
        )

    def test_the_record_keeps_entries_whose_files_are_still_on_disk(self):
        """A later run must not orphan a copy an earlier producer run wrote.

        The writer is called on every generation, including fallback ones. If it
        replaced the record wholesale, a consumer copy left in place by a run that
        did not rewrite it would silently lose its provenance and start failing.
        """
        with tempfile.TemporaryDirectory() as tmp:
            fixtures_dir = Path(tmp) / "fixtures"
            fixtures_dir.mkdir()
            kept = Path(tmp) / "kept.ply"
            kept.write_bytes(b"produced by an earlier run\n")
            _provenance.record_producer_output(fixtures_dir, {kept: "cpp_rich"})
            self.assertEqual(_provenance.recorded_variant(fixtures_dir, kept), "cpp_rich")

            # A later run that did not rewrite it, but the file is still there.
            _provenance.record_producer_output(fixtures_dir, {}, retain=[kept])
            self.assertEqual(
                _provenance.recorded_variant(fixtures_dir, kept),
                "cpp_rich",
                "a still-present producer output lost its record on a fallback run",
            )

            # And an entry whose file is gone is pruned rather than left to vouch
            # for bytes that no longer exist anywhere.
            _provenance.record_producer_output(fixtures_dir, {}, retain=[])
            self.assertIsNone(
                _provenance.recorded_variant(fixtures_dir, kept),
                "the record kept an entry for a file no longer in the workspace",
            )

    def test_a_produced_file_that_cannot_be_hashed_fails_the_record(self):
        """#969 review round 6: an incomplete record must not report success.

        Skipping an unhashable path -- a transient sharing lock on the Windows
        runner is the case that produces one -- wrote a record missing that
        fixture and still returned True, so the prep exited 0 and the benchmark
        rejected the corpus a job later, with the cause nowhere near the failure.
        """
        with tempfile.TemporaryDirectory() as tmp:
            fixtures_dir = Path(tmp) / "fixtures"
            fixtures_dir.mkdir()
            written = Path(tmp) / "written.ply"
            written.write_bytes(b"produced\n")
            vanished = Path(tmp) / "vanished.ply"  # never created: cannot be hashed

            self.assertFalse(
                _provenance.record_producer_output(
                    fixtures_dir, {written: "cpp_rich", vanished: "cpp_rich"}
                ),
                "a record missing one of the files it was asked to attest reported success",
            )
            self.assertIsNone(
                _provenance.recorded_variant(fixtures_dir, written),
                "a partial record was published for a run that could not be recorded",
            )

        # Discrimination: every file hashable, record written, True returned.
        with tempfile.TemporaryDirectory() as tmp:
            fixtures_dir = Path(tmp) / "fixtures"
            fixtures_dir.mkdir()
            produced = Path(tmp) / "produced.ply"
            produced.write_bytes(b"produced\n")
            self.assertTrue(
                _provenance.record_producer_output(fixtures_dir, {produced: "cpp_rich"})
            )
            self.assertEqual(
                _provenance.recorded_variant(fixtures_dir, produced), "cpp_rich"
            )

    def test_a_produced_file_that_vanishes_after_hashing_fails_the_record(self):
        """#969 review round 9: the same race, one line past the check for it.

        `path.stat().st_size` sat inside a `setdefault` default -- evaluated
        EAGERLY, so it ran for every produced path, including ones whose digest
        was already recorded. A file that disappeared between the hash and the
        stat therefore raised FileNotFoundError out of a function whose documented
        contract is to return False, bypassing `_generate()`'s own "provenance
        could not be recorded" diagnostic and killing the prep with a traceback.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixtures_dir = root / "fixtures"
            fixtures_dir.mkdir()
            produced = root / "produced.ply"
            produced.write_bytes(b"produced\n")

            real_digest = _provenance.file_digest

            def digest_then_vanish(path):
                value = real_digest(path)
                if Path(path).name == "produced.ply" and Path(path).is_file():
                    Path(path).unlink()
                return value

            with mock.patch.object(_provenance, "file_digest", digest_then_vanish):
                recorded = _provenance.record_producer_output(
                    fixtures_dir, {produced: "cpp_rich"}
                )

            self.assertFalse(
                recorded,
                "a produced file that vanished mid-record did not fail the record",
            )
            self.assertFalse(
                _provenance.provenance_path(fixtures_dir).exists(),
                "a partial record was published for a run that could not be recorded",
            )

    def test_a_damaged_record_authenticates_nothing(self):
        """Fail closed: a corrupt record must not be read as provenance."""
        with tempfile.TemporaryDirectory() as tmp:
            fixtures_dir = Path(tmp) / "fixtures"
            fixtures_dir.mkdir()
            produced = Path(tmp) / "produced.ply"
            produced.write_bytes(b"produced\n")
            _provenance.record_producer_output(fixtures_dir, {produced: "cpp_rich"})
            record = _provenance.provenance_path(fixtures_dir)

            for label, body in (
                ("not json", "{"),
                ("wrong version", json.dumps({"version": 999, "entries": {}})),
                (
                    "entries not a mapping",
                    json.dumps({"version": _provenance.PROVENANCE_VERSION, "entries": []}),
                ),
            ):
                with self.subTest(shape=label):
                    record.write_text(body, encoding="utf-8")
                    self.assertIsNone(
                        _provenance.recorded_variant(fixtures_dir, produced),
                        f"a {label} record was treated as provenance",
                    )

    def test_a_generation_run_writes_the_record(self):
        """The writer must be REACHED from _generate(), not merely exist.

        This is the other half of the wiring: with the reader in place and
        nothing writing the record, every fixture is unauthenticated and
        `--require-asset-variant cpp_rich` fails permanently -- a guard wired to
        nothing, failing closed instead of open, but just as broken.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            def fake_producer(binary, output_dir, quiet):
                output_dir.mkdir(parents=True, exist_ok=True)
                for name in _prepare.CPP_GENERATED_FILENAMES:
                    (output_dir / name).write_bytes(b"produced " + name.encode())
                return True

            with mock.patch.object(_prepare, "_generate_via_godot", side_effect=fake_producer):
                self.assertEqual(
                    _prepare._generate(root, quiet=True, godot_binary=Path("godot")), 0
                )

            fixtures_dir = root / "tests" / "fixtures"
            for name in sorted(_prepare.CPP_GENERATED_FILENAMES):
                with self.subTest(fixture=name):
                    self.assertEqual(
                        _provenance.recorded_variant(fixtures_dir, fixtures_dir / name),
                        "cpp_rich",
                        f"a generation run left {name} unrecorded",
                    )

            consumer = (
                root / "tests" / "examples" / "godot" / "test_project" / "tests" / "fixtures"
                / "synthetic_sphere.ply"
            )
            self.assertTrue(consumer.is_file(), "the consumer copy was not written")
            self.assertEqual(
                _provenance.recorded_variant(fixtures_dir, consumer),
                "cpp_rich",
                "the copy the lanes actually measure was not authenticated by the record",
            )

            # The Python producer is recorded too, or --require-asset-variant
            # python_fallback would rest on the vertex count alone.
            for name in ("synthetic_spiral.ply", "synthetic_flower_field.ply"):
                with self.subTest(fixture=name):
                    self.assertEqual(
                        _provenance.recorded_variant(fixtures_dir, fixtures_dir / name),
                        "python_fallback",
                        f"the fallback producer left {name} unrecorded",
                    )

    def test_every_classification_in_the_runner_consults_the_record(self):
        """The check must be unreachable-proof: a call site that omits it re-opens the hole.

        `recorded_variant` is required, so a call site cannot omit it -- but it
        could still pass a literal, which would hardcode provenance instead of
        reading it. Both are properties of the source, so both are asserted
        against the source.
        """
        tree = ast.parse((RUNTIME_DIR / "run_benchmark.py").read_text(encoding="utf-8"))
        calls = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "classify_fixture_variant"
        ]
        self.assertTrue(calls, "no call sites found; this guard would be vacuous")
        for call in calls:
            with self.subTest(line=call.lineno):
                keywords = {keyword.arg: keyword.value for keyword in call.keywords}
                self.assertIn(
                    "recorded_variant",
                    keywords,
                    f"run_benchmark.py:{call.lineno} classifies a fixture without "
                    "consulting the producer record",
                )
                self.assertNotIsInstance(
                    keywords["recorded_variant"],
                    ast.Constant,
                    f"run_benchmark.py:{call.lineno} passes a literal provenance verdict "
                    "instead of reading the record",
                )



def _producer_binary() -> "Path | None":
    """A Godot build carrying the C++ `[GeneratePLY]` case, or None.

    Resolved from `GODOT_BINARY` the way `run_module_tests.py` resolves it for
    the StringName orphan guard -- a path, or a name on PATH. There is no
    `--godot-binary` here because unittest owns the argv.
    """
    raw = os.environ.get("GODOT_BINARY", "").strip()
    if not raw:
        return None
    candidate = Path(raw)
    if candidate.is_file():
        return candidate
    resolved = shutil.which(raw)
    return Path(resolved) if resolved else None


def _header_property_names(path: Path) -> tuple[str, ...]:
    """The `property` names a PLY header declares, in order."""
    names: list[str] = []
    with path.open("rb") as handle:
        if handle.readline().strip() != b"ply":
            return ()
        for _ in range(512):
            line = handle.readline()
            if not line or line.strip() == b"end_header":
                break
            fields = line.strip().split()
            if len(fields) >= 3 and fields[0] == b"property":
                names.append(fields[-1].decode("ascii", "replace"))
    return tuple(names)


@unittest.skipUnless(
    _producer_binary() is not None,
    "GODOT_BINARY is not set to a build carrying the C++ [GeneratePLY] case",
)
class ProducerCapturedPositiveTests(unittest.TestCase):
    """The positive case on bytes the C++ producer actually wrote (#969 review).

    Every other positive in this file builds its input locally -- from properties
    parsed out of `synthetic_ply_writer.cpp`, which couples the assertion to the
    writer's source but not to its OUTPUT. If the real `[GeneratePLY]` route emits
    something those assertions do not describe, or fails to produce a usable
    provenance record, they stay green. This class removes that gap by running the
    producer and asserting against what it wrote.

    It is gated on `GODOT_BINARY`, so **say which mode a result came from**: with
    the variable unset this class is SKIPPED and proves nothing. The derived-shape
    tests above are what run unconditionally, and they are a coupling check, not a
    capture.
    """

    SPHERE = "res://tests/fixtures/synthetic_sphere.ply"

    def test_the_real_producer_run_is_accepted_end_to_end(self):
        binary = _producer_binary()
        with tempfile.TemporaryDirectory() as tmp:
            fixtures_dir = Path(tmp) / "fixtures"
            fixtures_dir.mkdir()

            self.assertTrue(
                _prepare._generate_via_godot(binary, fixtures_dir, True),
                f"the producer at {binary} did not generate the fixture corpus",
            )

            for name in sorted(_prepare.CPP_GENERATED_FILENAMES):
                path = fixtures_dir / name
                with self.subTest(fixture=name):
                    self.assertEqual(
                        _run_benchmark.read_ply_vertex_count(path),
                        _prepare.CPP_GENERATOR_SPLAT_COUNTS[name],
                        "the captured fixture's count is not the one derived from "
                        "generate_synthetic_ply_fixtures.h",
                    )
                    self.assertTrue(
                        _run_benchmark.ply_header_declares_rich_sh(path),
                        "the producer's own output failed the rich-SH header check",
                    )

            # The derived shape the invented positives use must be the shape the
            # producer emits. This is what makes those tests worth having: they
            # describe a real file, not one the author imagined.
            captured = _header_property_names(fixtures_dir / "synthetic_sphere.ply")
            self.assertEqual(
                captured,
                tuple(_prepare.parse_cpp_writer_properties()),
                "the properties parsed from synthetic_ply_writer.cpp are not the "
                "properties the producer wrote",
            )

            # And the real writer's record authenticates the real output, through
            # the real reader.
            self.assertTrue(
                _provenance.record_producer_output(
                    fixtures_dir,
                    {
                        fixtures_dir / name: _provenance.VARIANT_CPP_RICH
                        for name in _prepare.CPP_GENERATED_FILENAMES
                    },
                ),
                "the producer record could not be written",
            )
            sphere = fixtures_dir / "synthetic_sphere.ply"
            with mock.patch.object(
                _run_benchmark, "_fixtures_dir", return_value=fixtures_dir
            ):
                self.assertEqual(
                    _run_benchmark.recorded_fixture_producer(sphere),
                    _provenance.VARIANT_CPP_RICH,
                    "captured producer output was not authenticated by its own record",
                )
                self.assertEqual(
                    evaluate_fixture_contract(
                        lane_id="synthetic_sphere",
                        asset_path=self.SPHERE,
                        asset_file=sphere,
                        required_splats=_prepare.ASSET_MIN_SPLAT_COUNTS[self.SPHERE],
                        expected_variants=dict(
                            _prepare.ASSET_EXPECTED_SPLAT_COUNTS[self.SPHERE]
                        ),
                        asset_source="captured",
                    ),
                    "",
                    "the C++ producer's own fixture was rejected by the contract",
                )


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


class CppGeneratorDerivationTests(unittest.TestCase):
    """The rich-fixture counts are read out of the generator, never restated (#790).

    `CPP_GENERATED_FILENAMES` used to be a hand-written set with the counts living
    only in a comment. That is the shape of invariant `tests/AGENTS.md` says must
    be derived: a generator gaining a fixture, or changing a count, left the
    Python side describing a tree that no longer existed.
    """

    def test_derived_set_is_not_empty_and_counts_are_positive(self):
        counts = _prepare.CPP_GENERATOR_SPLAT_COUNTS
        self.assertTrue(counts, "no C++ fixture generators were derived - the guard covers nothing")
        for name, count in counts.items():
            self.assertTrue(name.endswith(".ply"), f"derived a non-PLY output: {name}")
            self.assertGreater(count, 0, f"{name} derived a non-positive count")

    def test_generated_filenames_are_exactly_the_derived_keys(self):
        self.assertEqual(
            _prepare.CPP_GENERATED_FILENAMES,
            frozenset(_prepare.CPP_GENERATOR_SPLAT_COUNTS),
        )

    def test_the_module_constant_is_a_parse_and_not_a_transcription(self):
        """The assertion that makes the rest non-vacuous.

        Every other check in this class compares the constant against itself, so
        replacing the derivation with a hand-written dict would leave them all
        green - which is precisely the defect being removed. This one re-parses
        the real generator and requires the constant to equal it.
        """
        self.assertEqual(
            _prepare.CPP_GENERATOR_SPLAT_COUNTS,
            _prepare.parse_cpp_generator_counts(_prepare.CPP_GENERATOR_HEADER),
            "CPP_GENERATOR_SPLAT_COUNTS no longer matches a fresh parse of "
            f"{_prepare.CPP_GENERATOR_HEADER.name}; it must be derived, not restated",
        )

    def test_test_splats_floor_is_the_derived_cpp_count_not_the_fallback(self):
        """#669's floor, re-anchored to the generator instead of a literal.

        test_splats.ply is gitignored and never committed, so the C++ count is
        the only honest floor for it; the Python fallback writes 10x less. Pinning
        the literal 10000 could not tell the two apart if the generator changed.
        """
        cpp_count = _prepare.CPP_GENERATOR_SPLAT_COUNTS["test_splats.ply"]
        fallback = _prepare.PYTHON_FALLBACK_SPLAT_COUNTS["test_splats.ply"]
        self.assertEqual(_prepare.ASSET_MIN_SPLAT_COUNTS[TEST_SPLATS_ASSET], cpp_count)
        self.assertGreater(
            cpp_count,
            fallback,
            "the fallback now matches the C++ generator - the floor no longer "
            "discriminates between the two corpora",
        )

    def test_parses_a_generator_shaped_header(self):
        """Discrimination probe, shaped like the real file.

        It carries both decoys the naive patterns hit: the `path_join("..")`
        chain that builds the output directory (no `.ply`, so it must not consume
        a pending count) and `CHECK(splats.size() == cfg.splat_count);` (no
        `= <digits>;`, so it must not be read as a declaration).
        """
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / "generate_synthetic_ply_fixtures.h"
            header.write_text(
                'static String _ply_output_dir() {\n'
                '    dir = base.path_join("..").path_join("tests").path_join("fixtures");\n'
                '}\n'
                '{\n'
                '    cfg.splat_count = 100000;\n'
                '    cfg.seed = 3601;\n'
                '    CHECK(splats.size() == cfg.splat_count);\n'
                '    const String path = output_dir.path_join("synthetic_mandelbulb.ply");\n'
                '}\n'
                '{\n'
                '    cfg.splat_count = 10000;\n'
                '    const String path = output_dir.path_join("test_splats.ply");\n'
                '}\n',
                encoding="utf-8",
            )
            self.assertEqual(
                _prepare.parse_cpp_generator_counts(header),
                {"synthetic_mandelbulb.ply": 100000, "test_splats.ply": 10000},
            )

    def test_output_without_a_declared_count_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / "h.h"
            header.write_text(
                'const String path = output_dir.path_join("synthetic_sphere.ply");\n',
                encoding="utf-8",
            )
            with self.assertRaises(RuntimeError):
                _prepare.parse_cpp_generator_counts(header)

    def test_conflicting_counts_for_one_fixture_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / "h.h"
            header.write_text(
                'cfg.splat_count = 50000;\n'
                'output_dir.path_join("synthetic_sphere.ply");\n'
                'cfg.splat_count = 60000;\n'
                'output_dir.path_join("synthetic_sphere.ply");\n',
                encoding="utf-8",
            )
            with self.assertRaises(RuntimeError):
                _prepare.parse_cpp_generator_counts(header)

    def test_a_header_with_no_generators_fails_closed(self):
        """An empty derivation must raise, not silently produce an empty contract."""
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / "h.h"
            header.write_text("// nothing here\n", encoding="utf-8")
            with self.assertRaises(RuntimeError):
                _prepare.parse_cpp_generator_counts(header)

    def test_a_missing_header_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(RuntimeError):
                _prepare.parse_cpp_generator_counts(Path(tmp) / "absent.h")


class FloorProvenanceTests(unittest.TestCase):
    """Every floor must name a producer, not a preference (#790)."""

    def test_declared_floors_all_match_a_generator_output(self):
        for asset_path, floor in _prepare.ASSET_MIN_SPLAT_COUNTS.items():
            variants = _prepare.ASSET_EXPECTED_SPLAT_COUNTS[asset_path]
            self.assertIn(
                floor,
                set(variants.values()),
                f"{asset_path} floor {floor} matches no producer output {variants}",
            )

    def test_an_invented_floor_is_rejected(self):
        """Mutation: a floor between the two producers passes neither test today."""
        invented = dict(_prepare.ASSET_MIN_SPLAT_COUNTS)
        invented[TEST_SPLATS_ASSET] = 5000  # between 1024 and 10000
        with mock.patch.object(_prepare, "ASSET_MIN_SPLAT_COUNTS", invented):
            with self.assertRaises(RuntimeError):
                _prepare._validate_floor_provenance()

    @staticmethod
    def _exec_prepare_source(name: str, source: str) -> None:
        """Execute prepare_synthetic_assets.py's body under a throwaway module name.

        Registered in sys.modules for the duration: @dataclass resolves its own
        module through sys.modules, so a detached namespace raises before any of
        the module's own logic runs.
        """
        script = RUNTIME_DIR / "prepare_synthetic_assets.py"
        module = types.ModuleType(name)
        module.__file__ = str(script)
        sys.modules[name] = module
        try:
            exec(compile(source, str(script), "exec"), module.__dict__)
        finally:
            sys.modules.pop(name, None)

    def test_the_provenance_check_actually_runs_at_import(self):
        """Wiring, not logic: a validator nobody calls is decorative.

        The module body is re-executed with one floor moved off every producer's
        output. If the top-level `_validate_floor_provenance()` call is deleted,
        no RuntimeError is raised and this goes red - which is the failure mode a
        direct call to the validator cannot detect.
        """
        script = RUNTIME_DIR / "prepare_synthetic_assets.py"
        source = script.read_text(encoding="utf-8")
        anchor = '"res://tests/fixtures/test_splats.ply": 10000,'
        self.assertIn(anchor, source, "floor literal moved; re-anchor this mutation")
        mutated = source.replace(anchor, '"res://tests/fixtures/test_splats.ply": 5000,')
        with self.assertRaises(RuntimeError):
            self._exec_prepare_source("_gs_prepare_floor_mutation", mutated)

    def test_the_unmutated_module_body_executes_cleanly(self):
        """Discrimination: the probe above must fail for the mutation, not for
        the act of re-executing the module."""
        script = RUNTIME_DIR / "prepare_synthetic_assets.py"
        self._exec_prepare_source(
            "_gs_prepare_reexec", script.read_text(encoding="utf-8")
        )

    def test_every_declared_asset_has_at_least_one_producer(self):
        for asset_path, variants in _prepare.ASSET_EXPECTED_SPLAT_COUNTS.items():
            self.assertTrue(variants, f"{asset_path} declares no producer counts")

    def test_python_only_fixtures_declare_no_rich_variant(self):
        """spiral and flower_field have no C++ generator; their committed size IS
        their maximum fidelity, and reporting them as a reduced 'fallback' would
        be a false claim in the opposite direction."""
        for name in ("synthetic_spiral.ply", "synthetic_flower_field.ply"):
            self.assertNotIn(
                name,
                _prepare.CPP_GENERATOR_SPLAT_COUNTS,
                f"{name} gained a C++ generator - the provenance report's "
                "'maximum available fidelity' wording is now wrong for it",
            )


class FixtureVariantClassificationTests(unittest.TestCase):
    """The half a floor can never catch: which producer wrote this fixture (#790)."""

    SPHERE = "res://tests/fixtures/synthetic_sphere.ply"

    def _sphere_variants(self) -> dict[str, int]:
        return dict(_prepare.ASSET_EXPECTED_SPLAT_COUNTS[self.SPHERE])

    def test_classifies_each_producer(self):
        variants = self._sphere_variants()
        self.assertEqual(
            _run_benchmark.classify_fixture_variant(
                variants["python_fallback"], variants, recorded_variant="python_fallback"
            ),
            "python_fallback",
        )
        self.assertEqual(
            _run_benchmark.classify_fixture_variant(
                variants["cpp_rich"], variants, recorded_variant="cpp_rich"
            ),
            "cpp_rich",
        )

    def test_a_count_no_producer_writes_is_unrecognized(self):
        variants = self._sphere_variants()
        self.assertEqual(
            # A recorded producer AND a count no producer writes: the count is
            # the only thing wrong, so this stays a count test.
            _run_benchmark.classify_fixture_variant(
                12000, variants, recorded_variant="cpp_rich"
            ),
            "unrecognized",
        )

    def test_no_declared_producers_is_undeclared_not_unrecognized(self):
        """A --benchmark-asset override or a chunked-ladder asset carries no
        contract; treating it as a violation would fail lanes that are correct."""
        self.assertEqual(
            _run_benchmark.classify_fixture_variant(999, {}, recorded_variant=None),
            "undeclared",
        )

    def test_the_fallback_fixture_passes_the_floor_but_is_labelled(self):
        """The headline #790 case: 2048 and 50000 both clear the sphere floor.

        The floor cannot separate them - and must not be raised, because 2048 is
        what a clean checkout ships. The label is the entire signal.
        """
        variants = self._sphere_variants()
        floor = _prepare.ASSET_MIN_SPLAT_COUNTS[self.SPHERE]
        self.assertGreaterEqual(variants["python_fallback"], floor)
        self.assertGreaterEqual(variants["cpp_rich"], floor)
        self.assertNotEqual(
            _run_benchmark.classify_fixture_variant(
                variants["python_fallback"], variants, recorded_variant="python_fallback"
            ),
            _run_benchmark.classify_fixture_variant(
                variants["cpp_rich"], variants, recorded_variant="cpp_rich"
            ),
        )

    def test_contract_rejects_an_oversized_fixture_no_producer_writes(self):
        """Above the floor, below no producer: exactly the gap a floor leaves open."""
        variants = self._sphere_variants()
        floor = _prepare.ASSET_MIN_SPLAT_COUNTS[self.SPHERE]
        rogue = variants["cpp_rich"] + 1
        self.assertGreater(rogue, floor)
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "synthetic_sphere.ply"
            _write_ply(ply, rogue, header_only=True)
            failure = evaluate_fixture_contract(
                lane_id="synthetic_sphere",
                asset_path=self.SPHERE,
                asset_file=ply,
                required_splats=floor,
                expected_variants=variants,
            )
        self.assertIn("UNRECOGNIZED", failure)
        self.assertIn(str(rogue), failure)

    def test_contract_still_accepts_both_real_producers(self):
        """A guard that rejects every input is the same bug wearing a hat."""
        variants = self._sphere_variants()
        floor = _prepare.ASSET_MIN_SPLAT_COUNTS[self.SPHERE]
        for variant, count in variants.items():
            with self.subTest(variant=variant):
                with tempfile.TemporaryDirectory() as tmp:
                    ply = Path(tmp) / "synthetic_sphere.ply"
                    # The rich producer emits f_rest_*; writing the fallback
                    # shape here would assert against a file the C++ generator
                    # could not have produced.
                    _write_ply(ply, count, header_only=True,
                               rich_sh=(variant == "cpp_rich"))
                    # Shape is not provenance: a rich fixture is the producer's
                    # output only if a generation run recorded writing it, so the
                    # positive case has to carry that record too.
                    with _producer_record({ply: variant}):
                        self.assertEqual(
                            evaluate_fixture_contract(
                                lane_id="synthetic_sphere",
                                asset_path=self.SPHERE,
                                asset_file=ply,
                                required_splats=floor,
                                expected_variants=variants,
                            ),
                            "",
                        )

    def test_undeclared_asset_is_not_failed_by_the_variant_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / "custom.ply"
            _write_ply(ply, 7, header_only=True)
            self.assertEqual(
                evaluate_fixture_contract(
                    lane_id="some_lane",
                    asset_path="res://tests/fixtures/custom.ply",
                    asset_file=ply,
                    required_splats=0,
                    expected_variants={},
                ),
                "",
            )


class PreflightVariantWiringTests(unittest.TestCase):
    """The classifier must be reached from the suite preflight, not only exist.

    `evaluate_fixture_contract` gained the producer check, but the caller has to
    hand it `expected_variants` from the manifest or the check is unreachable --
    the "guard wired to nothing" shape in docs/governance/evidence-integrity.md.
    """

    LANE_ID = "synthetic_sphere"
    ASSET = "res://tests/fixtures/synthetic_sphere.ply"

    def _lane(self):
        for lane in _run_benchmark.LANES:
            if lane.lane_id == self.LANE_ID:
                return lane
        self.fail(f"lane {self.LANE_ID} no longer exists; re-anchor this test")

    def _manifest(self):
        return _manifest_mod.load_benchmark_asset_manifest(
            ROOT / "tests" / "examples" / "godot" / "test_project" / "tests" / "fixtures"
            / "benchmark_asset_manifest.json"
        )

    def _project_with_sphere(self, tmp: str, splats: int, *, rich_sh: bool = False) -> Path:
        project = Path(tmp) / "project"
        fixtures = project / "tests" / "fixtures"
        fixtures.mkdir(parents=True)
        _write_ply(
            fixtures / "synthetic_sphere.ply", splats, header_only=True, rich_sh=rich_sh
        )
        return project

    def _sphere_failures(
        self, splats: int, *, rich_sh: bool = False, recorded: "str | None" = None
    ) -> list[str]:
        manifest = self._manifest()
        with tempfile.TemporaryDirectory() as tmp:
            project = self._project_with_sphere(tmp, splats, rich_sh=rich_sh)
            ply = project / "tests" / "fixtures" / "synthetic_sphere.ply"
            with _producer_record({ply: recorded} if recorded else {}):
                failures = _run_benchmark._validate_suite_dependencies(
                    project_path=project,
                    lanes=[self._lane()],
                    asset_manifest=manifest,
                    generated_assets={},
                )
        return [failure for failure in failures if "benchmark fixture" in failure]

    def test_preflight_rejects_a_fixture_no_producer_wrote(self):
        variants = self._manifest().expected_splat_counts_for(self.ASSET)
        self.assertTrue(variants, "the manifest lost the sphere's producer counts")
        failures = self._sphere_failures(max(variants.values()) + 1)
        self.assertTrue(
            any("UNRECOGNIZED" in failure for failure in failures),
            f"preflight did not reach the producer check; got: {failures}",
        )

    def test_preflight_accepts_both_real_producers(self):
        for variant, count in self._manifest().expected_splat_counts_for(self.ASSET).items():
            with self.subTest(variant=variant):
                rich = variant == "cpp_rich"
                self.assertEqual(
                    self._sphere_failures(count, rich_sh=rich, recorded=variant), []
                )

    def test_provenance_collection_labels_the_lane(self):
        manifest = self._manifest()
        variants = manifest.expected_splat_counts_for(self.ASSET)
        for variant, count in variants.items():
            with self.subTest(variant=variant):
                with tempfile.TemporaryDirectory() as tmp:
                    rich = variant == "cpp_rich"
                    project = self._project_with_sphere(tmp, count, rich_sh=rich)
                    ply = project / "tests" / "fixtures" / "synthetic_sphere.ply"
                    with _producer_record({ply: variant}):
                        records = _run_benchmark.collect_fixture_provenance(
                            project_path=project,
                            lanes=[self._lane()],
                            asset_manifest=manifest,
                            generated_assets={},
                        )
                self.assertEqual(len(records), 1)
                self.assertEqual(records[0]["asset_variant"], variant)
                self.assertEqual(records[0]["asset_splat_count"], count)
                self.assertTrue(records[0]["asset_rich_variant_available"])


class RequiredAssetVariantTests(unittest.TestCase):
    """`--require-asset-variant` turns the label into enforcement (#790)."""

    def _record(self, **overrides):
        record = {
            "lane_id": "static_baseline",
            "asset_path": TEST_SPLATS_ASSET,
            "asset_source": "lane_default",
            "asset_splat_count": 1024,
            "asset_variant": "python_fallback",
            "asset_expected_splat_counts": {"python_fallback": 1024, "cpp_rich": 10000},
            "asset_rich_variant_available": True,
        }
        record.update(overrides)
        return record

    def test_fallback_fixture_fails_a_cpp_rich_requirement(self):
        failures = _run_benchmark.evaluate_required_asset_variant([self._record()], "cpp_rich")
        self.assertEqual(len(failures), 1)
        self.assertIn("static_baseline", failures[0])
        self.assertIn("cpp_rich", failures[0])

    def test_an_undeclared_fixture_cannot_satisfy_the_requirement(self):
        """Empty expected counts must FAIL, not silently exempt the lane.

        `--generate-dummy-assets`, or a custom manifest without
        `asset_expected_splat_counts`, leaves `expected` empty. That is not the
        same as an asset whose declared producers simply exclude the required
        one -- it means the fidelity is UNKNOWN. Exempting it let the flag pass
        over precisely the lanes least likely to be running real content.
        """
        record = self._record(asset_expected_splat_counts={}, asset_variant="undeclared")
        failures = _run_benchmark.evaluate_required_asset_variant([record], "cpp_rich")
        self.assertEqual(len(failures), 1, "an undeclared fixture was silently exempted")
        self.assertIn("declares no producer counts", failures[0])
        self.assertIn("static_baseline", failures[0])

    def test_a_declared_asset_without_that_producer_is_still_exempt(self):
        """The legitimate exemption must survive -- otherwise the fix is a blunt gate.

        `synthetic_spiral` and `synthetic_flower_field` have no C++ generator at
        all, so their committed sizes ARE maximum fidelity. Demanding cpp_rich of
        them would be a contract no checkout can satisfy.
        """
        record = self._record(
            asset_splat_count=25000,
            asset_variant="python_fallback",
            asset_expected_splat_counts={"python_fallback": 25000},
        )
        self.assertEqual(
            _run_benchmark.evaluate_required_asset_variant([record], "cpp_rich"),
            [],
            "an asset with no such producer declared must stay exempt",
        )

    def test_a_chunked_world_lane_is_exempt_not_failed(self):
        """Regression: fail-closed must not break the openworld-proof dispatch.

        `open_world_corridor_proof` resolves to a chunked-world stage manifest,
        not a PLY, so it has no producer counts and never can. The first version
        of the fail-closed change above rejected it, which would have killed the
        `openworld-proof-dev` dispatch before Godot even launched. "Unknown
        fidelity" and "not a PLY at all" are different cases.
        """
        record = self._record(
            lane_id="open_world_corridor_proof",
            asset_source="chunked_world_contract",
            asset_expected_splat_counts={},
            asset_variant="undeclared",
        )
        self.assertEqual(
            _run_benchmark.evaluate_required_asset_variant([record], "cpp_rich"),
            [],
            "a chunked-world contract lane must be exempt, not failed",
        )

    def test_a_ply_lane_with_no_declaration_still_fails(self):
        """Non-vacuity: the exemption must not swallow the case it was added beside."""
        record = self._record(
            asset_source="lane_default",
            asset_expected_splat_counts={},
            asset_variant="undeclared",
        )
        self.assertEqual(
            len(_run_benchmark.evaluate_required_asset_variant([record], "cpp_rich")),
            1,
            "a PLY lane with no declared producers must still fail closed",
        )

    def test_rich_fixture_satisfies_the_requirement(self):
        record = self._record(asset_splat_count=10000, asset_variant="cpp_rich")
        self.assertEqual(_run_benchmark.evaluate_required_asset_variant([record], "cpp_rich"), [])

    def test_asset_without_that_producer_is_exempt(self):
        """synthetic_spiral has no C++ generator; demanding one would be a
        contract no tree can satisfy, which is a gate that never goes green."""
        record = self._record(
            lane_id="dense_resident_2m",
            asset_path="res://tests/fixtures/synthetic_spiral.ply",
            asset_splat_count=25000,
            asset_expected_splat_counts={"python_fallback": 25000},
            asset_rich_variant_available=False,
        )
        self.assertEqual(_run_benchmark.evaluate_required_asset_variant([record], "cpp_rich"), [])

    def test_unrecognized_fixture_fails_the_requirement(self):
        record = self._record(asset_splat_count=5000, asset_variant="unrecognized")
        self.assertEqual(len(_run_benchmark.evaluate_required_asset_variant([record], "cpp_rich")), 1)


class ManifestVariantContractTests(unittest.TestCase):
    """The producer table must survive generation -> JSON -> harness (#790)."""

    def test_manifest_on_disk_round_trips_the_producer_counts(self):
        manifest_path = (
            ROOT / "tests" / "examples" / "godot" / "test_project" / "tests" / "fixtures"
            / "benchmark_asset_manifest.json"
        )
        manifest = _manifest_mod.load_benchmark_asset_manifest(manifest_path)
        for asset_path, variants in _prepare.ASSET_EXPECTED_SPLAT_COUNTS.items():
            self.assertEqual(
                manifest.expected_splat_counts_for(asset_path),
                variants,
                "the manifest on disk must carry the producer counts; regenerate it with "
                "prepare_synthetic_assets.py if this fails",
            )
        self.assertEqual(manifest.expected_splat_counts_for("res://nope.ply"), {})

    def test_manifest_rejects_an_empty_producer_entry(self):
        """An empty entry would exempt the fixture instead of constraining it."""
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "manifest.json"
            bad.write_text(
                '{"default_asset": "res://a.ply", "lane_defaults": {}, "scene_defaults": {}, '
                '"asset_expected_splat_counts": {"res://a.ply": {}}}',
                encoding="utf-8",
            )
            with self.assertRaises(ValueError):
                _manifest_mod.load_benchmark_asset_manifest(bad)

    def test_manifest_rejects_non_integer_and_non_positive_producer_counts(self):
        for payload in ('{"res://a.ply": {"cpp_rich": "lots"}}', '{"res://a.ply": {"cpp_rich": 0}}'):
            with self.subTest(payload=payload):
                with tempfile.TemporaryDirectory() as tmp:
                    bad = Path(tmp) / "manifest.json"
                    bad.write_text(
                        '{"default_asset": "res://a.ply", "lane_defaults": {}, "scene_defaults": {}, '
                        f'"asset_expected_splat_counts": {payload}}}',
                        encoding="utf-8",
                    )
                    with self.assertRaises(ValueError):
                        _manifest_mod.load_benchmark_asset_manifest(bad)

    def test_manifest_without_producer_counts_loads_with_empty_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            legacy = Path(tmp) / "manifest.json"
            legacy.write_text(
                '{"default_asset": "res://a.ply", "lane_defaults": {}, "scene_defaults": {}}',
                encoding="utf-8",
            )
            manifest = _manifest_mod.load_benchmark_asset_manifest(legacy)
            self.assertEqual(manifest.asset_expected_splat_counts, {})


class CppGenerationProvesFreshOutputTests(unittest.TestCase):
    """A producer that writes nothing must not inherit pre-existing fixtures (#790 review).

    The previous check compared mtimes against the launch time with two seconds
    of slack. A caller who ran the fallback prep and immediately retried with a
    binary whose `GeneratePLY` filter matches zero tests but exits 0 left files
    inside that grace window, so every stale fallback fixture was accepted as
    freshly generated and `cpp_generated` was set on a corpus the producer never
    touched.
    """

    def _fake_binary_that_writes_nothing(self):
        """A producer that exits 0 and creates no files."""
        class _Proc:
            returncode = 0
            stdout = ""
            stderr = ""
        return lambda *a, **k: _Proc()

    def test_a_partially_written_failed_run_restores_the_originals(self):
        """Partial output must not survive a failed attempt (#790 review).

        A producer that writes some expected files and then dies leaves debris of
        unknown content and unknown splat count. Restoring only where the target
        was ABSENT left that debris in place and dropped the original underneath
        it -- a mix of partial output and stale originals, indistinguishable from
        a good corpus by filename.
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)
            for name in names:
                _write_ply(out / name, 1024, header_only=True)
            originals = {n: (out / n).read_bytes() for n in names}

            partial = names[0]

            class _Proc:
                returncode = 1
                stdout = ""
                stderr = "generator died halfway"

            def _writes_one_then_fails(*a, **k):
                # Simulate the producer emitting one file before dying.
                _write_ply(out / partial, 99999, header_only=True)
                return _Proc()

            with mock.patch.object(_prepare.subprocess, "run", _writes_one_then_fails):
                ok = _prepare._generate_via_godot(Path("godot"), out, quiet=True)

            self.assertFalse(ok, "a failed generator run was reported as success")
            for name in names:
                self.assertTrue((out / name).is_file(), f"{name} missing after failed run")
                self.assertEqual(
                    (out / name).read_bytes(), originals[name],
                    f"{name} was left as partial generator output instead of the original",
                )

    def test_a_truncated_fixture_is_refused_and_the_originals_come_back(self):
        """#969 review round 5: a file that is there is not a file that is whole.

        `synthetic_ply_writer.cpp` ignores the result of every `store_buffer` /
        `store_float` call and returns true regardless, so a short write -- a full
        disk on the persistent runner is the case that produces one -- reaches the
        prep script as a producer that exited 0. The file exists, this run wrote
        it, and its header declares the producer's count, so every earlier check
        passes: it was accepted, recorded as `cpp_rich`, and the originals it
        replaced were deleted.
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)
            for name in names:
                _write_ply(out / name, 512)
            originals = {name: (out / name).read_bytes() for name in names}
            starved = names[0]

            class _Proc:
                returncode = 0
                stdout = ""
                stderr = ""

            def short_write(*_args, **_kwargs):
                for name in names:
                    _write_ply(out / name, 2048)
                whole = (out / starved).read_bytes()
                (out / starved).write_bytes(whole[:-128])
                return _Proc()

            with mock.patch.object(_prepare.subprocess, "run", short_write):
                accepted = _prepare._generate_via_godot(Path("godot"), out, quiet=True)

            self.assertFalse(
                accepted, "a truncated fixture was accepted as this producer's output"
            )
            for name in names:
                with self.subTest(fixture=name):
                    self.assertEqual(
                        (out / name).read_bytes(),
                        originals[name],
                        f"{name}: the original was discarded for a truncated corpus",
                    )

    def test_a_complete_corpus_is_still_accepted(self):
        """Discrimination: the size formula must accept what the producer writes.

        A body-length check that is wrong by one property rejects every real run,
        which is the same defect in the other direction. (The formula is also
        checked against genuine producer output in ProducerCapturedPositiveTests.)
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)

            class _Proc:
                returncode = 0
                stdout = ""
                stderr = ""

            def complete_write(*_args, **_kwargs):
                for name in names:
                    _write_ply(out / name, 2048, rich_sh=True)
                return _Proc()

            with mock.patch.object(_prepare.subprocess, "run", complete_write):
                self.assertTrue(
                    _prepare._generate_via_godot(Path("godot"), out, quiet=True),
                    "a complete corpus was rejected by the payload check",
                )

    def test_the_payload_check_fails_closed_on_what_it_cannot_size(self):
        """Sizing a body wrongly and calling it complete is the failure to avoid."""
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)

            complete = out / "complete.ply"
            _write_ply(complete, 32)
            self.assertIsNone(
                _prepare.ply_payload_failure(complete),
                "a complete fixture was reported as truncated",
            )

            header_only = out / "header_only.ply"
            _write_ply(header_only, 32, header_only=True)
            self.assertIsNotNone(
                _prepare.ply_payload_failure(header_only),
                "a header with no body at all was accepted",
            )

            NL = chr(10)
            unsizeable = out / "double.ply"
            unsizeable.write_bytes(
                (
                    "ply" + NL
                    + "format binary_little_endian 1.0" + NL
                    + "element vertex 4" + NL
                    + "property double x" + NL
                    + "end_header" + NL
                ).encode("ascii")
                + b"\x00" * 32
            )
            self.assertIn(
                "not a float",
                _prepare.ply_payload_failure(unsizeable) or "",
                "a property this reader cannot size was treated as sizeable",
            )

            missing = out / "absent.ply"
            self.assertIsNotNone(
                _prepare.ply_payload_failure(missing), "an absent file was reported complete"
            )

    def test_a_quarantined_original_is_recovered_not_overwritten(self):
        """A failed restore must not become permanent loss on the next run (#969 review).

        When restoring an original raises -- a fixture locked by another process
        on the persistent Windows runner is the case that produces it -- the
        canonical path keeps that run's partial output and the original stays in
        `.pre_cpp_generation`. The isolation loop then deleted the existing
        quarantine entry to make room for the file at the canonical path, so the
        LAST COPY of the fixture was destroyed and the debris kept in its place.
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)
            quarantine = out / ".pre_cpp_generation"
            quarantine.mkdir()
            # Exactly the state a failed restore leaves behind: the originals in
            # the quarantine, this run's debris at the canonical paths, and the
            # marker naming what could not be put back. The marker is part of that
            # state -- see test_a_restore_that_cannot_write_says_so_and_keeps_the_original,
            # which produces it through the real restore path.
            for name in names:
                _write_ply(quarantine / name, 1024, header_only=True)
                _write_ply(out / name, 99999, header_only=True)
            (quarantine / _prepare.UNRESTORED_MARKER_FILENAME).write_text(
                json.dumps(names), encoding="utf-8"
            )
            originals = {name: (quarantine / name).read_bytes() for name in names}
            debris = {name: (out / name).read_bytes() for name in names}
            self.assertNotEqual(
                originals[names[0]], debris[names[0]], "the case needs distinguishable bytes"
            )

            with mock.patch.object(
                _prepare.subprocess, "run", self._fake_binary_that_writes_nothing()
            ):
                ok = _prepare._generate_via_godot(Path("godot"), out, quiet=True)

            self.assertFalse(ok, "a producer that created no files was accepted")
            for name in names:
                with self.subTest(fixture=name):
                    self.assertTrue((out / name).is_file(), f"{name} is missing entirely")
                    self.assertEqual(
                        (out / name).read_bytes(),
                        originals[name],
                        f"{name}: the quarantined original was destroyed and the failed "
                        "run's partial output kept in its place",
                    )

    def test_a_superseded_quarantine_copy_is_never_adopted(self):
        """#969 review round 7: two states, one look, and the wrong one assumed.

        When a SUCCESSFUL run cannot delete a stashed copy -- Windows denying the
        unlink is the case that produces it -- the quarantine keeps a file that
        looks exactly like an original a failed restore left behind. The recovery
        path adopted it: the current, valid fixture was deleted as "debris", and
        when this run's producer then failed, the superseded corpus was restored
        over it. A failed retry downgraded a workspace that was valid when it
        started.
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)
            quarantine = out / ".pre_cpp_generation"
            quarantine.mkdir()
            # No marker: nothing failed to restore, these are leftovers a
            # successful run could not remove. Both files are WHOLE -- that is
            # what makes the marker the deciding evidence, and a header-only
            # canonical file would be debris rather than a fixture.
            for name in names:
                _write_ply(quarantine / name, 32, rich_sh=True)
                _write_ply(out / name, 64, rich_sh=True)
            current = {name: (out / name).read_bytes() for name in names}

            with mock.patch.object(
                _prepare.subprocess, "run", self._fake_binary_that_writes_nothing()
            ):
                self.assertFalse(_prepare._generate_via_godot(Path("godot"), out, quiet=True))

            for name in names:
                with self.subTest(fixture=name):
                    self.assertEqual(
                        (out / name).read_bytes(),
                        current[name],
                        f"{name}: a superseded quarantine copy replaced the valid fixture",
                    )

    def test_debris_at_the_canonical_path_recovers_without_a_marker(self):
        """#969 review round 8: evidence outranks the marker where there is any.

        An original left by an older implementation, or by a marker write that
        itself failed, has no marker entry. Classifying it as superseded and
        unlinking it threw away the only copy while the canonical path held a
        failed producer's debris. A canonical file that is not a whole PLY is not
        a fixture, and that is enough to know which of the two the copy is.
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)
            quarantine = out / ".pre_cpp_generation"
            quarantine.mkdir()
            for name in names:
                _write_ply(quarantine / name, 32, rich_sh=True)      # the original
                _write_ply(out / name, 99999, header_only=True)      # debris
            originals = {name: (quarantine / name).read_bytes() for name in names}
            self.assertFalse((quarantine / _prepare.UNRESTORED_MARKER_FILENAME).exists())

            with mock.patch.object(
                _prepare.subprocess, "run", self._fake_binary_that_writes_nothing()
            ):
                self.assertFalse(_prepare._generate_via_godot(Path("godot"), out, quiet=True))

            for name in names:
                with self.subTest(fixture=name):
                    self.assertEqual(
                        (out / name).read_bytes(),
                        originals[name],
                        f"{name}: the only copy was deleted as superseded",
                    )

    def test_an_unreadable_marker_stops_the_run_instead_of_guessing(self):
        """Two whole files and no readable record of which is which: touch neither.

        This is the case evidence cannot settle. Deleting the wrong one is
        unrecoverable, so the run refuses and says what to remove.
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)
            quarantine = out / ".pre_cpp_generation"
            quarantine.mkdir()
            for name in names:
                _write_ply(quarantine / name, 32, rich_sh=True)
                _write_ply(out / name, 64, rich_sh=True)
            (quarantine / _prepare.UNRESTORED_MARKER_FILENAME).write_bytes(b"{not json")
            quarantined = {name: (quarantine / name).read_bytes() for name in names}
            canonical = {name: (out / name).read_bytes() for name in names}

            buffer = io.StringIO()
            with mock.patch.object(
                _prepare.subprocess, "run", self._fake_binary_that_writes_nothing()
            ):
                with contextlib.redirect_stdout(buffer):
                    self.assertFalse(
                        _prepare._generate_via_godot(Path("godot"), out, quiet=True)
                    )

            self.assertIn("cannot tell an unrestored original", buffer.getvalue())
            for name in names:
                with self.subTest(fixture=name):
                    self.assertEqual((out / name).read_bytes(), canonical[name])
                    self.assertEqual((quarantine / name).read_bytes(), quarantined[name])

    def test_the_undecidable_stop_survives_the_next_run(self):
        """#969 review round 9: a stop that clears its own evidence is not a stop.

        The undecidable branch refuses to choose between a quarantined original
        and a superseded copy, prints "nothing was moved or deleted", and asks for
        a human. But the rollback that followed rewrote the marker from
        `(_read_unrestored(...) or set()) | failures` -- and with an UNREADABLE
        marker that `or` yields the empty set, which `_write_unrestored` writes by
        DELETING the file. The next run then saw no marker at all, classified the
        quarantined original as a superseded copy, and unlinked it: the re-run the
        message asked for destroyed the copy instead of the human deciding.
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)
            quarantine = out / ".pre_cpp_generation"
            quarantine.mkdir()
            marker = quarantine / _prepare.UNRESTORED_MARKER_FILENAME
            for name in names:
                _write_ply(quarantine / name, 32, rich_sh=True)
                _write_ply(out / name, 64, rich_sh=True)
            marker.write_bytes(b"{not json")
            quarantined = {name: (quarantine / name).read_bytes() for name in names}

            buffer = io.StringIO()
            with mock.patch.object(
                _prepare.subprocess, "run", self._fake_binary_that_writes_nothing()
            ):
                with contextlib.redirect_stdout(buffer):
                    self.assertFalse(
                        _prepare._generate_via_godot(Path("godot"), out, quiet=True)
                    )

            self.assertTrue(
                marker.is_file(),
                "the stop deleted the marker it stopped on, so the state it refused "
                "to guess at is gone",
            )
            self.assertIsNone(
                _prepare._read_unrestored(quarantine),
                "the marker was rewritten, so the next run no longer knows the state "
                "is unknown",
            )

            # The decision has to still be pending on the NEXT run -- that is the
            # run the message asks for, and it is where the copies were destroyed.
            with mock.patch.object(
                _prepare.subprocess, "run", self._fake_binary_that_writes_nothing()
            ):
                with contextlib.redirect_stdout(buffer):
                    self.assertFalse(
                        _prepare._generate_via_godot(Path("godot"), out, quiet=True)
                    )
            for name in names:
                with self.subTest(fixture=name):
                    self.assertTrue(
                        (quarantine / name).is_file(),
                        f"{name}: the re-run discarded the quarantined original the "
                        "previous run refused to judge",
                    )
                    self.assertEqual((quarantine / name).read_bytes(), quarantined[name])

    def test_a_readable_marker_is_still_updated_and_cleared(self):
        """Discrimination: only an UNREADABLE marker is untouchable.

        Without this, the fix above is satisfied by never writing the marker at
        all -- which would strand every genuine recovery, since an adopted
        original must stop being listed as pending.
        """
        quarantine_names = sorted(_prepare.CPP_GENERATED_FILENAMES)[:2]
        with tempfile.TemporaryDirectory() as tmp:
            quarantine = Path(tmp) / ".pre_cpp_generation"
            quarantine.mkdir()

            # merging into a readable marker keeps both the old and the new names
            _prepare._write_unrestored(quarantine, {quarantine_names[0]})
            _prepare._merge_unrestored(quarantine, {quarantine_names[1]})
            self.assertEqual(_prepare._read_unrestored(quarantine), set(quarantine_names))

            # and clearing it outright still removes the file
            _prepare._write_unrestored(quarantine, set())
            self.assertEqual(_prepare._read_unrestored(quarantine), set())
            self.assertFalse((quarantine / _prepare.UNRESTORED_MARKER_FILENAME).exists())

    def test_a_successful_run_still_clears_an_unreadable_marker(self):
        """The one caller that may: a fresh corpus answers the marker's question.

        After the producer delivers, whatever is left in the quarantine is
        superseded whatever the marker said, so leaving an unreadable one behind
        would make the next run stop on a state that is no longer ambiguous.
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)
            quarantine = out / ".pre_cpp_generation"
            quarantine.mkdir()
            marker = quarantine / _prepare.UNRESTORED_MARKER_FILENAME
            for name in names:
                _write_ply(out / name, 16, rich_sh=True)
            marker.write_bytes(b"{not json")

            class _Proc:
                returncode = 0
                stdout = ""
                stderr = ""

            def producer(*_args, **_kwargs):
                for name in names:
                    _write_ply(out / name, 48, rich_sh=True)
                return _Proc()

            with mock.patch.object(_prepare.subprocess, "run", producer):
                self.assertTrue(_prepare._generate_via_godot(Path("godot"), out, quiet=True))

            self.assertEqual(
                _prepare._read_unrestored(quarantine),
                set(),
                "a successful generation left the marker unreadable, so the next run "
                "would stop on a question its own output already answered",
            )

    def test_a_failed_cleanup_after_success_is_reported_and_disarmed(self):
        """The other half: say so, and make sure the next run cannot be misled.

        The copy stays on disk because the delete failed, so what has to change is
        the record of what it MEANS -- the marker says nothing is pending recovery,
        which is what stops the next run adopting it.
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)
            for name in names:
                _write_ply(out / name, 1024, header_only=True)
            quarantine = out / ".pre_cpp_generation"
            real_unlink = Path.unlink

            def stubborn_unlink(self, *args, **kwargs):
                if self.parent == quarantine and self.suffix == ".ply":
                    raise OSError(13, "the file is locked by another process")
                return real_unlink(self, *args, **kwargs)

            class _Proc:
                returncode = 0
                stdout = ""
                stderr = ""

            def producer(*_args, **_kwargs):
                # A COMPLETE body: the payload check refuses a header-only file,
                # and this case is about the cleanup that follows a success.
                for name in names:
                    _write_ply(out / name, 64, rich_sh=True)
                return _Proc()

            buffer = io.StringIO()
            with mock.patch.object(_prepare.subprocess, "run", producer):
                with mock.patch.object(Path, "unlink", stubborn_unlink):
                    with contextlib.redirect_stdout(buffer):
                        self.assertTrue(
                            _prepare._generate_via_godot(Path("godot"), out, quiet=True),
                            "a successful generation was failed by a cleanup problem",
                        )

            output = buffer.getvalue()
            self.assertIn("could not remove superseded quarantine", output)
            self.assertEqual(
                _prepare._read_unrestored(quarantine),
                set(),
                "the leftovers were left looking like originals pending recovery",
            )
            self.assertTrue(
                any((quarantine / name).is_file() for name in names),
                "the case did not reproduce: the copies were removed after all",
            )

    def test_a_restore_that_cannot_write_says_so_and_keeps_the_original(self):
        """Silence is the other half: the state is recoverable only if it is known.

        The original survives in the quarantine, so nothing is lost -- but a log
        that says nothing leaves a corpus of partial output looking like fixtures
        until someone happens to regenerate.
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)
            for name in names:
                _write_ply(out / name, 1024, header_only=True)
            originals = {name: (out / name).read_bytes() for name in names}
            locked = names[0]
            quarantine = out / ".pre_cpp_generation"

            real_replace = Path.replace

            def flaky_replace(self, target):
                target = Path(target)
                if target.name == locked and target.parent == out:
                    raise OSError(13, "the file is locked by another process")
                return real_replace(self, target)

            buffer = io.StringIO()
            with mock.patch.object(
                _prepare.subprocess, "run", self._fake_binary_that_writes_nothing()
            ):
                with mock.patch.object(Path, "replace", flaky_replace):
                    with contextlib.redirect_stdout(buffer):
                        self.assertFalse(
                            _prepare._generate_via_godot(Path("godot"), out, quiet=True)
                        )

            output = buffer.getvalue()
            self.assertIn("could not restore the pre-run fixtures", output)
            self.assertIn(locked, output)
            self.assertTrue(
                (quarantine / locked).is_file(),
                "the original was neither restored nor preserved",
            )

            # And the next run picks it back up, so the failure is a delay rather
            # than a loss. This is the sequence the review described end to end.
            with mock.patch.object(
                _prepare.subprocess, "run", self._fake_binary_that_writes_nothing()
            ):
                with contextlib.redirect_stdout(buffer):
                    self.assertFalse(_prepare._generate_via_godot(Path("godot"), out, quiet=True))
            self.assertEqual(
                (out / locked).read_bytes(),
                originals[locked],
                "the original was not recovered by the following run",
            )

    def test_originals_stranded_in_the_quarantine_are_picked_back_up(self):
        """A restore that unlinked the target and then failed leaves NOTHING canonical.

        The isolation loop only ran when a canonical file existed, so in that
        state it did not run at all: the originals sat in `.pre_cpp_generation`
        while the producer wrote a fresh corpus over the top of nothing, and they
        were never seen again.
        """
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            names = sorted(_prepare.CPP_GENERATED_FILENAMES)
            quarantine = out / ".pre_cpp_generation"
            quarantine.mkdir()
            for name in names:
                _write_ply(quarantine / name, 1024, header_only=True)
            (quarantine / _prepare.UNRESTORED_MARKER_FILENAME).write_text(
                json.dumps(names), encoding="utf-8"
            )
            originals = {name: (quarantine / name).read_bytes() for name in names}

            with mock.patch.object(
                _prepare.subprocess, "run", self._fake_binary_that_writes_nothing()
            ):
                self.assertFalse(_prepare._generate_via_godot(Path("godot"), out, quiet=True))

            for name in names:
                with self.subTest(fixture=name):
                    self.assertEqual(
                        (out / name).read_bytes() if (out / name).is_file() else None,
                        originals[name],
                        f"{name} was left stranded in the quarantine",
                    )

    def test_a_producer_that_writes_nothing_fails_even_with_fresh_leftovers(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            # Leftovers with mtimes from *right now* -- inside the old grace window.
            for name in _prepare.CPP_GENERATED_FILENAMES:
                _write_ply(out / name, 1024, header_only=True)

            with mock.patch.object(_prepare.subprocess, "run",
                                   self._fake_binary_that_writes_nothing()):
                ok = _prepare._generate_via_godot(Path("godot"), out, quiet=True)

            self.assertFalse(
                ok, "a producer that created no files was accepted as having generated them"
            )
            # And the workspace is not left worse: the leftovers are restored.
            for name in _prepare.CPP_GENERATED_FILENAMES:
                self.assertTrue((out / name).is_file(),
                                f"{name} was not restored after the failed attempt")


class PrepFallbackPolicyTests(unittest.TestCase):
    """`--godot-binary` is a requirement, not a preference (#790, cause 3).

    Before this, passing a binary whose generators failed printed a line and
    silently produced the 10x-smaller corpus with exit code 0 - a caller that
    explicitly asked for the benchmark workload was handed a different one and
    told it had succeeded.
    """

    def test_failed_cpp_generation_with_a_binary_is_a_hard_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.object(_prepare, "_generate_via_godot", return_value=False):
                code = _prepare._generate(Path(tmp), quiet=True, godot_binary=Path(tmp) / "godot")
        self.assertEqual(code, 1)

    def test_allow_fallback_opts_back_in_explicitly(self):
        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.object(_prepare, "_generate_via_godot", return_value=False):
                code = _prepare._generate(
                    Path(tmp), quiet=True, godot_binary=Path(tmp) / "godot", allow_fallback=True
                )
        self.assertEqual(code, 0)

    def test_no_binary_still_generates_but_says_so(self):
        """The clean-checkout path must keep working; it just has to be audible."""
        buffer = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp:
            with contextlib.redirect_stdout(buffer):
                code = _prepare._generate(Path(tmp), quiet=True)
        self.assertEqual(code, 0)
        output = buffer.getvalue()
        self.assertIn("LOW-FIDELITY FIXTURES", output)
        self.assertIn("test_splats.ply", output)

    def test_the_fallback_notice_survives_quiet(self):
        """--quiet is what every CI invocation passes; a notice it suppresses is
        a notice that does not exist where it matters."""
        buffer = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp:
            with contextlib.redirect_stdout(buffer):
                _prepare._generate(Path(tmp), quiet=True)
        self.assertIn("LOW-FIDELITY FIXTURES", buffer.getvalue())

    def test_stale_outputs_do_not_satisfy_the_cpp_completeness_check(self):
        """A generator that exits 0 without writing must not pass because a
        previous Python-fallback run left files with the right names."""
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            for name in _prepare.CPP_GENERATED_FILENAMES:
                target = out / name
                target.write_bytes(b"stale")
                os.utime(target, (1_000_000, 1_000_000))
            completed = subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr="")
            with mock.patch.object(_prepare.subprocess, "run", return_value=completed):
                self.assertFalse(_prepare._generate_via_godot(Path("godot"), out, quiet=True))


GSPW_MAGIC = b"GSPW"
GSPW_WORLD_VERSION = 1
COMMITTED_WORLD_FIXTURE = (
    ROOT / "tests" / "examples" / "godot" / "test_project" / "tests" / "fixtures"
    / "test_splats.gsplatworld"
)
QA_BASELINE = ROOT / "tests" / "ci" / "baselines" / "qa_results.json"


def read_gsplatworld_splat_count(path: Path) -> int:
    """Read `splat_count` out of a `.gsplatworld` header.

    Field order is fixed by the saver in
    `modules/gaussian_splatting/io/gaussian_splat_world_io.cpp`: magic, version,
    flags, splat_count, each a little-endian uint32. Magic and version are checked
    first, so a format change fails loudly instead of returning whatever now sits
    at offset 12.
    """
    raw = path.read_bytes()[:16]
    if len(raw) < 16 or raw[:4] != GSPW_MAGIC:
        raise ValueError(f"{path.name} is not a GSPW world file")
    version, _flags, splat_count = struct.unpack("<3I", raw[4:16])
    if version != GSPW_WORLD_VERSION:
        raise ValueError(
            f"{path.name} declares world version {version}; this reader knows "
            f"{GSPW_WORLD_VERSION}. Re-derive the header layout from "
            "gaussian_splat_world_io.cpp before trusting the count."
        )
    return splat_count


def _collect_source_splat_counts(node, out: list[int]) -> None:
    if isinstance(node, dict):
        for key, value in node.items():
            if isinstance(key, str) and "source_splat_count" in key and isinstance(value, int):
                out.append(value)
            _collect_source_splat_counts(value, out)
    elif isinstance(node, list):
        for value in node:
            _collect_source_splat_counts(value, out)


class FallbackPinnedCorpusTests(unittest.TestCase):
    """Why `--godot-binary` is NOT forwarded by the shared CI prep runners (#790).

    #790's fix direction 5 says to add `--godot-binary` to the three CI prep
    invocations. Measured, only one of them can take it. `run_module_tests.py`,
    `run_baseline_qa.py` and `run_runtime_validation.py` all share a workspace with
    the QA scene suite, whose committed expectations were recorded against the
    Python-fallback `test_splats.ply`:

    * `tests/ci/baselines/qa_results.json` records `source_splat_count: 1024`;
    * the committed `test_splats.gsplatworld` is a 1024-splat bake, and the
      world-vs-instance A/B refuses to score when the world and the PLY disagree
      (`scripts/qa_route_capture_base.gd`).

    So regenerating the fixture at the C++ count in those runners would not
    upgrade a benchmark -- it would break a blocking gate whose numbers describe
    the other corpus. That coupling was invisible; these tests make it an enforced
    invariant, so it is discovered here rather than from a red GPU lane, and so
    that rebaking one side without the other fails immediately.
    """

    def _fallback_count(self) -> int:
        return _prepare.PYTHON_FALLBACK_SPLAT_COUNTS["test_splats.ply"]

    def test_committed_world_fixture_holds_the_fallback_count(self):
        self.assertTrue(
            COMMITTED_WORLD_FIXTURE.is_file(),
            f"{COMMITTED_WORLD_FIXTURE} is missing - this guard now covers nothing",
        )
        self.assertEqual(
            read_gsplatworld_splat_count(COMMITTED_WORLD_FIXTURE),
            self._fallback_count(),
            "the committed world fixture and the Python-fallback test_splats.ply must hold "
            "the same splats; the QA world-vs-instance A/B refuses to score otherwise. "
            "If the world was rebaked at the C++ count, the prep runners in tests/ci must "
            "forward --godot-binary in the same change (#790).",
        )

    def test_qa_baseline_records_the_fallback_count(self):
        self.assertTrue(QA_BASELINE.is_file(), f"{QA_BASELINE} is missing")
        counts: list[int] = []
        _collect_source_splat_counts(
            json.loads(QA_BASELINE.read_text(encoding="utf-8")), counts
        )
        self.assertTrue(
            counts,
            "no source_splat_count found in the QA baseline - this guard asserted nothing",
        )
        self.assertEqual(
            sorted(set(counts)),
            [self._fallback_count()],
            "the QA baseline's recorded source splat counts must match the corpus CI "
            "actually generates; a mismatch means the baseline and the fixture describe "
            "different workloads (#790)",
        )

    def test_the_world_header_reader_discriminates(self):
        """A guard that reads a constant is not reading the file."""
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / "probe.gsplatworld"
            probe.write_bytes(GSPW_MAGIC + struct.pack("<3I", GSPW_WORLD_VERSION, 4, 50000))
            self.assertEqual(read_gsplatworld_splat_count(probe), 50000)

            wrong_magic = Path(tmp) / "wrong.gsplatworld"
            wrong_magic.write_bytes(b"XXXX" + struct.pack("<3I", 1, 0, 1024))
            with self.assertRaises(ValueError):
                read_gsplatworld_splat_count(wrong_magic)

            wrong_version = Path(tmp) / "v99.gsplatworld"
            wrong_version.write_bytes(GSPW_MAGIC + struct.pack("<3I", 99, 0, 1024))
            with self.assertRaises(ValueError):
                read_gsplatworld_splat_count(wrong_version)

    def test_each_prep_runner_states_why_it_uses_the_fallback(self):
        """The downgrade must be reported, not merely be true.

        A runner that silently produced the small corpus is the #790 defect; a
        runner that produces it and says which committed artifacts pin it there is
        a documented constraint someone can act on.
        """
        runners = {
            "run_module_tests.py": ROOT / "tests" / "ci" / "run_module_tests.py",
            "run_baseline_qa.py": ROOT / "tests" / "ci" / "run_baseline_qa.py",
            "run_runtime_validation.py": RUNTIME_DIR / "run_runtime_validation.py",
        }
        for name, path in runners.items():
            with self.subTest(runner=name):
                module = _load_module(f"_gs_corpus_blocker_{path.stem}", path)
                blocker = getattr(module, "FIXTURE_CORPUS_BLOCKER", "")
                self.assertTrue(blocker, f"{name} does not state why it uses the fallback")
                self.assertIn("qa_results.json", blocker)
                self.assertIn("gsplatworld", blocker)
                self.assertIn("790", blocker)


WORKFLOW_DIR = ROOT / ".github" / "workflows"
PRODUCTION_GATES_WORKFLOW = WORKFLOW_DIR / "gaussian_production_gates.yml"

# Text matching, not a YAML parse, and deliberately so: no workflow in this
# repository pip-installs PyYAML, and `actions/setup-python@v5` provisions a bare
# tool-cache interpreter, so a module-scope `import yaml` would raise during
# unittest discovery in the guard lane. The same reasoning, with the measurement,
# is written out in tests/agentic/test_agentic_pr_gate_workflow.py.
STEP_HEADER_PREFIX = "      - name:"
STEP_KEY_PREFIX = "        "


def _workflow_step_blocks(text: str) -> list[tuple[str, list[str]]]:
    """Split a workflow into (step name, step lines), steps only.

    Job-level keys live at a shallower indent than STEP_HEADER_PREFIX, so a
    job-level `continue-on-error` can never be mistaken for a step-level one --
    which matters, because this job carries one by design.
    """
    blocks: list[tuple[str, list[str]]] = []
    current: list[str] | None = None
    name = ""
    for line in text.splitlines():
        if line.startswith(STEP_HEADER_PREFIX):
            if current is not None:
                blocks.append((name, current))
            name = line[len(STEP_HEADER_PREFIX):].strip()
            current = [line]
            continue
        if current is None:
            continue
        if line.strip() and not line.startswith(STEP_KEY_PREFIX):
            blocks.append((name, current))
            current = None
            continue
        current.append(line)
    if current is not None:
        blocks.append((name, current))
    return blocks


class BenchmarkEvidenceWorkflowWiringTests(unittest.TestCase):
    """The CI wiring #790 documents, pinned so it cannot silently come back.

    Two separate defects lived here: prep never received `--godot-binary`, so CI
    regenerated the small corpus; and the benchmark steps carried
    `continue-on-error: true`, so the fail-closed fixture guard rejecting 20 of 30
    lanes produced a green job.
    """

    def _workflow_texts(self) -> dict[Path, str]:
        return {
            path: path.read_text(encoding="utf-8")
            for path in sorted(WORKFLOW_DIR.glob("*.yml"))
        }

    def test_every_workflow_prep_invocation_passes_a_godot_binary(self):
        invocations = 0
        offenders: list[str] = []
        for path, text in self._workflow_texts().items():
            for idx, line in enumerate(text.splitlines(), start=1):
                if "prepare_synthetic_assets.py" not in line:
                    continue
                invocations += 1
                if "--godot-binary" not in line:
                    offenders.append(f"{path.name}:{idx}: {line.strip()}")
        self.assertGreater(
            invocations,
            0,
            "no workflow invokes prepare_synthetic_assets.py - this guard now covers nothing",
        )
        self.assertEqual(
            offenders,
            [],
            "workflow prep calls without --godot-binary regenerate the lightweight "
            "Python-fallback fixtures (#790):\n  " + "\n  ".join(offenders),
        )

    def test_no_benchmark_step_swallows_its_own_failure(self):
        text = PRODUCTION_GATES_WORKFLOW.read_text(encoding="utf-8")
        benchmark_steps = [
            (name, lines)
            for name, lines in _workflow_step_blocks(text)
            if any("run_benchmark.py" in line for line in lines)
        ]
        self.assertTrue(
            benchmark_steps,
            "no run_benchmark.py step found in gaussian_production_gates.yml - "
            "this guard is now inert; point it at the new location",
        )
        offenders = [
            name
            for name, lines in benchmark_steps
            if any(line.strip().startswith("continue-on-error:") for line in lines)
        ]
        self.assertEqual(
            offenders,
            [],
            "these benchmark steps swallow their own failure, which is what made a "
            f"working fail-closed fixture guard invisible (#790): {offenders}",
        )

    def test_the_step_splitter_separates_job_level_keys_from_step_level_ones(self):
        """Discrimination probe: the shape this file must not confuse.

        A job-level `continue-on-error` is legitimate here (the evidence job is
        non-blocking by design). Only the step-level one is the defect.
        """
        blocks = _workflow_step_blocks(
            "jobs:\n"
            "  evidence:\n"
            "    continue-on-error: true\n"
            "    steps:\n"
            "      - name: benign\n"
            "        run: python tests/runtime/run_benchmark.py\n"
            "      - name: swallowing\n"
            "        continue-on-error: true\n"
            "        run: python tests/runtime/run_benchmark.py\n"
        )
        self.assertEqual([name for name, _ in blocks], ["benign", "swallowing"])
        swallowing = dict(blocks)["swallowing"]
        benign = dict(blocks)["benign"]
        self.assertTrue(any("continue-on-error" in line for line in swallowing))
        self.assertFalse(any("continue-on-error" in line for line in benign))

CANONICAL_MANIFEST_PATHS = (
    ROOT / "tests" / "fixtures" / "benchmark_asset_manifest.json",
    ROOT
    / "tests"
    / "examples"
    / "godot"
    / "test_project"
    / "tests"
    / "fixtures"
    / "benchmark_asset_manifest.json",
)
PUBLISHED_BASELINE_LANE_ID = "dense_resident_2m"


class PublishedBaselineContractTests(unittest.TestCase):
    """#790: the published figure must describe a workload the project ships.

    The defect this pins: `evidence_role: published_baseline` sat on
    `static_baseline`, a single 10,000-splat instance of the lightweight
    canonical smoke fixture, published at ~455 FPS. The lane that actually
    exercises the resident sort/raster path, `dense_resident_2m`, measured
    ~12.3 FPS -- and carried `weight: 0.0` in every profile, so it could not
    influence the aggregate even on the runs where it executed, and was a
    member of no default profile, so it did not execute on a default run at
    all. The published headline therefore described a scene nobody ships,
    and nothing in the suite could notice.

    These cases pin the parts of the repair that can silently rot:

    * the role moved, and did not merely get added alongside the old one;
    * the lane carrying the role is not a smoke lane by classification or by
      resolved asset;
    * the lane is enrolled in the profile that produces published numbers and
      carries a non-zero weight there -- a role with no enrollment publishes
      nothing, which is the same defect wearing the opposite hat;
    * the committed manifests still match their generator, because the
      manifests are generated artifacts and the guard lane regenerates them.

    The list of published-baseline lanes is deliberately explicit rather than
    derived: which lane defines the published figure is a *decision*, and
    deriving it would let the corpus redefine what is published.
    """

    def _load(self, path: Path):
        return _manifest_mod.load_benchmark_asset_manifest(path)

    def _baseline_lane_ids(self, manifest) -> list[str]:
        return sorted(
            lane_id
            for lane_id, metadata in manifest.lane_metadata.items()
            if isinstance(metadata, dict)
            and str(metadata.get("evidence_role", "")).strip()
            == _manifest_mod.PUBLISHED_BASELINE_EVIDENCE_ROLE
        )

    def test_canonical_manifests_declare_exactly_one_published_baseline(self):
        checked = 0
        for path in CANONICAL_MANIFEST_PATHS:
            self.assertTrue(path.is_file(), f"canonical manifest missing: {path}")
            manifest = self._load(path)
            self.assertEqual(
                self._baseline_lane_ids(manifest),
                [PUBLISHED_BASELINE_LANE_ID],
                f"{path.name} must declare exactly one published_baseline lane, and it "
                f"must be {PUBLISHED_BASELINE_LANE_ID} (#790)",
            )
            checked += 1
        self.assertEqual(
            checked, len(CANONICAL_MANIFEST_PATHS), "not every canonical manifest was checked"
        )

    def test_static_baseline_is_no_longer_the_published_baseline(self):
        """The demotion, pinned directly.

        Asserting only "dense_resident_2m is published_baseline" would still
        pass if static_baseline kept the role too and the multi-role check
        were ever relaxed.
        """
        for path in CANONICAL_MANIFEST_PATHS:
            manifest = self._load(path)
            metadata = manifest.lane_metadata.get("static_baseline")
            self.assertIsNotNone(metadata, f"{path.name}: static_baseline lane was deleted")
            self.assertNotEqual(
                str(metadata.get("evidence_role", "")).strip(),
                _manifest_mod.PUBLISHED_BASELINE_EVIDENCE_ROLE,
                f"{path.name}: static_baseline is a 10k-splat smoke lane and must not "
                "publish the headline figure (#790)",
            )
            self.assertTrue(
                str(metadata.get("evidence_role", "")).strip(),
                f"{path.name}: static_baseline must keep an explicit evidence_role",
            )

    def test_published_baseline_is_not_a_lightweight_smoke_lane(self):
        for path in CANONICAL_MANIFEST_PATHS:
            manifest = self._load(path)
            metadata = manifest.lane_metadata[PUBLISHED_BASELINE_LANE_ID]
            self.assertNotIn(
                str(metadata.get("asset_classification", "")).strip(),
                _manifest_mod.LIGHTWEIGHT_SMOKE_CLASSIFICATIONS,
                f"{path.name}: the published baseline may not be classified as a smoke lane",
            )
            policy = _manifest_mod.resolve_lane_asset_policy(
                manifest, lane_id=PUBLISHED_BASELINE_LANE_ID, scene_path=""
            )
            self.assertFalse(
                _manifest_mod._is_lightweight_smoke_asset(policy.asset_path),
                f"{path.name}: the published baseline resolves to the smoke fixture "
                f"{policy.asset_path}",
            )

    def test_committed_manifests_satisfy_the_published_baseline_policy(self):
        for path in CANONICAL_MANIFEST_PATHS:
            manifest = self._load(path)
            self.assertEqual(
                _manifest_mod.validate_published_baseline_policy(manifest),
                [],
                f"{path.name} violates the published-baseline policy",
            )

    def test_published_baseline_lane_is_enrolled_in_the_performance_profile(self):
        """A role with no enrollment publishes nothing.

        Membership and weight are read out of run_benchmark.py rather than
        restated here, so this fails if the lane is dropped from the profile
        or re-zeroed.
        """
        performance_lanes = _run_benchmark.PROFILE_DEFAULT_LANE_IDS["performance"]
        self.assertIn(
            PUBLISHED_BASELINE_LANE_ID,
            performance_lanes,
            "the published baseline must run in the profile that produces published numbers",
        )
        lane = next(
            l for l in _run_benchmark.LANES if l.lane_id == PUBLISHED_BASELINE_LANE_ID
        )
        self.assertIn(
            "performance",
            lane.durations,
            "the published baseline must define a performance-profile duration",
        )
        self.assertGreater(
            lane.weights.get("performance", 0.0),
            0.0,
            "a zero-weight published baseline cannot influence the aggregate score (#790)",
        )

    def test_committed_manifests_match_their_generator(self):
        """The manifests are generated; a hand-edit is reverted by the guard lane.

        `run_module_tests.py` runs `prepare_synthetic_assets.py` before the
        guards, which rewrites both canonical manifests from `LANE_METADATA`.
        Without this check, a taxonomy change made only in the JSON looks
        committed and is erased on the next guard run.
        """
        expected = json.dumps(
            _prepare._benchmark_asset_manifest(), indent=2, sort_keys=True
        ) + "\n"
        for path in CANONICAL_MANIFEST_PATHS:
            actual = path.read_text(encoding="utf-8")
            self.assertEqual(
                json.loads(actual),
                json.loads(expected),
                f"{path.name} has drifted from prepare_synthetic_assets.py; regenerate it "
                "instead of hand-editing the JSON",
            )

    def _manifest_from(self, tmp: str, lane_metadata: dict, lane_defaults: dict):
        path = Path(tmp) / "manifest.json"
        path.write_text(
            json.dumps(
                {
                    "default_asset": "res://tests/fixtures/test_splats.ply",
                    "lane_defaults": lane_defaults,
                    "scene_defaults": {},
                    "lane_metadata": lane_metadata,
                }
            ),
            encoding="utf-8",
        )
        return self._load(path)

    def test_policy_rejects_a_lightweight_smoke_published_baseline(self):
        """Discrimination proof: the exact pre-#790 shape must fail."""
        with tempfile.TemporaryDirectory() as tmp:
            manifest = self._manifest_from(
                tmp,
                {
                    "smoke_lane": {
                        "asset_classification": "lightweight_smoke",
                        "evidence_role": "published_baseline",
                    }
                },
                {"smoke_lane": TEST_SPLATS_ASSET},
            )
            failures = _manifest_mod.validate_published_baseline_policy(manifest)
            self.assertTrue(failures, "a smoke-asset published baseline must be rejected")
            self.assertTrue(
                any("asset_classification=lightweight_smoke" in f for f in failures),
                f"the classification violation must be named: {failures}",
            )
            self.assertTrue(
                any("lightweight smoke asset" in f for f in failures),
                f"the resolved-asset violation must be named: {failures}",
            )

    def test_policy_rejects_a_smoke_asset_behind_an_honest_looking_classification(self):
        """Relabelling the classification must not launder the workload."""
        with tempfile.TemporaryDirectory() as tmp:
            manifest = self._manifest_from(
                tmp,
                {
                    "mislabelled": {
                        "asset_classification": "deterministic_synthetic",
                        "evidence_role": "published_baseline",
                    }
                },
                {"mislabelled": TEST_SPLATS_ASSET},
            )
            failures = _manifest_mod.validate_published_baseline_policy(manifest)
            self.assertTrue(
                any("lightweight smoke asset" in f for f in failures),
                f"a smoke asset must be caught by path even when relabelled: {failures}",
            )

    def test_policy_rejects_more_than_one_published_baseline(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = self._manifest_from(
                tmp,
                {
                    "lane_a": {
                        "asset_classification": "deterministic_synthetic",
                        "evidence_role": "published_baseline",
                    },
                    "lane_b": {
                        "asset_classification": "deterministic_synthetic",
                        "evidence_role": "published_baseline",
                    },
                },
                {
                    "lane_a": "res://tests/fixtures/synthetic_spiral.ply",
                    "lane_b": "res://tests/fixtures/synthetic_sphere.ply",
                },
            )
            failures = _manifest_mod.validate_published_baseline_policy(manifest)
            self.assertTrue(
                any("more than one" in f for f in failures),
                f"two published baselines must be rejected: {failures}",
            )

    def test_policy_still_discriminates_and_does_not_reject_everything(self):
        """A guard that rejects every input is the same bug wearing a hat.

        Satellite manifests publish nothing (the Steam Deck project carries
        only handheld smoke lanes), and an honest published baseline on a
        non-smoke asset must pass.
        """
        with tempfile.TemporaryDirectory() as tmp:
            no_baseline = self._manifest_from(
                tmp,
                {
                    "handheld": {
                        "asset_classification": "deterministic_synthetic",
                        "evidence_role": "handheld_smoke",
                    }
                },
                {"handheld": "res://tests/fixtures/synthetic_sphere.ply"},
            )
            self.assertEqual(
                _manifest_mod.validate_published_baseline_policy(no_baseline),
                [],
                "a manifest that publishes nothing must pass",
            )
        with tempfile.TemporaryDirectory() as tmp:
            honest = self._manifest_from(
                tmp,
                {
                    "dense": {
                        "asset_classification": "deterministic_synthetic",
                        "evidence_role": "published_baseline",
                    }
                },
                {"dense": "res://tests/fixtures/synthetic_spiral.ply"},
            )
            self.assertEqual(
                _manifest_mod.validate_published_baseline_policy(honest),
                [],
                "an honest published baseline must pass",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
