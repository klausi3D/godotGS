#!/usr/bin/env python3
"""#1455 optional telemetry contracts; semantic inputs, no claimed engine capture."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import re
import sys
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

# The requested optional telemetry contract, not an inventory of existing output.
FIELDS = (
    "hlod_drawn_node_ids", "hlod_drawn_node_digest", "hlod_cut_hysteresis_held",
    "hlod_cut_budget_bound", "hlod_cut_first_cap_refused_node", "hlod_cut_residency_missed",
    "hlod_cut_residency_missed_demanded",
)
VALUES = ([3, 8], "e43c7632e0fd9a6e", [1], True, 5, [2, 4], [True, False])


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


if __name__ == "__main__":
    unittest.main()
