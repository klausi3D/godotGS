#!/usr/bin/env python3
"""Unit tests for verify_world_manifest.py (BV-05).

The fixture is constructed here, byte by byte from the plain v3 layout, because the guard
lane has no Godot binary. It does not stand in for the producer: the producer-side proof is
the run of this verifier on real generator output (PR evidence, and generate_world.py runs
it after every generation). What these cases prove is that each check can fail and names
what broke: a flipped gaussian byte, a flipped SH byte, a chunk-record field, a truncation,
an edited parameter, an edited content table and a wrong Tier A table.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("verify_world_manifest", HERE / "verify_world_manifest.py")
vwm = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vwm)

SH_HIGH = 12


def build_world(directory: Path, tier: str = "test", counts=(3, 5, 2)) -> Path:
    """A three-chunk plain v3 world and a manifest describing it."""
    n = sum(counts)
    gauss = bytearray()
    for i in range(n):
        g = bytearray(vwm.GAUSSIAN_BYTES)
        struct.pack_into("<3f", g, 0, float(i), 0.0, float(i % 4))  # position
        struct.pack_into("<f", g, 12, 0.5)  # opacity
        struct.pack_into("<3f", g, 16, 0.25, 0.25, 0.25)  # scale
        gauss += g
    sh = bytes((i * 7) & 0xFF for i in range(n * SH_HIGH * vwm.VECTOR3_BYTES))
    params = {"schema": "godotgs-world-generator-params/1", "tier": tier, "seed": 7}
    params_sha = vwm.sha256_text(vwm.canonical_json(params))
    gen_inputs = {"generator_params_sha": params_sha, "generator_id": "x", "generator_version": 1}
    gen_sha = vwm.sha256_text(vwm.canonical_json(gen_inputs))
    metadata = json.dumps({"gs_generator": {"gen_sha": gen_sha, "generator_params_sha": params_sha}}, sort_keys=True).encode()

    g_off = vwm.PREFIX_BYTES
    sh_off = g_off + len(gauss)
    ct_off = sh_off + len(sh)
    idx_off = ct_off + len(counts) * vwm.CHUNK_RECORD_BYTES
    md_off = idx_off + n * 4
    flags = vwm.FLAG_HAS_METADATA | vwm.FLAG_HAS_CHUNKS | vwm.FLAG_HAS_HIGH_SH
    header = bytearray(vwm.PREFIX_BYTES)
    struct.pack_into("<7I", header, 0, vwm.MAGIC, vwm.VERSION, flags, n, 3, 3, SH_HIGH)
    struct.pack_into("<I", header, 52, len(counts))
    struct.pack_into("<6Q", header, 56, g_off, sh_off, ct_off, idx_off, md_off, len(metadata))
    struct.pack_into("<2I", header, 104, vwm.LAYOUT_VERSION, 0)

    records = bytearray()
    chunks = []
    first = 0
    for c, cnt in enumerate(counts):
        aabb_min = [float(first) - 1.0, -1.0, -1.0]
        aabb_size = [float(cnt) + 1.0, 2.0, 5.0]
        center = [aabb_min[k] + aabb_size[k] / 2 for k in range(3)]
        rec = bytearray(vwm.CHUNK_RECORD_BYTES)
        struct.pack_into("<10f", rec, 0, *aabb_min, *aabb_size, *center, 3.0)
        struct.pack_into("<QII", rec, 40, first, cnt, 0)
        records += rec
        g_bytes = cnt * vwm.GAUSSIAN_BYTES
        s_bytes = cnt * SH_HIGH * vwm.VECTOR3_BYTES
        chunks.append({
            "index": c, "first_splat": first, "count": cnt, "class_counts": [cnt, 0, 0, 0, 0],
            "aabb": aabb_min + [aabb_min[k] + aabb_size[k] for k in range(3)], "center": center, "radius": 3.0,
            "gaussian_offset": g_off + first * vwm.GAUSSIAN_BYTES, "gaussian_bytes": g_bytes,
            "gaussian_sha256": hashlib.sha256(gauss[first * vwm.GAUSSIAN_BYTES:first * vwm.GAUSSIAN_BYTES + g_bytes]).hexdigest(),
            "sh_offset": sh_off + first * SH_HIGH * vwm.VECTOR3_BYTES, "sh_bytes": s_bytes,
            "sh_sha256": hashlib.sha256(sh[first * SH_HIGH * 12:first * SH_HIGH * 12 + s_bytes]).hexdigest(),
        })
        first += cnt
    indices = struct.pack(f"<{n}I", *range(n))
    blob = bytes(header) + bytes(gauss) + sh + bytes(records) + indices + metadata
    world = directory / "world.gsplatworld"
    world.write_bytes(blob)
    table = {name: {"splats": 0, "instances": 0} for name in vwm.CLASSES}
    table["plain"]["splats"] = n
    table["total"] = n
    manifest = {
        "schema": vwm.MANIFEST_SCHEMA, "generator_params": params, "generator_params_sha": params_sha,
        "gen_sha": gen_sha, "gen_sha_inputs": gen_inputs, "content_table": table, "chunks": chunks,
        "world": {"file_bytes": len(blob), "file_sha256": hashlib.sha256(blob).hexdigest(), "splat_count": n,
                  "chunk_count": len(counts), "sh_degree": 3, "sh_first_order": 3, "sh_high_order": SH_HIGH},
    }
    (directory / "world.manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return world


class VerifyWorldManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.world = build_world(self.dir)
        self.manifest_path = self.dir / "world.manifest.json"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def problems(self) -> list:
        result = vwm.verify(self.world)
        return result["problems"]

    def flip(self, offset: int) -> None:
        b = bytearray(self.world.read_bytes())
        b[offset] ^= 0x10
        self.world.write_bytes(bytes(b))

    def edit_manifest(self, fn) -> None:
        m = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        fn(m)
        self.manifest_path.write_text(json.dumps(m), encoding="utf-8")

    def manifest(self) -> dict:
        return json.loads(self.manifest_path.read_text(encoding="utf-8"))

    def test_pristine_world_verifies(self) -> None:
        result = vwm.verify(self.world)
        self.assertTrue(result["ok"], result["problems"])
        self.assertEqual(len(result["checked"]), 2)

    def test_gaussian_byte_flip_names_the_chunk(self) -> None:
        self.flip(self.manifest()["chunks"][1]["gaussian_offset"] + 3)
        p = self.problems()
        self.assertIn("chunk_digest_mismatch: chunk 1 gaussian_sha256", p)
        self.assertIn("file_sha256_mismatch", p)
        self.assertFalse(any("chunk 0" in x or "chunk 2" in x for x in p), p)

    def test_sh_byte_flip_names_the_chunk(self) -> None:
        self.flip(self.manifest()["chunks"][2]["sh_offset"])
        self.assertIn("chunk_digest_mismatch: chunk 2 sh_sha256", self.problems())

    def test_chunk_record_radius_flip(self) -> None:
        m = self.manifest()
        ct = m["chunks"][2]["sh_offset"] + m["chunks"][2]["sh_bytes"]
        self.flip(ct + 1 * vwm.CHUNK_RECORD_BYTES + 36)
        self.assertIn("chunk_record_mismatch: chunk 1 centre/radius", self.problems())

    def test_index_list_corruption(self) -> None:
        m = self.manifest()
        ct = m["chunks"][2]["sh_offset"] + m["chunks"][2]["sh_bytes"]
        self.flip(ct + 3 * vwm.CHUNK_RECORD_BYTES + 4 * 4)
        self.assertIn("index lists are not the identity ranges of the stream", self.problems())

    def test_truncation(self) -> None:
        self.world.write_bytes(self.world.read_bytes()[:-1])
        self.assertTrue(any(x.startswith("file_bytes_mismatch") for x in self.problems()))

    def test_edited_parameter_breaks_generator_params_sha(self) -> None:
        self.edit_manifest(lambda m: m["generator_params"].__setitem__("seed", 8))
        self.assertIn("generator_params_sha_mismatch", self.problems())

    def test_edited_content_table(self) -> None:
        def edit(m):
            m["chunks"][0]["class_counts"] = [2, 1, 0, 0, 0]
        self.edit_manifest(edit)
        p = self.problems()
        self.assertTrue(any("content_table: plain" in x for x in p), p)
        self.assertTrue(any("content_table: town" in x for x in p), p)

    def test_class_counts_must_sum_to_count(self) -> None:
        self.edit_manifest(lambda m: m["chunks"][1].__setitem__("class_counts", [4, 0, 0, 0, 0]))
        self.assertIn("chunk 1: class counts do not sum to its count", self.problems())

    def test_tier_a_table_must_equal_the_adr(self) -> None:
        world = build_world(self.dir, tier="A")
        p = vwm.verify(world)["problems"]
        self.assertTrue(any("Tier A differs from ADR" in x for x in p), p)

    def test_missing_manifest(self) -> None:
        self.manifest_path.unlink()
        self.assertTrue(self.problems()[0].startswith("manifest_missing"))

    def test_canonical_json_matches_the_generator_form(self) -> None:
        # Sorted keys, no whitespace: the form generator_params_sha is defined over.
        self.assertEqual(vwm.canonical_json({"b": 1, "a": {"d": [1, 2], "c": "x"}}), '{"a":{"c":"x","d":[1,2]},"b":1}')


if __name__ == "__main__":
    unittest.main()
