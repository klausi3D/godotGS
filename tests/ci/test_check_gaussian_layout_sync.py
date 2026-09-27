#!/usr/bin/env python3
"""Wiring tests for tests/ci/check_gaussian_layout_sync.py (#54).

The IndirectDispatch ABI pin is one function, `_check_indirect_dispatch_abi`, invoked by one
line in `main()`. Deleting that line used to leave the script exiting 0 with every ABI and
binding check unreachable -- and its coverage summary was printed from the pin lists, not from
anything the check did, so the output looked identical (Codex review on PR #1041). These tests
drive the REAL `main()` against the committed tree and assert the check was reached and
examined something; `run_module_tests.py` runs this file before the guard itself.

Nothing is patched except a pass-through wrapper that records calls, so the tests see exactly
what the guard lane sees.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import re
import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "check_gaussian_layout_sync.py"
spec = importlib.util.spec_from_file_location("check_gaussian_layout_sync", SCRIPT)
assert spec and spec.loader
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)


class IndirectDispatchAbiWiringTests(unittest.TestCase):
    def _run_main_recording_abi_calls(self) -> tuple[list[dict[str, int]], str]:
        real = guard._check_indirect_dispatch_abi
        results: list[dict[str, int]] = []

        def recording(failures: list[str]) -> dict[str, int]:
            coverage = real(failures)
            results.append(coverage)
            return coverage

        stdout = io.StringIO()
        with mock.patch.object(guard, "_check_indirect_dispatch_abi", recording):
            with contextlib.redirect_stdout(stdout):
                guard.main()
        return results, stdout.getvalue()

    def test_main_reaches_the_indirect_dispatch_abi_check(self) -> None:
        results, _ = self._run_main_recording_abi_calls()
        self.assertEqual(
            len(results),
            1,
            "main() must call _check_indirect_dispatch_abi exactly once; without that call every "
            "IndirectDispatch ABI and binding pin is unreachable while the guard still exits 0",
        )

    def test_the_check_examined_every_kind_of_site(self) -> None:
        """Reached is not enough: a check that examined nothing is equally decorative."""
        results, _ = self._run_main_recording_abi_calls()
        if len(results) != 1:
            self.fail(f"expected one _check_indirect_dispatch_abi call, got {len(results)}")
            return
        coverage = results[0]
        for key in ("glsl_declarations", "host_binding_sites", "host_ctor_binding_sites", "host_struct_mirrors"):
            with self.subTest(key=key):
                self.assertGreater(coverage.get(key, 0), 0, f"_check_indirect_dispatch_abi examined no {key}")

    def test_summary_reports_what_the_check_examined(self) -> None:
        """The PASSED summary must be derived from the check's own result, not the pin lists,
        so it cannot claim coverage for a check that did not run."""
        results, output = self._run_main_recording_abi_calls()
        if len(results) != 1:
            self.fail(f"expected one _check_indirect_dispatch_abi call, got {len(results)}")
            return
        coverage = results[0]
        match = re.search(
            r"IndirectDispatchLayout is pinned across (\d+) GLSL declaration\(s\) .*, (\d+) host append_id "
            r"binding site\(s\), (\d+) host constructor binding site\(s\), and (\d+) host struct mirror\(s\)",
            output,
        )
        if match is None:
            self.fail(f"no IndirectDispatch coverage line in guard output:\n{output}")
            return
        self.assertEqual(
            tuple(int(group) for group in match.groups()),
            (
                coverage["glsl_declarations"],
                coverage["host_binding_sites"],
                coverage["host_ctor_binding_sites"],
                coverage["host_struct_mirrors"],
            ),
        )


if __name__ == "__main__":
    unittest.main()
