#!/usr/bin/env python3
"""Producer-written provenance for the generated PLY fixture corpus (#790).

Why a record exists at all: header shape is not provenance. `run_benchmark.py`
authenticated a `cpp_rich` fixture from its header alone -- the producer's
encoding, the generic Gaussian property block and the full `f_rest_0..44` SH
block. Every one of those describes a FILE SHAPE, so a `binary_little_endian`
PLY assembled outside the C++ generator with a declared producer count and that
property block satisfied all of them, was labelled `cpp_rich`, and could
therefore satisfy `--require-asset-variant cpp_rich` and publish numbers under a
producer that never wrote it. Deriving the expected shape from
`synthetic_ply_writer.cpp` made the check honest about the shape; it could not
make the shape mean provenance.

So the producer records what it wrote, and the label is only granted to a file
whose bytes that record names.

Entries are keyed by SHA-256 rather than by path for two reasons:

* `prepare_synthetic_assets.py` copies each primary fixture to the consumer
  project (`shutil.copy2`), so one entry authenticates every byte-identical
  copy without the record having to enumerate locations;
* a path-keyed record would still be satisfied by substituting a different file
  at a recorded path, which is the defect this closes.

The record lives beside the primary corpus in `tests/fixtures/` and is gitignored
like the fixtures themselves; it is written by a generation run, never by hand.
It is not a tamper-proof signature -- anyone able to write the fixtures can write
the record next to them -- and human review remains the control under an
adversarial model. What it does close is the case the review named: a file the
producer never wrote being accepted as its output.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

#: The producer labels. Defined here so the writer
#: (`prepare_synthetic_assets.py`) and the reader (`run_benchmark.py`) cannot
#: drift apart: both import these names rather than spelling the strings again.
VARIANT_CPP_RICH = "cpp_rich"
VARIANT_PYTHON_FALLBACK = "python_fallback"

PROVENANCE_FILENAME = ".fixture_provenance.json"

#: Bumped whenever the record's meaning changes. A record written by a different
#: version is ignored rather than reinterpreted, which fails CLOSED: an ignored
#: record authenticates nothing.
#:
#: 2: entries carry the filenames the producer wrote them as, and the Python
#:    fallback records its output too -- a v1 record binds neither, so reading one
#:    under these rules would authenticate more than it was ever asked to.
PROVENANCE_VERSION = 2

_DIGEST_CHUNK_BYTES = 1 << 20


def provenance_path(fixtures_dir: Path) -> Path:
    return Path(fixtures_dir) / PROVENANCE_FILENAME


def file_digest(path: Path) -> str | None:
    """SHA-256 of a file, or None when it cannot be read."""
    digest = hashlib.sha256()
    try:
        with Path(path).open("rb") as handle:
            for chunk in iter(lambda: handle.read(_DIGEST_CHUNK_BYTES), b""):
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest()


def load_records(fixtures_dir: Path) -> dict[str, dict]:
    """Digest -> entry for the current record version.

    Returns an empty mapping for every failure mode -- absent, unreadable,
    malformed, or written by another version. An empty mapping authenticates
    nothing, so a damaged record degrades to "no provenance" rather than to
    "provenance assumed".
    """
    try:
        raw = json.loads(provenance_path(fixtures_dir).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, UnicodeDecodeError):
        return {}
    if not isinstance(raw, dict) or raw.get("version") != PROVENANCE_VERSION:
        return {}
    entries = raw.get("entries")
    if not isinstance(entries, dict):
        return {}
    return {
        digest: entry
        for digest, entry in entries.items()
        if isinstance(digest, str)
        and isinstance(entry, dict)
        and entry.get("variant")
        and isinstance(entry.get("filenames"), list)
    }


def recorded_variant(fixtures_dir: Path, path: Path) -> str | None:
    """The producer label recorded for these exact bytes UNDER THIS NAME, or None.

    The name is part of the claim, not decoration. Four fixtures are declared as
    50,000-splat `cpp_rich` outputs (sphere, cube, plane, torus), so a digest-only
    lookup accepted recorded sphere bytes copied over `synthetic_cube.ply`: the
    cube lane then measured a sphere and published it as the cube (#969 review).
    A producer writes each fixture under one name, and that pairing is what the
    record attests.

    Byte-identical copies keep working, which is the point of keying by digest:
    `prepare_synthetic_assets.py` copies each primary into the consumer project
    under the SAME basename, so both are covered by one entry. `filenames` is a
    list for the case two names legitimately share content -- the record then
    attests both, rather than the last write silently dropping the other.
    """
    digest = file_digest(path)
    if digest is None:
        return None
    entry = load_records(fixtures_dir).get(digest)
    if not entry:
        return None
    if Path(path).name not in entry.get("filenames", []):
        return None
    return entry.get("variant")


def record_producer_output(
    fixtures_dir: Path,
    produced: "dict[Path, str] | None" = None,
    *,
    retain: "list[Path] | tuple[Path, ...]" = (),
) -> bool:
    """Record each written path under the producer that wrote it.

    `produced` maps a path to its producer label. BOTH producers are recorded:
    `--require-asset-variant python_fallback` is advertised as provenance too, and
    while it was unrecorded a fixture satisfied it on its vertex count alone --
    the 2,048-splat fallback cube placed at the sphere path passed as the sphere
    producer's output (#969 review).

    `retain` names the corpus's other copies. A prior entry survives only when
    some file in `retain` still hashes to it, which keeps the record self-pruning
    -- it cannot grow past the number of fixture copies in the workspace -- while
    not evicting a copy an earlier run wrote that this one deliberately left in
    place.

    Called with nothing produced, this is pure pruning. Returns False when the
    record could not be written; the caller must not report success on a corpus
    whose provenance was not recorded.
    """
    fixtures_dir = Path(fixtures_dir)
    previous = load_records(fixtures_dir)
    entries: dict[str, dict] = {}

    for path, variant in (produced or {}).items():
        path = Path(path)
        digest = file_digest(path)
        if digest is None:
            # Skipping it wrote an incomplete record and still returned True, so
            # the prep exited 0 with one fixture unauthenticated and the benchmark
            # rejected the corpus a job later (#969 review). A produced file that
            # cannot be hashed -- a transient sharing lock on the Windows runner is
            # the case that produces one -- is a record this run cannot make.
            print(
                f"[fixture_provenance] could not hash {path}, so its provenance "
                "cannot be recorded"
            )
            return False
        entry = entries.setdefault(
            digest,
            {"variant": variant, "filenames": [], "bytes": path.stat().st_size},
        )
        if entry["variant"] != variant:
            # Identical bytes attributed to two producers is not a thing either
            # producer can do; refusing to pick one is the only honest answer.
            print(
                "[fixture_provenance] refusing to record identical bytes as both "
                f"{entry['variant']} and {variant} ({path.name})"
            )
            return False
        if path.name not in entry["filenames"]:
            entry["filenames"].append(path.name)
            entry["filenames"].sort()

    for path in retain:
        digest = file_digest(Path(path))
        if digest is None or digest in entries:
            continue
        carried = previous.get(digest)
        if carried is not None:
            entries[digest] = carried

    document = {
        "version": PROVENANCE_VERSION,
        "written_by": "tests/runtime/prepare_synthetic_assets.py",
        "entries": entries,
    }
    try:
        fixtures_dir.mkdir(parents=True, exist_ok=True)
        provenance_path(fixtures_dir).write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    except OSError as exc:
        print(f"[fixture_provenance] could not write the producer record: {exc}")
        return False
    return True
