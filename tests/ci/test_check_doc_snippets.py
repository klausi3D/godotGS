#!/usr/bin/env python3
"""Tests for tests/ci/check_doc_snippets.py.

The defect-class tests run fixture Markdown against the REAL binding surface of
this tree (the `_bind_*` bodies, `register_types.cpp`, and the doc XML), because
that surface is what the guard judges the docs against; a hand-written stand-in
for it would test a fiction. Each test that relies on a specific member first
asserts the fact it depends on (for example "get_aabb is declared in C++ but not
bound"), so a binding change fails with a message that says which fixture to
update rather than a confusing verdict.

The binding PARSER is additionally tested on a constructed module (C++ written
here, engine XML copied from `doc/classes`), including the fail-closed paths.

`run_module_tests.py --guard-only` runs this file before the guard itself.
"""

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "check_doc_snippets.py"
spec = importlib.util.spec_from_file_location("check_doc_snippets", SCRIPT)
assert spec and spec.loader
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)

_SURFACE = None


def surface():
    global _SURFACE
    if _SURFACE is None:
        _SURFACE = guard.Surface(ROOT)
    return _SURFACE


def check(markdown: str, rel: str = "docs/features/fixture.md"):
    return guard.check_text(surface(), rel, textwrap.dedent(markdown))


def codes(markdown: str, rel: str = "docs/features/fixture.md") -> list[str]:
    return [f.code for f in check(markdown, rel) if f.severity == "error"]


def gd(body: str, prelude: str | None = None) -> str:
    marker = f'<!-- snippet: prelude="{prelude}" -->\n' if prelude else ""
    return f"# Fixture\n\n{marker}```gdscript\n{textwrap.dedent(body).strip()}\n```\n"


GOOD_SNIPPET = """
extends Node3D

signal finished(value: int)
const Helper = preload("res://helper.gd")
enum Mode { A, B }

@export var asset: GaussianSplatAsset
@onready var splat: GaussianSplatNode3D = $GaussianSplatNode3D
@onready var world := $World as GaussianSplatWorld3D

func _ready() -> void:
    var loaded := load("res://scan.ply") as GaussianSplatAsset
    splat.set_splat_asset(loaded)
    splat.set("rendering/opacity", 0.5)
    var stats: Dictionary = splat.get_statistics()
    print(stats.get("bounds", AABB()))
    await get_tree().create_timer(1.0).timeout
    for child in get_children():
        if child is GaussianSplatNode3D:
            child.reload_asset()
    var render_stats := world.get_renderer().get_render_stats()
    finished.emit(1)
    splat.asset_loaded.connect(_on_loaded)
    var data := GaussianData.new()
    data.set_positions(PackedVector3Array([Vector3(1, 2, 3)]))
    var parts := "a, b, c".split(",")
    _helper(Mode.A)

func _helper(mode: Mode) -> void:
    pass

func _on_loaded() -> void:
    pass
"""


class SurfaceFactsTest(unittest.TestCase):
    """Non-vacuity: the parsed surface is the real one, not an empty set."""

    def test_the_real_surface_is_parsed(self):
        s = surface()
        self.assertGreaterEqual(len(s.registered), 20, sorted(s.registered))
        self.assertIn("GaussianSplatNode3D", s.registered)
        self.assertGreaterEqual(len(s.binds["GaussianSplatNode3D"].methods), 50)
        self.assertIn("Node3D", s.engine_xml)
        self.assertIn("print", s.global_functions)
        self.assertIn("GaussianSplatManager", s.module_singletons)

    def test_bind_arity_agrees_with_the_generated_doc_xml(self):
        """D_METHOD + DEFVAL is the arity source; the doctool XML is generated from the
        live ClassDB, so every method present in both must agree."""
        s = surface()
        compared = 0
        for cls, bind in s.binds.items():
            xml_cls = s.module_xml.get(cls)
            if cls not in s.registered or xml_cls is None:
                continue
            for name, method in bind.methods.items():
                if name in xml_cls.methods:
                    compared += 1
                    xml_m = xml_cls.methods[name]
                    self.assertEqual((xml_m["min"], xml_m["max"]), (method.min_args, method.max_args), f"{cls}.{name}")
        self.assertGreater(compared, 100)


class GoodSnippetTest(unittest.TestCase):
    def test_a_correct_snippet_passes(self):
        self.assertEqual([], codes(gd(GOOD_SNIPPET)))

    def test_the_real_docs_contain_checked_gdscript(self):
        """The guard must have something to judge; a scan of zero blocks is vacuous."""
        count = 0
        for path, rel in guard.doc_files(ROOT):
            blocks, _prose, _ = guard.split_markdown(rel, path.read_text(encoding="utf-8"))
            count += sum(1 for b in blocks if guard.block_is_gdscript(b))
        self.assertGreaterEqual(count, 20)


class TypedReceiverTest(unittest.TestCase):
    def test_cpp_only_member(self):
        s = surface()
        self.assertIsNone(s.lookup("GaussianSplatNode3D", "get_aabb"), "fixture: get_aabb became bound")
        self.assertTrue(s.cpp_declaration("GaussianSplatNode3D", "get_aabb", True), "fixture: get_aabb gone")
        found = check(gd("var splat: GaussianSplatNode3D\nsplat.get_aabb()"))
        self.assertEqual(["MEMBER_CPP_ONLY"], [f.code for f in found])
        self.assertIn("gaussian_splat_node_3d.h", found[0].message)

    def test_nonexistent_member(self):
        self.assertEqual(
            ["MEMBER_NONEXISTENT"], codes(gd("var splat: GaussianSplatNode3D\nsplat.no_such_member_xyz = 3"))
        )

    def test_member_via_inferred_return_type(self):
        body = "var world: GaussianSplatWorld3D\nworld.get_renderer().no_such_renderer_call_xyz()"
        self.assertEqual(["MEMBER_NONEXISTENT"], codes(gd(body)))

    def test_member_on_singleton(self):
        body = 'var m := Engine.get_singleton("GaussianSplatManager")\nm.no_such_manager_call_xyz()'
        self.assertEqual(["MEMBER_NONEXISTENT"], codes(gd(body)))

    def test_bare_call_resolves_against_extends(self):
        body = "extends GaussianSplatNode3D\nfunc _ready():\n    get_aabb()"
        self.assertEqual(["MEMBER_CPP_ONLY"], codes(gd(body)))

    def test_instance_method_called_statically(self):
        self.assertEqual(["STATIC_CALL_OF_INSTANCE_METHOD"], codes(gd("GaussianSplatNode3D.set_splat_asset(null)")))


class UntypedReceiverTest(unittest.TestCase):
    def test_method_bound_nowhere(self):
        self.assertEqual(["UNTYPED_UNKNOWN_METHOD"], codes(gd("var node\nnode.no_such_method_xyz()")))

    def test_node_path_receiver(self):
        self.assertEqual(["UNTYPED_UNKNOWN_METHOD"], codes(gd("$Splat.no_such_method_xyz()")))

    def test_method_bound_on_some_class_passes(self):
        self.assertEqual([], codes(gd("var node\nnode.set_splat_asset(null)")))


class GroupedPropertyTest(unittest.TestCase):
    def test_fixture_property_is_grouped(self):
        self.assertIn("rendering/opacity", surface().grouped_properties)

    def test_typed_receiver(self):
        body = "var splat: GaussianSplatNode3D\nsplat.rendering.opacity = 0.5"
        self.assertEqual(["GROUPED_PROPERTY_DOT_ACCESS"], codes(gd(body)))

    def test_untyped_receiver(self):
        self.assertEqual(["GROUPED_PROPERTY_DOT_ACCESS"], codes(gd("var node\nnode.rendering.opacity = 0.5")))

    def test_set_with_the_full_name_passes(self):
        self.assertEqual([], codes(gd('var node\nnode.set("rendering/opacity", 0.5)')))


class ClassNameTest(unittest.TestCase):
    def test_unknown_class(self):
        self.assertEqual(["CLASS_UNKNOWN"], codes(gd("var x: GaussianSplatNodeTypo3D")))

    def test_unregistered_module_class(self):
        s = surface()
        unregistered = sorted(c for c in s.parents if c not in s.registered and s.is_module_class(c))
        self.assertTrue(unregistered, "fixture: every module GDCLASS is registered now")
        for position in (f"var x: {unregistered[0]}", f"var y = {unregistered[0]}.new()"):
            with self.subTest(position=position):
                self.assertEqual(["CLASS_NOT_REGISTERED"], codes(gd(position)))

    def test_local_classes_are_known(self):
        self.assertEqual([], codes(gd("class_name MyThing\nclass Inner:\n    pass\nvar a: MyThing\nvar b: Inner")))


class ArgCountTest(unittest.TestCase):
    def test_d_method_arity(self):
        method = surface().binds["GaussianSplatNode3D"].methods["set_splat_asset"]
        self.assertEqual((1, 1), (method.min_args, method.max_args), "fixture: set_splat_asset arity changed")
        for call in ("splat.set_splat_asset()", "splat.set_splat_asset(a, b)"):
            with self.subTest(call=call):
                self.assertEqual(["ARG_COUNT"], codes(gd(f"var splat: GaussianSplatNode3D\n{call}")))

    def test_defval_widens_the_range(self):
        method = surface().binds["GaussianAnimationStateMachine"].methods["add_clip"]
        self.assertEqual((1, 2), (method.min_args, method.max_args), "fixture: add_clip DEFVAL changed")
        prelude = "var sm: GaussianAnimationStateMachine"
        self.assertEqual([], codes(gd('sm.add_clip("a")\nsm.add_clip("a", 2.0)', prelude)))
        self.assertEqual(["ARG_COUNT"], codes(gd('sm.add_clip("a", 1.0, 3)', prelude)))

    def test_commas_inside_strings_and_brackets_are_not_arguments(self):
        body = 'var splat: GaussianSplatNode3D\nsplat.set_splat_asset(load("a,b").duplicate([1, 2]))'
        self.assertEqual([], codes(gd(body)))


class GlobalFunctionTest(unittest.TestCase):
    def test_unknown_global_function(self):
        self.assertEqual(["UNKNOWN_GLOBAL_FUNCTION"], codes(gd('snprintf("%d", 1)')))

    def test_page_functions_and_builtins_pass(self):
        self.assertEqual([], codes(gd("func helper():\n    pass\nhelper()\nprint(len([1]))")))


class ProseTest(unittest.TestCase):
    def test_cpp_only_in_backticks_and_code_tags(self):
        for text in ("See `GaussianSplatNode3D.get_aabb()`.", "See <code>GaussianSplatNode3D.get_aabb()</code>."):
            with self.subTest(text=text):
                self.assertEqual(["PROSE_MEMBER_CPP_ONLY"], codes(f"# P\n\n{text}\n"))

    def test_nonexistent_member(self):
        self.assertEqual(["PROSE_MEMBER_NONEXISTENT"], codes("# P\n\n`GaussianSplatNode3D.no_such_xyz`\n"))

    def test_unregistered_class_with_dot(self):
        s = surface()
        cls = sorted(c for c in s.parents if c not in s.registered and s.is_module_class(c))[0]
        self.assertEqual(["PROSE_CLASS_NOT_REGISTERED"], codes(f"# P\n\n`{cls}.anything()`\n"))

    def test_cpp_spelling_and_bound_members_pass(self):
        text = "# P\n\n`GaussianSplatNode3D::get_aabb()` and `GaussianSplatNode3D.set_splat_asset()`\n"
        self.assertEqual([], codes(text))

    def test_file_names_are_not_members(self):
        """The known false positive: a path was read as Class.member."""
        text = "# P\n\n`modules/gaussian_splatting/doc_classes/GaussianSplatNode3D.xml` and `GaussianSplatNode3D.xml`\n"
        self.assertEqual([], codes(text))


API_PAGE = """
# GaussianSplatNode3D API Reference

<table>
  <tr><th>Method</th><th>Behavior</th></tr>
  <tr>
    <td><code>set_splat_asset(asset)</code></td>
    <td>Mentions the removed <code>no_such_old_call()</code> in a description cell.</td>
  </tr>
  <tr><td><code>{bad}()</code></td><td>Bounds.</td></tr>
  <tr><td><code>get_aabb()</code></td><td>C++ only: not bound.</td></tr>
</table>

| Method | Notes |
| --- | --- |
| `reload_asset()` | fine |
| `{pipe_bad}()` | bad |
"""


class ApiTableTest(unittest.TestCase):
    def test_unbound_rows_fail_and_marked_rows_pass(self):
        found = check(API_PAGE.format(bad="get_aabb", pipe_bad="no_such_pipe_xyz"), rel="docs/api/node.md")
        self.assertEqual(["TABLE_METHOD_UNBOUND", "TABLE_METHOD_UNBOUND"], [f.code for f in found])
        self.assertIn("C++-only", found[0].message)
        self.assertIn("does not exist", found[1].message)

    def test_bound_rows_pass(self):
        found = check(API_PAGE.format(bad="reload_asset", pipe_bad="update_splats"), rel="docs/api/node.md")
        self.assertEqual([], [f.code for f in found])

    def test_only_api_pages_are_table_checked(self):
        found = check(API_PAGE.format(bad="get_aabb", pipe_bad="no_such_pipe_xyz"), rel="docs/features/node.md")
        self.assertEqual([], [f.code for f in found])


class MarkerTest(unittest.TestCase):
    def test_pseudo_skips_the_block(self):
        text = '# P\n\n<!-- snippet: pseudo reason="shape only" -->\n```gdscript\nno_such_function_xyz()\n```\n'
        self.assertEqual([], codes(text))

    def test_prelude_types_a_fragment(self):
        prelude = "var splat_node: GaussianSplatNode3D"
        self.assertEqual([], codes(gd("splat_node.set_splat_asset(null)", prelude)))
        self.assertEqual(["MEMBER_CPP_ONLY"], codes(gd("splat_node.get_aabb()", prelude)))

    def test_prelude_is_checked_too(self):
        self.assertEqual(["CLASS_UNKNOWN"], codes(gd("pass", "var s: NoSuchClassXyz")))

    def test_invalid_markers(self):
        for marker in (
            "<!-- snippet: pseudo -->",
            '<!-- snippet: pseudo reason="" -->',
            '<!-- snippet: frobnicate="x" -->',
            '<!-- snippet: pseudo reason="x" prelude="var a: Node" -->',
        ):
            with self.subTest(marker=marker):
                self.assertIn("MARKER_INVALID", codes(f"# P\n\n{marker}\n```gdscript\npass\n```\n"))

    def test_invalid_pseudo_does_not_skip_the_block(self):
        self.assertIn(
            "UNKNOWN_GLOBAL_FUNCTION", codes("# P\n\n<!-- snippet: pseudo -->\n```gdscript\nno_such_fn_xyz()\n```\n")
        )

    def test_unused_markers(self):
        cases = (
            '<!-- snippet: pseudo reason="x" -->\n```bash\necho\n```\n',
            '<!-- snippet: pseudo reason="x" -->\nSome prose.\n\n```gdscript\npass\n```\n',
            '<!-- snippet: pseudo reason="x" -->\n',
        )
        for body in cases:
            with self.subTest(body=body):
                self.assertIn("MARKER_UNUSED", codes(f"# P\n\n{body}"))


class PathPolicyTest(unittest.TestCase):
    BAD = gd("var splat: GaussianSplatNode3D\nsplat.get_aabb()")

    def test_architecture_and_adrs_only_warn(self):
        for rel in ("docs/architecture/design.md", "docs/governance/adr-something.md"):
            with self.subTest(rel=rel):
                found = check(self.BAD, rel)
                self.assertEqual(["MEMBER_CPP_ONLY"], [f.code for f in found])
                self.assertEqual(["warning"], [f.severity for f in found])

    def test_excluded_directories(self):
        for rel in ("docs/agent_memory/x.md", "docs/archive/x.md", "docs/reports/x.md"):
            self.assertTrue(guard.is_excluded(rel), rel)
        self.assertFalse(guard.is_excluded("docs/features/x.md"))


class CliTest(unittest.TestCase):
    """Exit codes, end to end, with fixture docs trees."""

    def _run(self, docs: dict[str, str], *extra: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as tmp:
            for rel, text in docs.items():
                path = Path(tmp) / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            return subprocess.run(
                [sys.executable, "-B", str(SCRIPT), "--root", str(ROOT), "--docs-root", tmp, *extra],
                capture_output=True,
                text=True,
                check=False,
            )

    def test_clean_docs_exit_zero(self):
        result = self._run({"docs/features/ok.md": gd(GOOD_SNIPPET)})
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_errors_exit_one(self):
        result = self._run({"docs/features/bad.md": PathPolicyTest.BAD})
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn("docs/features/bad.md:", result.stdout)
        self.assertIn("MEMBER_CPP_ONLY", result.stdout)

    def test_warn_only_and_excluded_pages_exit_zero(self):
        result = self._run(
            {
                "docs/architecture/old.md": PathPolicyTest.BAD,
                "docs/agent_memory/old.md": PathPolicyTest.BAD,
                "docs/archive/old.md": PathPolicyTest.BAD,
                "docs/reports/old.md": PathPolicyTest.BAD,
            }
        )
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("docs/architecture/old.md", result.stdout)
        self.assertNotIn("agent_memory", result.stdout)
        self.assertNotIn("docs/archive/", result.stdout)
        self.assertNotIn("docs/reports/", result.stdout)

    def test_no_pages_is_not_a_pass(self):
        result = self._run({"docs/agent_memory/only.md": "# x\n"})
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(text), encoding="utf-8")


class BindingParserTest(unittest.TestCase):
    """Constructed module; engine XML copied from doc/classes (the doctool's own output)."""

    HEADER = """
        class FixtureNode : public Node3D {
            GDCLASS(FixtureNode, Node3D);
        protected:
            static void _bind_methods();
        public:
            void set_value(int p_value, float p_weight = 1.0f);
            int get_value() const;
            void cpp_only_call();
        };
    """
    SOURCE = """
        void FixtureNode::_bind_methods() {
            ClassDB::bind_method(D_METHOD("set_value", "value", "weight"), &FixtureNode::set_value, DEFVAL(1.0));
            ClassDB::bind_method(D_METHOD("get_value"), &FixtureNode::get_value);
            ADD_PROPERTY(PropertyInfo(Variant::INT, "group/value"), "set_value", "get_value");
            ADD_SIGNAL(MethodInfo("changed"));
        }
    """

    def _root(self, tmp: str, source: str | None = None, register: str = "GDREGISTER_CLASS(FixtureNode);") -> Path:
        root = Path(tmp)
        for name in ("Object.xml", "@GlobalScope.xml", "Node.xml", "Node3D.xml", "RefCounted.xml", "Signal.xml"):
            target = root / "doc" / "classes" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / "doc" / "classes" / name, target)
        gdscript = root / "modules" / "gdscript" / "doc_classes" / "@GDScript.xml"
        gdscript.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / "modules" / "gdscript" / "doc_classes" / "@GDScript.xml", gdscript)
        module = root / "modules" / "gaussian_splatting"
        _write(module / "register_types.cpp", f"void initialize() {{ {register} }}\n")
        _write(module / "fixture_node.h", self.HEADER)
        _write(module / "fixture_node.cpp", source if source is not None else self.SOURCE)
        return root

    def test_parses_methods_arity_properties_and_signals(self):
        with tempfile.TemporaryDirectory() as tmp:
            s = guard.Surface(self._root(tmp))
            bind = s.binds["FixtureNode"]
            self.assertEqual((1, 2), (bind.methods["set_value"].min_args, bind.methods["set_value"].max_args))
            self.assertEqual((0, 0), (bind.methods["get_value"].min_args, bind.methods["get_value"].max_args))
            self.assertIn("group/value", s.grouped_properties)
            self.assertIn("changed", bind.signals)
            self.assertEqual("Node3D", s.parents["FixtureNode"])
            self.assertIsNone(s.lookup("FixtureNode", "cpp_only_call"))
            self.assertTrue(s.cpp_declaration("FixtureNode", "cpp_only_call", True))
            self.assertEqual("method", s.lookup("FixtureNode", "add_child")[0])  # engine base via XML

    def test_d_method_outside_a_bind_body_fails_closed(self):
        source = self.SOURCE + '\nstatic void helper() { ClassDB::bind_method(D_METHOD("hidden"), &f); }\n'
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(guard.SurfaceError, "D_METHOD outside"):
                guard.Surface(self._root(tmp, source=source))

    def test_registered_class_without_gdclass_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(guard.SurfaceError, "without a parsed GDCLASS"):
                guard.Surface(self._root(tmp, register="GDREGISTER_CLASS(FixtureNode); GDREGISTER_CLASS(Ghost);"))

    def test_missing_engine_xml_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self._root(tmp)
            (root / "doc" / "classes" / "Object.xml").unlink()
            with self.assertRaisesRegex(guard.SurfaceError, "engine class XML"):
                guard.Surface(root)

    def test_cli_reports_an_internal_error_as_exit_two(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [sys.executable, "-B", str(SCRIPT), "--root", tmp, "--docs-root", str(ROOT)],
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(2, result.returncode, result.stdout + result.stderr)
        self.assertIn("refusing to pass", result.stderr)


if __name__ == "__main__":
    unittest.main()
