#!/usr/bin/env python3
"""Unit tests for the release attestation (validated-bytes == shipped-bytes)."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "release_attestation.py"
spec = importlib.util.spec_from_file_location("release_attestation", SCRIPT)
assert spec and spec.loader
attest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(attest)

COMMIT = "a" * 40


class ReleaseAttestationTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.payload = self.tmp / "payload"
        (self.payload / "linux").mkdir(parents=True)
        (self.payload / "linux" / "godotgs-linux-x86_64-v1.0.tar.xz").write_bytes(b"linux-archive")
        (self.payload / "linux" / "BUILD-INFO.txt").write_text(f"commit={COMMIT}\n", encoding="utf-8")
        self.attestation = self.tmp / "att" / "release-attestation.json"

    @staticmethod
    def _run_main(args: list[str]) -> int:
        """Keep expected negative-path diagnostics out of the enclosing CI log."""
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return attest.main(args)

    def _generate(self, commit: str = COMMIT) -> int:
        return self._run_main(
            [
                "generate",
                "--root", str(self.payload),
                "--commit", commit,
                "--output", str(self.attestation),
                "--tag", "v1.0",
                "--channel", "stable",
            ]
        )

    def _verify(self, root: Path | None = None, commit: str = COMMIT, require: list[str] | None = None) -> int:
        args = [
            "verify",
            "--root", str(root or self.payload),
            "--commit", commit,
            "--attestation", str(self.attestation),
        ]
        for rel in require or []:
            args += ["--require", rel]
        return self._run_main(args)

    def test_generate_then_verify_passes(self) -> None:
        self.assertEqual(0, self._generate())
        self.assertEqual(0, self._verify(require=["linux/godotgs-linux-x86_64-v1.0.tar.xz"]))

    def test_generate_records_digests_and_commit(self) -> None:
        self._generate()
        data = json.loads(self.attestation.read_text(encoding="utf-8"))
        self.assertEqual(COMMIT, data["commit"])
        self.assertIn("linux/godotgs-linux-x86_64-v1.0.tar.xz", data["files"])

    def test_refuses_to_attest_empty_payload(self) -> None:
        empty = self.tmp / "empty"
        empty.mkdir()
        rc = self._run_main(
            ["generate", "--root", str(empty), "--commit", COMMIT, "--output", str(self.attestation)]
        )
        self.assertEqual(1, rc)

    def test_rebuilt_archive_fails_closed(self) -> None:
        """The publish job must reject a payload whose bytes changed after the gate."""
        self._generate()
        (self.payload / "linux" / "godotgs-linux-x86_64-v1.0.tar.xz").write_bytes(b"REBUILT-DIFFERENT")
        self.assertEqual(1, self._verify())

    def test_missing_file_fails_closed(self) -> None:
        self._generate()
        (self.payload / "linux" / "BUILD-INFO.txt").unlink()
        self.assertEqual(1, self._verify())

    def test_injected_extra_file_fails_closed(self) -> None:
        self._generate()
        (self.payload / "linux" / "evil.exe").write_bytes(b"payload")
        self.assertEqual(1, self._verify())

    def test_commit_mismatch_fails_closed(self) -> None:
        self._generate()
        self.assertEqual(1, self._verify(commit="b" * 40))

    def test_missing_attestation_fails_closed(self) -> None:
        self.assertFalse(self.attestation.exists())
        self.assertEqual(1, self._verify())

    def test_required_asset_absent_from_attestation_fails_closed(self) -> None:
        self._generate()
        self.assertEqual(1, self._verify(require=["windows/godotgs-windows-x86_64-v1.0.zip"]))

    def test_malformed_attestation_fails_closed(self) -> None:
        self.attestation.parent.mkdir(parents=True, exist_ok=True)
        self.attestation.write_text("not json", encoding="utf-8")
        self.assertEqual(1, self._verify())

    def test_schema_version_mismatch_fails_closed(self) -> None:
        self._generate()
        data = json.loads(self.attestation.read_text(encoding="utf-8"))
        data["schema_version"] = 99
        self.attestation.write_text(json.dumps(data), encoding="utf-8")
        self.assertEqual(1, self._verify())


if __name__ == "__main__":
    unittest.main()
