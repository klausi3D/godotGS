#!/usr/bin/env python3
"""Verify public owner grants using immutable policy and standard OpenSSH SSHSIG.

Transport supplies only a typed binding and signature. Keys, executable and
allowed-signers policy come exclusively from the trusted default commit bundle.
The shipped null key/tool pins deliberately disable every SUCCESS path.
"""
from __future__ import annotations

import base64
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

CONFIG_PATH = "scripts/agentic/owner_grant_policy.json"
NAMESPACE = "godotgs-owner-grant-v1"
PRINCIPAL = "godotgs-personal-owner"
EXECUTABLE = "/usr/bin/ssh-keygen"
REPOSITORY = "klausi3D/godotGS"
OWNER_ID = 103386187
MAX_ENVELOPE = 16384
BINDING_KEYS = {"version", "kind", "repository", "owner_id", "target_branch",
                "base_sha", "policy_sha", "head_sha", "context", "app_id",
                "run_id", "run_attempt", "expires_utc"}


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate owner-grant JSON key")
        result[key] = value
    return result


def parse_transport(raw: str) -> dict:
    if not isinstance(raw, str) or len(raw.encode("utf-8")) > MAX_ENVELOPE:
        raise ValueError("Owner grant transport exceeds its bound")
    value = json.loads(raw, object_pairs_hook=unique_object)
    if not isinstance(value, dict):
        raise ValueError("Owner grant transport must be a JSON object")
    return value


def configured_profile(bundle: dict[str, bytes]) -> dict:
    mode, raw = bundle[CONFIG_PATH].split(b"\0", 1)
    profile = json.loads(raw, object_pairs_hook=unique_object)
    keys = {"version", "repository", "owner_id", "public_key", "namespace",
            "principal", "executable", "executable_sha256"}
    if (mode != b"100644" or not isinstance(profile, dict) or set(profile) != keys
            or profile["version"] != "gs-owner-verifier-v1"
            or profile["repository"] != REPOSITORY
            or type(profile["owner_id"]) is not int or profile["owner_id"] != OWNER_ID
            or profile["namespace"] != NAMESPACE or profile["principal"] != PRINCIPAL
            or profile["executable"] != EXECUTABLE):
        raise ValueError("Unsupported immutable owner verifier policy")
    key = profile["public_key"]
    if not isinstance(key, str) or not re.fullmatch(r"ssh-ed25519 [A-Za-z0-9+/]+={0,2}", key):
        raise ValueError("NOT ACTIVE: reviewed owner Ed25519 public key is absent or invalid")
    try:
        blob = base64.b64decode(key.split()[1], validate=True)
    except ValueError as exc:
        raise ValueError("Malformed immutable owner public key") from exc
    # Exact SSH Ed25519 wire encoding: string algorithm, string 32-byte key.
    if len(blob) != 51 or blob[:19] != b"\0\0\0\x0bssh-ed25519\0\0\0\x20":
        raise ValueError("Unsupported owner SSH public key encoding")
    digest = profile["executable_sha256"]
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("NOT ACTIVE: reviewed verifier executable pin is absent")
    if sys.platform != "linux":
        raise ValueError("Owner verification requires the supported Linux hosted tool profile")
    tool = Path(EXECUTABLE)
    if (not stat.S_ISREG(tool.lstat().st_mode) or not os.access(tool, os.X_OK)
            or hashlib.sha256(tool.read_bytes()).hexdigest() != digest):
        raise ValueError("Immutable owner verifier executable drift")
    return profile


def canonical_binding(binding: dict) -> bytes:
    return json.dumps(binding, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def verify_grant(envelope: dict, bundle: dict[str, bytes], *, kind: str,
                 repo: str, owner: dict, base: str, head: str, app_id: int,
                 context: str | None = None, now: datetime | None = None) -> dict:
    profile = configured_profile(bundle)
    if not isinstance(envelope, dict) or set(envelope) != {"binding", "signature"}:
        raise ValueError("Unsigned or ambiguous owner grant envelope")
    if len(json.dumps(envelope, ensure_ascii=True).encode()) > MAX_ENVELOPE:
        raise ValueError("Owner grant envelope exceeds its bound")
    binding, signature = envelope["binding"], envelope["signature"]
    if not isinstance(binding, dict) or set(binding) != BINDING_KEYS:
        raise ValueError("Owner grant requires exact complete bindings")
    if (kind not in {"bootstrap", "migration"} or binding["version"] != "gs-owner-grant-v1"
            or binding["kind"] != kind or repo != profile["repository"]
            or binding["repository"] != repo or binding["target_branch"] != "master"
            or owner.get("type") != "User" or owner.get("login") != repo.split("/")[0]
            or type(owner.get("id")) is not int or owner["id"] != profile["owner_id"]
            or type(binding["owner_id"]) is not int or binding["owner_id"] != owner["id"]
            or type(app_id) is not int or app_id <= 0
            or type(binding["app_id"]) is not int or binding["app_id"] != app_id
            or binding["base_sha"] != base or binding["head_sha"] != head):
        raise ValueError("Owner grant identity/kind/base/head/App changed")
    for key in ("base_sha", "policy_sha", "head_sha"):
        if not isinstance(binding[key], str) or not re.fullmatch(r"[0-9a-f]{40}", binding[key]):
            raise ValueError("Owner grant requires canonical immutable SHAs")
    if ((kind == "bootstrap" and binding["policy_sha"] != base)
            or (kind == "migration" and binding["policy_sha"] in {base, head})):
        raise ValueError("Owner grant policy commit has the wrong role")
    if (not isinstance(binding["context"], str)
            or not re.fullmatch(r"gpu-code-evidence-v1-[0-9a-f]{64}", binding["context"])
            or (context is not None and binding["context"] != context)):
        raise ValueError("Owner grant policy context changed")
    for key in ("run_id", "run_attempt"):
        if not isinstance(binding[key], str) or not re.fullmatch(r"[1-9][0-9]*", binding[key]):
            raise ValueError("Owner grant requires exact positive producer run/attempt strings")
    if not isinstance(binding["expires_utc"], str):
        raise ValueError("Owner grant expiry must be an explicit UTC timestamp")
    expiry = datetime.fromisoformat(binding["expires_utc"])
    if expiry.utcoffset() != timezone.utc.utcoffset(expiry) or expiry <= (now or datetime.now(timezone.utc)):
        raise ValueError("Owner grant expired or lacks an explicit UTC offset")
    if (not isinstance(signature, str) or len(signature) > 4096
            or not re.fullmatch(r"-----BEGIN SSH SIGNATURE-----\n[A-Za-z0-9+/=\n]+\n-----END SSH SIGNATURE-----\n?", signature)):
        raise ValueError("Owner grant requires bounded ASCII SSHSIG armor")
    with tempfile.TemporaryDirectory(prefix="gs-owner-verify-") as scratch:
        signature_path = Path(scratch) / "grant.sig"
        signers_path = Path(scratch) / "allowed_signers"
        signature_path.write_text(signature, encoding="ascii")
        signers_path.write_text(f'{PRINCIPAL} namespaces="{NAMESPACE}" {profile["public_key"]}\n', encoding="ascii")
        result = subprocess.run([EXECUTABLE, "-Y", "verify", "-f", str(signers_path),
                                 "-I", PRINCIPAL, "-n", NAMESPACE, "-s", str(signature_path)],
                                input=canonical_binding(binding), capture_output=True,
                                timeout=20, shell=False, check=False)
    if result.returncode != 0:
        raise ValueError("Owner grant signature rejected by pinned OpenSSH")
    return dict(binding)
