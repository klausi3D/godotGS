#!/usr/bin/env python3
"""Independent verifier for BV-05 generated worlds and their sidecar manifests.

ADR docs/architecture/adr-beyond-vram-worlds.md §4.1 (sidecar manifest), W8 (digests per
chunk and per file), §6.2-§6.4 (content table, ground truth), V2 (manifest digests).

This is a second implementation, in Python and with nothing shared with the C++ generator
(modules/gaussian_splatting/io/gs_world_generator.cpp), of every check a consumer of the
world cache needs before trusting a world:

  * the manifest's generator_params_sha and gen_sha are the SHA-256 of the canonical JSON
    of the recorded inputs (sorted keys, no whitespace);
  * file size and whole-file SHA-256;
  * the plain v3 header (magic, version, flags, counts, layout word, section offsets) and
    the embedded generator metadata;
  * every chunk's gaussian and SH section digests, byte range by byte range;
  * for a GSQ v1 world (plain v3 + bit 7, BV-C7): the paged extension's codec block against
    the manifest's writer codec, the chunk and page tables (contiguous, aligned page runs),
    every chunk's page-run digest, the zero padding before each run, the stored bytes and
    the decoded digest set of the manifest's ``codec`` block (the pages themselves are not
    decoded here: that is the C++ verify, gs_world_generator::verify_world);
  * every chunk-table record (count, index range, AABB, centre, radius, bit for bit) and
    the identity index lists;
  * the content table: per-chunk class counts sum to the table, the table to the splat
    count, and for tier "A" the table equals the ADR §6.2 numbers written out below;
  * with --content (needs numpy): every splat is re-classified from its position against
    the manifest's features, and the per-chunk class counts, the "inside its cell" rule
    and the chunk-table AABBs are recomputed from the splats themselves.
  * V4 auto-tree sidecars: header, index-page and stored node digests, whole-file
    digest and the producer's tree key; decoded digest presence/format only.
    Actual GSQ decoding remains the builder's separate C++ W13 evidence.

Exit status: 0 when every check passes, 1 when any fails, 2 on usage errors.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
from pathlib import Path

MANIFEST_SCHEMA = "godotgs-world-manifest/1"
MAGIC = 0x57505347
VERSION = 3
PREFIX_BYTES = 112
CHUNK_RECORD_BYTES = 56
GAUSSIAN_BYTES = 144
VECTOR3_BYTES = 12
LAYOUT_VERSION = 1
FLAG_HAS_METADATA = 1 << 0
FLAG_HAS_CHUNKS = 1 << 2
FLAG_HAS_HIGH_SH = 1 << 3
FLAG_PAGED_CHUNKS = 1 << 7
PAGED_EXTENSION_BYTES = 64
PAGED_CHUNK_RECORD_BYTES = 56
PAGED_PAGE_RECORD_BYTES = 112
PAGE_ALIGNMENT = 4096
CODEC_NONE = 0
CODEC_GSQ_V1 = 1
CLASSES = ("plain", "town", "wall", "foliage", "beacon")
READ_SLICE = 4 << 20

# ADR §6.2, Tier A: the exact content table (towns, corridor walls, foliage, beacons,
# open plain, total). Written out here, not read from the generator.
ADR_TIER_A_TABLE = {
    "town": 7_680_000,
    "wall": 4_320_000,
    "foliage": 11_520_000,
    "beacon": 1_048_576,
    "plain": 176_758_016,
}
ADR_TIER_A_TOTAL = 201_326_592


def canonical_json(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def manifest_path_for(world: Path) -> Path:
    return world.with_name(world.stem + ".manifest.json")


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _godot_json(value) -> str:
    """JSON::stringify(..., "", true, true), for the finite tree-key schema.

    Godot formats FLOAT with fixed decimals (17 - floor(log10(abs(x))),
    clamped to [1, 32]), retaining one trailing fractional zero. Python's
    shortest-representation JSON differs for the GSQ float32 position step.
    """
    if isinstance(value, dict):
        return "{" + ",".join(json.dumps(k) + ":" + _godot_json(value[k]) for k in sorted(value)) + "}"
    if isinstance(value, list):
        return "[" + ",".join(_godot_json(v) for v in value) + "]"
    if type(value) is float:
        if not math.isfinite(value):
            raise ValueError("tree_key_nonfinite")
        if value == 0:
            return "0.0"
        precision = min(32, max(1, 17 - math.floor(math.log10(abs(value)))))
        formatted = format(value, f".{precision}f").rstrip("0")
        return formatted + "0" if formatted.endswith(".") else formatted
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"))


def _tree_number(value, *, integer: bool = False):
    if type(value) not in (int, float) or (type(value) is float and not math.isfinite(value)):
        raise ValueError("tree_key_number_invalid")
    if integer:
        if value != int(value) or not 0 <= value <= 0xFFFFFFFF:
            raise ValueError("tree_key_integer_invalid")
        return int(value)
    try:
        return float(value)
    except OverflowError as exc:
        raise ValueError("tree_key_number_invalid") from exc


def compute_tree_sha(sidecar: dict) -> str:
    """Recompute the producer's key; worker count is execution metadata only."""
    params = dict(sidecar["build_params"])
    params.pop("worker_count", None)
    if set(params) != {"leaf_max", "max_tile_splats", "interior"}:
        raise ValueError("tree_key_build_params_invalid")
    interior = dict(params["interior"])
    integer_fields = {"merge_grid", "node_max_splats", "max_eps_steps"}
    if set(interior) != integer_fields | {"reduction", "eps_growth"}:
        raise ValueError("tree_key_interior_invalid")
    params["interior"] = {k: _tree_number(v, integer=k in integer_fields) for k, v in interior.items()}
    for k in ("leaf_max", "max_tile_splats"):
        params[k] = _tree_number(params[k], integer=True)
    codec = dict(sidecar["codec"])
    if codec.get("codec") == "gsq":
        if set(codec) != {"codec", "codec_id", "sh_bits", "opacity_bits", "pos_step_m", "zstd_level"}:
            raise ValueError("tree_key_codec_invalid")
        for k in ("codec_id", "opacity_bits", "zstd_level"):
            codec[k] = _tree_number(codec[k], integer=True)
        if codec["codec_id"] != CODEC_GSQ_V1 or len(codec["sh_bits"]) != 3:
            raise ValueError("tree_key_codec_invalid")
        codec["sh_bits"] = [_tree_number(v, integer=True) for v in codec["sh_bits"]]
        codec["pos_step_m"] = _tree_number(codec["pos_step_m"])
    elif codec != {"codec": "none"}:
        raise ValueError("tree_key_codec_invalid")
    for key in ("source_fingerprint", "input_file_sha256"):
        if not isinstance(sidecar[key], str) or re.fullmatch(r"[0-9a-f]{64}", sidecar[key]) is None:
            raise ValueError(f"tree_key_{key}_invalid")
    return sha256_text(_godot_json({"source_fingerprint": sidecar["source_fingerprint"],
                                  "build_params": params, "codec": codec,
                                  "input_file_sha256": sidecar["input_file_sha256"]}))


class _V4Invalid(ValueError):
    pass


def verify_v4(world: Path) -> dict:
    """Stream V4 stored integrity in one file pass, with bounded read buffers.

    This does not decode GSQ or certify tree geometry. The C++ builder's W13
    re-open check provides that separate evidence; sidecar digest presence alone
    must never be represented as successful decoded verification.
    """
    result = {"ok": False, "status": "FAIL", "reason_code": "", "reason": "verification incomplete",
              "problems": [], "checked": [],
              "decoded_verification": {"status": "NOT_RUN", "reason_code": "builder_w13_required",
                                       "reason": "decoded content requires the builder's C++ W13 reopen check"}}

    def require(condition, reason):
        if not condition:
            raise _V4Invalid(reason)

    def digest_valid(value):
        return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None

    try:
        sidecar = manifest_path_for(world)
        require(sidecar.is_file(), "manifest_missing")
        # The on-disk reader caps nodes at 2**20; bound sidecar allocation too.
        require(sidecar.stat().st_size <= 256 << 20, "manifest_too_large")
        m = json.loads(sidecar.read_text(encoding="utf-8"))
        require(m["schema"] == "godotgs-auto-tree-manifest/1", "manifest_schema_mismatch")
        tree_sha = compute_tree_sha(m)
        require(tree_sha == m["tree_sha"], "tree_sha_mismatch")
        result["tree_sha"] = tree_sha
        require(digest_valid(m["output_file_sha256"]), "output_digest_invalid")
        nodes = m["nodes"]
        require(isinstance(nodes, list) and 0 < len(nodes) <= 1 << 20, "manifest_node_count_invalid")
        for i, node in enumerate(nodes):
            require("decoded_sha256" in node, f"decoded_digest_missing: node {i}")
            require(digest_valid(node["decoded_sha256"]), f"decoded_digest_invalid: node {i}")
            require(digest_valid(node["stored_sha256"]), f"stored_digest_invalid: node {i}")
        file_sha = hashlib.sha256()
        records = []
        with world.open("rb") as stream:
            def read(size):
                data = stream.read(size)
                require(len(data) == size, "short_read")
                file_sha.update(data)
                return data

            def consume(size, digest=None, *, zero=False):
                while size:
                    data = read(min(size, READ_SLICE))
                    if digest is not None:
                        digest.update(data)
                    require(not zero or not any(data), "padding_nonzero")
                    size -= len(data)

            header = read(4096)
            words = struct.unpack_from("<16I", header)
            require(words[:2] == (MAGIC, 4), "header: magic/version")
            require(hashlib.sha256(header[:4064]).digest() == header[4064:], "header_digest_mismatch")
            flags, leaf, interior, node_count, degree, first_sh, high_sh, bake_rule, codec = (
                words[2], *words[8:16])
            expected_codec = CODEC_GSQ_V1 if m["codec"]["codec"] == "gsq" else CODEC_NONE
            require(codec == expected_codec, "codec_mismatch")
            require(words[3:8] == (4096, 4096, 65536, 256, LAYOUT_VERSION), "header_geometry_mismatch")
            require(flags & ~3 == 0 and not any(header[240:4064]), "header_reserved_invalid")
            require(0 < node_count <= 1 << 20 and leaf > 0 and node_count <= leaf + interior <= 0xFFFFFFFF
                    and bake_rule > 0 and degree <= 4 and first_sh <= 3 and high_sh <= 21,
                    "header_counts_invalid")
            if codec == CODEC_GSQ_V1:
                require(degree <= 3 and first_sh == (0 if degree == 0 else 3)
                        and high_sh == (0, 0, 5, 12)[degree], "header_sh_layout_invalid")
            geometry = struct.unpack_from("<3d6f", header, 64)
            require(all(math.isfinite(v) for v in geometry) and all(v >= 0 for v in geometry[6:]),
                    "header_bounds_invalid")
            (index_offset, pages, payload_offset, payload_bytes, metadata_offset,
             metadata_size, file_bytes, generation) = struct.unpack_from("<8Q", header, 112)
            require(index_offset == 4096 and pages == (node_count + 254) // 255
                    and payload_offset == 4096 + pages * 65536 and payload_bytes > 0
                    and payload_bytes % 4096 == 0, "header_regions_invalid")
            require(metadata_size <= 16 << 20 and bool(flags & 2) == bool(metadata_size)
                    and metadata_offset == (payload_offset + payload_bytes if metadata_size else 0)
                    and file_bytes == payload_offset + payload_bytes + metadata_size
                    and file_bytes == world.stat().st_size, "file_bytes_mismatch")
            require(len(nodes) == node_count and m["totals"] == {
                "node_count": node_count, "leaf_splat_count": leaf, "interior_splat_count": interior},
                "manifest_counts_mismatch")
            require(m["content_generation"] == str(generation), "content_generation_mismatch")
            # #1432: the GSQ builder always records the reopened header bounds,
            # so their absence is a defect, not a reason to skip the comparison.
            require(codec != CODEC_GSQ_V1 or "header_bounds" in m, "header_bounds_missing")
            if "header_bounds" in m:
                bits = struct.unpack_from("<6I", header, 88)
                require(m["header_bounds"] == {"position_f32_bits": list(bits[:3]), "size_f32_bits": list(bits[3:])},
                        "header_bounds_mismatch")
            index_digest = hashlib.sha256()
            cursor = payload_offset
            for page_index in range(pages):
                page = read(65536)
                count = min(255, node_count - page_index * 255)
                require(struct.unpack_from("<4I", page) == (0x49505347, page_index, page_index * 255, count)
                        and not any(page[16:32]), "index_page_identity_mismatch")
                page_digest = hashlib.sha256(page[64:64 + count * 256]).digest()
                require(page_digest == page[32:64], f"index_page_digest_mismatch: page {page_index}")
                require(not any(page[64 + count * 256:]), "index_page_tail_nonzero")
                index_digest.update(page_digest)
                for j in range(count):
                    i = page_index * 255 + j
                    entry = page[64 + j * 256:64 + (j + 1) * 256]
                    payload_first, splats = struct.unpack_from("<QI", entry, 88)
                    offset, stored, extent = struct.unpack_from("<3Q", entry, 128)
                    require(not any(entry[120:128] + entry[152:192] + entry[224:256]),
                            f"node_reserved_invalid: node {i}")
                    require(0 < splats <= 16384 and offset == cursor and stored >= 64
                            and extent == (stored + 4095) // 4096 * 4096
                            and offset + extent <= payload_offset + payload_bytes,
                            f"node_extent_invalid: node {i}")
                    if codec == CODEC_NONE:
                        require(stored == 64 + splats * (GAUSSIAN_BYTES + high_sh * VECTOR3_BYTES),
                                f"node_size_invalid: node {i}")
                    else:
                        require(stored > 64 + 128, f"node_size_invalid: node {i}")
                    require(entry[192:224].hex() == nodes[i]["stored_sha256"], f"node_digest_mismatch: node {i} sidecar")
                    records.append((stored, extent, payload_first, splats, entry[192:224]))
                    cursor += extent
            require(index_digest.digest() == header[176:208], "index_digest_mismatch")
            require(cursor == payload_offset + payload_bytes, "payload_region_mismatch")
            result["checked"].append("V4 header and every index-page digest")
            for i, (stored, extent, payload_first, splats, expected) in enumerate(records):
                envelope = read(64)
                fields = struct.unpack_from("<4I4QI", envelope)
                require(fields == (0x504E5347, i, splats, high_sh, splats * GAUSSIAN_BYTES,
                                   splats * high_sh * VECTOR3_BYTES, generation, payload_first, LAYOUT_VERSION)
                        and not any(envelope[52:]), f"node_header_mismatch: node {i}")
                digest = hashlib.sha256(envelope)
                consume(stored - 64, digest)
                require(digest.digest() == expected, f"node_digest_mismatch: node {i}")
                consume(extent - stored, zero=True)
            metadata_hash = hashlib.sha256()
            consume(metadata_size, metadata_hash)
            require(header[208:240] == (metadata_hash.digest() if metadata_size else bytes(32)),
                    "metadata_digest_mismatch")
            require(not stream.read(1) and stream.tell() == file_bytes, "file_changed_length")
        require(file_sha.hexdigest() == m["output_file_sha256"], "file_sha256_mismatch")
        result.update(ok=True, status="OK", node_count=node_count, content_generation=str(generation),
                      output_file_sha256=file_sha.hexdigest(), reason="V4 structure, stored digests and tree identity verified")
        result["checked"].extend(["every V4 stored node digest and whole-file digest", "tree_sha inputs",
                                  "per-node decoded digest presence and format only"])
    except (OSError, ValueError, TypeError, KeyError, IndexError, OverflowError, struct.error) as exc:
        reason = str(exc) if isinstance(exc, _V4Invalid) else f"v4_invalid: {exc}"
        result["reason_code"] = reason.split(":", 1)[0]
        result["reason"] = reason
        result["problems"].append(reason)
    return result


class Verifier:
    def __init__(self, world: Path, manifest: dict, check_content: bool) -> None:
        self.world = world
        self.m = manifest
        self.check_content = check_content
        self.problems: list[str] = []
        self.checked: list[str] = []

    def fail(self, msg: str) -> None:
        self.problems.append(msg)

    # -- manifest-only checks -------------------------------------------------
    def check_manifest(self) -> None:
        m = self.m
        if m.get("schema") != MANIFEST_SCHEMA:
            self.fail(f"manifest_schema: {m.get('schema')!r}")
            return
        if sha256_text(canonical_json(m["generator_params"])) != m["generator_params_sha"]:
            self.fail("generator_params_sha_mismatch")
        if sha256_text(canonical_json(m["gen_sha_inputs"])) != m["gen_sha"]:
            self.fail("gen_sha_mismatch")
        if m["gen_sha_inputs"].get("generator_params_sha") != m["generator_params_sha"]:
            self.fail("gen_sha_inputs do not name this generator_params_sha")
        chunks = m["chunks"]
        world = m["world"]
        if len(chunks) != world["chunk_count"]:
            self.fail("chunk_count_mismatch")
        per_class = [0] * len(CLASSES)
        first = 0
        for i, c in enumerate(chunks):
            if c["index"] != i or c["first_splat"] != first:
                self.fail(f"chunk {i}: index/first_splat out of sequence")
            if sum(c["class_counts"]) != c["count"]:
                self.fail(f"chunk {i}: class counts do not sum to its count")
            for k, v in enumerate(c["class_counts"]):
                per_class[k] += v
            first += c["count"]
        table = m["content_table"]
        for k, name in enumerate(CLASSES):
            if table[name]["splats"] != per_class[k]:
                self.fail(f"content_table: {name} {table[name]['splats']} != chunk sum {per_class[k]}")
        if not (table["total"] == first == world["splat_count"]):
            self.fail("content_table: total, chunk sum and splat_count disagree")
        if m["generator_params"]["tier"] == "A" and "selection" not in m:
            got = {name: table[name]["splats"] for name in CLASSES}
            if got != ADR_TIER_A_TABLE or table["total"] != ADR_TIER_A_TOTAL:
                self.fail(f"content_table: Tier A differs from ADR §6.2: {got}")
            else:
                self.checked.append("tier A content table equals ADR §6.2")
        codec_id = m["writer"]["codec"]["codec_id"]
        if codec_id not in (CODEC_NONE, CODEC_GSQ_V1) or (codec_id == CODEC_GSQ_V1) != ("codec" in m):
            self.fail(f"codec_mismatch: writer codec {codec_id} and the manifest's codec block disagree")
        self.checked.append("manifest digests and content-table sums")

    def is_gsq(self) -> bool:
        return self.m["writer"]["codec"]["codec_id"] == CODEC_GSQ_V1

    # -- GSQ v1 file checks (plain v3 + bit 7, BV-C7) ---------------------------
    def check_paged_file(self) -> None:
        """Layout from modules/gaussian_splatting/io/gaussian_splat_world_format.h (bit 7)."""
        m = self.m
        world = m["world"]
        codec = m["codec"]
        wcodec = m["writer"]["codec"]
        size = self.world.stat().st_size
        if size != world["file_bytes"]:
            self.fail(f"file_bytes_mismatch: {size} on disk, {world['file_bytes']} in the manifest")
            return
        chunks = m["chunks"]
        sh_high = world["sh_high_order"]
        header_bytes = PREFIX_BYTES + PAGED_EXTENSION_BYTES
        with self.world.open("rb") as f:
            header = f.read(header_bytes)
            if len(header) != header_bytes:
                self.fail("short_read")
                return
            (magic, version, flags, splat_count, sh_degree, sh_first, sh_high_h) = struct.unpack_from("<7I", header, 0)
            chunk_count = struct.unpack_from("<I", header, 52)[0]
            g_off, sh_off, ct_off, idx_off, md_off, md_size = struct.unpack_from("<6Q", header, 56)
            layout, reserved = struct.unpack_from("<2I", header, 104)
            codec_id, b0, b1, b2, op_bits, pos_step, zstd, page_count, align, pt_off = struct.unpack_from(
                "<I4BfiIIQ", header, PREFIX_BYTES
            )
            if magic != MAGIC or version != VERSION:
                self.fail("header: magic/version")
            want_flags = FLAG_HAS_METADATA | FLAG_HAS_CHUNKS | FLAG_PAGED_CHUNKS | (FLAG_HAS_HIGH_SH if sh_high else 0)
            if flags != want_flags:
                self.fail(f"header: unexpected flags {flags:#x}")
            if (splat_count, chunk_count, sh_degree, sh_first, sh_high_h) != (
                world["splat_count"], world["chunk_count"], world["sh_degree"], world["sh_first_order"], sh_high
            ):
                self.fail("header: counts differ from the manifest")
            if layout != LAYOUT_VERSION or reserved != 0 or sh_off != 0 or idx_off != 0:
                self.fail("header: layout word / reserved / unused section offsets")
            if header[PREFIX_BYTES + 32:] != bytes(32):
                self.fail("header: paged extension reserved bytes")
            if (codec_id, [b0, b1, b2], op_bits, zstd) != (
                wcodec["codec_id"], wcodec["sh_bits"], wcodec["opacity_bits"], wcodec["zstd_level"]
            ) or pos_step != f32(wcodec["pos_step_m"]):
                self.fail("codec_params_mismatch: the file's codec block differs from the manifest's writer codec")
            if codec_id != CODEC_GSQ_V1 or align != PAGE_ALIGNMENT:
                self.fail("header: codec id / page alignment")
            if page_count != codec["page_count"]:
                self.fail("codec_block_mismatch: page_count")
            ct_bytes = chunk_count * PAGED_CHUNK_RECORD_BYTES
            pt_bytes = page_count * PAGED_PAGE_RECORD_BYTES
            if pt_off != ct_off + ct_bytes or md_off != pt_off + pt_bytes or md_off + md_size != size:
                self.fail("layout: chunk table, page table and metadata are not contiguous to the end")
                return
            f.seek(ct_off)
            tables = f.read(ct_bytes + pt_bytes)
            if len(tables) != ct_bytes + pt_bytes:
                self.fail("short_read")
                return
            md = json.loads(f.read(md_size).decode("utf-8"))
            gen = md.get("gs_generator", {})
            if gen.get("generator_params_sha") != m["generator_params_sha"] or gen.get("gen_sha") != m["gen_sha"]:
                self.fail("metadata: embedded generator identity differs from the manifest")

            def page(p: int) -> tuple:
                return struct.unpack_from("<QII32s32s6fII", tables, ct_bytes + p * PAGED_PAGE_RECORD_BYTES)

            if g_off != (page(0)[0] if page_count else g_off):
                self.fail("header: gaussian offset is not the first page")
            next_page = 0
            prev_end = header_bytes
            stored_total = 0
            decoded_set = hashlib.sha256()
            for i, c in enumerate(chunks):
                rec = tables[i * PAGED_CHUNK_RECORD_BYTES:(i + 1) * PAGED_CHUNK_RECORD_BYTES]
                px, py, pz, sx, sy, sz, cx, cy, cz, radius = struct.unpack_from("<10f", rec, 0)
                first, npages, count, rsv = struct.unpack_from("<4I", rec, 40)
                end = [f32(px + sx), f32(py + sy), f32(pz + sz)]
                aabb = [f32(v) for v in c["aabb"]]
                if [px, py, pz] != aabb[:3] or end != aabb[3:]:
                    self.fail(f"chunk_record_mismatch: chunk {i} aabb")
                if [cx, cy, cz] != [f32(v) for v in c["center"]] or radius != f32(c["radius"]):
                    self.fail(f"chunk_record_mismatch: chunk {i} centre/radius")
                if count != c["count"] or rsv != 0:
                    self.fail(f"chunk_record_mismatch: chunk {i} count")
                if first != next_page or first != c["first_page"] or npages != c["page_count"]:
                    self.fail(f"chunk_pages_mismatch: chunk {i} page run")
                    return
                splats = 0
                span_start = None
                for p in range(first, first + npages):
                    off, stored, n, _sd, dd, *_ext, chunk_index, prsv = page(p)
                    if chunk_index != i or prsv != 0 or off % PAGE_ALIGNMENT or off < prev_end or stored == 0:
                        self.fail(f"page {p}: record inconsistent")
                        return
                    span_start = off if span_start is None else span_start
                    prev_end = off + stored
                    splats += n
                    stored_total += stored
                    decoded_set.update(dd)
                if splats != count:
                    self.fail(f"chunk {i}: its pages hold {splats} splats, not {count}")
                span = (span_start or 0, (prev_end - span_start) if span_start is not None else 0)
                if [c["pages_offset"], c["pages_bytes"]] != list(span):
                    self.fail(f"chunk_pages_mismatch: chunk {i} byte span")
                    return
                next_page += npages
            if next_page != page_count or prev_end != ct_off:
                self.fail("layout: the pages do not end at the chunk table")
                return
            if stored_total != codec["pages_stored_bytes"]:
                self.fail("codec_block_mismatch: pages_stored_bytes")
            if decoded_set.hexdigest() != codec["decoded_digest_set_sha256"]:
                self.fail("codec_block_mismatch: decoded_digest_set_sha256")

            # One sequential pass: the whole-file digest, every page run, zero padding.
            f.seek(0)
            file_sha = hashlib.sha256()
            pos = 0

            def read_to(to: int, extra=None, zero: bool = False) -> bool:
                nonlocal pos
                while pos < to:
                    data = f.read(min(to - pos, READ_SLICE))
                    if not data:
                        raise EOFError
                    file_sha.update(data)
                    if extra is not None:
                        extra.update(data)
                    if zero and data.count(0) != len(data):
                        self.fail("layout: the padding between pages is not zero")
                    pos += len(data)
                return True

            try:
                read_to(header_bytes)
                for i, c in enumerate(chunks):
                    h = hashlib.sha256()
                    if c["page_count"]:
                        read_to(c["pages_offset"], zero=True)
                        read_to(c["pages_offset"] + c["pages_bytes"], extra=h)
                    if h.hexdigest() != c["pages_sha256"]:
                        self.fail(f"chunk_digest_mismatch: chunk {i} pages_sha256")
                read_to(size)
            except EOFError:
                self.fail("short_read")
                return
        if file_sha.hexdigest() != world["file_sha256"]:
            self.fail("file_sha256_mismatch")
        self.checked.append("GSQ file, header, codec block, chunk and page tables, every page-run digest")

    # -- file checks ----------------------------------------------------------
    def check_file(self) -> None:
        if self.is_gsq():
            self.check_paged_file()
            return
        m = self.m
        world = m["world"]
        size = self.world.stat().st_size
        if size != world["file_bytes"]:
            self.fail(f"file_bytes_mismatch: {size} on disk, {world['file_bytes']} in the manifest")
            return
        chunks = m["chunks"]
        sh_high = world["sh_high_order"]
        file_sha = hashlib.sha256()
        with self.world.open("rb") as f:

            def read(n: int) -> bytes:
                data = f.read(n)
                if len(data) != n:
                    raise EOFError
                file_sha.update(data)
                return data

            def hash_range(n: int) -> str:
                h = hashlib.sha256()
                while n > 0:
                    s = min(n, READ_SLICE)
                    h.update(read(s))
                    n -= s
                return h.hexdigest()

            try:
                header = read(PREFIX_BYTES)
                (magic, version, flags, splat_count, sh_degree, sh_first, sh_high_h) = struct.unpack_from("<7I", header, 0)
                chunk_count = struct.unpack_from("<I", header, 52)[0]
                g_off, sh_off, ct_off, idx_off, md_off, md_size = struct.unpack_from("<6Q", header, 56)
                layout, reserved = struct.unpack_from("<2I", header, 104)
                if magic != MAGIC or version != VERSION:
                    self.fail("header: magic/version")
                if flags != (FLAG_HAS_METADATA | FLAG_HAS_CHUNKS | (FLAG_HAS_HIGH_SH if sh_high else 0)):
                    self.fail(f"header: unexpected flags {flags:#x}")
                if (splat_count, chunk_count, sh_degree, sh_first, sh_high_h) != (
                    world["splat_count"], world["chunk_count"], world["sh_degree"], world["sh_first_order"], sh_high
                ):
                    self.fail("header: counts differ from the manifest")
                if layout != LAYOUT_VERSION or reserved != 0:
                    self.fail("header: layout word / reserved")
                if g_off != PREFIX_BYTES:
                    self.fail("header: gaussian offset")
                for i, c in enumerate(chunks):
                    if c["gaussian_offset"] != g_off + c["first_splat"] * GAUSSIAN_BYTES:
                        self.fail(f"chunk {i}: gaussian_offset")
                    if sh_high and c["sh_offset"] != sh_off + c["first_splat"] * sh_high * VECTOR3_BYTES:
                        self.fail(f"chunk {i}: sh_offset")
                for i, c in enumerate(chunks):
                    if hash_range(c["count"] * GAUSSIAN_BYTES) != c["gaussian_sha256"]:
                        self.fail(f"chunk_digest_mismatch: chunk {i} gaussian_sha256")
                if sh_high:
                    for i, c in enumerate(chunks):
                        if hash_range(c["count"] * sh_high * VECTOR3_BYTES) != c["sh_sha256"]:
                            self.fail(f"chunk_digest_mismatch: chunk {i} sh_sha256")
                if f.tell() != ct_off:
                    self.fail("layout: payload sections do not end at the chunk table")
                for i, c in enumerate(chunks):
                    rec = read(CHUNK_RECORD_BYTES)
                    px, py, pz, sx, sy, sz, cx, cy, cz, radius = struct.unpack_from("<10f", rec, 0)
                    first, count, rsv = struct.unpack_from("<QII", rec, 40)
                    end = [f32(px + sx), f32(py + sy), f32(pz + sz)]
                    aabb = [f32(v) for v in c["aabb"]]
                    if [px, py, pz] != aabb[:3] or end != aabb[3:]:
                        self.fail(f"chunk_record_mismatch: chunk {i} aabb")
                    if [cx, cy, cz] != [f32(v) for v in c["center"]] or radius != f32(c["radius"]):
                        self.fail(f"chunk_record_mismatch: chunk {i} centre/radius")
                    if first != c["first_splat"] or count != c["count"] or rsv != 0:
                        self.fail(f"chunk_record_mismatch: chunk {i} index range")
                if f.tell() != idx_off:
                    self.fail("layout: chunk table does not end at the index lists")
                expected = 0
                remaining = splat_count
                while remaining > 0:
                    n = min(remaining, READ_SLICE // 4)
                    data = read(n * 4)
                    if data != struct.pack(f"<{n}I", *range(expected, expected + n)):
                        self.fail("index lists are not the identity ranges of the stream")
                        break
                    expected += n
                    remaining -= n
                if md_off != f.tell():
                    self.fail("layout: metadata offset")
                md = json.loads(read(md_size).decode("utf-8"))
                gen = md.get("gs_generator", {})
                if gen.get("generator_params_sha") != m["generator_params_sha"] or gen.get("gen_sha") != m["gen_sha"]:
                    self.fail("metadata: embedded generator identity differs from the manifest")
                if f.read(1):
                    self.fail("layout: trailing bytes after the metadata")
            except EOFError:
                self.fail("short_read")
                return
        if file_sha.hexdigest() != world["file_sha256"]:
            self.fail("file_sha256_mismatch")
        self.checked.append("file, header, chunk records, index lists and every chunk digest")

    # -- content re-classification ---------------------------------------------
    def check_splats(self) -> None:
        if self.is_gsq():
            self.fail("--content re-reads raw splats; a GSQ world holds none (decode it with the C++ verify)")
            return
        try:
            import numpy as np
        except ImportError:
            self.fail("--content needs numpy")
            return
        m = self.m
        feats = m["features"]
        params = m["generator_params"]
        towns = [f["footprint"] for f in feats if f["class"] == "town"]
        foliage = [f["footprint"] for f in feats if f["class"] == "foliage"]
        walls = [(f["footprint"], f["plane_z_m"]) for f in feats if f["class"] == "wall"]
        beacons = [f for f in feats if f["class"] == "beacon"]
        beacon_base = params["beacon"]["base_y_q"] / 4096.0
        dt = np.dtype(
            [("pos", "<f4", 3), ("opacity", "<f4"), ("scale", "<f4", 3), ("rest", "V%d" % (GAUSSIAN_BYTES - 28))]
        )
        assert dt.itemsize == GAUSSIAN_BYTES

        def in_rect(x, z, r):
            return (x >= r[0]) & (x < r[2]) & (z >= r[1]) & (z < r[3])

        with self.world.open("rb") as f:
            for i, c in enumerate(m["chunks"]):
                f.seek(c["gaussian_offset"])
                g = np.frombuffer(f.read(c["count"] * GAUSSIAN_BYTES), dtype=dt)
                x = g["pos"][:, 0].astype(np.float64)
                y = g["pos"][:, 1].astype(np.float64)
                z = g["pos"][:, 2].astype(np.float64)
                cell = c["cell"]
                if not bool(np.all(in_rect(x, z, cell))):
                    self.fail(f"chunk {i}: a splat lies outside its cell")
                cls = np.zeros(len(g), dtype=np.int8)  # plain
                for r in towns:
                    cls[in_rect(x, z, r)] = 1
                for r, pz in walls:
                    cls[(z == pz) & (x >= r[0]) & (x < r[2])] = 2
                for r in foliage:
                    cls[in_rect(x, z, r)] = 3
                for b in beacons:
                    r = b["footprint"]
                    cls[(z == r[1]) & (x >= r[0]) & (x <= r[2]) & (y >= beacon_base)] = 4
                counts = [int(np.count_nonzero(cls == k)) for k in range(len(CLASSES))]
                if counts != c["class_counts"]:
                    self.fail(f"chunk {i}: re-classified counts {counts} != manifest {c['class_counts']}")
                # Plain splats must avoid every excluded footprint.
                plain = cls == 0
                for r in towns + foliage + [w[0] for w in walls]:
                    if bool(np.any(plain & in_rect(x, z, r))):
                        self.fail(f"chunk {i}: a plain splat lies in an excluded footprint")
                # Chunk-table AABB recomputed from the splats (float32 arithmetic).
                pos = g["pos"].astype(np.float32)
                rad = (np.float32(3.0) * g["scale"].max(axis=1)).astype(np.float32)[:, None]
                mn = (pos - rad).min(axis=0)
                mx = (pos + rad).max(axis=0)
                aabb = np.array(c["aabb"], dtype=np.float32)
                size = (mx - mn).astype(np.float32)
                if not (np.array_equal(mn, aabb[:3]) and np.array_equal((mn + size).astype(np.float32), aabb[3:])):
                    self.fail(f"chunk {i}: AABB recomputed from the splats differs")
        self.checked.append("every splat re-classified; per-chunk class counts and AABBs recomputed")

    def run(self) -> dict:
        self.check_manifest()
        if not self.problems:
            self.check_file()
        if not self.problems and self.check_content:
            self.check_splats()
        return {"ok": not self.problems, "problems": self.problems, "checked": self.checked}


def verify(world: Path, check_content: bool = False) -> dict:
    mpath = manifest_path_for(world)
    if not mpath.is_file():
        return {"ok": False, "problems": [f"manifest_missing: {mpath}"], "checked": []}
    try:
        manifest = json.loads(mpath.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return {"ok": False, "problems": [f"manifest_unparseable: {exc}"], "checked": []}
    if not world.is_file():
        return {"ok": False, "problems": ["world_missing"], "checked": []}
    if isinstance(manifest, dict) and manifest.get("schema") == "godotgs-auto-tree-manifest/1":
        result = verify_v4(world)
        if check_content:
            result.update(ok=False, status="FAIL", reason_code="v4_content_requires_builder_w13")
            result["problems"].append("v4_content_requires_builder_w13")
        return result
    try:
        return Verifier(world, manifest, check_content).run()
    except (KeyError, TypeError, IndexError) as exc:
        return {"ok": False, "problems": [f"manifest_malformed: {exc!r}"], "checked": []}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("world", type=Path)
    ap.add_argument("--content", action="store_true", help="re-classify every splat (needs numpy)")
    ap.add_argument("--report", type=Path)
    ap.add_argument("--v4", action="store_true", help="verify V4 stored integrity; decoded evidence remains builder W13")
    args = ap.parse_args(argv)
    if args.v4 and args.content:
        ap.error("--v4 --content requires the builder W13 re-open verifier")
    result = verify_v4(args.world) if args.v4 else verify(args.world, args.content)
    text = json.dumps(result, indent=2)
    print(text)
    if args.report:
        args.report.write_text(text + "\n", encoding="utf-8")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
