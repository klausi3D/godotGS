#!/usr/bin/env python3
"""Unit tests for tests/ci/check_sorter_error_class_parity.py (#586 rounds 7-9).

The guard exists because the retry policy for a failed sorter build rests on a
correspondence that nothing else checks: the error a failure site reports decides whether
`classify_sorter_creation_error()` latches the sorter off or retries it. One error constant
typed at the wrong site reverts the behaviour with every test still green.

So what matters is not that the guard passes on today's tree -- it is that it FAILS on the
shape of each regression, in both directions:

  * `test_correct_error_classes_pass`        -- the guard is not merely always-red.
  * `test_propagating_site_typing_a_constant_fails`
                                             -- THE round-9 shape at the SITE: a site that
                                                types `ERR_COMPILATION_FAILED` instead of
                                                handing on the helper's own answer re-decides
                                                what the helper already decided, and latches
                                                a driver allocation failure forever.
  * `test_helper_driver_leg_returning_program_error_fails`
                                             -- THE round-9 shape in the HELPER: the leg that
                                                ends in vkCreateShaderModule cannot tell an
                                                allocation failure from a rejection, so
                                                calling it deterministic re-creates the
                                                permanent black screen rounds 1-2 removed.
  * `test_helper_source_leg_returning_alloc_error_fails`
                                             -- the OPPOSITE direction, and the load-bearing
                                                control: if every leg were simply called
                                                retryable, the round-7 recompile loop is back
                                                and the test above still passes.
  * `test_fused_helper_fails`                -- collapsing the legs back onto
                                                RenderingDevice::shader_create_from_spirv()
                                                merges the two classes silently; the leg pin
                                                catches it.
  * `test_pipeline_returning_program_error_fails` -- the same round-9 argument for
                                                vkCreateComputePipelines.
  * `test_buffer_returning_program_error_fails` -- an allocation labelled deterministic would
                                                latch the sorter over a transient VRAM blip.
  * `test_unchecked_object_fails`            -- fail closed on a GPU object never validated.
  * `test_missing_return_fails`              -- fail closed when the failure branch is
                                                unreadable, rather than passing over it.
  * `test_fallthrough_does_not_inherit_the_next_sites_return`
                                             -- THE round-8 finding: the guard's own blind
                                                spot. A check that reports and falls through
                                                used to be credited with a LATER site's
                                                return.
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
    `test_no_producers_fails` /
    `test_missing_helper_fails`              -- a parser that finds nothing must FAIL.
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

PROGRAM_OK = "ERR_COMPILATION_FAILED"
ALLOC_OK = "ERR_CANT_CREATE"

# A minimal stand-in for the real create_variant(): one shader (propagating the helper's own
# error), one pipeline, one buffer, each in the shape the guard reads.
_SHADER = """
    Error histogram_shader_error = OK;
    RID histogram_shader_file = create_compute_shader_from_spirv(device, histogram_source, &histogram_shader_error);
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

# The helper's three legs, each reporting its own class. `%s` is the code that leg reports,
# so a test can mutate exactly one leg and leave the others correct.
_HELPER = """
static RID create_compute_shader_from_spirv(RenderingDevice *rd, const String &source, Error *r_error = nullptr) {
    Vector<uint8_t> spirv_data = rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_COMPUTE, source);
    if (spirv_data.is_empty()) {
        *r_error = %s;
        ERR_FAIL_V_MSG(RID(), "compile failed");
    }
    Vector<uint8_t> shader_binary = rd->shader_compile_binary_from_spirv(spirv_stages);
    if (shader_binary.is_empty()) {
        *r_error = %s;
        ERR_FAIL_V_MSG(RID(), "reflection failed");
    }
    RID shader = rd->shader_create_from_bytecode(shader_binary);
    if (!shader.is_valid()) {
        *r_error = %s;
        ERR_FAIL_V_MSG(RID(), "driver refused");
    }
    return shader;
}
"""

# The pre-round-9 helper: legs 2 and 3 fused back into shader_create_from_spirv(), which is
# how the distinction between "the source will not translate" and "the driver produced no
# object" gets lost without any site changing.
_FUSED_HELPER = """
static RID create_compute_shader_from_spirv(RenderingDevice *rd, const String &source, Error *r_error = nullptr) {
    Vector<uint8_t> spirv_data = rd->shader_compile_spirv_from_source(RD::SHADER_STAGE_COMPUTE, source);
    if (spirv_data.is_empty()) {
        *r_error = ERR_COMPILATION_FAILED;
        ERR_FAIL_V_MSG(RID(), "compile failed");
    }
    return rd->shader_create_from_spirv(spirv_stages);
}
"""


def _helper(spirv=PROGRAM_OK, binary=PROGRAM_OK, driver=ALLOC_OK) -> str:
    return _HELPER % (spirv, binary, driver)


def _source(create_variant_body: str, initialize_body: str, helper: str | None = None) -> str:
    return (
        (helper if helper is not None else _helper())
        + "\nError RadixSort::create_variant(RenderingDevice *device, uint32_t radix_bits) {\n"
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
        propagated_pin: int | None = None,
        device_object_pin: int | None = None,
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
            if propagated_pin is not None:
                patches.append(mock.patch.object(guard, "EXPECTED_PROPAGATED_SITES", propagated_pin))
            if device_object_pin is not None:
                patches.append(
                    mock.patch.object(guard, "EXPECTED_DEVICE_OBJECT_SITES", device_object_pin)
                )
            if allocation_pin is not None:
                patches.append(mock.patch.object(guard, "EXPECTED_ALLOCATION_SITES", allocation_pin))
            buffer = io.StringIO()
            with contextlib.ExitStack() as stack:
                for patch in patches:
                    stack.enter_context(patch)
                stack.enter_context(contextlib.redirect_stdout(buffer))
                code = guard.main()
            return code, buffer.getvalue()

    def _pins(self, propagated=1, device_object=1, allocation=1) -> dict:
        return {
            "propagated_pin": propagated,
            "device_object_pin": device_object,
            "allocation_pin": allocation,
        }

    # --- the guard is not always-red -------------------------------------------------

    def test_correct_error_classes_pass(self):
        code, output = self._run(
            _source(_SHADER % "histogram_shader_error" + _PIPELINE % ALLOC_OK, _BUFFER % ALLOC_OK),
            **self._pins(),
        )
        self.assertEqual(code, 0, output)
        self.assertIn("PASSED", output)

    # --- THE round-9 regression shapes -----------------------------------------------

    def test_propagating_site_typing_a_constant_fails(self):
        """A site that types a constant re-decides what the helper's legs already decided.
        `ERR_COMPILATION_FAILED` here is exactly the pre-round-9 source: a transient driver
        allocation failure reported by leg 3 arrives at the classifier as deterministic and
        latches the sorter off for the rest of the renderer's life."""
        code, output = self._run(
            _source(_SHADER % PROGRAM_OK + _PIPELINE % ALLOC_OK, _BUFFER % ALLOC_OK),
            **self._pins(),
        )
        self.assertEqual(code, 1, output)
        self.assertIn("histogram_shader_file", output)
        self.assertIn("re-decides", output)

    def test_helper_driver_leg_returning_program_error_fails(self):
        """THE round-9 finding in the helper. vkCreateShaderModule's OUT_OF_HOST_MEMORY and a
        genuine rejection are the same invalid RID here, so this leg cannot be deterministic."""
        code, output = self._run(
            _source(
                _SHADER % "histogram_shader_error" + _PIPELINE % ALLOC_OK,
                _BUFFER % ALLOC_OK,
                helper=_helper(driver=PROGRAM_OK),
            ),
            **self._pins(),
        )
        self.assertEqual(code, 1, output)
        self.assertIn("shader", output)
        self.assertIn("must report ERR_CANT_CREATE", output)

    def test_helper_source_leg_returning_alloc_error_fails(self):
        """THE LOAD-BEARING CONTROL. Calling every leg retryable would satisfy the test above
        while restoring the round-7 defect: the renderer recompiles a source that will never
        translate, forever, at the saturated backoff."""
        code, output = self._run(
            _source(
                _SHADER % "histogram_shader_error" + _PIPELINE % ALLOC_OK,
                _BUFFER % ALLOC_OK,
                helper=_helper(spirv=ALLOC_OK),
            ),
            **self._pins(),
        )
        self.assertEqual(code, 1, output)
        self.assertIn("spirv_data", output)
        self.assertIn("must report ERR_COMPILATION_FAILED", output)

    def test_fused_helper_fails(self):
        """Collapsing legs 2 and 3 back into RenderingDevice::shader_create_from_spirv() puts a
        deterministic failure and an indistinguishable driver one behind one return value
        again -- with every call site unchanged, so nothing else would notice."""
        code, output = self._run(
            _source(
                _SHADER % "histogram_shader_error" + _PIPELINE % ALLOC_OK,
                _BUFFER % ALLOC_OK,
                helper=_FUSED_HELPER,
            ),
            **self._pins(),
        )
        self.assertEqual(code, 1, output)
        self.assertIn("separately-checked legs", output)

    def test_helper_leg_that_reports_nothing_fails(self):
        """A leg that leaves `*r_error` alone hands the caller whatever an earlier leg wrote."""
        silent = _HELPER.replace("        *r_error = %s;\n        ERR_FAIL_V_MSG(RID(), \"driver refused\");", "        ERR_FAIL_V_MSG(RID(), \"driver refused\");")
        code, output = self._run(
            _source(
                _SHADER % "histogram_shader_error" + _PIPELINE % ALLOC_OK,
                _BUFFER % ALLOC_OK,
                helper=silent % (PROGRAM_OK, PROGRAM_OK),
            ),
            **self._pins(),
        )
        self.assertEqual(code, 1, output)
        self.assertIn("does not write", output)

    def test_pipeline_returning_program_error_fails(self):
        """vkCreateComputePipelines collapses its VkResult the same way vkCreateShaderModule
        does, so a pipeline failure cannot be classified deterministic either."""
        code, output = self._run(
            _source(_SHADER % "histogram_shader_error" + _PIPELINE % PROGRAM_OK, _BUFFER % ALLOC_OK),
            **self._pins(),
        )
        self.assertEqual(code, 1, output)
        self.assertIn("variant.histogram_pipeline", output)
        self.assertIn("must return ERR_CANT_CREATE", output)

    def test_buffer_returning_program_error_fails(self):
        # An allocation failure labelled deterministic would latch the sorter off over one
        # momentary VRAM blip -- the permanent black screen round 1 removed.
        code, output = self._run(
            _source(_SHADER % "histogram_shader_error" + _PIPELINE % ALLOC_OK, _BUFFER % PROGRAM_OK),
            **self._pins(),
        )
        self.assertEqual(code, 1, output)
        self.assertIn("histogram_buffer", output)
        self.assertIn("must return ERR_CANT_CREATE", output)

    # --- fail-closed behaviour --------------------------------------------------------

    def test_unchecked_object_fails(self):
        unchecked = "    RID s = create_compute_shader_from_spirv(device, src, &e);\n"
        code, output = self._run(
            _source(
                _SHADER % "histogram_shader_error" + _PIPELINE % ALLOC_OK + unchecked,
                _BUFFER % ALLOC_OK,
            ),
            **self._pins(),
        )
        self.assertEqual(code, 1, output)
        self.assertIn("never checked for failure", output)

    def test_missing_return_fails(self):
        # A failure branch that reports some other way: the guard must say it cannot verify
        # the class rather than pass over the site.
        no_return = (
            "    RID s = create_compute_shader_from_spirv(device, src, &e);\n"
            "    if (!s.is_valid()) {\n        report_failure();\n    }\n"
        )
        code, output = self._run(
            _source(no_return, _BUFFER % ALLOC_OK), **self._pins(propagated=0, device_object=0)
        )
        self.assertEqual(code, 1, output)
        self.assertIn("does not RETURN", output)

    def test_site_without_an_error_out_parameter_fails(self):
        """The pre-round-9 call shape: two arguments, so the site has nothing to hand on and
        must invent a constant."""
        two_arg = (
            "    RID s = create_compute_shader_from_spirv(device, src);\n"
            "    if (!s.is_valid()) {\n        return ERR_COMPILATION_FAILED;\n    }\n"
        )
        code, output = self._run(
            _source(two_arg, _BUFFER % ALLOC_OK), **self._pins(propagated=0, device_object=0)
        )
        self.assertEqual(code, 1, output)
        self.assertIn("argument(s)", output)

    # --- round-8: the return must be BOUND to its own branch ---------------------------

    def test_fallthrough_does_not_inherit_the_next_sites_return(self):
        """THE round-8 finding, verbatim. The first shader check only REPORTS and falls
        through; the next shader check returns its error. The pre-round-8 guard searched
        forward from the check without a bound, attributed the second site's return to the
        first, and recorded BOTH as correctly classified with no problems -- so the mutation
        it exists to catch was invisible to it."""
        fallthrough = (
            "    RID first_shader = create_compute_shader_from_spirv(device, src_a, &e1);\n"
            "    if (!first_shader.is_valid()) {\n"
            "        GS_LOG_ERROR_DEFAULT(\"first shader failed\");\n"
            "    }\n"
            "    RID second_shader = create_compute_shader_from_spirv(device, src_b, &e2);\n"
            "    if (!second_shader.is_valid()) {\n"
            "        return e2;\n"
            "    }\n"
        )
        code, output = self._run(
            _source(fallthrough, _BUFFER % ALLOC_OK), **self._pins(propagated=1, device_object=0)
        )
        self.assertEqual(code, 1, output)
        self.assertIn("first_shader", output)
        self.assertIn("does not RETURN", output)
        # ...and the site that DOES return correctly must not be dragged down with it.
        self.assertNotIn("`second_shader`", output)

    def test_unreadable_return_in_branch_fails(self):
        """The other half of the round-8 finding: a branch whose return this guard cannot
        read as this site's error used to be skipped, so the NEXT site's matching return stood
        in for it. It must be reported instead."""
        unreadable = (
            "    RID s = create_compute_shader_from_spirv(device, src, &e1);\n"
            "    if (!s.is_valid()) {\n        return map_error(err);\n    }\n"
            "    RID t = create_compute_shader_from_spirv(device, src2, &e2);\n"
            "    if (!t.is_valid()) {\n        return e2;\n    }\n"
        )
        code, output = self._run(
            _source(unreadable, _BUFFER % ALLOC_OK), **self._pins(propagated=1, device_object=0)
        )
        self.assertEqual(code, 1, output)
        self.assertIn("instead of the `e1`", output)

    def test_unbraced_failure_branch_is_refused(self):
        """Fail closed rather than guess where an unbraced branch ends -- guessing is what
        made a later site's return look like this one's."""
        unbraced = (
            "    variant.p = device->compute_pipeline_create(variant.s);\n"
            "    if (!variant.p.is_valid())\n        return ERR_CANT_CREATE;\n"
        )
        code, output = self._run(
            _source(unbraced, _BUFFER % ALLOC_OK), **self._pins(propagated=0, device_object=0)
        )
        self.assertEqual(code, 1, output)
        self.assertIn("UNBRACED failure branch", output)

    def test_two_error_classes_in_one_branch_fails(self):
        """classify_sorter_creation_error() sees exactly one code, so a branch that can
        return either is not a classification this guard can certify."""
        ambiguous = (
            "    variant.p = device->compute_pipeline_create(variant.s);\n"
            "    if (!variant.p.is_valid()) {\n"
            "        if (device) { return ERR_CANT_CREATE; }\n"
            "        return ERR_COMPILATION_FAILED;\n"
            "    }\n"
        )
        code, output = self._run(
            _source(ambiguous, _BUFFER % ALLOC_OK), **self._pins(propagated=0, device_object=0)
        )
        self.assertEqual(code, 1, output)
        self.assertIn("more than one error class", output)

    def test_branch_with_cleanup_and_nested_block_still_passes(self):
        """The negative control for the bounded parse: real failure branches log, clean up
        and may contain a nested block before returning. That must still resolve, or the fix
        would 'pass' by rejecting everything."""
        realistic = (
            "    Error e = OK;\n"
            "    RID s = create_compute_shader_from_spirv(device, src, &e);\n"
            "    if (!s.is_valid()) {\n"
            "        GS_LOG_ERROR_DEFAULT(\"failed\");\n"
            "        if (device) { cleanup_variant(variant); }\n"
            "        return e;\n"
            "    }\n"
        )
        code, output = self._run(
            _source(realistic, _BUFFER % ALLOC_OK), **self._pins(propagated=1, device_object=0)
        )
        self.assertEqual(code, 0, output)
        self.assertIn("PASSED", output)

    def test_site_count_pin_enforced(self):
        # Deleting covered sites must not leave a guard that still prints PASSED.
        code, output = self._run(
            _source(_SHADER % "histogram_shader_error" + _PIPELINE % ALLOC_OK, _BUFFER % ALLOC_OK),
            **self._pins(propagated=5),
        )
        self.assertEqual(code, 1, output)
        self.assertIn("covered site count changed", output)

    def test_missing_function_fails(self):
        # create_variant() is absent entirely: a renamed or removed scoped function must be
        # reported, not silently skipped.
        source = (
            _helper()
            + "\nError RadixSort::initialize(RenderingDevice *p_rd, uint32_t p_max_elements) {\n"
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

    def test_missing_helper_fails(self):
        """The leg check is the whole reason a propagating site is trustworthy; a helper the
        guard cannot find must fail rather than leave the sites unbacked."""
        source = _source(
            _SHADER % "histogram_shader_error" + _PIPELINE % ALLOC_OK, _BUFFER % ALLOC_OK, helper=""
        )
        code, output = self._run(source, **self._pins())
        self.assertEqual(code, 1, output)
        self.assertIn("could not parse", output)

    def test_policy_header_must_still_map_codes(self):
        # If the classifier stops mentioning a code, the contract changed and a human must
        # look; the guard must not keep passing over a dead invariant.
        code, output = self._run(
            _source(_SHADER % "histogram_shader_error" + _PIPELINE % ALLOC_OK, _BUFFER % ALLOC_OK),
            policy="case ERR_CANT_CREATE: return X;\n",
            **self._pins(),
        )
        self.assertEqual(code, 1, output)
        self.assertIn("ERR_COMPILATION_FAILED", output)
        self.assertIn("no longer appears", output)

    # --- comments must not be able to fake or hide a site ------------------------------

    def test_commented_out_site_is_not_counted(self):
        commented = (
            "    // RID ghost = create_compute_shader_from_spirv(device, src, &ge);\n"
            "    // if (!ghost.is_valid()) { return ERR_CANT_CREATE; }\n"
        )
        code, output = self._run(
            _source(
                _SHADER % "histogram_shader_error" + _PIPELINE % ALLOC_OK + commented,
                _BUFFER % ALLOC_OK,
            ),
            **self._pins(),
        )
        self.assertEqual(code, 0, output)


if __name__ == "__main__":
    unittest.main()
