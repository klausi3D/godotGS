"""Bounded CPU controls for the genuine Editor producer; no Godot invocation."""
from __future__ import annotations

import contextlib
import copy
import json
import tempfile
import types
import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class CanonicalEditorWiring(unittest.TestCase):
    def setUp(self):
        self.module = load("live_editor_module_control", ROOT / "tests/ci/run_module_tests.py")

    def run_lane(self, name):
        result = self.module.GodotRunResult(False, False, "controlled missing summary", 9)
        with mock.patch.object(self.module, "_run_godot", return_value=result) as ordinary, \
                mock.patch.object(self.module, "_run_live_editor", return_value=result, create=True) as editor, \
                contextlib.redirect_stdout(io.StringIO()):
            self.module._execute_lane("OWN_FAKE_NO_GODOT", name, ["--headless", "--test"],
                                      True, "strict", False, {}, self.module.DoctestTotals())
        return ordinary, editor

    def test_actual_canonical_editor_consumer_uses_real_runtime(self):
        ordinary, editor = self.run_lane("GaussianSplatting [Editor]")
        editor.assert_called_once()
        ordinary.assert_not_called()

    def test_other_canonical_lane_preserves_ordinary_producer(self):
        ordinary, editor = self.run_lane("GaussianSplatting [Config]")
        ordinary.assert_called_once()
        editor.assert_not_called()

    def test_actual_canonical_guard_entry_deletion_is_rejected_by_distinct_ledger(self):
        source_path = ROOT / "tests/ci/run_module_tests.py"
        source = source_path.read_text(encoding="utf-8")
        tuple_text = """        (
            True,
            _run_live_editor_guard,
            \"Live Editor contract guard failed.\",
            \"Live Editor contract guard passed.\",
        ),
"""
        self.assertEqual(source.count(tuple_text), 1)
        for removed in (False, True):
            with self.subTest(removed=removed):
                candidate = types.ModuleType("live_editor_canonical_mutant")
                candidate.__file__ = str(source_path)
                sys.modules[candidate.__name__] = candidate
                exec(compile(source.replace(tuple_text, "") if removed else source, str(source_path), "exec"), candidate.__dict__)
                ledger = load("live_editor_distinct_ledger", ROOT / "tests/ci/test_run_module_tests_lane_ledger.py")
                ledger.harness = candidate
                launched = []
                def command(argv, *args, **kwargs):
                    launched.append(list(argv))
                    if str(candidate.LANE_LEDGER_TEST_SCRIPT) in list(map(str, argv)):
                        case = ledger.GuardScriptWiringTests("test_live_editor_guard_is_independently_wired")
                        result = unittest.TestResult()
                        case.run(result)
                        return int(not result.wasSuccessful()), str(result.failures + result.errors), ""
                    return 0, "", ""
                args = types.SimpleNamespace(skip_build_metadata_guard=False, skip_render_guards=False,
                                             skip_static_guards=False, base_ref="control-base", godot_binary="NO_GODOT")
                with contextlib.ExitStack() as stack:
                    stack.enter_context(mock.patch.object(candidate, "_run_command", command))
                    for name in ("_run_required_message_guards", "_run_history_artifact_guard_step",
                                 "_run_configured_render_and_static_guards", "_run_stringname_orphan_guard_step"):
                        stack.enter_context(mock.patch.object(candidate, name, return_value=None))
                    stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                    rc = candidate._run_ci_guard_steps(args)
                self.assertEqual(rc, 1 if removed else None)
                self.assertEqual(any(str(candidate.LIVE_EDITOR_TEST_SCRIPT) in list(map(str, c)) for c in launched), not removed)
                self.assertTrue(any(str(candidate.LANE_LEDGER_TEST_SCRIPT) in list(map(str, c)) for c in launched))



class ProtocolCountermodels(unittest.TestCase):
    """Constructed protocol countermodels, NOT captured native-format fixtures."""
    def setUp(self):
        self.runtime = load("live_editor_protocol_control", ROOT / "tests/ci/run_live_editor_tests.py")
        self.names = self.runtime.selected_cases()
        self.token = "a" * 32
        self.line = 157

    def constructed_records(self, pid):
        records = [{"kind": "begin", "owner": "11", "borrowed_ids": [str(i) for i in range(100, 114)],
                    "logger_captured": True, "queues_not_flushing": True}]
        # Construction follows actual source fixture allocation, not a native assertion-count claim.
        fixture_names = self.runtime.fixture_cases()
        for ordinal, name in enumerate(self.names, 1):
            fixtures = int(name in fixture_names)
            records.append({"kind": "case", "ordinal": ordinal, "name": name, "assertions": 21 if 'Advanced import settings' in name else 1,
                            "failed": 0, "borrowed_valid": True, "logger_before_restore": True, "callback_closed": True,
                            "fixture_created": fixtures, "fixture_closed": fixtures, "fixture_parented": bool(fixtures)})
        events = []
        def event(kind, case, thread="11", **fields):
            row = {"kind": "event", "event": kind, "case": case, "thread": thread, "sequence": len(events) + 1,
                   "phase": 0, "arm": "0", "generator": "0", "asset": "0", "token_id": "0", "value": 0,
                   "expected": False, "notify": False, "function": "", "file": "", "line": 0, "error": "", "message": ""}
            row.update(fields)
            events.append(row)
        tid = next(i for i, n in enumerate(self.names, 1) if 'A failed thumbnail canvas allocation' in n)
        for phase in (1, 2):
            event(2, tid, arm=str(phase), message='GaussianThumbnailGenerator::_project_to_canvas hits')
            fields = dict(phase=phase, arm=str(phase), generator="801", asset="802", token_id=str(phase))
            event(4, tid, value=16, **fields)
            event(3, tid, message='GaussianThumbnailGenerator::_project_to_canvas hits', **fields)
            event(1, tid, expected=True, function='gs_resize_or_fail', file='modules/gaussian_splatting/core/gs_vector_alloc.h',
                  line=self.line, error='Method/function failed. Returning: false', message=self.runtime.RATIONALE, **fields)
            event(5, tid, **fields)
        for i, name in enumerate(self.names, 1):
            if '(#592)' not in name:
                continue
            fields = dict(generator=str(900 + i), token_id=str(1000 + i))
            event(6, i, "22", **fields)
            event(9, i, generator=fields['generator'], token_id="22")
            event(7, i, **fields)
            event(8, i, **fields)
        records.extend(events)
        total = sum(c['assertions'] for c in records if c['kind'] == 'case')
        records.append({"kind": "final", "valid": True, "drained": True, "logger_restored": True, "borrowed_valid": True,
                        "borrowed_ids": records[0]['borrowed_ids'], "cases": 23, "assertions": total, "failed_assertions": 0,
                        "created": 7, "closed": 7})
        for row in records:
            row.update(token=self.token, pid=pid)
        return records

    def text(self, records):
        total = next(r['assertions'] for r in records if r['kind'] == 'final')
        return (f'test cases: 23 | 23 passed | 0 failed\nassertions: {total} | {total} passed | 0 failed\n' +
                ''.join(self.runtime.PREFIX + json.dumps(r) + '\n' for r in records))

    def validate(self, records):
        return self.runtime.validate(self.text(records), token=self.token, pid=17, names=self.names, injection_line=self.line)

    def test_constructed_protocol_positive_only_not_native_format_certification(self):
        self.validate(self.constructed_records(17))

    def test_negative_identity_counts_and_ownership(self):
        changes = [lambda r: r[0].update(owner="0"), lambda r: r[-1].update(drained=False),
                   lambda r: r[-1].update(logger_restored=False), lambda r: r[-1].update(borrowed_ids=["0"] * 14),
                   lambda r: r[1].update(assertions=0), lambda r: r[1].update(callback_closed=False),
                   lambda r: r[1].update(name=r[2]['name']), lambda r: r[-1].update(closed=6),
                   lambda r: r[-1].update(token="b" * 32),
                   lambda r: next(c for c in r if c.get('kind') == 'case' and c.get('fixture_created') == 0).update(fixture_created=1, fixture_closed=1, fixture_parented=True)]
        for mutation in changes:
            with self.subTest(mutation=mutation):
                records = self.constructed_records(17)
                mutation(records)
                with self.assertRaises(ValueError):
                    self.validate(records)

    def test_negative_worker_same_message_disconnect_duplicate_and_cas(self):
        for mutation in ('worker', 'disconnected', 'duplicate', 'wrongline', 'reset', 'consume', 'late', 'logger'):
            with self.subTest(mutation=mutation):
                records = self.constructed_records(17)
                error = next(x for x in records if x.get('event') == 1)
                if mutation == 'worker': error.update(thread="22")
                if mutation == 'disconnected': records.remove(error)
                if mutation == 'duplicate': records.insert(-1, copy.deepcopy(error))
                if mutation == 'wrongline': error.update(line=self.line + 1)
                if mutation == 'reset': error.update(token_id="1", phase=2)
                if mutation == 'consume': next(x for x in records if x.get('event') == 3).update(token_id="0")
                if mutation == 'late': records.append(copy.deepcopy(error))
                if mutation == 'logger': error.update(event=10)
                with self.assertRaises(ValueError): self.validate(records)

    def test_actual_popen_stream_full_eof_rejects_trailing_error(self):
        # Fake OWN Python children, never a native Godot process or a lease/probe.
        for trailing in ('', 'ERROR: unexpected teardown error\n'):
            with self.subTest(trailing=trailing), tempfile.TemporaryDirectory() as tmp:
                d = Path(tmp)
                rows = self.constructed_records(0)
                child = d / 'own_protocol_child.py'
                child.write_text('import json,os\nr=' + repr(rows) + '\nfor x in r:x["pid"]=os.getpid()\n' +
                                 'print(' + repr(self.text(rows).split(self.runtime.PREFIX, 1)[0]) + ',end="")\n' +
                                 'for x in r:print(' + repr(self.runtime.PREFIX) + '+json.dumps(x),flush=True)\n' +
                                 'print(' + repr(trailing) + ',end="",flush=True)\n', encoding='utf-8')
                ok, output, code = self.runtime.supervise([sys.executable, str(child)], d, self.token, self.names, self.line, timeout=10)
                self.assertEqual(code, 0)
                self.assertEqual(ok, not trailing)
                self.assertTrue((d / 'stdout.log').is_file())

    def test_direct_local_launch_refuses_before_any_probe_or_child(self):
        with mock.patch.dict(self.runtime.os.environ, {"GITHUB_ACTIONS": "false"}), mock.patch.object(self.runtime.subprocess, "Popen") as popen:
            result = self.runtime.run("OWN_FAKE_NO_GODOT", [])
        self.assertFalse(result[0])
        popen.assert_not_called()


class ShippingRegistrationControls(unittest.TestCase):
    def setUp(self):
        self.guard = load("live_editor_shipping_control", ROOT / "tests/ci/check_doc_classes_complete.py")

    def test_shipping_unknown_tools_else_ifndef_and_nested_branches(self):
        cases = [
            ("GDREGISTER_CLASS(Ordinary);", {"Ordinary"}),
            ("#ifdef TOOLS_ENABLED\nGDREGISTER_CLASS(Tools);\n#endif", {"Tools"}),
            ("#ifdef TESTS_ENABLED\nGDREGISTER_CLASS(Test);\n#else\nGDREGISTER_CLASS(Shipping);\n#endif", {"Shipping"}),
            ("#ifndef TESTS_ENABLED\nGDREGISTER_CLASS(Shipping);\n#else\nGDREGISTER_CLASS(Test);\n#endif", {"Shipping"}),
            ("#ifdef UNKNOWN\n#ifdef TESTS_ENABLED\nGDREGISTER_CLASS(Test);\n#endif\nGDREGISTER_CLASS(Maybe);\n#endif", {"Maybe"}),
            ("#if defined(TESTS_ENABLED) && defined(TOOLS_ENABLED)\nGDREGISTER_CLASS(Test);\n#endif", set()),
            ("#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)\nGDREGISTER_CLASS(Maybe);\n#endif", {"Maybe"}),
            ("#if TESTS_ENABLED + UNKNOWN\nGDREGISTER_CLASS(Ambiguous);\n#else\nGDREGISTER_CLASS(AlsoPossible);\n#endif", {"Ambiguous", "AlsoPossible"}),
            ("#if defined(UNKNOWN)\nGDREGISTER_CLASS(A);\n#elif !defined(TESTS_ENABLED)\nGDREGISTER_CLASS(B);\n#else\nGDREGISTER_CLASS(Test);\n#endif", {"A", "B"}),
        ]
        for text, expected in cases:
            with self.subTest(text=text):
                self.assertEqual(self.guard._registered_classes(text), expected)

    def test_malformed_nesting_fails_closed(self):
        for text in ('#endif', '#ifdef TESTS_ENABLED', '#if defined(TESTS_ENABLED)\n#else\n#else\n#endif'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.guard._registered_classes(text)

    def test_actual_documentation_consumer_rejects_helper_moved_to_shipping(self):
        current = self.guard.REGISTER_TYPES.read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'register_types.cpp'
            path.write_text(current + '\nGDREGISTER_CLASS(GSLiveEditorTestRunner);\n', encoding='utf-8')
            with mock.patch.object(self.guard, 'REGISTER_TYPES', path), contextlib.redirect_stdout(io.StringIO()):
                self.assertNotEqual(self.guard.main(), 0)




if __name__ == "__main__":
    unittest.main()
