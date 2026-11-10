#!/usr/bin/env python3
"""Unit tests for verify_world_manifest.py (BV-05).

The fixture is constructed here, byte by byte from the plain v3 layout, because the guard
lane has no Godot binary. It does not stand in for the producer: the producer-side proof is
the run of this verifier on real generator output (PR evidence, and generate_world.py runs
it after every generation). What these cases prove is that each check can fail and names
what broke: a flipped gaussian byte, a flipped SH byte, a chunk-record field, a truncation,
an edited parameter, an edited content table and a wrong Tier A table. For a GSQ v1 world
(plain v3 + bit 7, BV-C7; build_paged_world): a flipped page byte names its chunk's page
run, non-zero padding, a codec block that differs from the manifest's writer codec, an edited
decoded digest and a manifest whose codec disagrees with its codec block.
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
FLAT_CODEC = {"codec_id": 0, "sh_bits": [0, 0, 0], "opacity_bits": 0, "pos_step_m": 0.0, "zstd_level": 0}
GSQ_CODEC = {"codec_id": 1, "sh_bits": [6, 5, 5], "opacity_bits": 16, "pos_step_m": 0.00025, "zstd_level": 3}


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
        "writer": {"codec": dict(FLAT_CODEC)},
        "world": {"file_bytes": len(blob), "file_sha256": hashlib.sha256(blob).hexdigest(), "splat_count": n,
                  "chunk_count": len(counts), "sh_degree": 3, "sh_first_order": 3, "sh_high_order": SH_HIGH},
    }
    (directory / "world.manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return world


def build_paged_world(directory: Path, counts=(3, 5, 2), pages=(1, 2, 1)) -> Path:
    """A three-chunk plain v3 + bit 7 world: page runs of opaque stored bytes, 4 KiB aligned."""
    n = sum(counts)
    params = {"schema": "godotgs-world-generator-params/1", "tier": "test", "seed": 7}
    params_sha = vwm.sha256_text(vwm.canonical_json(params))
    gen_inputs = {"generator_params_sha": params_sha, "generator_id": "x", "generator_version": 1}
    gen_sha = vwm.sha256_text(vwm.canonical_json(gen_inputs))
    metadata = json.dumps({"gs_generator": {"gen_sha": gen_sha, "generator_params_sha": params_sha}}, sort_keys=True).encode()
    header_bytes = vwm.PREFIX_BYTES + vwm.PAGED_EXTENSION_BYTES
    body = bytearray(header_bytes)
    page_records = bytearray()
    records = bytearray()
    chunks = []
    decoded_set = hashlib.sha256()
    stored_total = 0
    first_splat = 0
    page_index = 0
    for c, (cnt, npages) in enumerate(zip(counts, pages)):
        per_page = [cnt // npages + (1 if k < cnt % npages else 0) for k in range(npages)]
        span_start = None
        for splats in per_page:
            body += bytes(-len(body) % vwm.PAGE_ALIGNMENT)
            off = len(body)
            span_start = off if span_start is None else span_start
            stored = bytes(((page_index * 31 + j) % 251) + 1 for j in range(100 + 17 * page_index))
            body += stored
            stored_total += len(stored)
            decoded = hashlib.sha256(b"decoded" + bytes([page_index])).digest()
            decoded_set.update(decoded)
            rec = bytearray(vwm.PAGED_PAGE_RECORD_BYTES)
            struct.pack_into("<QII32s32s6fII", rec, 0, off, len(stored), splats, hashlib.sha256(stored).digest(), decoded,
                             0, 0, 0, 1, 1, 1, c, 0)
            page_records += rec
            page_index += 1
        aabb_min = [float(first_splat) - 1.0, -1.0, -1.0]
        aabb_size = [float(cnt) + 1.0, 2.0, 5.0]
        center = [aabb_min[k] + aabb_size[k] / 2 for k in range(3)]
        rec = bytearray(vwm.PAGED_CHUNK_RECORD_BYTES)
        struct.pack_into("<10f", rec, 0, *aabb_min, *aabb_size, *center, 3.0)
        struct.pack_into("<4I", rec, 40, page_index - npages, npages, cnt, 0)
        records += rec
        span = bytes(body[span_start:])
        chunks.append({
            "index": c, "first_splat": first_splat, "count": cnt, "class_counts": [cnt, 0, 0, 0, 0],
            "aabb": aabb_min + [aabb_min[k] + aabb_size[k] for k in range(3)], "center": center, "radius": 3.0,
            "first_page": page_index - npages, "page_count": npages, "pages_offset": span_start,
            "pages_bytes": len(span), "pages_sha256": hashlib.sha256(span).hexdigest(),
        })
        first_splat += cnt
    ct_off = len(body)
    pt_off = ct_off + len(records)
    md_off = pt_off + len(page_records)
    flags = vwm.FLAG_HAS_METADATA | vwm.FLAG_HAS_CHUNKS | vwm.FLAG_HAS_HIGH_SH | vwm.FLAG_PAGED_CHUNKS
    struct.pack_into("<7I", body, 0, vwm.MAGIC, vwm.VERSION, flags, n, 3, 3, SH_HIGH)
    struct.pack_into("<I", body, 52, len(counts))
    struct.pack_into("<6Q", body, 56, chunks[0]["pages_offset"], 0, ct_off, 0, md_off, len(metadata))
    struct.pack_into("<2I", body, 104, vwm.LAYOUT_VERSION, 0)
    struct.pack_into("<I4BfiIIQ", body, vwm.PREFIX_BYTES, 1, 6, 5, 5, 16, 0.00025, 3, page_index, vwm.PAGE_ALIGNMENT, pt_off)
    blob = bytes(body) + bytes(records) + bytes(page_records) + metadata
    world = directory / "world.gsplatworld"
    world.write_bytes(blob)
    table = {name: {"splats": 0, "instances": 0} for name in vwm.CLASSES}
    table["plain"]["splats"] = n
    table["total"] = n
    manifest = {
        "schema": vwm.MANIFEST_SCHEMA, "generator_params": params, "generator_params_sha": params_sha,
        "gen_sha": gen_sha, "gen_sha_inputs": gen_inputs, "content_table": table, "chunks": chunks,
        "writer": {"codec": dict(GSQ_CODEC)},
        "codec": {"codec_id": 1, "page_count": page_index, "pages_stored_bytes": stored_total,
                  "decoded_digest_set_sha256": decoded_set.hexdigest()},
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


class VerifyPagedWorldTest(unittest.TestCase):
    """GSQ v1 worlds (BV-C7): each paged check can fail and names what broke."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.world = build_paged_world(self.dir)
        self.manifest_path = self.dir / "world.manifest.json"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def problems(self) -> list:
        return vwm.verify(self.world)["problems"]

    def manifest(self) -> dict:
        return json.loads(self.manifest_path.read_text(encoding="utf-8"))

    def edit_manifest(self, fn) -> None:
        m = self.manifest()
        fn(m)
        self.manifest_path.write_text(json.dumps(m), encoding="utf-8")

    def xor(self, offset: int, mask: int) -> None:
        b = bytearray(self.world.read_bytes())
        b[offset] ^= mask
        self.world.write_bytes(bytes(b))

    def test_pristine_paged_world_verifies(self) -> None:
        result = vwm.verify(self.world)
        self.assertTrue(result["ok"], result["problems"])
        self.assertTrue(any(x.startswith("GSQ file") for x in result["checked"]), result["checked"])

    def test_page_byte_flip_names_the_chunk(self) -> None:
        c = self.manifest()["chunks"][1]
        self.xor(c["pages_offset"] + c["pages_bytes"] - 5, 0x20)
        p = self.problems()
        self.assertIn("chunk_digest_mismatch: chunk 1 pages_sha256", p)
        self.assertIn("file_sha256_mismatch", p)
        self.assertFalse(any("chunk 0" in x or "chunk 2" in x for x in p), p)

    def test_padding_before_a_page_run_must_be_zero(self) -> None:
        self.xor(self.manifest()["chunks"][2]["pages_offset"] - 1, 0x07)
        self.assertIn("layout: the padding between pages is not zero", self.problems())

    def test_codec_block_must_be_the_writer_codec(self) -> None:
        self.edit_manifest(lambda m: m["writer"]["codec"].__setitem__("zstd_level", 9))
        self.assertTrue(any(x.startswith("codec_params_mismatch") for x in self.problems()))

    def test_decoded_digest_set_is_checked(self) -> None:
        ct_off = struct.unpack_from("<Q", self.world.read_bytes(), 72)[0]
        pt_off = ct_off + 3 * vwm.PAGED_CHUNK_RECORD_BYTES
        self.xor(pt_off + vwm.PAGED_PAGE_RECORD_BYTES + 48, 0x01)
        self.assertIn("codec_block_mismatch: decoded_digest_set_sha256", self.problems())

    def test_manifest_codec_must_match_its_codec_block(self) -> None:
        self.edit_manifest(lambda m: m["writer"].__setitem__("codec", dict(FLAT_CODEC)))
        self.assertTrue(any(x.startswith("codec_mismatch") for x in self.problems()))

    def test_content_check_refuses_a_gsq_world(self) -> None:
        p = vwm.verify(self.world, check_content=True)["problems"]
        self.assertTrue(any(x.startswith("--content re-reads raw splats") for x in p), p)


if __name__ == "__main__":
    unittest.main()
