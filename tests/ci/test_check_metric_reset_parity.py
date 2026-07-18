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
    to empty so synthetic field names never collide with the real allow-list).

    EXPECTED_NOT_RESET_COUNT is patched to match the isolated allow-list: the
    pin exists to make growth of the REAL allow-list visible, and a fixture
    supplying its own allow-list is not that. AllowListPinTests exercises the
    pin itself directly."""
    fields = {} if not_reset_fields is None else not_reset_fields
    with tempfile.TemporaryDirectory(dir=str(ROOT)) as tmp:
        path = Path(tmp) / "poc_render_performance_types.h"
        path.write_text(text, encoding="utf-8")
        with mock.patch.object(guard, "PERF_HEADER", path), mock.patch.object(
            guard, "NOT_RESET_FIELDS", fields
        ), mock.patch.object(guard, "EXPECTED_NOT_RESET_COUNT", len(fields)):
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
        #
        # The mutator's in-struct DECLARATION is omitted on purpose. Since the
        # constructor-initializer fix, the struct-body parser fails closed on
        # any method declaration that is not a `void reset_*();` (covered by
        # FailClosedParserSurfaceTests), which would short-circuit this test
        # before `_RESET_DEF_RE` is ever reached. `_extract_reset_methods`
        # scans the whole file for out-of-line definitions, so the definition
        # alone is what this test needs -- and isolating it keeps this test
        # aimed at the reset-prefix defect it was written for.
        header = """
struct PerformanceMetrics {
\tuint32_t widget_touch_count = 0;

\tvoid reset_something();
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
        # A struct whose only zero-arg method is non-reset_* must be reported as
        # having found NO reset helpers at all (distinct failure mode from the
        # per-field failure above, exercised for a struct with no reset_* text
        # whatsoever). Declaration omitted for the same reason as above.
        header = """
struct PerformanceMetrics {
\tuint32_t widget_touch_count = 0;
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


class ConstructorInitializedFieldTests(unittest.TestCase):
    """Second reviewer finding on PR #627, confirmed: `_extract_struct_fields`
    skipped ANY struct-body line containing "(" in order to step over the
    `void reset_x();` declarations. A data member with a constructor-call
    initializer therefore disappeared from the check entirely -- the guard
    still printed "all N fields" and exited 0 while N silently excluded it.
    That is a false pass, contradicting the guard's own fail-closed contract.
    """

    COUNTEREXAMPLE = "\tVector2 viewport_extent = Vector2(0, 0);"

    def test_constructor_initialized_field_is_not_silently_dropped(self) -> None:
        # The field has NO disposition: no reset_* coverage, no NOT_RESET entry.
        # Pre-fix the "(" skip made it vanish and the guard PASSED. It must now
        # either be counted (and thus fail as unclassified) or be reported as
        # unrecognized -- in both cases, a FAIL rather than a silent pass.
        header = f"""
struct PerformanceMetrics {{
\tuint32_t legit_field = 0;
{self.COUNTEREXAMPLE}

\tvoid reset_legit_group();
}};

inline void PerformanceMetrics::reset_legit_group() {{
\tlegit_field = 0;
}}
"""
        with _header(header):
            rc, out = _run_main()
        self.assertEqual(rc, 1, f"guard must not pass on an undisposed field:\n{out}")
        self.assertIn("viewport_extent", out)

    def test_constructor_initialized_field_is_parsed_as_a_real_field(self) -> None:
        """Counted, not merely rejected: once given a disposition it passes,
        which proves the field is genuinely being parsed rather than tripping
        a blanket 'anything with parens fails' rule."""
        header = f"""
struct PerformanceMetrics {{
\tuint32_t legit_field = 0;
{self.COUNTEREXAMPLE}

\tvoid reset_legit_group();
}};

inline void PerformanceMetrics::reset_legit_group() {{
\tlegit_field = 0;
\tviewport_extent = Vector2(0, 0);
}}
"""
        with _header(header):
            rc, out = _run_main()
        self.assertEqual(rc, 0, out)
        self.assertIn("all 2 PerformanceMetrics fields", out)

    def test_reset_method_declarations_are_still_skipped(self) -> None:
        """The narrowed skip must still step over the declarations it exists
        for, otherwise the fix would just be always-red."""
        self.assertTrue(guard._METHOD_DECL_RE.match("void reset_legit_group();"))
        self.assertTrue(guard._METHOD_DECL_RE.match("inline void reset_legit_group();"))
        # ...and must NOT swallow a data member that merely looks similar.
        self.assertFalse(guard._METHOD_DECL_RE.match("Vector2 x = Vector2(0, 0);"))
        self.assertFalse(guard._METHOD_DECL_RE.match("void reset_x(int a);"))
        self.assertFalse(guard._METHOD_DECL_RE.match("void other_helper();"))


class FailClosedParserSurfaceTests(unittest.TestCase):
    """The shapes the parser refuses to guess at must stay refused."""

    def _assert_line_fails_closed(self, declaration: str) -> None:
        header = f"""
struct PerformanceMetrics {{
\tuint32_t legit_field = 0;
\t{declaration}

\tvoid reset_legit_group();
}};

inline void PerformanceMetrics::reset_legit_group() {{
\tlegit_field = 0;
}}
"""
        with _header(header):
            rc, out = _run_main()
        self.assertEqual(rc, 1, f"`{declaration}` must fail closed, got PASS:\n{out}")

    def test_brace_initialized_member_fails_closed(self) -> None:
        self._assert_line_fails_closed("Vector2 brace_init{0, 0};")

    def test_templated_type_fails_closed(self) -> None:
        self._assert_line_fails_closed("Vector<float> templated_field;")

    def test_comma_list_declaration_fails_closed(self) -> None:
        self._assert_line_fails_closed("uint32_t first_field = 0, second_field = 0;")

    def test_multi_line_declaration_fails_closed(self) -> None:
        self._assert_line_fails_closed("Vector2 split_decl = Vector2(\n\t\t0, 0);")

    def test_unparsed_method_declaration_fails_closed(self) -> None:
        self._assert_line_fails_closed("void some_other_helper();")


class AllowListPinTests(unittest.TestCase):
    """NOT_RESET_FIELDS could be grown by a one-line append, moving a field
    from "must be reset" to "exempt" with nothing else in the diff changing
    and the reason string never validated."""

    def test_pin_matches_the_committed_allow_list(self) -> None:
        self.assertEqual(len(guard.NOT_RESET_FIELDS), guard.EXPECTED_NOT_RESET_COUNT)

    def test_growing_the_allow_list_without_bumping_the_pin_fails(self) -> None:
        grown = dict(guard.NOT_RESET_FIELDS)
        grown["sneaked_in_field"] = "quietly exempted without review"
        with mock.patch.object(guard, "NOT_RESET_FIELDS", grown):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("EXPECTED_NOT_RESET_COUNT", out)

    def test_shrinking_the_allow_list_without_bumping_the_pin_fails(self) -> None:
        shrunk = dict(guard.NOT_RESET_FIELDS)
        shrunk.pop(next(iter(shrunk)))
        with mock.patch.object(guard, "NOT_RESET_FIELDS", shrunk):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)

    def test_empty_or_placeholder_reason_is_rejected(self) -> None:
        for reason in ("", "   ", "TODO", "tbd", "n/a"):
            with self.subTest(reason=reason):
                fields = dict(guard.NOT_RESET_FIELDS)
                fields[next(iter(fields))] = reason
                with mock.patch.object(guard, "NOT_RESET_FIELDS", fields):
                    rc, out = _run_main()
                self.assertEqual(rc, 1, out)
                self.assertIn("placeholder", out)

    def test_every_committed_reason_is_substantive(self) -> None:
        for field, reason in guard.NOT_RESET_FIELDS.items():
            with self.subTest(field=field):
                self.assertGreaterEqual(len(reason.strip()), guard.MINIMUM_REASON_LENGTH)


if __name__ == "__main__":
    unittest.main()
