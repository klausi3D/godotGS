"""Self-tests for tests/ci/check_unchecked_resize.py.

Every case here corresponds to a way the guard could be EVADED -- i.e. a way a new
unchecked `resize()` feeding a raw write could be added while the ratchet still
passed. A guard with no self-test is an assertion about the codebase that nobody
checks; this file makes each evasion a failing test instead.

Each case is written so it FAILS against the pre-fix guard:

  test_distinct_functions_get_distinct_keys      <- key omitted the function name
  test_regenerate_refuses_to_add_a_key           <- --regenerate used a NET delta
  test_function_scope_is_not_window_truncated    <- MAX_FUNCTION_SPAN = 200 cap
  test_multiline_resize_is_detected              <- line-anchored regex
  test_unreadable_source_fails_the_guard         <- `except OSError: continue`

plus two regression tests for defects found while fixing the above:

  test_consumer_does_not_leak_across_functions   <- namespace-as-span over-scanned
  test_namespaced_functions_resolve_by_name      <- `namespace {` swallowed a file
"""
from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

GUARD_PATH = pathlib.Path(__file__).resolve().parent / "check_unchecked_resize.py"


def _load_guard():
    spec = importlib.util.spec_from_file_location("check_unchecked_resize", GUARD_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules["check_unchecked_resize"] = module
    spec.loader.exec_module(module)
    return module


class UncheckedResizeGuardTest(unittest.TestCase):
    def setUp(self):
        self.guard = _load_guard()
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        (self.root / "modules" / "gaussian_splatting").mkdir(parents=True)
        self.guard.REPO_ROOT = self.root
        self.guard.MODULE_ROOT = self.root / "modules" / "gaussian_splatting"
        self.guard.BASELINE_PATH = self.root / "baseline.json"
        self.addCleanup(self._tmp.cleanup)

    def _write(self, name: str, body: str) -> None:
        (self.guard.MODULE_ROOT / name).write_text(body, encoding="utf-8")

    def _sites(self) -> list[str]:
        sites, errors = self.guard.find_sites()
        self.assertEqual(errors, [], "fixture sources must all be readable")
        return sites

    # -- P1: key collision -------------------------------------------------
    def test_distinct_functions_get_distinct_keys(self):
        """Two statements identical except for their enclosing function.

        Pre-fix the key was file::symbol.resize(count), so these collapsed to ONE
        entry: fixing one, or adding the other, left the key set unchanged and the
        ratchet passed silently. Measured on the real tree, this folded 80
        statements into 69 keys.
        """
        self._write("a.cpp", """
Error First::build() {
    Vector<float> result;
    result.resize(splat_count);
    float *w = result.ptrw();
    return OK;
}

Error Second::build() {
    Vector<float> result;
    result.resize(splat_count);
    float *w = result.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 2, f"expected two independently-tracked sites, got {sites}")
        self.assertEqual(len(set(sites)), 2, f"keys collapsed onto each other: {sites}")

    # -- P1: regenerate blessing a new site --------------------------------
    def test_regenerate_refuses_to_add_a_key(self):
        """Fixing one site while adding another nets zero.

        Pre-fix, --regenerate wrote the baseline unconditionally and only compared
        len(), so the net-zero case printed no warning at all and returned 0 -- and
        the committed baseline then made normal CI pass over a real new defect.
        """
        self.guard.BASELINE_PATH.write_text(
            json.dumps({"schema_version": 2, "sites": ["modules/gaussian_splatting/old.cpp::Gone::v.resize(n)"]}),
            encoding="utf-8",
        )
        before = self.guard.BASELINE_PATH.read_text(encoding="utf-8")
        self._write("a.cpp", """
Error Fresh::build() {
    Vector<float> values;
    values.resize(runtime_count);
    float *w = values.ptrw();
    return OK;
}
""")
        argv = sys.argv
        sys.argv = ["check_unchecked_resize.py", "--regenerate"]
        try:
            code = self.guard.main()
        finally:
            sys.argv = argv
        self.assertEqual(code, 1, "regeneration that ADDS a key must fail, not warn")
        self.assertEqual(
            self.guard.BASELINE_PATH.read_text(encoding="utf-8"),
            before,
            "a refused regeneration must not have written the baseline",
        )

    # -- P2: window truncation ---------------------------------------------
    def test_function_scope_is_not_window_truncated(self):
        """Consumer far below the resize, still inside the same function.

        Pre-fix a fixed 200-line cap was returned as the function end, so this site
        was dropped -- recreating the very window-length evasion that motivated
        scanning by function scope. The real defect that motivated it sat ~38 lines
        down; nothing stops an evader from using 250.
        """
        filler = "\n".join(f"    int pad{i} = {i};" for i in range(400))
        self._write("a.cpp", f"""
Error Wide::build() {{
    Vector<float> values;
    values.resize(runtime_count);
{filler}
    float *w = values.ptrw();
    return OK;
}}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"a consumer 400 lines below must still be found: {sites}")

    # -- P2: multiline statement -------------------------------------------
    def test_multiline_resize_is_detected(self):
        """A normally wrapped call is one statement across two physical lines.

        Pre-fix the line-anchored regex never matched it, so the ptrw() below was
        never examined and the site passed the guard entirely.
        """
        self._write("a.cpp", """
Error Wrapped::build() {
    Vector<float> values;
    values.resize(
            runtime_count);
    float *w = values.ptrw();
    return OK;
}
""")
        sites = self._sites()
        self.assertEqual(len(sites), 1, f"a wrapped resize() must still be detected: {sites}")

    # -- P2: unreadable source ---------------------------------------------
    def test_unreadable_source_fails_the_guard(self):
        """An unreadable source must fail, not silently shrink the scan.

        Pre-fix `except OSError: continue` dropped every site in that file and the
        run still exited 0 -- reporting the file's baseline entries as "fixed",
        which is an incomplete scan presented as evidence of safety.
        """
        missing = self.guard.MODULE_ROOT / "vanished.cpp"
        self.guard._module_sources = lambda: [missing]
        sites, errors = self.guard.find_sites()
        self.assertEqual(sites, [])
        self.assertTrue(errors, "an unreadable source must be reported, not skipped")

        self.guard.BASELINE_PATH.write_text(
            json.dumps({"schema_version": 2, "sites": []}), encoding="utf-8"
        )
        argv = sys.argv
        sys.argv = ["check_unchecked_resize.py"]
        try:
            code = self.guard.main()
        finally:
            sys.argv = argv
        self.assertEqual(code, 1, "an incomplete scan must fail the guard")

    # -- regressions found while fixing the above --------------------------
    def test_consumer_does_not_leak_across_functions(self):
        """A ptrw() in a LATER function must not satisfy an earlier resize.

        While making scope real, treating `namespace X {` as the enclosing span made
        the scan run to the end of the namespace, so an unrelated consumer in another
        function produced a false positive.
        """
        self._write("a.cpp", """
namespace GaussianSplatting {

Error NoConsumer::build() {
    Vector<float> values;
    values.resize(runtime_count);
    return OK;
}

Error Other::use() {
    Vector<float> values;
    int n = values.size();
    float *w = values.ptrw();
    return OK;
}

}
""")
        sites = self._sites()
        self.assertEqual(sites, [], f"consumer in a different function must not count: {sites}")

    def test_namespaced_functions_resolve_by_name(self):
        """Functions inside a namespace must key by their own name.

        `namespace {` opening a span collapsed every function in the file back onto
        one identity, which re-created the P1 collision behind an ordinal suffix.
        """
        self._write("a.cpp", """
namespace GaussianSplatting {
namespace {

Error Alpha::build() {
    Vector<float> payload;
    payload.resize(len);
    uint8_t *w = payload.ptrw();
    return OK;
}

Error Beta::build() {
    Vector<float> payload;
    payload.resize(len);
    uint8_t *w = payload.ptrw();
    return OK;
}

}
}
""")
        sites = self._sites()
        self.assertEqual(len(set(sites)), 2, f"namespaced functions must key separately: {sites}")
        self.assertTrue(all("<anonymous>" not in s for s in sites), f"unresolved span: {sites}")
        self.assertTrue(all("#" not in s for s in sites), f"ordinal fallback used instead of names: {sites}")


if __name__ == "__main__":
    unittest.main()
