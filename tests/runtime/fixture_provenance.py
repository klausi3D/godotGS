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

#: The producer label recorded for C++ `[GeneratePLY]` output. Defined here so
#: the writer (`prepare_synthetic_assets.py`) and the reader (`run_benchmark.py`,
#: which imports this name as its own `VARIANT_CPP_RICH`) cannot drift apart.
VARIANT_CPP_RICH = "cpp_rich"

PROVENANCE_FILENAME = ".fixture_provenance.json"

#: Bumped whenever the record's meaning changes. A record written by a different
#: version is ignored rather than reinterpreted, which fails CLOSED: an ignored
#: record authenticates nothing.
PROVENANCE_VERSION = 1

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
        if isinstance(digest, str) and isinstance(entry, dict) and entry.get("variant")
    }


def recorded_variant(fixtures_dir: Path, path: Path) -> str | None:
    """The producer label recorded for this exact file, or None."""
    digest = file_digest(path)
    if digest is None:
        return None
    entry = load_records(fixtures_dir).get(digest)
    return entry.get("variant") if entry else None


def record_producer_output(
    fixtures_dir: Path,
    produced: "list[Path] | tuple[Path, ...]" = (),
    *,
    variant: str = VARIANT_CPP_RICH,
    retain: "list[Path] | tuple[Path, ...]" = (),
) -> bool:
    """Record `produced` as `variant`, keeping prior entries still on disk.

    `retain` names the other copies of the corpus (the consumer project's
    duplicates). A prior entry survives only when some file in `retain` still
    hashes to it, which keeps the record self-pruning -- it cannot grow past the
    number of fixture copies in the workspace -- while not evicting a copy that a
    previous producer run wrote and this run deliberately left in place (the
    `preserve_floor_valid` branch in `prepare_synthetic_assets.py` does exactly
    that).

    Called with no `produced` after a fallback run, this is pure pruning.
    Returns False when the record could not be written.
    """
    fixtures_dir = Path(fixtures_dir)
    previous = load_records(fixtures_dir)
    entries: dict[str, dict] = {}

    for path in produced:
        path = Path(path)
        digest = file_digest(path)
        if digest is None:
            continue
        entries[digest] = {
            "variant": variant,
            "filename": path.name,
            "bytes": path.stat().st_size if path.is_file() else 0,
        }

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
