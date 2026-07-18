#!/usr/bin/env python3
"""Unit tests for tests/ci/check_metric_reset_parity.py (#528 follow-up).

An external reviewer on PR #627 flagged that the guard's out-of-line-method
regex accepted ANY zero-arg inline mutator on `PerformanceMetrics`, not only
`reset_*` helpers -- so a field touched only by some unrelated no-arg setter
(never a genuine reset) could be counted as "reset-covered" and the guard
would falsely PASS. `test_non_reset_mutator_is_not_counted_as_coverage` below
is that exact counterexample: it must make the guard FAIL. A companion
`test_genuine_reset_helper_provides_coverage` proves the guard still passes a
field that IS covered by a real `reset_*` helper, so the fix is not merely
"always red".

Fixtures never touch the committed header: every synthetic struct lives in a
temp file inside ROOT (`check_metric_reset_parity.main()` computes
`PERF_HEADER.relative_to(ROOT)`, so the fixture must be a real subpath), and
`PERF_HEADER` / `NOT_RESET_FIELDS` are patched via `mock.patch.object` for the
duration of each test.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "check_metric_reset_parity.py"
spec = importlib.util.spec_from_file_location("check_metric_reset_parity", SCRIPT)
assert spec and spec.loader
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)


@contextlib.contextmanager
def _header(text: str, not_reset_fields: dict[str, str] | None = None):
    """Write `text` as a temp PerformanceMetrics header inside ROOT and patch
    the guard module to read it, with an isolated NOT_RESET_FIELDS (defaults
    to empty so synthetic field names never collide with the real allow-list)."""
    with tempfile.TemporaryDirectory(dir=str(ROOT)) as tmp:
        path = Path(tmp) / "poc_render_performance_types.h"
        path.write_text(text, encoding="utf-8")
        with mock.patch.object(guard, "PERF_HEADER", path), mock.patch.object(
            guard, "NOT_RESET_FIELDS", {} if not_reset_fields is None else not_reset_fields
        ):
            yield path


def _run_main() -> tuple[int, str]:
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        rc = guard.main()
    return rc, buffer.getvalue()


class NonResetMutatorCounterexampleTests(unittest.TestCase):
    """The reviewer's claim on PR #627, confirmed: `_RESET_DEF_RE` matched any
    `inline void PerformanceMetrics::<name>() { ... }`, regardless of whether
    <name> started with `reset_`. A field assigned only inside a non-reset
    no-arg mutator was therefore counted as reset-covered."""

    def test_non_reset_mutator_is_not_counted_as_coverage(self) -> None:
        # `bump_widget_touch_count` is a plausible non-reset mutator (an
        # increment) that is NOT a reset_* helper and does not appear in any
        # reset_* helper body. The field it touches has no other disposition
        # (no reset_* coverage, no NOT_RESET_FIELDS entry) -- it is genuinely
        # unclassified and the guard must fail closed on it.
        header = """
struct PerformanceMetrics {
\tuint32_t widget_touch_count = 0;

\tvoid reset_something();
\tvoid bump_widget_touch_count();
};

inline void PerformanceMetrics::reset_something() {
\t// deliberately does not touch widget_touch_count
}

inline void PerformanceMetrics::bump_widget_touch_count() {
\twidget_touch_count = widget_touch_count + 1;
}
"""
        with _header(header):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("widget_touch_count", out)
        self.assertIn("is neither cleared by a reset_*() helper", out)
        # The non-reset mutator must not be silently accepted as a helper.
        self.assertNotIn("bump_widget_touch_count", out)

    def test_non_reset_mutator_alone_yields_zero_reset_helpers(self) -> None:
        # A struct with only a non-reset_* zero-arg method must be reported as
        # having found NO reset helpers at all (distinct failure mode from the
        # per-field failure above, exercised for a struct with no reset_* text
        # whatsoever).
        header = """
struct PerformanceMetrics {
\tuint32_t widget_touch_count = 0;

\tvoid bump_widget_touch_count();
};

inline void PerformanceMetrics::bump_widget_touch_count() {
\twidget_touch_count = widget_touch_count + 1;
}
"""
        with _header(header):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("found no `inline void PerformanceMetrics::reset_*()` helpers", out)


class GenuineResetHelperCoverageTests(unittest.TestCase):
    """Positive control: the guard is not merely always-red after the fix."""

    def test_genuine_reset_helper_provides_coverage(self) -> None:
        header = """
struct PerformanceMetrics {
\tuint32_t legit_field = 0;

\tvoid reset_legit_group();
};

inline void PerformanceMetrics::reset_legit_group() {
\tlegit_field = 0;
}
"""
        with _header(header):
            rc, out = _run_main()
        self.assertEqual(rc, 0, out)
        self.assertIn("PASSED", out)

    def test_not_reset_allow_listed_field_still_passes(self) -> None:
        header = """
struct PerformanceMetrics {
\tuint32_t legit_field = 0;
\tuint32_t cumulative_counter = 0;

\tvoid reset_legit_group();
};

inline void PerformanceMetrics::reset_legit_group() {
\tlegit_field = 0;
}
"""
        with _header(
            header, not_reset_fields={"cumulative_counter": "cumulative lifetime counter"}
        ):
            rc, out = _run_main()
        self.assertEqual(rc, 0, out)

    def test_real_header_passes_the_tightened_guard(self) -> None:
        # Regression check: the tightened regex must not newly flag any of the
        # committed PerformanceMetrics fields. If this starts failing, it is a
        # genuine coverage gap (fix the metric or add a justified
        # NOT_RESET_FIELDS entry) -- never weaken the regex to silence it.
        rc, out = _run_main()
        self.assertEqual(rc, 0, out)


if __name__ == "__main__":
    unittest.main()
