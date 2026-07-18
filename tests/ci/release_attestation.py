#!/usr/bin/env python3
"""Record and re-verify the exact bytes a release publishes.

Why this exists (#593)
----------------------
``release_candidate_gate`` decides whether a release may publish. It validates a
candidate evidence bundle and the two platform builds. ``publish_release`` then
runs as a *separate job*, re-downloads the build artifacts, and uploads them to
GitHub. Nothing used to connect the two: the gate never saw the bytes the publish
job uploaded, and the publish job never saw what the gate validated. Any
divergence between them -- a re-run of a build job that regenerates the artifact,
an artifact overwritten by a concurrent run, a hand-uploaded artifact with a
colliding name -- would publish unvalidated bytes behind a green gate.

This script closes that: the gate hashes every file it validated into an
attestation, and the publish job re-hashes what it downloaded and requires an
exact match against that attestation *and* against the commit being published.
It fails closed on every ambiguity: a missing attestation, a missing file, a
changed digest, an unexpected extra file, or a commit mismatch.

Usage
-----
    release_attestation.py generate --root DIR --commit SHA --output FILE
                                    [--tag TAG] [--channel CHANNEL]
    release_attestation.py verify --root DIR --commit SHA --attestation FILE
                                  [--require RELPATH ...]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 1
_CHUNK = 1024 * 1024


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(_CHUNK)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _normalize_sha(value: Any) -> str:
    return str(value).strip().lower()


def _collect(root: Path) -> dict[str, dict[str, Any]]:
    """Hash every regular file under ``root``, keyed by posix-style relative path."""
    entries: dict[str, dict[str, Any]] = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        entries[rel] = {"sha256": _sha256(path), "size": path.stat().st_size}
    return entries


def _generate(args: argparse.Namespace) -> int:
    root = Path(args.root)
    if not root.is_dir():
        print(f"::error::attestation root is not a directory: {root}", file=sys.stderr)
        return 1
    entries = _collect(root)
    if not entries:
        # An empty release payload must never be attested: it would let a publish
        # job that downloaded nothing pass verification.
        print(f"::error::no files found under {root}; refusing to attest an empty release payload", file=sys.stderr)
        return 1
    attestation = {
        "schema_version": SCHEMA_VERSION,
        "commit": _normalize_sha(args.commit),
        "tag": args.tag or "",
        "channel": args.channel or "",
        "files": entries,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(attestation, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Attested {len(entries)} file(s) for commit {attestation['commit']}:")
    for rel in sorted(entries):
        print(f"  {entries[rel]['sha256']}  {rel}")
    return 0


def _verify(args: argparse.Namespace) -> int:
    attestation_path = Path(args.attestation)
    if not attestation_path.is_file():
        print(
            f"::error::release attestation not found at {attestation_path}. Publication is "
            "gated on an attestation produced by release_candidate_gate; without it the "
            "shipped bytes cannot be proven to be the validated bytes.",
            file=sys.stderr,
        )
        return 1
    try:
        attestation = json.loads(attestation_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"::error::release attestation unreadable: {attestation_path} ({exc})", file=sys.stderr)
        return 1
    if not isinstance(attestation, dict):
        print("::error::release attestation must be a JSON object", file=sys.stderr)
        return 1

    failures: list[str] = []

    if attestation.get("schema_version") != SCHEMA_VERSION:
        failures.append(
            f"attestation schema_version {attestation.get('schema_version')!r} != {SCHEMA_VERSION}"
        )

    attested_commit = _normalize_sha(attestation.get("commit") or "")
    expected_commit = _normalize_sha(args.commit)
    if not attested_commit:
        failures.append("attestation records no commit")
    elif attested_commit != expected_commit:
        failures.append(
            f"attestation commit {attested_commit} != commit being published {expected_commit}"
        )

    files = attestation.get("files")
    if not isinstance(files, dict) or not files:
        failures.append("attestation records no files")
        files = {}

    root = Path(args.root)
    if not root.is_dir():
        failures.append(f"release payload root is not a directory: {root}")
        present: dict[str, dict[str, Any]] = {}
    else:
        present = _collect(root)

    for rel in sorted(files):
        record = files[rel]
        expected_sha = _normalize_sha(record.get("sha256") if isinstance(record, dict) else "")
        if not expected_sha:
            failures.append(f"attested file {rel} records no sha256")
            continue
        actual = present.get(rel)
        if actual is None:
            failures.append(f"attested file is missing from the release payload: {rel}")
            continue
        if actual["sha256"] != expected_sha:
            failures.append(
                f"release payload byte mismatch for {rel}: attested {expected_sha} "
                f"got {actual['sha256']}"
            )

    # An unattested extra file in the payload is an injection: fail closed rather
    # than publishing bytes nobody validated.
    for rel in sorted(set(present) - set(files)):
        failures.append(f"release payload contains an unattested file: {rel}")

    for rel in args.require:
        rel_posix = Path(rel).as_posix()
        if rel_posix not in files:
            failures.append(f"required release asset is not attested: {rel_posix}")
        elif rel_posix not in present:
            failures.append(f"required release asset is missing from the payload: {rel_posix}")

    if failures:
        print("Release attestation verification FAILED:")
        for failure in failures:
            print(f" - {failure}")
            print(f"::error::{failure}")
        return 1

    print(f"Release attestation verified: {len(files)} file(s) match commit {expected_commit}.")
    return 0


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    gen = sub.add_parser("generate", help="Hash a release payload into an attestation JSON.")
    gen.add_argument("--root", required=True, help="Directory holding the release payload.")
    gen.add_argument("--commit", required=True, help="Commit SHA being released.")
    gen.add_argument("--output", required=True, help="Path to write the attestation JSON to.")
    gen.add_argument("--tag", default="", help="Release tag (recorded for traceability).")
    gen.add_argument("--channel", default="", help="Release channel (recorded for traceability).")
    gen.set_defaults(func=_generate)

    ver = sub.add_parser("verify", help="Re-hash a release payload and require it to match an attestation.")
    ver.add_argument("--root", required=True, help="Directory holding the payload about to be published.")
    ver.add_argument("--commit", required=True, help="Commit SHA being published.")
    ver.add_argument("--attestation", required=True, help="Attestation JSON produced by 'generate'.")
    ver.add_argument(
        "--require",
        action="append",
        default=[],
        metavar="RELPATH",
        help="Relative path that must be attested and present (repeatable).",
    )
    ver.set_defaults(func=_verify)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
