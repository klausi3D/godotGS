#!/usr/bin/env python3
"""Unit tests for tests/ci/check_reject_telemetry_parity.py (#586 round-4 / round-5).

The guard's job is to make "every per-frame telemetry field is invalidated on a
rejected frame" a CI failure rather than a review finding. Two review rounds in a row
found a field missing from `_reject_frame()`'s hand-written assignment list, so what
matters is not that the guard passes on today's tree -- it is that it FAILS on the
shape of the next missing field.

These tests pin exactly that, against synthetic fixtures rather than the committed
sources, so they keep discriminating even after the real files are fixed:

  * `test_uncovered_field_fails`            -- the round-4 defect shape (and round-3's:
                                               they are the same shape).
  * `test_cleared_field_passes`             -- the fix is not merely "always red".
  * `test_exempt_field_passes`              -- a justified exemption is accepted.
  * `test_covered_and_exempt_fails`         -- a stale exemption is reported.
  * `test_nontrivial_getter_fails`          -- fail closed when backing state cannot be
                                               resolved, instead of skipping the field.
  * `test_unpopulated_field_fails`          -- a field get_performance() never fills.
  * `test_stale_exemption_fails`            -- an exemption naming a dead field.
  * pin / reason tests                      -- the allow-list cannot grow invisibly.

`InvalidationValueTests` pins the round-5 finding: the guard used to record only the
TARGET of each `_reject_frame()` assignment, so an assignment that invalidates NOTHING
satisfied it -- `renderer.timing_state.last_widget_cpu_ms = renderer.timing_state.
last_widget_cpu_ms;` passed, as did `= 42.0f` and `= true` on a validity flag. A
completeness check a no-op can satisfy is not a completeness check, so those three, plus
an assignment hidden inside a conditional or a preprocessor branch, are now RED, and a
type the guard has no invalid-value rule for fails rather than being assumed fine.

`UnbracedControlFlowTests` pins the round-8 review finding on the guard itself: "at the
top brace level" was established from brace depth alone, and an unbraced conditional's
body sits at brace depth 0. `if (condition)` followed on the next line by the
invalidation was therefore credited for EVERY reject while every other rejection path
kept publishing stale telemetry -- the defect this blocking guard exists to prevent,
passing green. The unbraced `if`, `else` arm and loop body are now RED, braced control
flow still resolves (so the fix is not "reject everything"), and literals cannot shift
the structure.

Fixtures never touch the committed sources: each synthetic file lives in a temp dir
inside ROOT (the guard prints paths `relative_to(ROOT)`, so fixtures must be real
subpaths), and the four module-level path constants plus the allow-list are patched
via `mock.patch.object` for the duration of each test.
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
SCRIPT = ROOT / "tests" / "ci" / "check_reject_telemetry_parity.py"
spec = importlib.util.spec_from_file_location("check_reject_telemetry_parity", SCRIPT)
assert spec and spec.loader
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)


def _struct(fields: str) -> str:
    return "struct RasterPerformance {\n" + fields + "};\n"


def _populator(assignments: str) -> str:
    return (
        "RasterPerformance TileRasterizer::get_performance() const {\n"
        "    RasterPerformance perf;\n"
        "    if (!tile_renderer.is_valid()) {\n"
        "        return perf;\n"
        "    }\n" + assignments + "    return perf;\n}\n"
    )


def _renderer_header(getters: str, other_classes: str = "") -> str:
    """`other_classes` is emitted BEFORE `class TileRenderer` so tests can prove that a
    same-named getter on a different class in the same header does not resolve."""
    return other_classes + "class TileRenderer {\npublic:\n" + getters + "};\n"


def _renderer_source(assignments: str) -> str:
    return (
        "RID _reject_frame(GaussianSplatting::FrameRejectStage p_stage) {\n"
        + assignments
        + "\treturn RID();\n}\n"
    )


@contextlib.contextmanager
def _tree(
    struct_text: str,
    populator_text: str,
    header_text: str,
    source_text: str,
    survives: dict[str, str] | None = None,
):
    """Write the four synthetic sources inside ROOT and point the guard at them.

    `SURVIVES_REJECT_FIELDS` defaults to empty so synthetic field names never collide
    with the real allow-list, and the pin is patched to match -- the pin exists to make
    growth of the REAL allow-list visible, which a fixture supplying its own list is
    not. AllowListPinTests exercises the pin itself."""
    fields = {} if survives is None else survives
    with tempfile.TemporaryDirectory(dir=str(ROOT)) as tmp:
        base = Path(tmp)
        struct_path = base / "poc_rasterizer_interfaces.h"
        populator_path = base / "poc_tile_rasterizer.cpp"
        header_path = base / "poc_tile_renderer.h"
        source_path = base / "poc_tile_renderer.cpp"
        struct_path.write_text(struct_text, encoding="utf-8")
        populator_path.write_text(populator_text, encoding="utf-8")
        header_path.write_text(header_text, encoding="utf-8")
        source_path.write_text(source_text, encoding="utf-8")
        with mock.patch.object(guard, "PERF_STRUCT_HEADER", struct_path), mock.patch.object(
            guard, "POPULATOR_SOURCE", populator_path
        ), mock.patch.object(guard, "RENDERER_HEADER", header_path), mock.patch.object(
            guard, "RENDERER_SOURCE", source_path
        ), mock.patch.object(
            guard, "SURVIVES_REJECT_FIELDS", fields
        ), mock.patch.object(
            guard, "EXPECTED_SURVIVES_REJECT_COUNT", len(fields)
        ):
            yield


def _run_main() -> tuple[int, str]:
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        rc = guard.main()
    return rc, buffer.getvalue()


# A minimal but complete chain for one field, parameterised by whether _reject_frame
# clears it. This is the exact shape of the real code:
#   struct field -> perf.<field> = tile_renderer-><getter>();
#   -> <getter>() const { return timing_state.<member>; }
#   -> renderer.timing_state.<member> = ...;
_STRUCT_ONE = _struct("    float widget_cpu_ms = 0.0f;\n")
_POPULATOR_ONE = _populator("    perf.widget_cpu_ms = tile_renderer->get_widget_cpu_ms();\n")
_HEADER_ONE = _renderer_header(
    "\tfloat get_widget_cpu_ms() const { return timing_state.last_widget_cpu_ms; }\n"
)


class RejectCoverageTests(unittest.TestCase):
    def test_uncovered_field_fails(self) -> None:
        """THE round-4 defect shape: a per-frame CPU timing that _reject_frame() does
        not clear, with no exemption. Structurally identical to round-3's finding, so
        this one test pins both rounds' regressions."""
        source = _renderer_source("\trenderer.perf_metrics.rasterization_ms = 0.0f;\n")
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("widget_cpu_ms", out)
        self.assertIn("REJECTED frame does not invalidate", out)
        # The message must name the backing member, or a fixer cannot act on it.
        self.assertIn("timing_state.last_widget_cpu_ms", out)

    def test_cleared_field_passes(self) -> None:
        """The control: the guard is not simply always-red. A field whose backing
        member IS assigned in _reject_frame() is accepted with no allow-list entry."""
        source = _renderer_source("\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n")
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 0, out)
        self.assertIn("PASSED", out)

    def test_exempt_field_passes(self) -> None:
        source = _renderer_source("\trenderer.perf_metrics.rasterization_ms = 0.0f;\n")
        survives = {"widget_cpu_ms": "cumulative lifetime counter, must survive a reject"}
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source, survives=survives):
            rc, out = _run_main()
        self.assertEqual(rc, 0, out)
        self.assertIn("PASSED", out)

    def test_covered_and_exempt_fails(self) -> None:
        """A field cannot be both cleared and exempt: one of the two is stale, and
        leaving the exemption behind would hide a later removal of the clear."""
        source = _renderer_source("\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n")
        survives = {"widget_cpu_ms": "cumulative lifetime counter, must survive a reject"}
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source, survives=survives):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("both cleared by _reject_frame()", out)

    def test_stale_exemption_fails(self) -> None:
        source = _renderer_source("\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n")
        survives = {"field_that_no_longer_exists": "was removed in an earlier refactor"}
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source, survives=survives):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("field_that_no_longer_exists", out)
        self.assertIn("remove the stale entry", out)


class InvalidationValueTests(unittest.TestCase):
    """An assignment is not an invalidation. These pin the round-5 review finding: the
    guard recorded only the left-hand side, so a no-op satisfied it."""

    def test_self_assignment_fails(self) -> None:
        """The reported case, verbatim: the field is 'assigned' to itself, so the last
        SUCCESSFUL frame's CPU cost keeps being published on a rejected frame."""
        source = _renderer_source(
            "\trenderer.timing_state.last_widget_cpu_ms = renderer.timing_state.last_widget_cpu_ms;\n"
        )
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("widget_cpu_ms", out)
        self.assertIn("not the invalid state for a `float`", out)

    def test_nonzero_constant_fails(self) -> None:
        source = _renderer_source("\trenderer.timing_state.last_widget_cpu_ms = 42.0f;\n")
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("`42.0f`", out)
        self.assertIn("expected 0.0f", out)

    def test_validity_flag_set_true_fails(self) -> None:
        """Worse than a stale duration: the flag is the only thing that tells a consumer
        'this stage did not run' rather than 'it ran and measured 0'."""
        struct = _struct("    bool widget_cpu_valid = false;\n")
        populator = _populator("    perf.widget_cpu_valid = tile_renderer->is_widget_cpu_valid();\n")
        header = _renderer_header(
            "\tbool is_widget_cpu_valid() const { return timing_state.widget_cpu_valid; }\n"
        )
        source = _renderer_source("\trenderer.timing_state.widget_cpu_valid = true;\n")
        with _tree(struct, populator, header, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("`true`", out)
        self.assertIn("expected false", out)

    def test_conditional_assignment_fails(self) -> None:
        """Invalidated on SOME rejects is the bug, not the fix: the assignment must be
        unconditional at the top level of _reject_frame()."""
        source = _renderer_source(
            "\tif (p_stage == GaussianSplatting::FrameRejectStage::SETTINGS) {\n"
            "\t\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n"
            "\t}\n"
        )
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("CONDITIONALLY REACHED", out)

    def test_preprocessor_directive_in_reject_body_fails(self) -> None:
        """An invalidation the guard cannot prove compiles must not be credited."""
        source = _renderer_source(
            "#ifdef DEBUG_ENABLED\n\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n#endif\n"
        )
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("_reject_frame() contains a preprocessor directive", out)

    def test_unknown_field_type_fails(self) -> None:
        """Fail closed on a type with no invalid-value rule: without one the guard cannot
        distinguish an invalidation from a no-op, and guessing is the original defect."""
        struct = _struct("    Vector2i widget_extent = Vector2i();\n")
        populator = _populator("    perf.widget_extent = tile_renderer->get_widget_extent();\n")
        header = _renderer_header(
            "\tVector2i get_widget_extent() const { return timing_state.widget_extent; }\n"
        )
        source = _renderer_source("\trenderer.timing_state.widget_extent = Vector2i();\n")
        with _tree(struct, populator, header, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("no invalid-value rule for the declared type `Vector2i`", out)

    def test_zero_forms_pass_for_each_known_type(self) -> None:
        """The control for the value check: bool/float/integer/String fields cleared to
        their own invalid literal are accepted, so the guard is not simply always-red."""
        struct = _struct(
            "    bool widget_valid = false;\n"
            "    float widget_ms = 0.0f;\n"
            "    uint64_t widget_count = 0;\n"
            "    String widget_reason;\n"
        )
        populator = _populator(
            "    perf.widget_valid = tile_renderer->is_widget_valid();\n"
            "    perf.widget_ms = tile_renderer->get_widget_ms();\n"
            "    perf.widget_count = tile_renderer->get_widget_count();\n"
            "    perf.widget_reason = tile_renderer->get_widget_reason();\n"
        )
        header = _renderer_header(
            "\tbool is_widget_valid() const { return timing_state.widget_valid; }\n"
            "\tfloat get_widget_ms() const { return timing_state.widget_ms; }\n"
            "\tuint64_t get_widget_count() const { return timing_state.widget_count; }\n"
            "\tString get_widget_reason() const { return timing_state.widget_reason; }\n"
        )
        source = _renderer_source(
            "\trenderer.timing_state.widget_valid = false;\n"
            "\trenderer.timing_state.widget_ms = 0.0f;\n"
            "\trenderer.timing_state.widget_count = 0;\n"
            "\trenderer.timing_state.widget_reason = String();\n"
        )
        with _tree(struct, populator, header, source):
            rc, out = _run_main()
        self.assertEqual(rc, 0, out)
        self.assertIn("PASSED", out)


class UnbracedControlFlowTests(unittest.TestCase):
    """Round-8: braces are not control flow.

    "Unconditional" was established from brace depth alone, and an unbraced conditional's
    body sits at brace depth 0. The guard therefore credited it for EVERY reject while the
    other rejection paths kept publishing the last successful frame's telemetry -- the exact
    defect this blocking guard exists to prevent, passing green. These tests pin that the
    shape is now refused, and that legal braced control flow still resolves."""

    def test_unbraced_conditional_assignment_fails(self) -> None:
        """THE round-8 finding, verbatim: `if (condition)` followed on the NEXT line by the
        invalidation. Brace depth reports it at 0; statement depth does not."""
        source = _renderer_source(
            "\tif (p_stage != GaussianSplatting::FrameRejectStage::DEVICE)\n"
            "\t\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n"
        )
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("UNBRACED control statement body", out)

    def test_unbraced_else_arm_fails(self) -> None:
        source = _renderer_source(
            "\tif (p_stage == GaussianSplatting::FrameRejectStage::DEVICE) {\n"
            "\t\t(void)p_stage;\n"
            "\t} else\n"
            "\t\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n"
        )
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("UNBRACED control statement body", out)

    def test_unbraced_loop_body_fails(self) -> None:
        """Not only `if`: a body governed by an unbraced `for` runs zero or more times, and
        zero is exactly the stale-telemetry case."""
        source = _renderer_source(
            "\tfor (int i = 0; i < frame_count; ++i)\n"
            "\t\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n"
        )
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("UNBRACED control statement body", out)

    def test_unbraced_conditional_before_a_valid_invalidation_fails(self) -> None:
        """The assignment itself is fine and unconditional; an unbraced conditional
        elsewhere in the body still fails the guard, because once the body contains a shape
        whose extent the guard must guess at, nothing in it can be certified."""
        source = _renderer_source(
            "\tif (p_stage == GaussianSplatting::FrameRejectStage::DEVICE)\n"
            "\t\tlog_reject();\n"
            "\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n"
        )
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("UNBRACED control statement body", out)

    def test_braced_control_flow_still_resolves(self) -> None:
        """The negative control: the fix must not 'pass' by rejecting every shape. A braced
        `for`, a braced `if / else if / else` chain and a braced `do { } while (...)` all
        parse, and a top-level invalidation after them is still credited."""
        source = _renderer_source(
            "\tfor (int i = 0; i < 2; ++i) {\n"
            "\t\tif (i == 0) {\n"
            "\t\t\t(void)i;\n"
            "\t\t} else if (i == 1) {\n"
            "\t\t\t(void)p_stage;\n"
            "\t\t} else {\n"
            "\t\t\t(void)i;\n"
            "\t\t}\n"
            "\t}\n"
            "\tdo {\n"
            "\t\t(void)p_stage;\n"
            "\t} while (false);\n"
            "\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n"
        )
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 0, out)
        self.assertIn("PASSED", out)

    def test_braces_inside_string_literals_do_not_shift_structure(self) -> None:
        """A brace, paren or semicolon inside a literal must not be read as structure --
        otherwise a log line could make a conditional look unconditional or the reverse."""
        source = _renderer_source(
            '\tGS_LOG_WARN_DEFAULT("reject { ; ) stage %s", name);\n'
            "\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n"
        )
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 0, out)
        self.assertIn("PASSED", out)


class FailClosedResolutionTests(unittest.TestCase):
    """The guard resolves backing state through a narrow, trivial getter shape. When it
    cannot resolve a field it must SAY SO, not skip the field -- a skipped field is a
    silent false pass, which is the failure mode this whole guard exists to prevent."""

    def test_nontrivial_getter_fails(self) -> None:
        # A second, trivially-resolvable field is present so the guard's earlier
        # "parsed no inline getters at all" bail does not short-circuit this test
        # before reaching the per-field resolution it is aimed at.
        struct = _struct("    float widget_cpu_ms = 0.0f;\n    float other_cpu_ms = 0.0f;\n")
        populator = _populator(
            "    perf.widget_cpu_ms = tile_renderer->get_widget_cpu_ms();\n"
            "    perf.other_cpu_ms = tile_renderer->get_other_cpu_ms();\n"
        )
        header = _renderer_header(
            "\tfloat get_widget_cpu_ms() const { return timing_state.a + timing_state.b; }\n"
            "\tfloat get_other_cpu_ms() const { return timing_state.last_other_cpu_ms; }\n"
        )
        source = _renderer_source(
            "\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n"
            "\trenderer.timing_state.last_other_cpu_ms = 0.0f;\n"
        )
        with _tree(struct, populator, header, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("not a simple", out)
        self.assertIn("do not drop the field from the check", out)

    def test_unpopulated_field_fails(self) -> None:
        """A struct field get_performance() never assigns cannot be resolved, so it must
        be classified by a human rather than silently ignored."""
        # Again a second, fully-wired field, so the failure under test is the per-field
        # one and not the guard's "no populators parsed at all" bail.
        struct = _struct("    float widget_cpu_ms = 0.0f;\n    float other_cpu_ms = 0.0f;\n")
        populator = _populator("    perf.other_cpu_ms = tile_renderer->get_other_cpu_ms();\n")
        header = _renderer_header(
            "\tfloat get_other_cpu_ms() const { return timing_state.last_other_cpu_ms; }\n"
        )
        source = _renderer_source("\trenderer.timing_state.last_other_cpu_ms = 0.0f;\n")
        with _tree(struct, populator, header, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("widget_cpu_ms", out)
        self.assertIn("never assigned in get_performance()", out)

    def test_getter_on_another_class_does_not_resolve(self) -> None:
        """Same weakness one link up: matching a getter by NAME anywhere in the header
        would resolve a name, not the member `_reject_frame()` clears. Getters are read
        from `class TileRenderer` only, so a lookalike elsewhere leaves the field
        unresolved -- and unresolved is reported, never skipped."""
        struct = _struct("    float widget_cpu_ms = 0.0f;\n    float other_cpu_ms = 0.0f;\n")
        populator = _populator(
            "    perf.widget_cpu_ms = tile_renderer->get_widget_cpu_ms();\n"
            "    perf.other_cpu_ms = tile_renderer->get_other_cpu_ms();\n"
        )
        header = _renderer_header(
            "\tfloat get_other_cpu_ms() const { return timing_state.last_other_cpu_ms; }\n",
            other_classes=(
                "class SomeOtherRenderer {\npublic:\n"
                "\tfloat get_widget_cpu_ms() const { return timing_state.last_widget_cpu_ms; }\n"
                "};\n"
            ),
        )
        source = _renderer_source(
            "\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n"
            "\trenderer.timing_state.last_other_cpu_ms = 0.0f;\n"
        )
        with _tree(struct, populator, header, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("widget_cpu_ms", out)
        self.assertIn("not a simple", out)

    def test_missing_renderer_class_fails(self) -> None:
        header = "class NotTheRenderer {\npublic:\n\tfloat get_widget_cpu_ms() const { return a.b; }\n};\n"
        source = _renderer_source("\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n")
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, header, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("class TileRenderer", out)

    def test_missing_reject_anchor_fails(self) -> None:
        source = "RID _some_other_function() {\n\treturn RID();\n}\n"
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("could not find", out)

    def test_empty_reject_body_fails(self) -> None:
        """A reject boundary that invalidates nothing at all is reported rather than
        treated as 'no fields covered, check the allow-list'."""
        source = _renderer_source("")
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("assigns no", out)

    def test_unrecognized_struct_line_fails(self) -> None:
        struct = _struct("    float widget_cpu_ms = 0.0f;\n    float a, b;\n")
        source = _renderer_source("\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n")
        with _tree(struct, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("unrecognized declaration", out)

    def test_preprocessor_directive_in_struct_fails(self) -> None:
        struct = _struct("    float widget_cpu_ms = 0.0f;\n#ifdef DEV_ENABLED\n    int d = 0;\n#endif\n")
        source = _renderer_source("\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n")
        with _tree(struct, _POPULATOR_ONE, _HEADER_ONE, source):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("preprocessor directive", out)


class AllowListPinTests(unittest.TestCase):
    """The allow-list is the one hand-written part of the guard, so its growth must be
    visible in the diff. These exercise the pin and the reason validation directly."""

    def test_pin_mismatch_fails(self) -> None:
        source = _renderer_source("\trenderer.timing_state.last_widget_cpu_ms = 0.0f;\n")
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source):
            with mock.patch.object(
                guard, "SURVIVES_REJECT_FIELDS", {"widget_cpu_ms": "a perfectly good reason"}
            ):
                # EXPECTED_SURVIVES_REJECT_COUNT stays 0 from _tree: the entry was added
                # without bumping the pin, which is the exact invisible-growth move.
                rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("EXPECTED_SURVIVES_REJECT_COUNT", out)

    def test_placeholder_reason_fails(self) -> None:
        source = _renderer_source("\trenderer.perf_metrics.rasterization_ms = 0.0f;\n")
        with _tree(_STRUCT_ONE, _POPULATOR_ONE, _HEADER_ONE, source, survives={"widget_cpu_ms": "TODO"}):
            rc, out = _run_main()
        self.assertEqual(rc, 1, out)
        self.assertIn("placeholder reason", out)

    def test_real_allow_list_matches_its_pin(self) -> None:
        """Guards the committed constant, not a fixture."""
        self.assertEqual(
            len(guard.SURVIVES_REJECT_FIELDS), guard.EXPECTED_SURVIVES_REJECT_COUNT
        )


class CommittedTreeTests(unittest.TestCase):
    def test_guard_passes_on_the_committed_sources(self) -> None:
        rc, out = _run_main()
        self.assertEqual(rc, 0, out)
        self.assertIn("PASSED", out)


if __name__ == "__main__":
    unittest.main()
