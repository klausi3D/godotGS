#!/usr/bin/env python3
"""#1455 optional telemetry contracts; semantic inputs, no claimed engine capture."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


driver = _load("compare_shared_layers_t0")
shared = _load("shared_layers_t0")
ps = _load("probe_summary")

# The requested optional telemetry contract, not an inventory of existing output.
FIELDS = (
    "hlod_drawn_node_ids", "hlod_drawn_node_digest", "hlod_cut_hysteresis_held",
    "hlod_cut_budget_bound", "hlod_cut_first_cap_refused_node", "hlod_cut_residency_missed",
    "hlod_cut_residency_missed_demanded",
)
VALUES = ([3, 8], "e43c7632e0fd9a6e", [1], True, 5, [2, 4], [True, False])
# Requested transition policy, not an inventory of the native producer's output.
TRANSITION_PRESSURE_FIELDS = (
    "hlod_stream_protected_sibling_nodes",
    "hlod_stream_abandoned_transitions_total",
    "hlod_stream_abandoned_transitions_this_frame",
    "hlod_stream_transition_state",
)


class HlodCutTelemetry(unittest.TestCase):
    def test_checkpoint_reads_each_optional_field_without_fallback(self):
        """Textual wiring only; JSON export by Godot is not exercised locally."""
        source = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        checkpoint = re.search(r"^func _checkpoint\([^\n]+\n(.*?)(?=^func |\Z)", source, re.M | re.S)
        self.assertIsNotNone(checkpoint)
        body = checkpoint.group(1)
        for field in FIELDS:
            with self.subTest(field=field):
                self.assertRegex(body, rf'"{field}"\s*:\s*_last_stats\.get\("{field}"\)\s*,')

    def test_capture_and_comparison_preserve_values_and_legacy_null(self):
        # These dictionaries prescribe the optional contract. They are not
        # presented as a recording of a native producer or a settled GPU run.
        for values in (VALUES, (None,) * len(FIELDS)):
            with self.subTest(values=values):
                telemetry = dict(zip(FIELDS, values))
                checkpoint = {"checkpoint": "C2", "pass_index": 0, **telemetry}
                result = {"route": "hlod", "checkpoints": [checkpoint]}
                captured = shared.payload_behavior(result, None)["drawn_counts"][0]
                self.assertEqual({field: captured[field] for field in FIELDS}, telemetry)
                problems = []
                summary, migration = driver._checkpoint_observations({}, {
                    "candidate": {"payload_behavior": {"drawn_counts": [captured]}}
                }, lambda *args: problems.append(args))
                row = migration["candidate"][0]
                self.assertEqual({field: row[field] for field in FIELDS}, telemetry)
                persisted = json.loads(json.dumps(summary))["checkpoints"][0]
                self.assertEqual({field: persisted[field] for field in FIELDS}, telemetry)
                self.assertEqual(summary["verdict"], "UNUSABLE", "optional telemetry cannot establish settlement")
                self.assertTrue(problems)

    def test_candidate_repeat_difference_is_a_sorted_set_difference(self):
        rows = [{"checkpoint": "C2", "pass_index": 0, "hlod_drawn_node_ids": [8, 3]},
                {"checkpoint": "C2", "pass_index": 1, "hlod_drawn_node_ids": [3, 5]}]
        saved = copy.deepcopy(rows)
        report = driver._candidate_drawn_id_repeat(rows)
        self.assertEqual(report, [{"checkpoint": "C2", "measured": True,
                                   "symmetric_difference": [5, 8], "count": 2}])
        self.assertEqual(rows, saved)
        observations = driver._migration_observations({}, {"base": [], "candidate": rows})
        self.assertEqual(observations["candidate_drawn_id_repeat"], report, "report generator must call the diagnostic")
        markdown = driver.render_markdown({"migration_observations": observations})
        self.assertIn("Candidate C2 pass-0/pass-1 drawn-ID symmetric difference (report-only): 2 node IDs [5, 8]", markdown)

    def test_empty_measured_sets_are_distinct_from_absent_or_malformed_ids(self):
        rows = [{"checkpoint": "C2", "pass_index": index, "hlod_drawn_node_ids": []} for index in (0, 1)]
        measured = driver._candidate_drawn_id_repeat(rows)[0]
        self.assertIs(measured["measured"], True)
        self.assertEqual(measured["symmetric_difference"], [])
        self.assertEqual(measured["count"], 0)
        for value in (None, "", [-1], [True], [2, 2]):
            with self.subTest(value=value):
                altered = copy.deepcopy(rows)
                altered[0]["hlod_drawn_node_ids"] = value
                missing = driver._candidate_drawn_id_repeat(altered)[0]
                self.assertIs(missing["measured"], False)
                self.assertIsNone(missing["symmetric_difference"])
                self.assertIsNone(missing["count"])
                self.assertTrue(missing["reason"])
        for altered in (rows[:1], rows + [rows[0]], [dict(row) for row in rows]):
            if len(altered) == 2:
                altered[0].pop("hlod_drawn_node_ids")
            self.assertIs(driver._candidate_drawn_id_repeat(altered)[0]["measured"], False)


class HlodPressureTelemetry(unittest.TestCase):
    """Constructed format contracts and source wiring, never an engine capture.

    Godot export is exercised by CI. Here the binary payload is deliberately
    constructed from the declared producer fields to test the versioned reader.
    """

    @classmethod
    def setUpClass(cls):
        cls.source = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        declaration = re.search(r"const HLOD_PRESSURE_FIELDS := \[(.*?)\n\]", cls.source, re.S)
        if declaration is None:
            raise AssertionError("probe pressure trace declaration missing")
        cls.fields = re.findall(r'\["([^"]+)", "q"\]', declaration.group(1))

    def test_new_scalar_columns_are_wired_to_trace_and_both_settle_endpoints(self):
        # The named categories are the requested policy, not a copied list of
        # whichever native fields exist. All declared fields are checked below.
        for field in ("hlod_stream_pressure_measured", "hlod_stream_frames", "hlod_stream_allocation_refused_total",
                      "hlod_stream_skipped_groups_total", "hlod_stream_fragmented_groups",
                      "hlod_stream_pages_free", "hlod_stream_largest_free_run",
                      "hlod_stream_demanded_closure_pages", "hlod_stream_placement_reserve_pages",
                      "hlod_stream_placement_reserve_state",
                      "hlod_stream_bootstrap_bypass_pages",
                      "hlod_stream_resident_refinement_bypass_pages", "hlod_stream_page_refused_groups_total",
                      "hlod_stream_evicted_in_cut_total", "hlod_stream_evicted_in_transition_total",
                      "hlod_stream_evicted_neither_total", "hlod_stream_refusing_gate",
                      "hlod_stream_live_reservations", "hlod_stream_oversized_reservation_bytes",
                      *TRANSITION_PRESSURE_FIELDS):
            with self.subTest(field=field):
                self.assertIn(field, self.fields)
        self.assertEqual(len(self.fields), len(set(self.fields)), "trace fields must be unique")
        self.assertIn("FIELDS += HLOD_PRESSURE_FIELDS", self.source)
        self.assertRegex(self.source, r'for f in HLOD_PRESSURE_FIELDS:\n\s+values.append\(int\(_g\(stats, f\[0\], -1\)\)\)')
        self.assertIn('"hlod_pressure": _hlod_pressure_checkpoint(settle)', self.source)
        self.assertIn('var pressure_start := _hlod_pressure_snapshot() if capture_progress else {}', self.source)
        for outcome in ("missed", "settled"):
            self.assertIn(f'{outcome}["hlod_pressure"] = _hlod_pressure_settle_window(pressure_start)', self.source)
        self.assertIn('values[key] = null', self.source)
        self.assertIn('if not key.ends_with("_total"):', self.source)
        self.assertIn('"counter_deltas": deltas', self.source)
        self.assertIn('_sample_hlod_pressure_victims(stats)', self.source)
        self.assertIn('sequence <= _hlod_pressure_victim_last_sequence', self.source)
        self.assertIn('"dropped_count": _hlod_pressure_victim_dropped if measured else null', self.source)
        self.assertIn('"sequence_gaps_total": sequence_gaps if measured else null', self.source)
        self.assertIn('if epoch != _hlod_pressure_victim_epoch:', self.source)
        self.assertIn('_hlod_pressure_victim_last_sequence = 0', self.source)
        self.assertIn('"epoch_resets_total": _hlod_pressure_victim_epoch_resets', self.source)
        self.assertIn('var measured: bool = bool(same_epoch and before != null', self.source)
        self.assertIn('const HLOD_PRESSURE_VICTIM_LIMIT := 4096', self.source)
        summary_source = (HERE / "run_probe.py").read_text(encoding="utf-8")
        self.assertIn('out["hlod_pressure"] = ps.hlod_pressure_summary(result.get("checkpoints"), trace)', summary_source)
        compare_source = (HERE / "compare_shared_layers_t0.py").read_text(encoding="utf-8")
        self.assertIn('"hlod_pressure_report": _hlod_pressure_report(arms)', compare_source)

    def test_trace_version_three_round_trip_declared_columns_and_legacy_reader(self):
        schema = re.search(r'const TRACE_SCHEMA := "([^"]+)"', self.source).group(1)
        self.assertEqual(schema, ps.TRACE_SCHEMA)
        self.assertEqual(schema, "godotgs-bv07-probe-trace/3")
        for version, fields in ((schema, ["row", *self.fields]),
                                ("godotgs-bv07-probe-trace/2", ["row", *self.fields]),
                                ("godotgs-bv07-probe-trace/1", ["row"])):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as tmp:
                types = "q" * len(fields)
                header = json.dumps({"schema": version, "fields": fields, "types": types}).encode()
                # Constructed format input; no native producer capture is claimed.
                path = Path(tmp) / "constructed_contract.trace.bin"
                path.write_bytes(ps.TRACE_MAGIC + struct.pack("<I", len(header)) + header
                                 + struct.pack("<" + types, 0, *([7] * (len(fields) - 1))))
                trace = ps.read_trace(path)
                self.assertEqual(trace.fields, fields)
                for field in fields[1:]:
                    self.assertEqual(trace.get(trace.rows[0], field), 7)

    def test_native_pressure_exports_and_gate_names_all_reach_declared_columns(self):
        root = HERE.parents[2]
        native = (root / "modules/gaussian_splatting/renderer/render_diagnostics_orchestrator.cpp").read_text(encoding="utf-8")
        self.assertIn("cut.append_stats(stats, drawn_valid);", native)
        publisher = (root / "modules/gaussian_splatting/renderer/resident_instance_contract_publisher.cpp").read_text(encoding="utf-8")
        self.assertIn("state.hlod_cut_telemetry.capture(*diagnostic_history, diagnostic_cut, world_nodes,", publisher)
        streamer = (root / "modules/gaussian_splatting/renderer/hlod_node_streamer.cpp").read_text(encoding="utf-8")
        refresh = re.search(r"void HlodNodeStreamer::refresh_telemetry\(\) \{(.*?)(?=\n\})", streamer, re.S)
        self.assertIsNotNone(refresh)
        self.assertIn("telemetry.allocation_refused_total = s.allocation_refused_total;", refresh.group(1))
        self.assertIn("telemetry.evicted_in_transition_total = s.evicted_in_transition_total;", refresh.group(1))
        update = re.search(r"void HlodNodeStreamer::update\((.*?)(?=\nvoid HlodNodeStreamer::)", streamer, re.S)
        self.assertIsNotNone(update)
        self.assertIn("refresh_telemetry();", update.group(1), "frame update must publish native counters")
        start = native.index('stats["hlod_stream_pressure_measured"]')
        end = native.index("// Package E1:", start)
        pressure_block = native[start:end]
        exported = set(re.findall(r'stats\["(hlod_stream_[^"]+)"\]', pressure_block))
        exported.discard("hlod_stream_eviction_events") # Bounded array is checkpoint-only.
        closure = (root / "modules/gaussian_splatting/renderer/hlod_cut_telemetry.h").read_text(encoding="utf-8")
        exported.update(re.findall(r'r_stats\["(hlod_stream_[^"]+)"\]', closure))
        names = re.search(r"const char \*gate_names\[\] = \{(.*?)\};", pressure_block, re.S)
        self.assertIsNotNone(names)
        gate_names = re.findall(r'"([a-z_]+)"', names.group(1))
        self.assertEqual(dict(enumerate(gate_names)), ps.HLOD_REFUSING_GATE_NAMES)
        exported.update(f"hlod_stream_gate_{name}_frames_total" for name in gate_names)
        for field in TRANSITION_PRESSURE_FIELDS:
            with self.subTest(required_transition_field=field):
                self.assertIn(field, exported, "transition diagnostic requires a native scalar producer")
        for field in exported:
            with self.subTest(field=field):
                self.assertIn(field, self.fields, "native pressure field never reaches the trace/checkpoint scalar schema")
        for field in self.fields:
            if field not in exported:
                self.assertIn(f'"{field}"', native, "declared legacy pressure field has no native producer")

    def test_absent_partial_and_zero_columns_remain_distinct(self):
        key = "hlod_stream_allocation_refused_total"
        for values in ((), (-1,), (0, -1)):
            trace = ps.Trace([key] if values else ["row"], "q", [(value,) for value in values], 0)
            observation = ps.hlod_pressure_column(trace, list(range(len(trace.rows))), key)
            with self.subTest(values=values):
                self.assertIs(observation["measured"], False)
                self.assertIsNone(observation["value"])
        zero = ps.Trace([key, "hlod_stream_pressure_epoch"], "qq", [(0, 1), (0, 1)], 0)
        observation = ps.hlod_pressure_column(zero, [0, 1], key)
        self.assertIs(observation["measured"], True)
        self.assertEqual(observation["value"], 0)
        self.assertEqual(observation["delta"], {"measured": True, "value": 0})
        self.assertIs(ps.hlod_pressure_summary([], zero)["trace"]["measured"], False,
                      "a counter without the native measurement marker is not a new pressure capture")

    def test_transition_columns_distinguish_absence_from_measured_zero_and_positive(self):
        # Constructed semantic inputs for the existing reader, never a native capture.
        for field in TRANSITION_PRESSURE_FIELDS:
            for columns, values in ((["row"], (0,)), ([field], ()),
                                    ([field], (-1,)), ([field], (0, -1))):
                with self.subTest(field=field, columns=columns, values=values):
                    trace = ps.Trace(columns, "q", [(value,) for value in values], 0)
                    observation = ps.hlod_pressure_column(trace, list(range(len(trace.rows))), field)
                    self.assertIs(observation["measured"], False)
                    self.assertIsNone(observation["value"])
                    self.assertTrue(observation["reason"])
            for value in (0, 3):
                with self.subTest(field=field, measured_value=value):
                    trace = ps.Trace([field, "hlod_stream_pressure_epoch"], "qq", [(value, 7)], 0)
                    observation = ps.hlod_pressure_column(trace, [0], field)
                    self.assertIs(observation["measured"], True)
                    self.assertEqual(observation["value"], value)

    def test_transition_gauges_can_reset_to_zero_without_counter_deltas(self):
        for field in ("hlod_stream_protected_sibling_nodes",
                      "hlod_stream_abandoned_transitions_this_frame", "hlod_stream_transition_state"):
            with self.subTest(field=field):
                trace = ps.Trace(["hlod_stream_pressure_measured", "hlod_stream_pressure_epoch", field],
                                 "qqq", [(1, 7, 3), (1, 7, 0)], 0)
                observation = ps.hlod_pressure_column(trace, [0, 1], field)
                self.assertIs(observation["measured"], True)
                self.assertEqual(observation["value"], 0)
                self.assertEqual(observation["max"], 3)
                self.assertNotIn("delta", observation, "a per-frame count/state is a gauge")
                summary = ps.hlod_pressure_summary([], trace)
                self.assertIs(summary["trace"]["measured"], True)
                self.assertEqual(summary["trace"]["columns"][field], observation)

    def test_transition_state_protocol_matches_native_enum(self):
        path = HERE.parents[2] / "modules/gaussian_splatting/lod/hlod_node_residency.h"
        enum = re.search(r"enum TransitionState(?:\s*:\s*\w+)?\s*\{([^}]+)\}", path.read_text(encoding="utf-8"))
        self.assertIsNotNone(enum)
        states = {int(value): name.removeprefix("TRANSITIONS_").lower()
                  for name, value in re.findall(r"(\w+)\s*=\s*(-?\d+)", enum.group(1))}
        self.assertEqual(states, ps.HLOD_TRANSITION_STATES)

    def test_transition_native_numeric_exports_are_nullable_and_state_keeps_absence(self):
        root = HERE.parents[2] / "modules/gaussian_splatting/renderer"
        native = (root / "render_diagnostics_orchestrator.cpp").read_text(encoding="utf-8")
        for field in TRANSITION_PRESSURE_FIELDS[:-1]:
            with self.subTest(field=field):
                exports = re.findall(rf'stats\["{field}"\]\s*=\s*([^;]+);', native)
                self.assertEqual(len(exports), 1)
                self.assertRegex(exports[0], r"\bt\.transitions_measured\s*\?")
                self.assertIn("t." + field.removeprefix("hlod_stream_"), exports[0])
                self.assertRegex(exports[0], r":\s*Variant\(\)\s*$")
        exports = re.findall(r'stats\["hlod_stream_transition_state"\]\s*=\s*([^;]+);', native)
        self.assertEqual(exports, ["int64_t(t.transition_state)"])
        header = (root / "hlod_node_streamer.h").read_text(encoding="utf-8")
        self.assertIsNotNone(re.search(r"bool\s+transitions_measured\s*=\s*false\s*;", header))
        self.assertIsNotNone(re.search(r"transition_state\s*=\s*[\w:]+::TRANSITIONS_ABSENT\s*;", header))

    def test_transition_state_decoding_requires_a_known_integer_code(self):
        # Prescribed semantic input, not a native capture or settlement evidence.
        state = ps.HLOD_TRANSITION_STATE
        for code in (None, -1, 4, 999999, True, 0.0, "0"):
            with self.subTest(code=code):
                observation = ps.hlod_transition_observation({state: code})
                self.assertEqual(observation["state"], "absent")
                self.assertIs(observation["measured"], False)
                self.assertIsNone(observation["value"])
                self.assertTrue(observation["reason"])
                trace = ps.Trace([state], "q", [(code,)], 0)
                column = ps.hlod_pressure_column(trace, [0], state)
                self.assertIs(column["measured"], False)
                self.assertIsNone(column["value"])
        for code, name in ps.HLOD_TRANSITION_STATES.items():
            if code < 0:
                continue
            with self.subTest(code=code):
                observation = ps.hlod_transition_observation({state: code})
                self.assertEqual(observation, {"state": name, "measured": True, "value": code})
                trace = ps.Trace([state], "q", [(code,), (0,)], 0)
                column = ps.hlod_pressure_column(trace, [0, 1], state)
                self.assertIs(column["measured"], True)
                self.assertEqual(column["value"], 0)
                self.assertEqual(column["state"], "normal")
                self.assertEqual(column["states"], sorted({name, "normal"}))
                self.assertNotIn("delta", column)

    def test_abandoned_transition_total_deltas_require_monotonic_same_epoch_samples(self):
        field = "hlod_stream_abandoned_transitions_total"
        for values, epochs, expected in (((0, 0, 0), (7, 7, 7), 0),
                                        ((3, 5, 8), (7, 7, 7), 5),
                                        ((3, 2, 8), (7, 7, 7), None),
                                        ((3, 5, 8), (7, 7, 8), None),
                                        ((3, 5, 8), (-1, -1, -1), None)):
            with self.subTest(values=values, epochs=epochs):
                trace = ps.Trace([field, "hlod_stream_pressure_epoch"], "qq", list(zip(values, epochs)), 0)
                observation = ps.hlod_pressure_column(trace, [0, 1, 2], field)
                self.assertIs(observation["measured"], True)
                self.assertEqual(observation["value"], values[-1])
                self.assertIs(observation["delta"]["measured"], expected is not None)
                self.assertEqual(observation["delta"]["value"], expected)
                if expected is None:
                    self.assertTrue(observation["delta"]["reason"])
        trace = ps.Trace([field], "q", [(0,), (3,)], 0)
        observation = ps.hlod_pressure_column(trace, [0, 1], field)
        self.assertIs(observation["delta"]["measured"], False)
        self.assertIsNone(observation["delta"]["value"])

    def test_transition_checkpoint_receipts_are_preserved_without_conferring_a_verdict(self):
        field = "hlod_stream_abandoned_transitions_total"
        receipt = {"report_only": True,
                   "snapshot": {"values": dict(zip(TRANSITION_PRESSURE_FIELDS, (0, 8, 0, 3)))},
                   "settle_window": {"counter_deltas": {field: {"measured": True, "value": 5}}}}
        checkpoint = {"checkpoint": "C5", "pass_index": 0, "segment": "stop", "hlod_pressure": receipt}
        expected = copy.deepcopy(receipt)
        summary = ps.hlod_pressure_summary([checkpoint])
        self.assertEqual(summary["checkpoints"][0]["observation"], expected)
        self.assertNotIn("verdict", summary)
        result = {"checkpoints": [checkpoint], "segments": [{"pass_index": 0, "id": "stop", "kind": "stop"}]}
        pressure = shared.pressure_observations(result, None, "hlod", {})
        self.assertEqual(pressure["settles"][0]["hlod_pressure"], expected)
        receipt["snapshot"]["values"][field] = 999
        receipt["settle_window"]["counter_deltas"][field]["value"] = 999
        self.assertEqual(summary["checkpoints"][0]["observation"], expected,
                         "persisted transition receipts must not alias mutable checkpoint input")

    def test_placement_reserve_observation_is_declared_on_selection_and_streamer(self):
        root = HERE.parents[2] / "modules/gaussian_splatting"
        for path in (root / "lod/hlod_runtime_selection.h", root / "renderer/hlod_node_streamer.h"):
            source = path.read_text(encoding="utf-8")
            with self.subTest(path=path.name):
                self.assertIsNotNone(re.search(r"bool\s+placement_reserve_measured\s*=\s*false\s*;", source),
                                     "reserve availability must default to unmeasured")
                self.assertIsNotNone(re.search(r"uint64_t\s+placement_reserve_pages\s*=\s*0\s*;", source),
                                     "selection and streamer must carry the observed reserve pages")
                self.assertIsNotNone(re.search(r"placement_reserve_state\s*=\s*(?:[\w:]+::)?RESERVE_ABSENT\s*;", source),
                                     "reserve state must default to absent, distinct from an unprepared selection")

    def test_placement_reserve_is_handed_from_selection_to_streamer(self):
        root = HERE.parents[2] / "modules/gaussian_splatting/renderer"
        publisher = (root / "resident_instance_contract_publisher.cpp").read_text(encoding="utf-8")
        # Discover the handoff method rather than prescribing a new C++ API in red tests.
        handoff = re.search(r"state\.hlod_streamer->(\w+)\([^;]*\bdiagnostic_cut\b[^;]*\);", publisher)
        self.assertIsNotNone(handoff, "the selection observation must cross the streamer boundary")
        streamer = (root / "hlod_node_streamer.cpp").read_text(encoding="utf-8")
        body = re.search(rf"void HlodNodeStreamer::{handoff.group(1)}\([^{{]*\{{(.*?)(?=\n\}})", streamer, re.S)
        self.assertIsNotNone(body, "the publisher's handoff must have a streamer implementation")
        for field in ("placement_reserve_measured", "placement_reserve_pages", "placement_reserve_state"):
            self.assertTrue(field in body.group(1), f"streamer handoff must capture {field}")

    def test_placement_reserve_native_export_is_nullable_and_owned_by_streamer(self):
        root = HERE.parents[2] / "modules/gaussian_splatting/renderer"
        key = "hlod_stream_placement_reserve_pages"
        native = (root / "render_diagnostics_orchestrator.cpp").read_text(encoding="utf-8")
        exports = re.findall(rf'stats\["{key}"\]\s*=\s*([^;]+);', native)
        self.assertEqual(len(exports), 1, "reserve must have one render-stats export")
        expression = exports[0]
        self.assertRegex(expression, r"\bt\.placement_reserve_measured\s*\?")
        self.assertIn("t.placement_reserve_pages", expression)
        self.assertRegex(expression, r":\s*Variant\(\)\s*$", "absent selection must export null, never measured zero")
        state_key = "hlod_stream_placement_reserve_state"
        state_exports = re.findall(rf'stats\["{state_key}"\]\s*=\s*([^;]+);', native)
        self.assertEqual(len(state_exports), 1, "reserve state must have one render-stats export")
        self.assertIn("t.placement_reserve_state", state_exports[0])
        self.assertIn("RESERVE_ABSENT", state_exports[0])
        self.assertRegex(state_exports[0], r":\s*Variant\(\)\s*$")
        closure = (root / "hlod_cut_telemetry.h").read_text(encoding="utf-8")
        for field in (key, state_key):
            self.assertIsNone(re.search(rf'r_stats\["{field}"\]\s*=', closure),
                              "direct cut telemetry must not overwrite the streamer's observation")

    def test_placement_reserve_state_codes_match_native_protocol(self):
        path = HERE.parents[2] / "modules/gaussian_splatting/lod/hlod_runtime_selection.h"
        enum = re.search(r"enum PlacementReserveState\s*\{([^}]+)\}", path.read_text(encoding="utf-8"))
        self.assertIsNotNone(enum)
        states = {int(value): name.lower() for name, value in re.findall(r"RESERVE_(\w+)\s*=\s*(-?\d+)", enum.group(1))}
        self.assertEqual(states, ps.HLOD_PLACEMENT_RESERVE_STATES)

    def test_placement_reserve_checkpoint_encodings_require_state_and_value(self):
        key, state = ps.HLOD_PLACEMENT_RESERVE_PAGES, ps.HLOD_PLACEMENT_RESERVE_STATE
        cases = (({}, "absent", None), ({key: 0}, "absent", None),
                 ({state: None, key: 9}, "absent", None), ({state: -1, key: 9}, "absent", None),
                 ({state: 0, key: None}, "unprepared", None), ({state: 0, key: 9}, "unprepared", None),
                 ({state: 1, key: 0}, "measured", 0), ({state: 1, key: 3}, "measured", 3),
                 ({state: 1}, "absent", None), ({state: 1, key: -1}, "absent", None),
                 ({state: 1, key: True}, "absent", None), ({state: True, key: 0}, "absent", None),
                 ({state: 7, key: 3}, "absent", None))
        for values, expected_state, expected_value in cases:
            with self.subTest(values=values):
                observation = ps.hlod_placement_reserve_observation(values)
                self.assertEqual(observation["state"], expected_state)
                self.assertEqual(observation["value"], expected_value)
                self.assertIs(observation["measured"], expected_state == "measured")
                if expected_state == "unprepared":
                    self.assertIn("physical-page admission", observation["reason"])

    def test_placement_reserve_clears_before_frame_preparation_can_return(self):
        # Wiring check for fallback paths that return before the streamer's begin_frame boundary.
        path = HERE.parents[2] / "modules/gaussian_splatting/renderer/resident_instance_contract_publisher.cpp"
        source = path.read_text(encoding="utf-8")
        prepare = source[source.index("bool GaussianSplatRenderer::prepare_hlod_frame("):]
        first_return = prepare.index("return ")
        reset = re.search(r"capture_placement_reserve\(gs_hlod_runtime::CutHistory::Result\(\)\)", prepare)
        self.assertIsNotNone(reset, "a frame without selection must clear the previous reserve observation")
        self.assertLess(reset.start(), first_return, "fallback before begin_frame must not publish stale reserve pages")

    def test_placement_reserve_gauge_preserves_zero_absence_and_reset(self):
        # Constructed semantic inputs, not recordings of the native producer.
        key = "hlod_stream_placement_reserve_pages"
        state = "hlod_stream_placement_reserve_state"
        for fields, values in ((["row"], (0,)), ([key], ()), ([key], (-1,)), ([key], (3, -1))):
            with self.subTest(fields=fields, values=values):
                trace = ps.Trace(fields, "q", [(value,) for value in values], 0)
                observation = ps.hlod_pressure_column(trace, list(range(len(trace.rows))), key)
                self.assertIs(observation["measured"], False)
                self.assertIsNone(observation["value"])
                self.assertTrue(observation["reason"])
        for values in ((0, 0), (3, 3), (7, 0)):
            with self.subTest(values=values):
                trace = ps.Trace([key, state], "qq", [(value, 1) for value in values], 0)
                observation = ps.hlod_pressure_column(trace, [0, 1], key)
                self.assertIs(observation["measured"], True)
                self.assertEqual(observation["value"], values[-1])
                self.assertEqual(observation["max"], max(values))
                self.assertNotIn("delta", observation, "reserve is a gauge, not a cumulative counter")
                summary = ps.hlod_pressure_summary([], trace)
                self.assertEqual(summary["trace"]["columns"][key], observation)

    def test_placement_reserve_trace_distinguishes_unprepared_absent_and_mixed_states(self):
        key, state = ps.HLOD_PLACEMENT_RESERVE_PAGES, ps.HLOD_PLACEMENT_RESERVE_STATE
        for codes, expected in (((0, 0), "unprepared"), ((-1, -1), "absent"),
                                ((1, 0), "mixed"), ((0, 1), "mixed"), ((1, -1), "mixed")):
            with self.subTest(codes=codes):
                trace = ps.Trace([key, state], "qq", [(9, code) for code in codes], 0)
                observation = ps.hlod_pressure_column(trace, [0, 1], key)
                self.assertEqual(observation["state"], expected)
                self.assertIs(observation["measured"], False)
                self.assertIsNone(observation["value"])
                self.assertNotIn("max", observation)

    def test_trace_counter_deltas_and_checkpoint_receipts_are_report_only(self):
        key = "hlod_stream_allocation_refused_total"
        trace = ps.Trace(["hlod_stream_pressure_measured", key, "hlod_stream_pressure_epoch"], "qqq",
                         [(1, 2, 7), (1, 5, 7), (1, 8, 7)], 0)
        receipt = {"report_only": True, "snapshot": {"values": {key: 8}},
                   "settle_window": {"counter_deltas": {key: {"measured": True, "value": 6}}},
                   "eviction_victims": {"measured": True, "events": [], "dropped_count": 0, "sequence_gaps_total": 0}}
        checkpoint = {"checkpoint": "C5", "pass_index": 1, "hlod_pressure": receipt}
        summary = ps.hlod_pressure_summary([checkpoint], trace)
        self.assertIs(summary["trace"]["measured"], True)
        self.assertEqual(summary["trace"]["columns"][key]["delta"], {"measured": True, "value": 6})
        self.assertEqual(summary["checkpoints"][0]["observation"], receipt)
        self.assertNotIn("verdict", summary)
        legacy = ps.hlod_pressure_summary([{"checkpoint": "C5"}])
        self.assertIs(legacy["checkpoints"][0]["observation"]["measured"], False)
        self.assertIsNone(legacy["checkpoints"][0]["observation"]["value"])
        checkpoint["hlod_pressure"]["snapshot"]["values"][key] = 999
        self.assertEqual(summary["checkpoints"][0]["observation"]["snapshot"]["values"][key], 8)
        trace.rows[2] = (1, 1, 7)
        decreased = ps.hlod_pressure_column(trace, [0, 1, 2], key)
        self.assertIs(decreased["delta"]["measured"], False)
        self.assertIsNone(decreased["delta"]["value"])
        trace.rows[2] = (1, 8, 8)
        reset = ps.hlod_pressure_column(trace, [0, 1, 2], key)
        self.assertIs(reset["delta"]["measured"], False, "even an increasing counter cannot cross streamer epochs")
        self.assertIsNone(reset["delta"]["value"])

    def test_pressure_observation_rows_preserve_checkpoint_diagnostics(self):
        # Constructed semantic input for the orchestration-owned record.
        receipt = {"report_only": True, "snapshot": {"values": {"hlod_stream_allocation_refused_total": 0}}}
        checkpoint = {"checkpoint": "C5", "pass_index": 0, "segment": "stop", "hlod_pressure": receipt}
        result = {"checkpoints": [checkpoint], "segments": [{"pass_index": 0, "id": "stop", "kind": "stop"}]}
        observed = shared.pressure_observations(result, None, "hlod", {})
        self.assertEqual(observed["settles"][0]["hlod_pressure"], receipt)
        checkpoint.pop("hlod_pressure")
        legacy = shared.pressure_observations(result, None, "hlod", {})
        self.assertIs(legacy["settles"][0]["hlod_pressure"]["measured"], False)


if __name__ == "__main__":
    unittest.main()
