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
  * every chunk-table record (count, index range, AABB, centre, radius, bit for bit) and
    the identity index lists;
  * the content table: per-chunk class counts sum to the table, the table to the splat
    count, and for tier "A" the table equals the ADR §6.2 numbers written out below;
  * with --content (needs numpy): every splat is re-classified from its position against
    the manifest's features, and the per-chunk class counts, the "inside its cell" rule
    and the chunk-table AABBs are recomputed from the splats themselves.

Exit status: 0 when every check passes, 1 when any fails, 2 on usage errors.
"""

from __future__ import annotations

import argparse
import hashlib
import json
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
        self.checked.append("manifest digests and content-table sums")

    # -- file checks ----------------------------------------------------------
    def check_file(self) -> None:
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
    try:
        return Verifier(world, manifest, check_content).run()
    except (KeyError, TypeError, IndexError) as exc:
        return {"ok": False, "problems": [f"manifest_malformed: {exc!r}"], "checked": []}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("world", type=Path)
    ap.add_argument("--content", action="store_true", help="re-classify every splat (needs numpy)")
    ap.add_argument("--report", type=Path)
    args = ap.parse_args(argv)
    result = verify(args.world, args.content)
    text = json.dumps(result, indent=2)
    print(text)
    if args.report:
        args.report.write_text(text + "\n", encoding="utf-8")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
