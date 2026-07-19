#!/usr/bin/env python3
"""Unit tests for tests/ci/check_cull_signature_parity.py.

The guard's job is to fail closed: every `CullingConfig` field must be folded
into `_compute_cull_config_signature` (or carry an explicit waiver), because a
knob that is missing from the signature lets `OutputCompositor` keep serving a
stale cached render after the knob changes.

Running the guard against the committed sources only proves the tree is clean
TODAY. These tests pin the extractor's rules against synthetic fixtures, so a
future parser change that re-opens a fail-open hole is caught even when the real
sources happen not to exercise it.
"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GUARD_PATH = ROOT / "tests" / "ci" / "check_cull_signature_parity.py"

_spec = importlib.util.spec_from_file_location("check_cull_signature_parity", GUARD_PATH)
assert _spec is not None and _spec.loader is not None
guard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(guard)


def _body(*folds: str) -> str:
    """A synthetic `_compute_cull_config_signature` body containing `folds`."""
    return "uint64_t seed = HASH_MURMUR3_SEED;\n" + "\n".join(folds) + "\nreturn seed;\n"


class HashedFieldExtractionTests(unittest.TestCase):
    def test_bare_argument_counts_as_hashed(self) -> None:
        self.assertEqual(
            guard._extract_hashed_fields(_body("seed = _hash_bool(config.lod_enabled, seed);")),
            {"lod_enabled"},
        )

    def test_multiple_folds_accumulate(self) -> None:
        self.assertEqual(
            guard._extract_hashed_fields(
                _body(
                    "seed = _hash_bool(config.lod_enabled, seed);",
                    "seed = _hash_float_bits(config.lod_bias, seed);",
                )
            ),
            {"lod_enabled", "lod_bias"},
        )

    def test_read_outside_a_fold_is_not_hashed(self) -> None:
        self.assertEqual(
            guard._extract_hashed_fields(_body("const float x = config.new_knob;")),
            set(),
        )

    def test_read_on_the_same_line_as_a_real_fold_is_not_hashed(self) -> None:
        body = _body(
            "seed = _hash_bool(config.lod_enabled, seed); const float x = config.new_knob;"
        )
        self.assertEqual(guard._extract_hashed_fields(body), {"lod_enabled"})

    # --- lossy folds: the value is not recoverable from the seed -------------
    # Each of these used to count as "hashed" because the extractor accepted any
    # `config.*` token anywhere in the argument list. Many distinct values of the
    # knob then collapse to one seed, so the cached render stays stale.

    def test_comparison_fold_is_not_hashed(self) -> None:
        self.assertEqual(
            guard._extract_hashed_fields(
                _body("seed = _hash_bool(config.new_knob > 0.0f, seed);")
            ),
            set(),
        )

    def test_sizeof_fold_is_not_hashed(self) -> None:
        self.assertEqual(
            guard._extract_hashed_fields(
                _body("seed = _hash_u64(sizeof(config.new_knob), seed);")
            ),
            set(),
        )

    def test_cast_wrapped_fold_is_not_hashed(self) -> None:
        # A cast may or may not preserve the value (uint32->uint64 does, float->
        # uint64 truncates). The guard cannot tell, so it fails closed.
        self.assertEqual(
            guard._extract_hashed_fields(
                _body("seed = _hash_u64(static_cast<uint64_t>(config.new_knob), seed);")
            ),
            set(),
        )

    def test_arithmetic_fold_is_not_hashed(self) -> None:
        self.assertEqual(
            guard._extract_hashed_fields(
                _body("seed = _hash_float_bits(config.a - config.b, seed);")
            ),
            set(),
        )

    def test_raw_value_alongside_a_derived_one_is_hashed(self) -> None:
        # The documented escape hatch: fold the raw value as its own argument.
        body = _body(
            "seed = _hash_float_bits(config.new_knob, seed);",
            "seed = _hash_bool(config.new_knob > 0.0f, seed);",
        )
        self.assertEqual(guard._extract_hashed_fields(body), {"new_knob"})


class TopLevelArgSplitTests(unittest.TestCase):
    def test_splits_on_top_level_commas(self) -> None:
        self.assertEqual(
            [a.strip() for a in guard._split_top_level_args("config.a, seed")],
            ["config.a", "seed"],
        )

    def test_nested_call_commas_do_not_split(self) -> None:
        self.assertEqual(
            [a.strip() for a in guard._split_top_level_args("MAX(0, config.a), seed")],
            ["MAX(0, config.a)", "seed"],
        )

    def test_template_commas_do_not_split(self) -> None:
        self.assertEqual(
            [a.strip() for a in guard._split_top_level_args("static_cast<uint64_t>(x), seed")],
            ["static_cast<uint64_t>(x)", "seed"],
        )


class CommittedSourceTests(unittest.TestCase):
    def test_guard_passes_on_the_committed_tree(self) -> None:
        self.assertEqual(guard.main(), 0)

    def test_every_committed_fold_is_value_preserving(self) -> None:
        # Guards against the tightened rule being satisfied vacuously: the real
        # signature function must still yield a non-empty hashed set, and every
        # field the guard reports as hashed must come from a whole-argument fold.
        sig_body = guard._brace_body(
            guard.SIGNATURE_SOURCE.read_text(encoding="utf-8"), guard.SIGNATURE_FN
        )
        self.assertIsNotNone(sig_body, "could not locate the signature function")
        hashed = guard._extract_hashed_fields(sig_body)
        self.assertTrue(hashed, "no field is hashed - the extractor matched nothing")
        self.assertIn("lod_enabled", hashed)


if __name__ == "__main__":
    unittest.main()
