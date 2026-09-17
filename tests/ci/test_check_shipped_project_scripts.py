#!/usr/bin/env python3
"""Discrimination tests for tests/ci/check_shipped_project_scripts.py (#833, #1019).

A guard that only ever passes proves nothing. Every detector here is exercised
twice: once with the defect, which must make the guard RED, and once with the
*correct* spelling of the same construct, which must stay GREEN. The
false-positive halves are not padding -- the guard's first draft blanked string
literals to whitespace, which turned `Engine.get_singleton("GaussianSplatManager")`
(correct) into something indistinguishable from `Performance.get_singleton()`
(the #833 defect) and flagged six innocent files. `test_engine_get_singleton_
with_argument_is_clean` is that counterexample.

Two further tests cover the guard's own failure modes, because "found nothing
to check" must never read as "passed": an empty corpus and a stale exclusion
path both exit 2.

Fixtures are complete synthetic repository roots in a temp directory -- the
committed tree is never touched and never read.
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "check_shipped_project_scripts.py"
spec = importlib.util.spec_from_file_location("check_shipped_project_scripts", SCRIPT)
assert spec and spec.loader
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)


CLEAN_GD = """extends Node

func _ready() -> void:
\tvar manager = Engine.get_singleton("GaussianSplatManager")
\tvar label := "ready" if manager else "missing"
\tprint(label)
"""

CLEAN_TSCN = """[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://clean.gd" id="1"]

[node name="Root" type="Node"]
script = ExtResource("1")

[node name="Child" type="Node" parent="." index="0"]
"""


class GuardFixture:
    """A synthetic repository root: one clean .gd, one clean .tscn, plus the
    upstream GDScript corpus that the guard must skip."""

    def __init__(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "shipped").mkdir(parents=True)
        self.write("shipped/clean.gd", CLEAN_GD)
        self.write("shipped/clean.tscn", CLEAN_TSCN)
        # The excluded upstream corpus, with a file that WOULD be flagged.
        self.write(
            "modules/gdscript/tests/scripts/parser/errors/invalid_ternary_operator.gd",
            'func t():\n\tvar x = 1 < 2 ? "yes" : "no"\n',
        )

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run(self) -> tuple[int, list[str]]:
        return guard.run(self.root)

    def detectors(self) -> list[str]:
        _code, messages = self.run()
        found = []
        for line in messages:
            if "[" in line and "]" in line:
                found.append(line.split("[", 1)[1].split("]", 1)[0])
        return found

    def close(self) -> None:
        self._tmp.cleanup()


class ShippedProjectScriptGuardTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fx = GuardFixture()
        self.addCleanup(self.fx.close)

    # ---- baseline ---------------------------------------------------------

    def test_clean_tree_passes_and_says_what_it_scanned(self) -> None:
        code, messages = self.fx.run()
        self.assertEqual(code, 0, messages)
        self.assertTrue(any("clean:" in m for m in messages), messages)

    def test_upstream_gdscript_corpus_is_excluded(self) -> None:
        # The fixture's modules/gdscript/tests/... file contains a C ternary.
        # If the exclusion stopped working this test goes red, which is the
        # point: the exclusion is policy, not an accident of the walk.
        code, _ = self.fx.run()
        self.assertEqual(code, 0)

    # ---- detector 1: C ternary -------------------------------------------

    def test_c_ternary_is_flagged(self) -> None:
        self.fx.write("shipped/bad.gd",
                      "func f(a):\n\tvar x = a > 0 ? 1 : 2\n\treturn x\n")
        code, messages = self.fx.run()
        self.assertEqual(code, 1, messages)
        self.assertIn("gdscript-c-ternary", self.fx.detectors())

    def test_gdscript_conditional_expression_is_clean(self) -> None:
        self.fx.write("shipped/ok.gd",
                      "func f(a):\n\tvar x = 1 if a > 0 else 2\n\treturn x\n")
        self.assertEqual(self.fx.run()[0], 0)

    def test_question_mark_inside_a_string_is_clean(self) -> None:
        self.fx.write("shipped/str.gd",
                      'func f():\n\tprint("what? really?")\n\tprint(\'a?b\')\n')
        self.assertEqual(self.fx.run()[0], 0)

    def test_question_mark_inside_a_comment_is_clean(self) -> None:
        self.fx.write("shipped/cmt.gd",
                      "func f():\n\t# is this a ternary? no.\n\tpass\n")
        self.assertEqual(self.fx.run()[0], 0)

    def test_question_mark_inside_a_triple_quoted_docstring_is_clean(self) -> None:
        self.fx.write("shipped/doc.gd",
                      'func f():\n\t"""why? because.\n\tstill? yes."""\n\tpass\n')
        self.assertEqual(self.fx.run()[0], 0)

    # ---- detector 2: zero-argument get_singleton --------------------------

    def test_zero_arg_get_singleton_is_flagged(self) -> None:
        self.fx.write("shipped/perf.gd",
                      "func f():\n\tvar p = Performance.get_singleton()\n\treturn p\n")
        code, messages = self.fx.run()
        self.assertEqual(code, 1, messages)
        self.assertIn("gdscript-zero-arg-get-singleton", self.fx.detectors())

    def test_engine_get_singleton_with_argument_is_clean(self) -> None:
        # Regression: masking string literals to whitespace made this read as
        # the zero-argument form and produced six false positives.
        self.fx.write(
            "shipped/engine.gd",
            'func f():\n\treturn Engine.get_singleton("GaussianSplatManager")\n')
        code, messages = self.fx.run()
        self.assertEqual(code, 0, messages)

    def test_get_singleton_with_empty_string_argument_is_clean(self) -> None:
        self.fx.write("shipped/empty.gd",
                      'func f():\n\treturn Engine.get_singleton("")\n')
        self.assertEqual(self.fx.run()[0], 0)

    # ---- detector 3: silently discarded node-header attributes ------------

    def test_script_inside_node_header_is_flagged(self) -> None:
        self.fx.write("shipped/hdr.tscn", """[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://clean.gd" id="1"]

[node name="Root" type="Node" script=ExtResource("1")]
""")
        code, messages = self.fx.run()
        self.assertEqual(code, 1, messages)
        self.assertIn("scene-unknown-node-header-attribute", self.fx.detectors())
        self.assertTrue(any("property line" in m for m in messages), messages)

    def test_script_as_a_property_line_is_clean(self) -> None:
        # The same scene written the Godot 4 way. Without this half, the
        # detector could be "always red on any scene with a script".
        self.assertEqual(self.fx.run()[0], 0)

    def test_legal_header_attributes_are_clean(self) -> None:
        self.fx.write("shipped/legal.tscn", """[gd_scene load_steps=2 format=3]

[ext_resource type="PackedScene" path="res://clean.tscn" id="3"]

[node name="Root" type="Node"]

[node name="Inst" parent="." index="1" instance=ExtResource("3")]

[node name="Grouped" type="Node" parent="." groups=["a", "b"]]
""")
        code, messages = self.fx.run()
        self.assertEqual(code, 0, messages)

    def test_node_paths_written_by_godot_itself_is_clean(self) -> None:
        # `scene/resources/resource_format_text.cpp:1989` emits ` node_paths=`
        # into the header whenever a node has a deferred NodePath property --
        # i.e. the moment a shipped script gains an exported Node reference
        # wired in the editor. Flagging it would be a false accusation against
        # correct engine output, and the finding's own advice would make the
        # scene wrong.
        self.fx.write("shipped/nodepaths.tscn", """[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://clean.gd" id="1"]

[node name="Root" type="Node" node_paths=PackedStringArray("target")]
script = ExtResource("1")
target = NodePath("Child")

[node name="Child" type="Node" parent="."]
""")
        code, messages = self.fx.run()
        self.assertEqual(code, 0, messages)

    # ---- detector 4: Godot 3 theme overrides ------------------------------

    def test_godot3_theme_override_is_flagged(self) -> None:
        self.fx.write("shipped/theme.tscn", """[gd_scene load_steps=1 format=3]

[node name="Root" type="Panel"]
custom_styles/panel = null
""")
        code, messages = self.fx.run()
        self.assertEqual(code, 1, messages)
        self.assertIn("scene-godot3-theme-override", self.fx.detectors())
        self.assertTrue(
            any("theme_override_styles/panel" in m for m in messages), messages)

    def test_godot4_theme_override_is_clean(self) -> None:
        self.fx.write("shipped/theme4.tscn", """[gd_scene load_steps=1 format=3]

[node name="Root" type="Panel"]
theme_override_styles/panel = null
""")
        self.assertEqual(self.fx.run()[0], 0)

    # ---- the guard's own failure modes ------------------------------------

    def test_empty_corpus_is_an_error_not_a_pass(self) -> None:
        (self.fx.root / "shipped" / "clean.gd").unlink()
        (self.fx.root / "shipped" / "clean.tscn").unlink()
        code, messages = self.fx.run()
        self.assertEqual(code, 2, messages)
        self.assertTrue(any("corpus is empty" in m for m in messages), messages)

    def test_stale_exclusion_path_is_an_error_not_a_pass(self) -> None:
        for path in sorted(
                (self.fx.root / "modules" / "gdscript" / "tests").rglob("*"),
                reverse=True):
            path.rmdir() if path.is_dir() else path.unlink()
        (self.fx.root / "modules" / "gdscript" / "tests").rmdir()
        code, messages = self.fx.run()
        self.assertEqual(code, 2, messages)
        self.assertTrue(any("no longer exist" in m for m in messages), messages)


class RealTreeTests(unittest.TestCase):
    """The detectors must be able to see the actual shipped defects, not only
    synthetic ones. Pins the two constructs #833/#1019 were about against the
    guard's own masking logic."""

    def test_masking_preserves_line_numbers(self) -> None:
        src = 'a\n"""x\ny"""\n# ?\nb ? c : d\n'
        lines = guard.strip_gdscript_strings_and_comments(src)
        self.assertEqual(len(lines), 6)
        self.assertIn("?", lines[4])
        self.assertNotIn("?", lines[3])

    def test_repository_tree_is_clean(self) -> None:
        code, messages = guard.run(ROOT)
        self.assertEqual(code, 0, "\n".join(messages))


if __name__ == "__main__":
    unittest.main()
