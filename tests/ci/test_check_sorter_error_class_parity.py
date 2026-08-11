#!/usr/bin/env python3
"""Unit tests for tests/ci/check_sorter_error_class_parity.py (#586 round-7).

The guard exists because the round-7 retry fix rests on a correspondence that nothing else
checks: the deterministic failure sites in the RadixSort init path return
`ERR_COMPILATION_FAILED`, the allocation sites return `ERR_CANT_CREATE`, and
`classify_sorter_creation_error()` turns that difference into "latch" vs "retry". One
`return ERR_CANT_CREATE;` typed at a shader-creation site reverts the behaviour with every
test still green.

So what matters is not that the guard passes on today's tree -- it is that it FAILS on the
shape of that regression. These tests pin it against synthetic fixtures rather than the
committed sources, so they keep discriminating after the real file changes:

  * `test_correct_error_classes_pass`        -- the guard is not merely always-red.
  * `test_shader_returning_alloc_error_fails`   -- THE round-7 regression shape.
  * `test_pipeline_returning_alloc_error_fails` -- the same, for pipeline creation.
  * `test_buffer_returning_program_error_fails` -- the opposite mistake, which would latch
                                                   the sorter over a transient VRAM blip and
                                                   re-create the round-1 black screen.
  * `test_unchecked_object_fails`            -- fail closed on a GPU object never validated.
  * `test_missing_return_fails`              -- fail closed when the failure branch is
                                                unreadable, rather than passing over it.
  * `test_fallthrough_does_not_inherit_the_next_sites_return`
                                             -- THE round-8 finding: the guard's own blind
                                                spot. A check that reports and falls through
                                                used to be credited with a LATER site's
                                                return, so the mutation the guard claims to
                                                cover was invisible to it.
  * `test_unreadable_return_in_branch_fails` /
    `test_unbraced_failure_branch_is_refused` /
    `test_two_error_classes_in_one_branch_fails`
                                             -- the other shapes an unbounded search hid.
  * `test_branch_with_cleanup_and_nested_block_still_passes`
                                             -- the bounded parse does not pass by rejecting
                                                everything.
  * `test_site_count_pin_enforced`           -- the guard cannot be hollowed out by deleting
                                                sites until it has nothing left to check.
  * `test_missing_function_fails` /
    `test_no_producers_fails`                -- a parser that finds nothing must FAIL.
  * `test_policy_header_must_still_map_codes` -- the guard refuses to enforce a contract whose
                                                consumer has gone away.

Fixtures never touch the committed sources: each synthetic file lives in a temp dir inside
ROOT (the guard prints paths `relative_to(ROOT)`, so fixtures must be real subpaths), and the
module-level path constants and pins are patched for the duration of each test.
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
SCRIPT = ROOT / "tests" / "ci" / "check_sorter_error_class_parity.py"
spec = importlib.util.spec_from_file_location("check_sorter_error_class_parity", SCRIPT)
assert spec and spec.loader
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)

# A minimal stand-in for the real create_variant(): one shader, one pipeline, one buffer, each
# with the `if (!x.is_valid()) { return ERR_...; }` shape the guard reads.
_SHADER = """
    RID histogram_shader_file = create_compute_shader_from_spirv(device, histogram_source);
    if (!histogram_shader_file.is_valid()) {
        return %s;
    }
"""
_PIPELINE = """
    variant.histogram_pipeline = device->compute_pipeline_create(variant.histogram_shader);
    if (!variant.histogram_pipeline.is_valid()) {
        cleanup_variant(variant);
        return %s;
    }
"""
_BUFFER = """
    histogram_buffer = resource_rd->storage_buffer_create(histogram_bytes);
    if (!histogram_buffer.is_valid()) {
        _cleanup_partial_init(resource_rd);
        return %s;
    }
"""

PROGRAM_OK = "ERR_COMPILATION_FAILED"
ALLOC_OK = "ERR_CANT_CREATE"


def _source(create_variant_body: str, initialize_body: str) -> str:
    return (
        "Error RadixSort::create_variant(RenderingDevice *device, uint32_t radix_bits) {\n"
        + create_variant_body
        + "    return OK;\n}\n\n"
        "Error RadixSort::initialize(RenderingDevice *p_rd, uint32_t p_max_elements) {\n"
        + initialize_body
        + "    return OK;\n}\n"
    )


class SorterErrorClassParityTests(unittest.TestCase):
    def _run(
        self,
        source: str,
        *,
        policy: str | None = None,
        program_pin: int | None = None,
        allocation_pin: int | None = None,
    ) -> tuple[int, str]:
        """Run the guard over synthetic sources; return (exit code, captured output)."""
        with tempfile.TemporaryDirectory(dir=ROOT) as tmp:
            tmp_path = Path(tmp)
            source_path = tmp_path / "gpu_sorter.cpp"
            source_path.write_text(source, encoding="utf-8")
            policy_path = tmp_path / "sort_fallback_policy.h"
            policy_path.write_text(
                policy
                if policy is not None
                else "case ERR_COMPILATION_FAILED: case ERR_CANT_CREATE: return X;\n",
                encoding="utf-8",
            )
            patches = [
                mock.patch.object(guard, "SORTER_SOURCE", source_path),
                mock.patch.object(guard, "POLICY_HEADER", policy_path),
            ]
            if program_pin is not None:
                patches.append(mock.patch.object(guard, "EXPECTED_PROGRAM_SITES", program_pin))
            if allocation_pin is not None:
                patches.append(mock.patch.object(guard, "EXPECTED_ALLOCATION_SITES", allocation_pin))
            buffer = io.StringIO()
            with contextlib.ExitStack() as stack:
                for patch in patches:
                    stack.enter_context(patch)
                stack.enter_context(contextlib.redirect_stdout(buffer))
                code = guard.main()
            return code, buffer.getvalue()

    # --- the guard is not always-red -------------------------------------------------

    def test_correct_error_classes_pass(self):
        code, output = self._run(
            _source(_SHADER % PROGRAM_OK + _PIPELINE % PROGRAM_OK, _BUFFER % ALLOC_OK),
            program_pin=2,
            allocation_pin=1,
        )
        self.assertEqual(code, 0, output)
        self.assertIn("PASSED", output)

    # --- THE round-7 regression shapes -----------------------------------------------

    def test_shader_returning_alloc_error_fails(self):
        code, output = self._run(
            _source(_SHADER % ALLOC_OK + _PIPELINE % PROGRAM_OK, _BUFFER % ALLOC_OK),
            program_pin=2,
            allocation_pin=1,
        )
        self.assertEqual(code, 1, output)
        self.assertIn("histogram_shader_file", output)
        self.assertIn("must return ERR_COMPILATION_FAILED", output)

    def test_pipeline_returning_alloc_error_fails(self):
        code, output = self._run(
            _source(_SHADER % PROGRAM_OK + _PIPELINE % ALLOC_OK, _BUFFER % ALLOC_OK),
            program_pin=2,
            allocation_pin=1,
        )
        self.assertEqual(code, 1, output)
        self.assertIn("variant.histogram_pipeline", output)
        self.assertIn("must return ERR_COMPILATION_FAILED", output)

    def test_buffer_returning_program_error_fails(self):
        # The opposite mistake: an allocation failure labelled deterministic would latch the
        # sorter off over one momentary VRAM blip -- the permanent black screen round 1 removed.
        code, output = self._run(
            _source(_SHADER % PROGRAM_OK + _PIPELINE % PROGRAM_OK, _BUFFER % PROGRAM_OK),
            program_pin=2,
            allocation_pin=1,
        )
        self.assertEqual(code, 1, output)
        self.assertIn("histogram_buffer", output)
        self.assertIn("must return ERR_CANT_CREATE", output)

    # --- fail-closed behaviour --------------------------------------------------------

    def test_unchecked_object_fails(self):
        unchecked = "    RID s = create_compute_shader_from_spirv(device, src);\n"
        code, output = self._run(
            _source(_SHADER % PROGRAM_OK + _PIPELINE % PROGRAM_OK + unchecked, _BUFFER % ALLOC_OK),
            program_pin=2,
            allocation_pin=1,
        )
        self.assertEqual(code, 1, output)
        self.assertIn("never checked with is_valid()", output)

    def test_missing_return_fails(self):
        # A failure branch that reports some other way: the guard must say it cannot verify
        # the class rather than pass over the site.
        no_return = (
            "    RID s = create_compute_shader_from_spirv(device, src);\n"
            "    if (!s.is_valid()) {\n        report_failure();\n    }\n"
        )
        code, output = self._run(
            _source(no_return, _BUFFER % ALLOC_OK), program_pin=0, allocation_pin=1
        )
        self.assertEqual(code, 1, output)
        self.assertIn("does not RETURN", output)

    # --- round-8: the return must be BOUND to its own branch ---------------------------

    def test_fallthrough_does_not_inherit_the_next_sites_return(self):
        """THE round-8 finding, verbatim. The first shader check only REPORTS and falls
        through; the next shader check returns ERR_COMPILATION_FAILED. The pre-round-8 guard
        searched forward from the check without a bound, attributed the second site's return
        to the first, and recorded BOTH as correctly classified with no problems -- so the
        mutation it exists to catch was invisible to it."""
        fallthrough = (
            "    RID first_shader = create_compute_shader_from_spirv(device, src_a);\n"
            "    if (!first_shader.is_valid()) {\n"
            "        GS_LOG_ERROR_DEFAULT(\"first shader failed\");\n"
            "    }\n"
            "    RID second_shader = create_compute_shader_from_spirv(device, src_b);\n"
            "    if (!second_shader.is_valid()) {\n"
            "        return ERR_COMPILATION_FAILED;\n"
            "    }\n"
        )
        code, output = self._run(
            _source(fallthrough, _BUFFER % ALLOC_OK), program_pin=1, allocation_pin=1
        )
        self.assertEqual(code, 1, output)
        self.assertIn("first_shader", output)
        self.assertIn("does not RETURN", output)
        # ...and the site that DOES return correctly must not be dragged down with it.
        self.assertNotIn("`second_shader`", output)

    def test_unreadable_return_in_branch_fails(self):
        """The other half of the round-8 finding: a branch whose return this guard cannot
        read as an error class used to be skipped, so the NEXT site's matching return stood
        in for it. It must be reported instead."""
        unreadable = (
            "    RID s = create_compute_shader_from_spirv(device, src);\n"
            "    if (!s.is_valid()) {\n        return map_error(err);\n    }\n"
            "    RID t = create_compute_shader_from_spirv(device, src2);\n"
            "    if (!t.is_valid()) {\n        return ERR_COMPILATION_FAILED;\n    }\n"
        )
        code, output = self._run(
            _source(unreadable, _BUFFER % ALLOC_OK), program_pin=1, allocation_pin=1
        )
        self.assertEqual(code, 1, output)
        self.assertIn("cannot read as an error class", output)

    def test_unbraced_failure_branch_is_refused(self):
        """Fail closed rather than guess where an unbraced branch ends -- guessing is what
        made a later site's return look like this one's."""
        unbraced = (
            "    RID s = create_compute_shader_from_spirv(device, src);\n"
            "    if (!s.is_valid())\n        return ERR_COMPILATION_FAILED;\n"
        )
        code, output = self._run(
            _source(unbraced, _BUFFER % ALLOC_OK), program_pin=0, allocation_pin=1
        )
        self.assertEqual(code, 1, output)
        self.assertIn("UNBRACED failure branch", output)

    def test_two_error_classes_in_one_branch_fails(self):
        """classify_sorter_creation_error() sees exactly one code, so a branch that can
        return either is not a classification this guard can certify."""
        ambiguous = (
            "    RID s = create_compute_shader_from_spirv(device, src);\n"
            "    if (!s.is_valid()) {\n"
            "        if (device) { return ERR_CANT_CREATE; }\n"
            "        return ERR_COMPILATION_FAILED;\n"
            "    }\n"
        )
        code, output = self._run(
            _source(ambiguous, _BUFFER % ALLOC_OK), program_pin=0, allocation_pin=1
        )
        self.assertEqual(code, 1, output)
        self.assertIn("more than one error class", output)

    def test_branch_with_cleanup_and_nested_block_still_passes(self):
        """The negative control for the bounded parse: real failure branches log, clean up
        and may contain a nested block before returning. That must still resolve, or the fix
        would 'pass' by rejecting everything."""
        realistic = (
            "    RID s = create_compute_shader_from_spirv(device, src);\n"
            "    if (!s.is_valid()) {\n"
            "        GS_LOG_ERROR_DEFAULT(\"failed\");\n"
            "        if (device) { cleanup_variant(variant); }\n"
            "        return ERR_COMPILATION_FAILED;\n"
            "    }\n"
        )
        code, output = self._run(
            _source(realistic, _BUFFER % ALLOC_OK), program_pin=1, allocation_pin=1
        )
        self.assertEqual(code, 0, output)
        self.assertIn("PASSED", output)

    def test_site_count_pin_enforced(self):
        # Deleting covered sites must not leave a guard that still prints PASSED.
        code, output = self._run(
            _source(_SHADER % PROGRAM_OK + _PIPELINE % PROGRAM_OK, _BUFFER % ALLOC_OK),
            program_pin=5,
            allocation_pin=1,
        )
        self.assertEqual(code, 1, output)
        self.assertIn("covered site count changed", output)

    def test_missing_function_fails(self):
        # create_variant() is absent entirely: a renamed or removed scoped function must be
        # reported, not silently skipped.
        source = (
            "Error RadixSort::initialize(RenderingDevice *p_rd, uint32_t p_max_elements) {\n"
            + _BUFFER % ALLOC_OK
            + "    return OK;\n}\n"
        )
        code, output = self._run(source)
        self.assertEqual(code, 1, output)
        self.assertIn("could not parse", output)

    def test_no_producers_fails(self):
        code, output = self._run(_source("    int x = 0;\n", "    int y = 0;\n"))
        self.assertEqual(code, 1, output)
        self.assertIn("parsed no shader/pipeline/buffer creation", output)

    def test_policy_header_must_still_map_codes(self):
        # If the classifier stops mentioning a code, the contract changed and a human must
        # look; the guard must not keep passing over a dead invariant.
        code, output = self._run(
            _source(_SHADER % PROGRAM_OK + _PIPELINE % PROGRAM_OK, _BUFFER % ALLOC_OK),
            policy="case ERR_CANT_CREATE: return X;\n",
            program_pin=2,
            allocation_pin=1,
        )
        self.assertEqual(code, 1, output)
        self.assertIn("ERR_COMPILATION_FAILED", output)
        self.assertIn("no longer appears", output)

    # --- comments must not be able to fake or hide a site ------------------------------

    def test_commented_out_site_is_not_counted(self):
        commented = (
            "    // RID ghost = create_compute_shader_from_spirv(device, src);\n"
            "    // if (!ghost.is_valid()) { return ERR_CANT_CREATE; }\n"
        )
        code, output = self._run(
            _source(_SHADER % PROGRAM_OK + _PIPELINE % PROGRAM_OK + commented, _BUFFER % ALLOC_OK),
            program_pin=2,
            allocation_pin=1,
        )
        self.assertEqual(code, 0, output)


if __name__ == "__main__":
    unittest.main()
