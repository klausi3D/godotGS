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


#: The property set a real Gaussian fixture declares: REQUIRED_PLY_PROPERTIES, read
#: from the module under test rather than restated (#934 review). The helper writes
#: this by default so tests exercise the header shape the generators actually produce;
#: `gaussian=False` writes a bare point cloud -- what a fixture swapped for an
#: unrelated file looks like.
_GAUSSIAN_PLY_PROPERTIES = tuple(_prepare.REQUIRED_PLY_PROPERTIES)


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
    # The identity rotation, not zeros: an all-zero quaternion is a value the
    # loader normalizes into NaN and refuses, so a zero body is not a fixture.
    record = [0.0] * len(props)
    if "rot_0" in props:  # a bare point cloud (gaussian=False) has no rotation
        record[props.index("rot_0")] = 1.0
    body = b"" if header_only else struct.pack(f"<{len(props)}f", *record) * vertex_count
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

            # `_generate_via_godot` is stubbed to "succeeded" without writing the other
            # primaries, so the producer record -- which hashes every file it is told
            # was produced (#969) -- is stubbed too, and asked the question that
            # matters where preservation meets provenance: a copy left in place is
            # RETAINED, never claimed as written by this run.
            with mock.patch.object(_prepare, "CANONICAL_SPECS", (spec,)), \
                    mock.patch.object(_prepare, "_generate_via_godot", return_value=True), \
                    mock.patch.object(_prepare, "_write_manifest"), \
                    mock.patch.object(_prepare, "FORBIDDEN_LEGACY_PLYS", ()), \
                    mock.patch.object(_prepare, "FORBIDDEN_LEGACY_ASSET_DIRS", ()), \
                    mock.patch.object(
                        _prepare.fixture_provenance, "record_producer_output", return_value=True
                    ) as record:
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
            produced = record.call_args.args[1]
            retained = record.call_args.kwargs["retain"]
            self.assertNotIn(consumer, produced, "a preserved copy was recorded as this run's output")
            self.assertIn(consumer, retained, "a preserved copy's provenance would be pruned")

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


class FloorGateChecksTheBodyNotTheClaimTests(unittest.TestCase):
    """#934 review: `--require-asset-floors` believed a header (#934 round 8).

    Round 7 taught the STAGING path that a declared vertex count is a claim the
    file makes about itself. Every decision about the PUBLISHED corpus still
    rested on that claim: `asset_floor_failures()` -- the gate the flag runs and
    the runtime harness depends on -- read `read_ply_vertex_count()` and nothing
    else. A fixture truncated by a short write, an interrupted copy or a killed
    job keeps a header claiming enough splats, so the gate passed it and the
    lanes measured it.
    """

    ASSET = "res://tests/fixtures/test_splats.ply"

    def _corpus(self, root: Path, *, vertices: int) -> list[Path]:
        written = []
        for relative in (
            Path("tests/fixtures/test_splats.ply"),
            Path("tests/examples/godot/test_project/tests/fixtures/test_splats.ply"),
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            _write_ply(path, vertices)
            written.append(path)
        return written

    def test_a_truncated_fixture_does_not_satisfy_its_floor(self):
        with tempfile.TemporaryDirectory() as raw_td:
            root = Path(raw_td)
            copies = self._corpus(root, vertices=10000)
            whole = copies[0].read_bytes()
            copies[0].write_bytes(whole[:-24])  # a short write: header intact

            self.assertEqual(
                read_ply_vertex_count(copies[0]),
                10000,
                "the truncated file no longer claims enough splats; the case is moot",
            )
            with mock.patch.dict(
                _prepare.ASSET_MIN_SPLAT_COUNTS, {self.ASSET: 10000}, clear=True
            ):
                failures = _prepare.asset_floor_failures(root)

        self.assertEqual(
            len(failures),
            1,
            f"a truncated fixture satisfied its floor on its header alone: {failures}",
        )
        self.assertIn("INCOMPLETE", failures[0])

    def test_a_whole_fixture_at_the_floor_still_passes(self):
        """Discrimination: the check must not start rejecting real fixtures.

        Both producers write bodies this reader has to accept -- see
        RealFixtureCorpusTests, which runs the same predicate over the committed
        corpus rather than over a file this test invented.
        """
        with tempfile.TemporaryDirectory() as raw_td:
            root = Path(raw_td)
            self._corpus(root, vertices=10000)
            with mock.patch.dict(
                _prepare.ASSET_MIN_SPLAT_COUNTS, {self.ASSET: 10000}, clear=True
            ):
                self.assertEqual(_prepare.asset_floor_failures(root), [])

    def test_values_the_loader_refuses_are_not_a_fixture(self):
        """#934 review: complete and schema-correct does not mean loadable.

        Mirrors the loader: every render field must be finite
        (`GaussianData::all_render_fields_finite()`), `exp(scale_n)` must not
        overflow, and a rotation must not be all zero (`normalize()` divides by its
        length). Each case is otherwise a whole fixture above its floor.
        """
        props = list(_prepare.REQUIRED_PLY_PROPERTIES)
        inf = float("inf")
        cases = {
            "nan position": {"x": float("nan")},
            "infinite colour": {"f_dc_0": inf},
            "opposite infinities": {"y": inf, "z": -inf},
            "zero rotation": {"rot_0": 0.0},
            "negative-zero rotation": {"rot_0": -0.0, "rot_1": -0.0, "rot_2": -0.0, "rot_3": -0.0},
            # #934 review: nonzero, but every square rounds to a float32 zero.
            "underflowing rotation": {"rot_0": 1e-30},
            "subnormal rotation": {"rot_0": 1e-40, "rot_2": -1e-40},
            # Just below the float32 rounding edge: 2e-23 squared is ~4e-46, under half
            # of the smallest subnormal (~1.4e-45), so it rounds to zero.
            "boundary-underflowing rotation": {"rot_0": 2e-23},
            "overflowing scale": {"scale_2": _prepare.MAX_LOG_SCALE + 1.0},
        }
        with tempfile.TemporaryDirectory() as tmp:
            for label, overrides in cases.items():
                with self.subTest(case=label):
                    ply = Path(tmp) / f"{label.replace(' ', '_')}.ply"
                    _write_ply(ply, 32)
                    data = bytearray(ply.read_bytes())
                    body_start = data.index(b"end_header\n") + len(b"end_header\n")
                    for prop, value in overrides.items():
                        offset = body_start + (9 * len(props) + props.index(prop)) * 4
                        data[offset:offset + 4] = struct.pack("<f", value)
                    ply.write_bytes(bytes(data))
                    self.assertIsNone(_prepare.ply_payload_failure(ply))
                    self.assertIsNone(_prepare.ply_schema_failure(ply))
                    problem = _prepare.ply_value_failure(ply)
                    self.assertIsNotNone(problem, f"a fixture with a {label} was accepted")
                    self.assertIn("splat 9", problem)
                    if "rotation" in label:
                        self.assertIn("float32 length is zero", problem)
                    self.assertIn("UNLOADABLE", _prepare.fixture_floor_failure(ply, 32) or "")

            # Discrimination: legal extremes the loader accepts are accepted.
            edge = Path(tmp) / "edge.ply"
            _write_ply(edge, 32)
            data = bytearray(edge.read_bytes())
            body_start = data.index(b"end_header\n") + len(b"end_header\n")
            for splat, overrides in {
                5: {"scale_0": 88.0, "scale_1": -300.0, "rot_0": 0.0, "rot_3": 1e-3},
                # Tiny but NOT underflowing: 1e-20 is above the emulation bound, and
                # 5e-23 is below it yet squares to ~2.5e-45, a nonzero float32 subnormal.
                6: {"rot_0": 1e-20},
                7: {"rot_0": 5e-23},
            }.items():
                for prop, value in overrides.items():
                    offset = body_start + (splat * len(props) + props.index(prop)) * 4
                    data[offset:offset + 4] = struct.pack("<f", value)
            edge.write_bytes(bytes(data))
            self.assertIsNone(_prepare.ply_value_failure(edge))
            self.assertIsNone(_prepare.fixture_floor_failure(edge, 32))

    def test_the_python_producers_real_output_is_loadable(self):
        """Coupling: the check must accept what the fallback producer really writes."""
        spec = _prepare.CANONICAL_SPECS[0]
        with tempfile.TemporaryDirectory() as tmp:
            ply = Path(tmp) / Path(spec.relative_path).name
            _prepare._write_ply(ply, _prepare._generate_rows(spec))
            self.assertIsNone(_prepare.ply_payload_failure(ply))
            self.assertIsNone(_prepare.ply_schema_failure(ply))
            self.assertIsNone(_prepare.ply_value_failure(ply))

    def test_the_predicate_separates_the_four_ways_a_fixture_fails(self):
        with tempfile.TemporaryDirectory() as raw_td:
            root = Path(raw_td)

            whole = root / "whole.ply"
            _write_ply(whole, 64)
            self.assertIsNone(_prepare.fixture_floor_failure(whole, 64))

            self.assertEqual(
                _prepare.fixture_floor_failure(root / "absent.ply", 64), "MISSING"
            )

            small = root / "small.ply"
            _write_ply(small, 8)
            self.assertIn("UNDERSIZED", _prepare.fixture_floor_failure(small, 64) or "")

            truncated = root / "truncated.ply"
            _write_ply(truncated, 64)
            truncated.write_bytes(truncated.read_bytes()[:-4])
            self.assertIn(
                "INCOMPLETE", _prepare.fixture_floor_failure(truncated, 64) or ""
            )

            junk = root / "junk.ply"
            junk.write_bytes(b"not a ply at all\n")
            self.assertIsNotNone(_prepare.fixture_floor_failure(junk, 64))


def _tracked_fixture_plys() -> "list[Path]":
    """The `.ply` files git actually tracks, asked of git rather than of the disk.

    "Committed" and "present" are different questions, and this file got them
    confused: `tests/fixtures/*.ply` and the consumer `test_splats.ply` are
    GITIGNORED and generated, so on a runner that has already run the fallback
    prep they exist at the fallback's own size while their floor is the C++
    generator's count. A test that globbed the directory therefore asserted a
    property of whatever the last generation run happened to leave behind, and
    failed on CI for a corpus that was exactly what it should have been.

    Tracked fixtures are committed at a known size and must satisfy their floors.
    Generated ones are the prep script's business, and the prep script has its own
    guards for them.
    """
    result = subprocess.run(
        ["git", "ls-files", "-z", "*.ply"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return []
    paths = [ROOT / name for name in result.stdout.split("\0") if name]
    return sorted(path for path in paths if path.is_file())


class FixtureSchemaIsTheProducersSchemaTests(unittest.TestCase):
    """#934 review: a complete payload is not a splat fixture.

    `ply_payload_failure()` sizes the body from whatever properties the header
    declares, so a 10,000-vertex file holding only `property float x` and 40,000
    bytes is complete by that rule and clears every floor. The loader would not
    refuse it either: a missing property is filled with a default, so it would load
    as 10,000 splats at the origin -- a fixture that measures nothing, silently.
    """

    WRITER = ROOT / "modules" / "gaussian_splatting" / "tests" / "synthetic_ply_writer.cpp"
    LOADER = ROOT / "modules" / "gaussian_splatting" / "io" / "ply_loader.cpp"

    def _ply(self, path: Path, count: int, props) -> Path:
        header = (
            "ply\n"
            "format binary_little_endian 1.0\n"
            f"element vertex {count}\n"
            + "".join(f"property float {name}\n" for name in props)
            + "end_header\n"
        ).encode("ascii")
        record = [0.0] * len(props)
        if "rot_0" in props:
            record[list(props).index("rot_0")] = 1.0  # identity; zeros normalize to NaN
        path.write_bytes(header + struct.pack(f"<{len(props)}f", *record) * count)
        return path

    def test_a_one_property_file_is_complete_but_not_a_fixture(self):
        with tempfile.TemporaryDirectory() as tmp:
            ply = self._ply(Path(tmp) / "test_splats.ply", 10000, ["x"])
            self.assertIsNone(
                _prepare.ply_payload_failure(ply),
                "premise: the file is structurally complete, which is what let it through",
            )
            self.assertIn("missing required properties", _prepare.ply_schema_failure(ply) or "")
            self.assertIn(
                "NOT A SPLAT FIXTURE",
                _prepare.fixture_floor_failure(ply, 10000) or "",
                "a one-property file satisfied the test_splats.ply floor",
            )

    def test_a_duplicated_property_is_not_a_fixture(self):
        props = list(_prepare.REQUIRED_PLY_PROPERTIES) + ["x"]
        with tempfile.TemporaryDirectory() as tmp:
            ply = self._ply(Path(tmp) / "dup.ply", 8, props)
            self.assertIn("more than once", _prepare.ply_schema_failure(ply) or "")

    def test_the_required_schema_is_accepted_with_or_without_optional_blocks(self):
        """Discrimination: the C++ producer adds normals and f_rest_*, and must pass."""
        required = list(_prepare.REQUIRED_PLY_PROPERTIES)
        rich = required[:3] + ["nx", "ny", "nz"] + required[3:6] + [
            f"f_rest_{i}" for i in range(45)
        ] + required[6:]
        with tempfile.TemporaryDirectory() as tmp:
            for label, props in (("fallback", required), ("rich", rich)):
                with self.subTest(shape=label):
                    ply = self._ply(Path(tmp) / f"{label}.ply", 16, props)
                    self.assertIsNone(_prepare.ply_schema_failure(ply))
                    self.assertIsNone(_prepare.fixture_floor_failure(ply, 16))

    def test_a_property_declared_before_the_vertex_element_is_not_counted(self):
        """#934 review: the loader scopes properties to elements; so must this.

        `PLYLoader::parse_header()` adds a property to the vertex schema only while
        the current element is `vertex`. Moving `property float x` above
        `element vertex` therefore keeps 14 declared floats and a matching byte
        count -- both checks passed -- while the loader reads 13 at the wrong stride
        with `x` missing.
        """
        props = list(_prepare.REQUIRED_PLY_PROPERTIES)
        count = 16
        with tempfile.TemporaryDirectory() as tmp:
            header = (
                "ply\n"
                "format binary_little_endian 1.0\n"
                f"property float {props[0]}\n"
                f"element vertex {count}\n"
                + "".join(f"property float {name}\n" for name in props[1:])
                + "end_header\n"
            )
            misplaced = Path(tmp) / "misplaced.ply"
            misplaced.write_bytes(header.encode("ascii") + b"\x00" * (count * 4 * len(props)))

            self.assertIn("before the vertex element", _prepare.ply_payload_failure(misplaced) or "")
            self.assertNotIn(
                props[0],
                _prepare.ply_property_names(misplaced) or (),
                "a property declared outside the vertex element was read as a vertex property",
            )
            self.assertIsNotNone(_prepare.ply_schema_failure(misplaced))
            self.assertIsNotNone(_prepare.fixture_floor_failure(misplaced, count))

            # Discrimination: the same properties inside the element are accepted.
            placed = self._ply(Path(tmp) / "placed.ply", count, props)
            self.assertEqual(_prepare.ply_property_names(placed), tuple(props))
            self.assertIsNone(_prepare.fixture_floor_failure(placed, count))

    def test_a_missing_or_wrong_format_line_is_not_a_fixture(self):
        """#934 review: the loader reads the body as ASCII unless told otherwise.

        `PLYLoader::PLYHeader::is_binary` defaults to false and only a `format`
        line sets it, so a binary payload under a missing or corrupt declaration is
        handed to the ASCII parser and rejected. Every other check here -- the
        properties, the byte count, the floor -- still matched that file.
        """
        props = list(_prepare.REQUIRED_PLY_PROPERTIES)
        body_per_vertex = b"\x00" * (4 * len(props))
        cases = {
            "no format line": None,
            "ascii": "format ascii 1.0",
            "big endian": "format binary_big_endian 1.0",
            "corrupt": "format binary_little_endian 9.9",
            "two format lines": "format binary_little_endian 1.0\nformat binary_little_endian 1.0",
        }
        with tempfile.TemporaryDirectory() as tmp:
            for label, fmt in cases.items():
                with self.subTest(case=label):
                    header = "ply\n" + (fmt + "\n" if fmt else "") + (
                        "element vertex 16\n"
                        + "".join(f"property float {name}\n" for name in props)
                        + "end_header\n"
                    )
                    ply = Path(tmp) / f"{label.replace(' ', '_')}.ply"
                    ply.write_bytes(header.encode("ascii") + body_per_vertex * 16)
                    self.assertIsNotNone(
                        _prepare.ply_payload_failure(ply),
                        f"a PLY with {label} was accepted as a complete binary fixture",
                    )
                    self.assertIsNotNone(_prepare.fixture_floor_failure(ply, 16))

            # Discrimination: the producers' own declaration is accepted.
            good = self._ply(Path(tmp) / "good.ply", 16, props)
            self.assertIsNone(_prepare.ply_payload_failure(good))

    def test_the_required_format_is_the_one_the_cpp_writer_emits(self):
        """Read from the fallback's header; the C++ producer must write the same line."""
        writer = self.WRITER.read_text(encoding="utf-8")
        expected = _prepare.REQUIRED_PLY_FORMAT_LINE.decode("ascii")
        self.assertIn(
            f'header += "{expected}\\n";',
            writer,
            f"the C++ writer does not emit {expected!r}; staged producer output would be "
            "rejected on every run",
        )

    def test_every_required_property_is_an_unconditional_cpp_emission(self):
        """The set is read from the fallback's header; the C++ producer must agree.

        If the fallback gained a property the C++ writer does not ALWAYS write,
        staged producer output would be rejected on every run. "Always" is the
        load-bearing word: normals and f_rest_* are emitted only under their
        `p_write_*` flags, so they may not be required.
        """
        unconditional: set[str] = set()
        conditional_depth = 0
        for line in self.WRITER.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if conditional_depth == 0 and stripped.startswith("if (p_write"):
                conditional_depth = stripped.count("{") - stripped.count("}")
                continue
            if conditional_depth > 0:
                conditional_depth += stripped.count("{") - stripped.count("}")
                continue
            match = re.search(r'header \+= "property float ([A-Za-z0-9_]+)\\n"', stripped)
            if match:
                unconditional.add(match.group(1))
        self.assertTrue(unconditional, "no unconditional emissions parsed; this guard is vacuous")
        self.assertNotIn("nx", unconditional, "the conditional-block parse is not working")
        missing = [name for name in _prepare.REQUIRED_PLY_PROPERTIES if name not in unconditional]
        self.assertEqual(
            missing,
            [],
            f"required properties the C++ producer does not always write: {missing}",
        )

    def test_every_required_property_is_one_the_loader_reads(self):
        """Required means the loader USES it -- otherwise the rule is ceremony."""
        loader = self.LOADER.read_text(encoding="utf-8")
        for name in _prepare.REQUIRED_PLY_PROPERTIES:
            with self.subTest(property=name):
                if name.startswith("f_dc_"):
                    self.assertIn('vformat("f_dc_%d", c)', loader)
                else:
                    self.assertIn(f'find_property_index("{name}")', loader)


class RealFixtureCorpusTests(unittest.TestCase):
    """The completeness and floor rules, run over fixtures nobody in this file invented.

    A body-length rule is only safe if it accepts what the producers actually
    write, and a floor is only meaningful if the corpus committed at it clears it.
    Both read the tracked corpus in the repository rather than fixtures authored
    to match the rules.
    """

    def test_every_tracked_fixture_is_complete(self):
        tracked = _tracked_fixture_plys()
        self.assertTrue(tracked, "git tracks no .ply fixtures; this test is vacuous")
        for path in tracked:
            with self.subTest(fixture=str(path.relative_to(ROOT))):
                self.assertIsNone(
                    _prepare.ply_payload_failure(path),
                    f"{path.name} is a tracked fixture the completeness rule rejects",
                )
                self.assertIsNone(
                    _prepare.ply_schema_failure(path),
                    f"{path.name} is a tracked fixture the schema rule rejects",
                )

    def test_every_tracked_fixture_satisfies_its_own_floor(self):
        checked = 0
        for path in _tracked_fixture_plys():
            resource_path = f"res://tests/fixtures/{path.name}"
            required = _prepare.ASSET_MIN_SPLAT_COUNTS.get(resource_path, 0)
            if required <= 0:
                continue
            checked += 1
            with self.subTest(fixture=str(path.relative_to(ROOT))):
                self.assertIsNone(
                    _prepare.fixture_floor_failure(path, required),
                    f"{path.name} does not satisfy the floor it is committed at",
                )
        self.assertGreater(checked, 0, "no tracked fixture carried a floor; vacuous")

    def test_a_generated_fixture_is_not_judged_as_a_committed_one(self):
        """The regression this replaces: a fallback-sized generated file failed CI.

        `test_splats.ply` is gitignored on both paths and generated. A runner that
        has run the fallback prep holds it at 1024 splats against a floor of 10000
        -- correct behaviour for a generated corpus, and not something a test about
        the COMMITTED corpus may fail on.
        """
        generated = "res://tests/fixtures/test_splats.ply"
        fallback_counts = {
            Path(spec.relative_path).name: spec.count for spec in _prepare.CANONICAL_SPECS
        }
        self.assertGreater(
            _prepare.ASSET_MIN_SPLAT_COUNTS.get(generated, 0),
            fallback_counts.get("test_splats.ply", 0),
            "test_splats.ply's floor no longer exceeds its fallback size; this case is moot",
        )
        tracked_names = {path.name for path in _tracked_fixture_plys()}
        self.assertNotIn(
            "test_splats.ply",
            tracked_names,
            "test_splats.ply is tracked now; the floor tests above must cover it",
        )


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


class StagingReplacesTheQuarantineTests(unittest.TestCase):
    """The C++ producer writes into staging; the canonical corpus is not moved aside.

    #969 proved fresh output by moving the existing fixtures into a quarantine
    (`.pre_cpp_generation`) before the producer wrote straight into
    `tests/fixtures`, and restoring them on failure -- with an `.unrestored.json`
    marker to tell an original a failed restore stranded from a superseded copy.
    #934 proves it by giving the producer an EMPTY staging directory and publishing
    only a validated corpus, all or nothing. The reconciliation keeps staging: the
    canonical files are never touched until the new corpus is known good, so the
    states the marker existed to disambiguate cannot arise. What each quarantine
    test guaranteed is held here or in
    `SelectedProducerFailureIsNotSuccess` (test_runtime_validation_proof_contract.py):
    partial or failed output never reaches the canonical paths, a truncated or
    unloadable corpus is refused, a complete one is accepted, a producer that
    writes nothing fails even beside fresh-looking leftovers, and a failed publish
    rolls back.
    """

    def _corpus(self, out: Path, vertices: int = 32) -> dict:
        written = {}
        for name in sorted(_prepare.CPP_GENERATED_FILENAMES):
            _write_ply(out / name, vertices)
            written[name] = (out / name).read_bytes()
        return written

    def test_the_producer_writes_into_staging_and_the_originals_stay_in_place(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            originals = self._corpus(out)
            seen = {}

            def fake_run(cmd, **kwargs):
                staging = Path(kwargs["env"]["SYNTHETIC_PLY_OUTPUT_DIR"])
                seen["staging"] = staging
                # While the producer runs, every original is still where it was.
                seen["originals_in_place"] = all(
                    (out / name).read_bytes() == data for name, data in originals.items()
                )
                return subprocess.CompletedProcess(cmd, 1, "", "boom")

            with mock.patch.object(_prepare.subprocess, "run", side_effect=fake_run):
                with contextlib.redirect_stdout(io.StringIO()):
                    accepted = _prepare._generate_via_godot(Path("godot"), out, quiet=True)

            self.assertFalse(accepted)
            self.assertNotEqual(seen["staging"], out, "the producer wrote into the canonical directory")
            self.assertTrue(seen["staging"].name.startswith(_prepare.STAGING_DIR_PREFIX))
            self.assertTrue(seen["originals_in_place"], "the originals were moved aside")
            self.assertFalse((out / _prepare.LEGACY_QUARANTINE_DIRNAME).exists())

    def test_a_failed_producer_that_wrote_partial_output_leaves_the_corpus_as_it_was(self):
        """#969's first quarantine test, restated: debris never reaches the canonical paths."""
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            originals = self._corpus(out)
            names = sorted(originals)

            def fake_run(cmd, **kwargs):
                staging = Path(kwargs["env"]["SYNTHETIC_PLY_OUTPUT_DIR"])
                (staging / names[0]).write_bytes(b"ply\npartial")
                return subprocess.CompletedProcess(cmd, 1, "", "died half way")

            with mock.patch.object(_prepare.subprocess, "run", side_effect=fake_run):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertFalse(_prepare._generate_via_godot(Path("godot"), out, quiet=True))

            changed = sorted(n for n, data in originals.items() if (out / n).read_bytes() != data)
            self.assertEqual(changed, [], "partial producer output replaced an original")
            leftovers = sorted(p.name for p in out.iterdir() if p.name.startswith(_prepare.STAGING_DIR_PREFIX))
            self.assertEqual(leftovers, [], "the staging directory outlived the run")

    def test_a_legacy_quarantine_is_reported_and_left_alone(self):
        """A directory an older prep left may hold originals; it is named, never read or deleted."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            legacy = root / "tests" / "fixtures" / _prepare.LEGACY_QUARANTINE_DIRNAME
            legacy.mkdir(parents=True)
            (legacy / "test_splats.ply").write_bytes(b"original")
            buffer = io.StringIO()
            with contextlib.redirect_stdout(buffer):
                _prepare._generate(root, quiet=True)
            self.assertIn(str(legacy), buffer.getvalue())
            self.assertIn("WARNING", buffer.getvalue())
            self.assertEqual((legacy / "test_splats.ply").read_bytes(), b"original")

    def test_a_legacy_quarantine_cannot_be_committed(self):
        probe = "tests/fixtures/.pre_cpp_generation/test_splats.ply"
        result = subprocess.run(
            ["git", "check-ignore", "--no-index", "-q", probe], cwd=ROOT, capture_output=True
        )
        self.assertEqual(result.returncode, 0, f"{probe} is not ignored; a leftover is committable")

    def test_no_quarantine_machinery_is_left_to_drift(self):
        """One mechanism, not two: the marker state machine is gone, not dormant."""
        for name in (
            "QuarantineStateError",
            "_read_unrestored",
            "_merge_unrestored",
            "_write_unrestored",
            "_clear_unrestored",
            "UNRESTORED_MARKER_FILENAME",
        ):
            self.assertFalse(hasattr(_prepare, name), f"{name} survived the reconciliation")


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


class PrepRunnerCorpusStatementTests(unittest.TestCase):
    """What each shared prep runner does with the corpus, and why -- stated truthfully.

    #969 pinned run_module_tests.py, run_runtime_validation.py and
    run_baseline_qa.py to the Python fallback, because the QA scene suite loaded
    test_splats.ply and its baseline and world bake were measured at 1024 splats
    (FallbackPinnedCorpusTests held that coupling). #991 gave the QA suite its own
    pinned qa_splats_1024.ply, and QaCorpusIsPinnedTest now holds THAT coupling, so
    the pin had nothing left to protect. #934's floor consumers take the C++ corpus
    and fail closed; the forwarding itself is pinned by
    `test_prep_command_requires_floors_and_forwards_the_binary` and
    `test_selected_fixture_consumer_passes_binary_to_asset_prep`.
    """

    RUNNERS = {
        "run_module_tests.py": ROOT / "tests" / "ci" / "run_module_tests.py",
        "run_runtime_validation.py": RUNTIME_DIR / "run_runtime_validation.py",
        "run_baseline_qa.py": ROOT / "tests" / "ci" / "run_baseline_qa.py",
    }

    def test_no_runner_still_claims_the_qa_corpus_pins_it(self):
        for name, path in self.RUNNERS.items():
            with self.subTest(runner=name):
                text = path.read_text(encoding="utf-8")
                self.assertNotIn("FIXTURE_CORPUS_BLOCKER", text)
                self.assertNotIn("requires rebaking the world", text)

    def test_baseline_qa_states_why_its_own_prep_uses_the_fallback(self):
        """The downgrade must be reported, not merely be true (#790)."""
        module = _load_module("_gs_baseline_qa_corpus_reason", self.RUNNERS["run_baseline_qa.py"])
        reason = getattr(module, "FALLBACK_CORPUS_REASON", "")
        self.assertTrue(reason, "run_baseline_qa.py no longer says why it preps the fallback")
        self.assertIn("qa_splats_1024.ply", reason)
        self.assertIn("--require-asset-floors", reason)
        self.assertIn(
            "FALLBACK_CORPUS_REASON",
            self.RUNNERS["run_baseline_qa.py"].read_text(encoding="utf-8").split("def prepare_synthetic_assets", 1)[1],
            "the reason is defined but never printed at the point of prep",
        )

    def test_the_qa_suite_no_longer_loads_the_file_the_floor_consumers_regenerate(self):
        """The premise of lifting the pin, asserted where the pin was lifted."""
        qa_scenes = ROOT / "tests" / "examples" / "godot" / "test_project" / "scenes" / "qa"
        loaders = sorted(
            path.name
            for path in qa_scenes.glob("*.tscn")
            if "res://tests/fixtures/test_splats.ply" in path.read_text(encoding="utf-8")
        )
        self.assertEqual(loaders, [], "a QA scene loads test_splats.ply again; the pin's reason is back")


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

    def _executed_prep_steps(self):
        """(workflow, job, step name, run script) for every step that RUNS the prep.

        Read through the GPU-environment guard's structural step parser rather than
        by grepping lines. The first version of this test matched any line that
        MENTIONED prepare_synthetic_assets.py, and #873 then added that path to
        release_builds.yml's `paths:` trigger filter -- two filter entries, neither
        of them an invocation, and this guard failed master for both. A path in a
        trigger filter, a step name or a comment is not execution; the sibling
        guard already refuses to count those (#918), so the same reader is reused
        instead of writing a second, looser one.
        """
        ci_dir = ROOT / "tests" / "ci"
        if str(ci_dir) not in sys.path:
            sys.path.insert(0, str(ci_dir))
        env_guard = _load_module(
            "_gs_preflight_runner_gpu_environment",
            ci_dir / "test_preflight_runner_gpu_environment.py",
        )
        found = []
        for path in env_guard.workflow_paths():
            lines = path.read_text(encoding="utf-8").splitlines()
            for job, (first, last) in sorted(env_guard.job_spans(lines).items()):
                job_lines = lines[first:last]
                if not any(line.strip() == "steps:" for line in job_lines):
                    continue  # a reusable-workflow call: no steps of its own to run
                for step in env_guard.workflow_steps(job_lines):
                    executed = [
                        line
                        for line in step.run.splitlines()
                        if "prepare_synthetic_assets.py" in line
                        and not line.lstrip().startswith("#")
                    ]
                    if executed:
                        found.append((path.name, job, step.name, step.run))
        return found

    def test_every_workflow_prep_invocation_passes_a_godot_binary(self):
        steps = self._executed_prep_steps()
        self.assertTrue(
            steps,
            "no workflow step runs prepare_synthetic_assets.py - this guard now covers nothing",
        )
        offenders = [
            f"{workflow}: job {job!r} step {name!r}"
            for workflow, job, name, run in steps
            if "--godot-binary" not in run
        ]
        self.assertEqual(
            offenders,
            [],
            "workflow prep calls without --godot-binary regenerate the lightweight "
            "Python-fallback fixtures (#790):\n  " + "\n  ".join(offenders),
        )

    def test_a_trigger_path_filter_is_not_a_prep_invocation(self):
        """The regression this replaces: two `paths:` entries failed master.

        Discrimination in both directions. The filter entries must exist -- if they
        stop existing, this case says so rather than passing over nothing -- and
        they must not be counted, while the real invocation in
        gaussian_production_gates.yml still is.
        """
        release = (WORKFLOW_DIR / "release_builds.yml").read_text(encoding="utf-8")
        filter_entries = [
            line for line in release.splitlines()
            if line.strip() == '- "tests/runtime/prepare_synthetic_assets.py"'
        ]
        self.assertTrue(
            filter_entries,
            "release_builds.yml no longer lists the prep script as a trigger path; "
            "this case no longer exercises anything",
        )
        steps = self._executed_prep_steps()
        self.assertNotIn(
            "release_builds.yml",
            {workflow for workflow, _job, _name, _run in steps},
            "a trigger path filter was counted as an invocation of the prep script",
        )
        self.assertIn(
            "gaussian_production_gates.yml",
            {workflow for workflow, _job, _name, _run in steps},
            "the real prep invocation is no longer found; the reader is too strict",
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
