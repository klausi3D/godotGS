#!/usr/bin/env python3
"""Unit tests for scripts/agentic/validate_review.py."""

from __future__ import annotations

import copy
import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "agentic" / "validate_review.py"
spec = importlib.util.spec_from_file_location("validate_review", SCRIPT)
assert spec and spec.loader
vr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vr)

SCHEMA = json.loads((ROOT / ".agentic" / "schemas" / "review.schema.json").read_text(encoding="utf-8"))
TEMPLATE = json.loads((ROOT / ".agentic" / "templates" / "review.json").read_text(encoding="utf-8"))


class ValidateReviewTest(unittest.TestCase):
    def test_template_is_valid(self):
        self.assertEqual(vr.validate_review(copy.deepcopy(TEMPLATE), SCHEMA), [])

    def test_missing_base_sha_fails(self):
        review = copy.deepcopy(TEMPLATE)
        del review["base_sha"]
        errors = vr.validate_review(review, SCHEMA)
        self.assertTrue(any("base_sha" in e for e in errors))

    def test_empty_head_sha_fails(self):
        review = copy.deepcopy(TEMPLATE)
        review["head_sha"] = "   "
        errors = vr.validate_review(review, SCHEMA)
        self.assertTrue(any("head_sha" in e for e in errors))

    def test_unknown_verdict_fails(self):
        review = copy.deepcopy(TEMPLATE)
        review["verdict"] = "looks_good"
        errors = vr.validate_review(review, SCHEMA)
        self.assertTrue(any("verdict" in e for e in errors))

    def test_unknown_reviewer_role_fails(self):
        review = copy.deepcopy(TEMPLATE)
        review["reviewer_role"] = "vibes-reviewer"
        errors = vr.validate_review(review, SCHEMA)
        self.assertTrue(any("reviewer_role" in e for e in errors))

    def test_missing_blind_spots_fails(self):
        review = copy.deepcopy(TEMPLATE)
        del review["blind_spots"]
        errors = vr.validate_review(review, SCHEMA)
        self.assertTrue(any("blind_spots" in e for e in errors))

    def test_blocker_without_required_action_fails(self):
        review = copy.deepcopy(TEMPLATE)
        review["findings"][0]["severity"] = "blocker"
        review["findings"][0]["required_action"] = ""
        errors = vr.validate_review(review, SCHEMA)
        self.assertTrue(any("required_action" in e for e in errors))

    def test_unknown_severity_fails(self):
        review = copy.deepcopy(TEMPLATE)
        review["findings"][0]["severity"] = "catastrophic"
        errors = vr.validate_review(review, SCHEMA)
        self.assertTrue(any("severity" in e for e in errors))

    def test_unexpected_property_fails(self):
        review = copy.deepcopy(TEMPLATE)
        review["extra"] = True
        errors = vr.validate_review(review, SCHEMA)
        self.assertTrue(any("unexpected property" in e for e in errors))

    def test_malformed_enum_schema_returns_error_instead_of_raising(self):
        errors = vr.validate_instance("value", {"type": "string", "enum": None})
        self.assertTrue(any("enum" in error and "must be an array" in error for error in errors))

    def test_malformed_container_schema_keywords_return_errors(self):
        cases = (
            ({}, {"type": "object", "properties": None}, "properties"),
            ({}, {"type": "object", "required": None}, "required"),
            ([], {"type": "array", "items": None}, "items"),
        )
        for instance, schema, keyword in cases:
            with self.subTest(keyword=keyword):
                errors = vr.validate_instance(instance, schema)
                self.assertTrue(any(keyword in error for error in errors))


if __name__ == "__main__":
    unittest.main()
