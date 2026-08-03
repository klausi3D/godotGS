#!/usr/bin/env python3
"""Unit tests for tests/ci/check_environment_skip_marker.py (#595).

The guard passing against the committed tree only proves the tree is clean
today. These cases pin the *rules*: what counts as an environment-skip site,
what deliberately does not, that the ratchet fails in BOTH directions, that a
missing or corrupt baseline fails closed, and that `--write-baseline` cannot be
used to launder a new skip into the inventory.

Everything runs against synthetic source trees in a tempdir, so a case cannot
pass or fail because of an unrelated edit to the real module tests. stdlib
`unittest` + `tempfile` only — no new dependency.
"""

from __future__ import annotations

import importlib.util
import io
import json
import contextlib
import os
import re
import subprocess
import tempfile
import unittest
from unittest import mock
from pathlib import Path

GUARD_PATH = Path(__file__).resolve().parent / "check_environment_skip_marker.py"


def _load_guard():
    spec = importlib.util.spec_from_file_location("check_environment_skip_marker", GUARD_PATH)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


guard = _load_guard()


# A minimal, VALID macro header: GS_ENV_SKIP defined and emitting the token, and
# all four guarded macros routing through it. The macro-contract check runs on
# every guard invocation, so every synthetic tree needs one.
VALID_MACRO_HEADER = """\
#define GS_ENV_SKIP(m_reason) MESSAGE(String("GS_ENV_SKIP: ") + String(m_reason))

#define REQUIRE_GPU_DEVICE()                        \\
    RenderingDevice *rd = get_rd();                 \\
    if (rd == nullptr) {                            \\
        GS_ENV_SKIP("RenderingDevice unavailable"); \\
        return;                                     \\
    }

#define REQUIRE_LOCAL_GPU_DEVICE()                        \\
    RenderingDevice *local_device = make_local();         \\
    if (local_device == nullptr) {                        \\
        GS_ENV_SKIP("local RenderingDevice unavailable"); \\
        return;                                           \\
    }

#define REQUIRE_STREAMING_CAPABLE()             \\
    do {                                        \\
        if (!streaming_ok()) {                  \\
            GS_ENV_SKIP("streaming unavailable"); \\
            return;                             \\
        }                                       \\
    } while (0)

#define REQUIRE_WORKER_THREAD_POOL()                    \\
    do {                                                \\
        if (!pool_ok()) {                               \\
            GS_ENV_SKIP("worker thread pool unavailable"); \\
            return;                                     \\
        }                                               \\
    } while (0)
"""


def _valid_allowance_entry(allowed: int) -> dict:
    """A schema-valid allowance entry, for fixtures that need one."""
    return {
        "allowed": allowed,
        "owner": "gaussian-splatting-module",
        "reason": "measured environment skip frozen by #595",
        "issue_url": "https://github.com/klausi3D/godotGS/issues/595",
        "expires_utc": "2026-11-01T00:00:00+00:00",
    }


class GuardTestCase(unittest.TestCase):
    """Base: a synthetic module-tests tree plus a redirected baseline path."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.tests_dir = self.root / "tests"
        self.tests_dir.mkdir()
        self.baseline_path = self.root / "environment_skip_baseline.json"

        # The engine test dir is redirected at an EMPTY tempdir too. The scan
        # covers tests/test_*.{h,cpp} as well as the module tree, so leaving it
        # pointed at the real repo would make every synthetic tree carry the
        # real corpus's sources.
        self.engine_dir = self.root / "engine_tests"
        self.engine_dir.mkdir()

        self._saved = (
            guard.ROOT,
            guard.MODULE_TESTS_DIR,
            guard.ENGINE_TESTS_DIR,
            guard.BASELINE_PATH,
        )
        guard.ROOT = self.root
        guard.MODULE_TESTS_DIR = self.tests_dir
        guard.ENGINE_TESTS_DIR = self.engine_dir
        guard.BASELINE_PATH = self.baseline_path
        self.write_macro_header(VALID_MACRO_HEADER)

        # A real (empty) git history, so base resolution succeeds. These cases
        # are about the scan and the writer, not the base ratchet: the baseline
        # does not exist at this base, so the base comparison correctly reports
        # ABSENT_AT_BASE and stands aside. Without a repo, every one of them
        # would fail closed on "cannot resolve the review base" -- which is the
        # right behaviour, but not what they are testing.
        subprocess.run(
            ["git", "init", "-q", "-b", "main"], cwd=self.root, check=True, capture_output=True
        )
        for key, value in (("user.email", "t@example.com"), ("user.name", "t")):
            subprocess.run(
                ["git", "config", key, value], cwd=self.root, check=True, capture_output=True
            )
        subprocess.run(
            ["git", "commit", "-q", "--allow-empty", "-m", "base"],
            cwd=self.root,
            check=True,
            capture_output=True,
        )
        self.addCleanup(self._restore)

    def _restore(self) -> None:
        (
            guard.ROOT,
            guard.MODULE_TESTS_DIR,
            guard.ENGINE_TESTS_DIR,
            guard.BASELINE_PATH,
        ) = self._saved
        self._tmp.cleanup()

    def write_macro_header(self, text: str) -> None:
        (self.tests_dir / "test_macros.h").write_text(text, encoding="utf-8")

    def write_source(self, name: str, text: str) -> None:
        (self.tests_dir / name).write_text(text, encoding="utf-8")

    def key(self, name: str) -> str:
        """The baseline key for a synthetic source.

        Keys are repo-relative POSIX paths, not basenames (two `test_utils.h`
        exist in the real tree). Tests go through this rather than hardcoding a
        shape, so a future key change breaks one helper instead of twenty
        assertions -- and cannot silently make an assertNotIn vacuous.
        """
        return f"tests/{name}"

    def run_guard(self, argv: list[str] | None = None) -> tuple[int, str]:
        argv = list(argv or [])
        if "--base-ref" not in argv:
            argv += ["--base-ref", "main"]
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = guard.main(argv)
        return code, buffer.getvalue()

    def seed_baseline(self) -> None:
        """Compose an initial baseline WITHOUT going through write_baseline().

        There is deliberately no create-from-nothing path in the guard: every
        such path has turned out to be a laundering bypass. The composition
        helpers are pure -- they render a document and have no inclusion
        semantics to subvert -- so the tests use those to seed a synthetic tree
        and exercise the real shrink-only writer for everything after.
        """
        document = guard.build_baseline_document(guard.scan_fingerprints(), None)
        self.baseline_path.write_text(guard._serialize(document), encoding="utf-8")

    def regenerate(self) -> None:
        """Seed if absent, otherwise go through the shrink-only writer."""
        if not self.baseline_path.is_file():
            self.seed_baseline()
            return
        code, out = self.run_guard(["--write-baseline"])
        self.assertEqual(code, 0, out)


class SiteRecognitionTests(GuardTestCase):
    def test_canonical_macro_site_is_recognised(self) -> None:
        """A skip behind the canonical macro is a site (A7)."""
        self.write_source(
            "test_alpha.h",
            "TEST_CASE(\"a\") {\n    REQUIRE_GPU_DEVICE();\n    CHECK(rd);\n}\n",
        )
        sites = guard.scan_source((self.tests_dir / "test_alpha.h").read_text(encoding="utf-8"))
        self.assertEqual([(2, "macro", "REQUIRE_GPU_DEVICE", "a")], sites)

    def test_direct_gs_env_skip_site_is_recognised(self) -> None:
        self.write_source(
            "test_alpha.h",
            'TEST_CASE("a") {\n    if (!ok) {\n        GS_ENV_SKIP("no device");\n'
            "        return;\n    }\n}\n",
        )
        sites = guard.scan_source((self.tests_dir / "test_alpha.h").read_text(encoding="utf-8"))
        self.assertEqual([(3, "macro", "GS_ENV_SKIP", "a")], sites)

    def test_free_form_prose_site_is_recognised(self) -> None:
        """An unconverted MESSAGE("Skip…") is a site (A7): the 356 legacy sites
        must be COUNTED, not silently dropped, or the inventory shrinks while
        the hidden surface grows."""
        self.write_source(
            "test_alpha.h",
            'TEST_CASE("a") {\n    MESSAGE("Skipping test - renderer unavailable");\n'
            "    return;\n}\n",
        )
        sites = guard.scan_source((self.tests_dir / "test_alpha.h").read_text(encoding="utf-8"))
        self.assertEqual([(2, "message", "Skipping test - renderer unavailable", "a")], sites)

    def test_vformat_prose_site_is_recognised(self) -> None:
        self.write_source(
            "test_alpha.h",
            'MESSAGE(vformat("Skipping %s - unsupported", label));\n',
        )
        sites = guard.scan_source((self.tests_dir / "test_alpha.h").read_text(encoding="utf-8"))
        self.assertEqual([(1, "message", "Skipping %s - unsupported", guard.FILE_SCOPE)], sites)

    def test_warn_print_prose_site_is_recognised(self) -> None:
        self.write_source(
            "test_alpha.h",
            'WARN_PRINT("RenderingDevice unavailable - skipping painterly test");\n',
        )
        sites = guard.scan_source((self.tests_dir / "test_alpha.h").read_text(encoding="utf-8"))
        self.assertEqual(1, len(sites))
        self.assertEqual("warn", sites[0][1])

    def test_comment_mentioning_macro_is_not_counted(self) -> None:
        """A7: prose about the macro must not inflate the count.

        `git grep -c REQUIRE_GPU_DEVICE` DID count these during the #595
        investigation, which is why the guard strips comments first.
        """
        self.write_source(
            "test_alpha.h",
            "// Use REQUIRE_GPU_DEVICE() here once the lane exists.\n"
            "/* REQUIRE_LOCAL_GPU_DEVICE() and MESSAGE(\"Skipping test - x\") are\n"
            "   documented above. */\n"
            "TEST_CASE(\"a\") { CHECK(true); }\n",
        )
        self.assertEqual([], guard.scan_source((self.tests_dir / "test_alpha.h").read_text(encoding="utf-8")))

    def test_failing_precondition_macro_is_not_a_site(self) -> None:
        """REQUIRE_RENDERING_DEVICE_SINGLETON() FAILs rather than skipping, so it
        is deliberately outside the silent-pass surface (contract non-goal)."""
        self.write_source("test_alpha.h", "REQUIRE_RENDERING_DEVICE_SINGLETON();\n")
        self.assertEqual([], guard.scan_source((self.tests_dir / "test_alpha.h").read_text(encoding="utf-8")))

    def test_non_skip_message_is_not_a_site(self) -> None:
        """Only prose that BEGINS with skip wording counts; ordinary diagnostics
        that happen to mention skipping do not."""
        self.write_source(
            "test_alpha.h",
            'MESSAGE("Pre-teardown sample: skipped 3 chunks");\n',
        )
        self.assertEqual([], guard.scan_source((self.tests_dir / "test_alpha.h").read_text(encoding="utf-8")))

    def test_embedded_prose_form_is_knowingly_not_counted(self) -> None:
        """The documented boundary of the shape contract, asserted rather than
        left emergent.

        A message that mentions skipping MID-SENTENCE is a genuine environment
        skip that this detector does not see. Closing that is follow-on
        GS-595-E; until then the exclusion is pinned here so nobody can widen or
        narrow it by accident, and so the number in the baseline keeps meaning
        exactly one thing.
        """
        for literal in (
            "Cache file not created (caching may be disabled); skipping version guard test",
            "[TileRenderer] RenderingServer not available, skipping regression test",
            "Renderer unavailable (headless mode) - skipping renderer state checks",
        ):
            self.write_source("test_alpha.h", f'MESSAGE("{literal}");\n')
            self.assertEqual(
                [],
                guard.scan_source((self.tests_dir / "test_alpha.h").read_text(encoding="utf-8")),
                literal,
            )

    def test_embedded_gap_on_the_committed_tree_is_exactly_nine(self) -> None:
        """The gap is measured, not estimated, and its size is pinned.

        If this number moves, either someone converted an embedded site (good,
        lower it) or added one (bad). Either way it must be a deliberate edit,
        not a silent drift in the hidden surface.
        """
        real = _load_guard()
        anywhere = re.compile(r"\bskipp(?:ing|ed)\b", re.IGNORECASE)
        embedded: list[str] = []
        for path in real.test_sources():
            text = real.strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            for match in real._MESSAGE_RE.finditer(text):
                literal = match.group("text")
                if not real._SKIP_PROSE_PREFIX_RE.search(literal) and anywhere.search(literal):
                    embedded.append(path.name)
        self.assertEqual(9, len(embedded), sorted(embedded))
        self.assertEqual(
            {
                "test_ply_importer.h",
                "test_shadow_instance_subset.h",
                "tile_renderer_regression_test.cpp",
                "test_node_bootstrap.h",
            },
            set(embedded),
        )

    def test_embedded_only_files_are_absent_from_the_inventory(self) -> None:
        """The consequence spelled out: two files skip and are counted nowhere.

        Both hold [SceneTree] cases, so the strict `GaussianSplatting [SceneTree]`
        lane can read zero skip markers while skipping at runtime. Pinning this
        keeps the fact discoverable from the test suite rather than only from a
        docstring.
        """
        real = _load_guard()
        found = real.scan_all()
        prefix = "modules/gaussian_splatting/tests/"

        # Positive control FIRST. Keys are repo-relative paths, so asserting
        # `assertNotIn("test_node_bootstrap.h", found)` passes no matter what --
        # a bare basename is never a key. That is exactly how this assertion was
        # vacuous until the keys changed shape underneath it, so the test now
        # proves it can see a file before claiming it cannot see these two.
        self.assertIn(
            prefix + "test_painterly_pipeline.h",
            found,
            "positive control missing: this test can no longer detect ANY file, "
            "so its negative assertions below prove nothing",
        )
        for name in ("test_shadow_instance_subset.h", "test_node_bootstrap.h"):
            self.assertNotIn(
                prefix + name, found, f"{name} unexpectedly entered the inventory"
            )

    def test_macro_header_is_excluded_from_the_inventory(self) -> None:
        """The definitions are not call sites; counting them would make the
        inventory move whenever the macro is edited."""
        self.assertNotIn("test_macros.h", [p.name for p in guard.test_sources()])


class RatchetTests(GuardTestCase):
    def test_clean_tree_passes_with_summary_line(self) -> None:
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        code, out = self.run_guard()
        self.assertEqual(0, code, out)
        self.assertIn("[env-skip] PASS", out)
        self.assertIn("1 baselined site(s) across 1 file(s), 0 new, 0 stale.", out)

    def test_new_unbaselined_site_fails(self) -> None:
        """A7 / A11(c)."""
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        self.write_source(
            "test_alpha.h",
            'REQUIRE_GPU_DEVICE();\nMESSAGE("Skipping - synthetic");\nreturn;\n',
        )
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("1 NEW environment-skip site(s)", out)
        self.assertIn("Skipping - synthetic", out)

    def test_new_site_of_an_already_baselined_shape_fails(self) -> None:
        """The multiset, not the set: a SECOND identical skip is still new."""
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\nREQUIRE_GPU_DEVICE();\n")
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("1 NEW environment-skip site(s)", out)
        self.assertIn("line 2:", out)

    def test_removed_site_fails_as_stale(self) -> None:
        """A7 / A11(d): fixing a site must tighten the ratchet, not leave slack."""
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\nREQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("1 baselined site(s) no longer found", out)
        self.assertIn("the ratchet must tighten", out)

    def test_deleted_baseline_entry_fails(self) -> None:
        """A11(d): dropping an entry must not silently pass."""
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        document["files"].pop(self.key("test_alpha.h"))
        self.baseline_path.write_text(json.dumps(document, indent=2), encoding="utf-8")
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("NEW environment-skip site(s)", out)

    def test_moving_an_identical_skip_to_another_case_is_detected(self) -> None:
        """(m) The site-vs-shape defect, as a property.

        Before the re-key, every `REQUIRE_GPU_DEVICE()` in a file shared one key
        (`macro|REQUIRE_GPU_DEVICE`). Deleting one from case A and adding an
        identical one to case B therefore left the multiset unchanged: the
        shrink-only inventory silently gained skipped coverage in a case that
        previously had none, with no baseline edit and no report. Several
        committed files already carry duplicate shapes, so it was reachable.
        """
        self.write_source(
            "test_alpha.h",
            'TEST_CASE("case A") {\n    REQUIRE_GPU_DEVICE();\n}\n'
            'TEST_CASE("case B") {\n    CHECK(true);\n}\n',
        )
        self.regenerate()
        code, out = self.run_guard()
        self.assertEqual(0, code, out)

        # Same shape, same file, same COUNT -- moved to a different case.
        self.write_source(
            "test_alpha.h",
            'TEST_CASE("case A") {\n    CHECK(true);\n}\n'
            'TEST_CASE("case B") {\n    REQUIRE_GPU_DEVICE();\n}\n',
        )
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("NEW environment-skip site(s)", out)
        self.assertIn("no longer found", out)

    def test_identical_shapes_in_different_cases_have_different_keys(self) -> None:
        """The mechanism behind the property above, pinned directly."""
        a = guard.fingerprint("macro", "REQUIRE_GPU_DEVICE", "case A")
        b = guard.fingerprint("macro", "REQUIRE_GPU_DEVICE", "case B")
        self.assertNotEqual(a, b)
        self.assertEqual(a, guard.fingerprint("macro", "REQUIRE_GPU_DEVICE", "case A"))

    def test_swap_is_detected(self) -> None:
        """A count-only baseline would license this: remove one site, add a
        different one, count unchanged."""
        self.write_source("test_alpha.h", 'MESSAGE("Skipping - reason A");\n')
        self.regenerate()
        self.write_source("test_alpha.h", 'MESSAGE("Skipping - reason B");\n')
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("NEW environment-skip site(s)", out)
        self.assertIn("no longer found", out)


class FailClosedTests(GuardTestCase):
    def test_missing_baseline_fails(self) -> None:
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("Baseline file missing", out)

    def test_unparseable_baseline_fails(self) -> None:
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.baseline_path.write_text("{not json", encoding="utf-8")
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("not readable JSON", out)

    def test_desynced_count_fails(self) -> None:
        """A count that disagrees with its site list is a corrupt baseline."""
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        document["files"][self.key("test_alpha.h")]["count"] = 99
        self.baseline_path.write_text(json.dumps(document, indent=2), encoding="utf-8")
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("does not equal len(sites)", out)

    def test_entry_without_owner_fails(self) -> None:
        """A10: every tolerated entry carries an owner and a tracking issue."""
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        document["files"][self.key("test_alpha.h")].pop("owner")
        self.baseline_path.write_text(json.dumps(document, indent=2), encoding="utf-8")
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("missing a non-empty 'owner'", out)

    def test_empty_source_tree_fails(self) -> None:
        """A scan that finds no sources is broken, not clean."""
        (self.tests_dir / "test_macros.h").unlink()
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("no module test sources found", out)


class WriteBaselineTests(GuardTestCase):
    def test_writer_is_idempotent(self) -> None:
        """A6: two runs on an unchanged tree are byte-identical."""
        self.write_source("test_alpha.h", 'REQUIRE_GPU_DEVICE();\nMESSAGE("Skipping - x");\n')
        self.regenerate()
        first = self.baseline_path.read_bytes()
        self.regenerate()
        self.assertEqual(first, self.baseline_path.read_bytes())

    def test_writer_refuses_to_add_a_site(self) -> None:
        """The one edit this file exists to prevent: laundering a new skip into
        the baseline by regenerating it."""
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        self.write_source("test_alpha.h", 'REQUIRE_GPU_DEVICE();\nMESSAGE("Skipping - new");\n')
        code, out = self.run_guard(["--write-baseline"])
        self.assertEqual(1, code, out)
        self.assertIn("refuses to ADD environment-skip sites", out)

    def test_writer_records_removals(self) -> None:
        """The ratchet must be able to tighten without hand-editing."""
        self.write_source("test_alpha.h", 'REQUIRE_GPU_DEVICE();\nMESSAGE("Skipping - x");\n')
        self.regenerate()
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        self.assertEqual(1, document["files"][self.key("test_alpha.h")]["count"])
        code, out = self.run_guard()
        self.assertEqual(0, code, out)

    def test_deleting_the_baseline_does_not_launder_an_addition(self) -> None:
        """F2, the reproduced bypass: refuse -> rm baseline -> write -> pass.

        The no-additions check used to sit inside `if path.is_file()`, so
        deleting the baseline skipped it entirely and the new site landed with
        exit 0. The resulting diff looks like an ordinary one-line addition.
        """
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        self.write_source("test_alpha.h", 'REQUIRE_GPU_DEVICE();\nMESSAGE("Skipping - new");\n')

        code, out = self.run_guard(["--write-baseline"])
        self.assertEqual(1, code, out)
        self.assertIn("refuses to ADD", out)

        self.baseline_path.unlink()
        code, out = self.run_guard(["--write-baseline"])
        self.assertEqual(1, code, out)
        self.assertIn("NO path that creates it from the current tree", out)
        self.assertFalse(self.baseline_path.is_file(), "a bypassed write still created the file")

    def test_there_is_no_cli_flag_that_creates_a_baseline(self) -> None:
        """The blocker: version 2 of the F2 fix RELOCATED the bypass.

        `--bootstrap-baseline` wrote unconditionally, so the laundering sequence
        survived verbatim -- only the flag name changed, and its diff still
        looked like an ordinary shrink. Any future flag that writes a baseline
        from the current tree re-opens it, so the absence of one is the thing
        being asserted.
        """
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        self.write_source("test_alpha.h", 'REQUIRE_GPU_DEVICE();\nMESSAGE("Skipping - new");\n')
        self.baseline_path.unlink()

        for argv in (["--bootstrap-baseline"], ["--write-baseline"], []):
            with self.assertRaises(SystemExit) if argv == ["--bootstrap-baseline"] else contextlib.nullcontext():
                code, out = self.run_guard(argv)
                self.assertEqual(1, code, out)
            self.assertFalse(
                self.baseline_path.is_file(),
                f"{argv} created a baseline from the current tree",
            )

    def test_rename_rekeys_an_entry_without_adding(self) -> None:
        """F5: a renamed file must not be a deadlock between guard and writer."""
        self.write_source("test_alpha.h", 'REQUIRE_GPU_DEVICE();\nMESSAGE("Skipping - x");\n')
        self.regenerate()
        (self.tests_dir / "test_alpha.h").rename(self.tests_dir / "test_renamed.h")

        code, out = self.run_guard()
        self.assertEqual(1, code, out)

        code, out = self.run_guard(["--write-baseline"])
        self.assertEqual(1, code, out)
        self.assertIn("refuses to ADD", out)

        code, out = self.run_guard(["--write-baseline", "--rename", f"{self.key('test_alpha.h')}={self.key('test_renamed.h')}"])
        self.assertEqual(0, code, out)
        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        self.assertNotIn(self.key("test_alpha.h"), document["files"])
        self.assertEqual(2, document["files"][self.key("test_renamed.h")]["count"])
        code, out = self.run_guard()
        self.assertEqual(0, code, out)

    def test_rename_cannot_introduce_a_site(self) -> None:
        """Renaming re-keys fingerprints; it must never add one."""
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        (self.tests_dir / "test_alpha.h").rename(self.tests_dir / "test_renamed.h")
        self.write_source("test_renamed.h", 'REQUIRE_GPU_DEVICE();\nMESSAGE("Skipping - extra");\n')
        code, out = self.run_guard(["--write-baseline", "--rename", f"{self.key('test_alpha.h')}={self.key('test_renamed.h')}"])
        self.assertEqual(1, code, out)
        self.assertIn("refuses to ADD", out)

    def test_rename_of_an_unknown_key_is_rejected(self) -> None:
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        code, out = self.run_guard(["--write-baseline", "--rename", "nope.h=other.h"])
        self.assertEqual(1, code, out)
        self.assertIn("not in the baseline", out)

    def test_rename_cannot_transfer_credit_between_two_live_files(self) -> None:
        """The laundering primitive --rename used to be.

        Delete three sites from A, add three to an unrelated live B, then
        `--rename A=B`: the total cannot grow, so the inclusion check is happy,
        but the ATTRIBUTION is a lie and a same-fingerprint swap is invisible.
        A real rename means the source file is GONE.
        """
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\nREQUIRE_GPU_DEVICE();\n")
        self.write_source("test_beta.h", "REQUIRE_LOCAL_GPU_DEVICE();\n")
        self.regenerate()

        # A keeps existing but loses its sites; B gains two.
        self.write_source("test_alpha.h", "// emptied\n")
        self.write_source(
            "test_beta.h",
            "REQUIRE_LOCAL_GPU_DEVICE();\nREQUIRE_GPU_DEVICE();\nREQUIRE_GPU_DEVICE();\n",
        )
        code, out = self.run_guard(
            ["--write-baseline", "--rename", f"{self.key('test_alpha.h')}={self.key('test_beta.h')}"]
        )
        self.assertEqual(1, code, out)
        self.assertIn("still exists on disk", out)

    def test_rename_cannot_park_credit_on_an_invented_path(self) -> None:
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        (self.tests_dir / "test_alpha.h").unlink()
        code, out = self.run_guard(
            ["--write-baseline", "--rename", f"{self.key('test_alpha.h')}=totally/made/up.h"]
        )
        self.assertEqual(1, code, out)
        self.assertIn("not a scanned source file", out)

    def test_writer_preserves_runtime_lane_allowance(self) -> None:
        """The runtime allowance is measured from real lane output, not from the
        static scan; regenerating the static half must not drop it.

        The fixture used to be `{"Painterly": 3}` -- simultaneously the bare-int
        silencer shape the schema now rejects AND a lane that does not exist. It
        asserted that an invalid value must SURVIVE regeneration, i.e. it pinned
        the wrong thing in two ways at once.
        """
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        allowance = {"GaussianSplatting [Editor]": _valid_allowance_entry(1)}
        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        document["runtime_lane_allowance"] = allowance
        self.baseline_path.write_text(guard._serialize(document), encoding="utf-8")
        self.regenerate()
        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        self.assertEqual(allowance, document["runtime_lane_allowance"])

    def test_bare_integer_allowance_fails_the_guard(self) -> None:
        """Always-on validation: the allowance used to be copied through
        untouched and read only when a lane tripped."""
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        document["runtime_lane_allowance"] = {"GaussianSplatting [Editor]": 9999}
        self.baseline_path.write_text(guard._serialize(document), encoding="utf-8")
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("must be an object", out)

    def test_allowance_ratchet_rejects_a_raise_and_a_new_lane(self) -> None:
        """Shrink-only, checked against the previously committed document."""
        previous = {
            "runtime_lane_allowance": {"GaussianSplatting [Editor]": _valid_allowance_entry(1)}
        }
        raised = {
            "runtime_lane_allowance": {"GaussianSplatting [Editor]": _valid_allowance_entry(2)}
        }
        added = {
            "runtime_lane_allowance": {
                "GaussianSplatting [Editor]": _valid_allowance_entry(1),
                "GaussianSplatting [PLY]": _valid_allowance_entry(1),
            }
        }
        lowered = {
            "runtime_lane_allowance": {"GaussianSplatting [Editor]": _valid_allowance_entry(0)}
        }
        self.assertTrue(any("rose 1 -> 2" in f for f in guard.check_allowance(raised, previous)))
        self.assertTrue(any("is NEW" in f for f in guard.check_allowance(added, previous)))
        self.assertEqual([], guard.check_allowance(lowered, previous))
        self.assertEqual([], guard.check_allowance(previous, previous))


class MacroContractTests(GuardTestCase):
    def test_valid_header_passes_the_contract_check(self) -> None:
        self.assertEqual([], guard.check_macro_contract())

    def test_reverted_macro_body_fails(self) -> None:
        """A11(b): if a macro stops routing through GS_ENV_SKIP, the guard must go
        RED even though the static site count is unchanged."""
        self.write_macro_header(
            VALID_MACRO_HEADER.replace(
                'GS_ENV_SKIP("RenderingDevice unavailable");',
                'MESSAGE("Skipping test - RenderingDevice unavailable");',
            )
        )
        failures = guard.check_macro_contract()
        self.assertTrue(failures)
        self.assertTrue(
            any("REQUIRE_GPU_DEVICE does not route its skip through GS_ENV_SKIP" in f for f in failures),
            failures,
        )

    def test_retired_token_fails(self) -> None:
        """Changing the emitted token silently un-counts every environment skip,
        so the guard pins the token itself."""
        self.write_macro_header(VALID_MACRO_HEADER.replace('"GS_ENV_SKIP: "', '"SKIPPED: "'))
        failures = guard.check_macro_contract()
        self.assertTrue(any("no longer emits the literal token" in f for f in failures), failures)

    def test_missing_helper_fails(self) -> None:
        self.write_macro_header("// nothing here\n")
        failures = guard.check_macro_contract()
        self.assertTrue(any("GS_ENV_SKIP(reason) is not defined" in f for f in failures), failures)

    def test_macro_contract_failure_fails_the_whole_guard(self) -> None:
        """The contract check must be wired into main(), not merely available."""
        self.write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self.regenerate()
        self.write_macro_header(
            VALID_MACRO_HEADER.replace(
                'GS_ENV_SKIP("streaming unavailable");',
                'MESSAGE("Skipping test - streaming unavailable");',
            )
        )
        code, out = self.run_guard()
        self.assertEqual(1, code, out)
        self.assertIn("REQUIRE_STREAMING_CAPABLE does not route its skip through GS_ENV_SKIP", out)


class TwoCommitRatchetTests(unittest.TestCase):
    """PROPERTIES of the ratchet, exercised across two real commits.

    These are written as properties, not as assertions about a mechanism:

      * no commit can increase any `allowed` value or add a lane, BY ANY ROUTE,
        including a direct edit of the baseline file;
      * no commit can add a site to the inventory, BY ANY ROUTE, including a
        direct edit of the baseline file.

    The two-commit fixture is load-bearing. Every earlier test here used a
    single working tree, where the "committed reference" and the "current
    document" are necessarily the same thing -- so they passed against a guard
    that read its reference from HEAD and compared the change with itself. A
    single-commit test CANNOT distinguish HEAD from the review base, which is
    exactly why three rounds of tests missed this. The repo had already recorded
    the lesson in tests/ci/test_gpu_harness_deferred_contract.py: "The first
    version of this guard read the allowed backlog out of the manifest and
    compared the manifest against itself. That is not a ratchet."
    """

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name) / "repo"
        (self.repo / "modules" / "gaussian_splatting" / "tests").mkdir(parents=True)
        (self.repo / "tests" / "ci").mkdir(parents=True)
        (self.repo / "engine_tests").mkdir()

        self.tests_dir = self.repo / "modules" / "gaussian_splatting" / "tests"
        self.baseline_path = self.repo / "tests" / "ci" / "environment_skip_baseline.json"

        self._saved = (
            guard.ROOT,
            guard.MODULE_TESTS_DIR,
            guard.ENGINE_TESTS_DIR,
            guard.BASELINE_PATH,
        )
        guard.ROOT = self.repo
        guard.MODULE_TESTS_DIR = self.tests_dir
        guard.ENGINE_TESTS_DIR = self.repo / "engine_tests"
        guard.BASELINE_PATH = self.baseline_path
        self.addCleanup(self._restore)

        (self.tests_dir / "test_macros.h").write_text(VALID_MACRO_HEADER, encoding="utf-8")
        self._git("init", "-q", "-b", "main")
        self._git("config", "user.email", "t@example.com")
        self._git("config", "user.name", "t")

    def _restore(self) -> None:
        (
            guard.ROOT,
            guard.MODULE_TESTS_DIR,
            guard.ENGINE_TESTS_DIR,
            guard.BASELINE_PATH,
        ) = self._saved
        self._tmp.cleanup()

    def _git(self, *args: str) -> None:
        subprocess.run(
            ["git", *args], cwd=self.repo, check=True, capture_output=True, text=True
        )

    def _write_source(self, name: str, text: str) -> None:
        (self.tests_dir / name).write_text(text, encoding="utf-8")

    def _seed_and_commit(self) -> None:
        """Commit 1: the base. Baseline matches the tree exactly."""
        document = guard.build_baseline_document(guard.scan_fingerprints(), None)
        self.baseline_path.write_text(guard._serialize(document), encoding="utf-8")
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "base")

    def _commit_all(self, message: str) -> None:
        """Commit 2: the change under review. HEAD now IS the change."""
        self._git("add", "-A")
        self._git("commit", "-q", "-m", message)

    def _run(self, argv: list[str] | None = None) -> tuple[int, str]:
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = guard.main(argv if argv is not None else ["--base-ref", "main"])
        return code, buffer.getvalue()

    def _set_allowance(self, entry: dict | None) -> None:
        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        document["runtime_lane_allowance"] = (
            {} if entry is None else {"GaussianSplatting [Editor]": entry}
        )
        self.baseline_path.write_text(guard._serialize(document), encoding="utf-8")

    # -- property 1: the allowance may never grow -------------------------

    def test_raising_an_allowance_by_direct_edit_fails(self) -> None:
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        self._set_allowance(_valid_allowance_entry(1))
        self._commit_all("record a measured allowance")
        # `base` is the commit that CARRIES allowance=1: that is the immutable
        # reference the next commit must be judged against.
        self._git("branch", "-f", "base", "HEAD")

        self._set_allowance(_valid_allowance_entry(9999))
        self._commit_all("raise the allowance by hand")
        code, out = self._run(["--base-ref", "base"])
        self.assertEqual(1, code, out)
        self.assertIn("rose 1 -> 9999", out)

    def test_adding_an_allowance_lane_by_direct_edit_fails(self) -> None:
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        self._git("branch", "-f", "base", "HEAD")

        self._set_allowance(_valid_allowance_entry(3))
        self._commit_all("add a brand new allowance lane")
        code, out = self._run(["--base-ref", "base"])
        self.assertEqual(1, code, out)
        self.assertIn("is NEW", out)

    def test_lowering_an_allowance_is_allowed(self) -> None:
        """The ratchet must still turn the good way."""
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        self._set_allowance(_valid_allowance_entry(2))
        self._commit_all("measured allowance")
        self._git("branch", "-f", "base", "HEAD")

        self._set_allowance(_valid_allowance_entry(1))
        self._commit_all("tighten")
        code, out = self._run(["--base-ref", "base"])
        self.assertEqual(0, code, out)

    # -- property 2: the site inventory may never grow --------------------

    def test_adding_a_site_and_hand_editing_the_baseline_fails(self) -> None:
        """The hole P1-2 named: tool paths were closed, the text editor was not.

        Adding a skip AND its fingerprint in one commit makes `actual` and
        `allowed` agree, so the scan-vs-baseline check reports nothing. Only a
        base-relative comparison of the baseline FILE catches it.
        """
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        self._git("branch", "-f", "base", "HEAD")

        # A real new skip, plus a hand edit that "authorises" it.
        self._write_source(
            "test_alpha.h", 'REQUIRE_GPU_DEVICE();\nMESSAGE("Skipping - smuggled");\n'
        )
        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        key = "modules/gaussian_splatting/tests/test_alpha.h"
        entry = document["files"][key]
        entry["sites"] = sorted(entry["sites"] + [guard.fingerprint("message", "Skipping - smuggled")])
        entry["count"] = len(entry["sites"])
        self.baseline_path.write_text(guard._serialize(document), encoding="utf-8")
        self._commit_all("add a skip and authorise it by hand")

        # The scan-vs-baseline check is satisfied -- that is the whole point.
        code, out = self._run(["--base-ref", "base"])
        self.assertEqual(1, code, out)
        self.assertIn("the BASELINE FILE gained", out)

    def test_adding_a_whole_new_baselined_file_by_hand_fails(self) -> None:
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        self._git("branch", "-f", "base", "HEAD")

        self._write_source("test_beta.h", "REQUIRE_LOCAL_GPU_DEVICE();\n")
        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        document["files"]["modules/gaussian_splatting/tests/test_beta.h"] = {
            "owner": "x",
            "issue_url": "x",
            "conversion_slice": "x",
            "count": 1,
            "sites": ["macro|REQUIRE_LOCAL_GPU_DEVICE"],
        }
        self.baseline_path.write_text(guard._serialize(document), encoding="utf-8")
        self._commit_all("smuggle a whole file")

        code, out = self._run(["--base-ref", "base"])
        self.assertEqual(1, code, out)
        self.assertIn("the BASELINE FILE gained", out)

    def test_removing_a_site_and_shrinking_the_baseline_is_allowed(self) -> None:
        self._write_source(
            "test_alpha.h", 'REQUIRE_GPU_DEVICE();\nMESSAGE("Skipping - x");\n'
        )
        self._seed_and_commit()
        self._git("branch", "-f", "base", "HEAD")

        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        code, _ = self._run(["--write-baseline"])
        self.assertEqual(0, code)
        self._commit_all("fix one skip")
        code, out = self._run(["--base-ref", "base"])
        self.assertEqual(0, code, out)

    def test_rename_round_trip_through_the_documented_flow_passes(self) -> None:
        """(n) EVERY documented workflow must have a legal route.

        The round-3 base-relative fix broke this one: a moved file's
        fingerprints land under a new key, which against the base reads as pure
        growth, so `--write-baseline --rename` could no longer produce a
        baseline the guard accepts. A guard with no legal path for a legitimate
        operation gets bypassed, not obeyed -- so this test walks the documented
        flow end to end and asserts it GOES GREEN.
        """
        self._write_source(
            "test_alpha.h", 'REQUIRE_GPU_DEVICE();\nMESSAGE("Skipping - x");\n'
        )
        self._seed_and_commit()
        self._git("branch", "-f", "base", "HEAD")

        # The documented move: rename the file, then re-key via the writer.
        (self.tests_dir / "test_alpha.h").rename(self.tests_dir / "test_renamed.h")
        old_key = "modules/gaussian_splatting/tests/test_alpha.h"
        new_key = "modules/gaussian_splatting/tests/test_renamed.h"
        code, out = self._run(
            ["--write-baseline", "--rename", f"{old_key}={new_key}", "--base-ref", "base"]
        )
        self.assertEqual(0, code, out)
        self._commit_all("move a test file")

        code, out = self._run(["--base-ref", "base"])
        self.assertEqual(0, code, out)

        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        self.assertEqual([{"from": old_key, "to": new_key}], document["rename_ledger"])
        self.assertNotIn(old_key, document["files"])
        self.assertEqual(2, document["files"][new_key]["count"])

    def test_rename_onto_a_file_that_existed_at_the_base_is_rejected(self) -> None:
        """(B) A rename CREATES its destination.

        Deleting baselined file A and adding an identical skip to a PRE-EXISTING
        file B was accepted as a rename: the writer moved A's fingerprints onto
        B and the ledger replayed the same move, so both checks agreed -- while
        what happened was a deletion PLUS new skipped coverage in B. Requiring
        the destination to be absent at the review base is what distinguishes
        the two, and the base is now available to ask.
        """
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._write_source("test_beta.h", 'MESSAGE("Skipping - pre-existing");\n')
        self._seed_and_commit()
        self._git("branch", "-f", "base", "HEAD")

        # A is deleted; B (pre-existing) gains an identical skip to A's.
        (self.tests_dir / "test_alpha.h").unlink()
        self._write_source(
            "test_beta.h",
            'MESSAGE("Skipping - pre-existing");\nREQUIRE_GPU_DEVICE();\n',
        )
        old_key = "modules/gaussian_splatting/tests/test_alpha.h"
        new_key = "modules/gaussian_splatting/tests/test_beta.h"
        code, out = self._run(
            ["--write-baseline", "--rename", f"{old_key}={new_key}", "--base-ref", "base"]
        )
        self.assertEqual(1, code, out)
        self.assertIn("already existed at the review base", out)

    def test_a_hand_written_ledger_entry_cannot_move_live_credit(self) -> None:
        """The ledger must not become the bypass the writer refuses to be."""
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._write_source("test_beta.h", "REQUIRE_LOCAL_GPU_DEVICE();\n")
        self._seed_and_commit()
        self._git("branch", "-f", "base", "HEAD")

        document = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        document["rename_ledger"] = [
            {
                "from": "modules/gaussian_splatting/tests/test_alpha.h",
                "to": "modules/gaussian_splatting/tests/test_beta.h",
            }
        ]
        self.baseline_path.write_text(guard._serialize(document), encoding="utf-8")
        self._commit_all("hand-written ledger entry")

        code, out = self._run(["--base-ref", "base"])
        self.assertEqual(1, code, out)
        self.assertIn("still a live scanned source", out)

    # -- base resolution --------------------------------------------------

    def _make_remote_ref(self, name: str, commit: str = "HEAD") -> None:
        """Create refs/remotes/origin/<name> without needing a real remote."""
        out = subprocess.run(
            ["git", "rev-parse", commit],
            cwd=self.repo,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        self._git("update-ref", f"refs/remotes/origin/{name}", out)

    def test_branch_name_base_resolves_via_origin_when_no_local_branch(self) -> None:
        """GITHUB_BASE_REF is a BRANCH NAME, and under actions/checkout the base
        usually exists only as a remote-tracking ref.

        A bare `<ref>` lookup then fails and the guard fails closed on a
        legitimate PR -- correct-but-unusable, the same shape as the --rename
        path. A STACKED base is the realistic case: `gs/650-quarantine-ratchet`
        will not exist locally.
        """
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        self._make_remote_ref("gs/650-quarantine-ratchet")
        # Deliberately NO local branch of that name.
        probe = subprocess.run(
            ["git", "rev-parse", "--verify", "gs/650-quarantine-ratchet^{commit}"],
            cwd=self.repo,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(0, probe.returncode, "fixture is wrong: a local branch exists")

        sha, failures = guard.resolve_base_sha("gs/650-quarantine-ratchet")
        self.assertEqual([], failures)
        self.assertIsNotNone(sha)

        code, out = self._run(["--base-ref", "gs/650-quarantine-ratchet"])
        self.assertEqual(0, code, out)

    def test_stale_local_branch_loses_to_the_remote_tracking_ref(self) -> None:
        """The tightest available reference wins.

        A stale local branch yields an OLDER merge-base, and an older base is
        not merely imprecise: if the baseline did not exist there, the whole
        comparison degrades to ABSENT_AT_BASE and the ratchet stops constraining
        anything. Observed on the real worktree -- local `master` sat at
        b6b2d7258bf while origin/master was a3bb6925132.
        """
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        stale = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.repo,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        self._git("branch", "-f", "release", stale)

        # Move on, and point origin/release at the newer commit.
        self._write_source("test_beta.h", "REQUIRE_LOCAL_GPU_DEVICE();\n")
        code, _ = self._run(["--write-baseline"])
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "advance")
        self._make_remote_ref("release", "HEAD")
        fresh = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.repo,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

        sha, failures = guard.resolve_base_sha("release")
        self.assertEqual([], failures)
        self.assertEqual(fresh, sha, "resolution took the STALE local branch")
        self.assertNotEqual(stale, sha)

    def test_origin_prefixed_ref_is_not_double_prefixed(self) -> None:
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        self._make_remote_ref("main")
        sha, failures = guard.resolve_base_sha("origin/main")
        self.assertEqual([], failures)
        self.assertIsNotNone(sha)

    def test_an_explicit_but_unresolvable_base_does_not_fall_back_to_master(self) -> None:
        """Found while verifying the origin/ fix, and worse than the bug it sat next to.

        An explicit base from the environment was appended to the DEFAULT list
        rather than replacing it, so an unreachable stacked base fell through to
        origin/master and the run went GREEN -- graded against the wrong branch,
        which is exactly what forwarding the base was supposed to prevent.
        'The base you named is unreachable' and 'you named no base' must not
        share an outcome.
        """
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        self._git("branch", "-f", "master", "HEAD")

        sha, failures = guard.resolve_base_sha("gs/no-such-stacked-branch")
        self.assertIsNone(sha)
        self.assertTrue(failures)
        self.assertIn("an explicitly named base did not resolve", failures[0])

        with mock.patch.dict(
            os.environ, {"GITHUB_BASE_REF": "gs/no-such-stacked-branch"}, clear=False
        ):
            sha, failures = guard.resolve_base_sha(None)
        self.assertIsNone(sha, "an unresolvable env base silently fell back")
        self.assertTrue(failures)

    def test_no_base_named_at_all_still_uses_local_defaults(self) -> None:
        """Local ergonomics must survive the stricter explicit-base rule."""
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        self._git("branch", "-f", "master", "HEAD")
        with mock.patch.dict(os.environ, {}, clear=False):
            for name in guard.BASE_REF_ENV_VARS:
                os.environ.pop(name, None)
            sha, failures = guard.resolve_base_sha(None)
        self.assertEqual([], failures)
        self.assertIsNotNone(sha)

    def test_unresolvable_base_fails_closed(self) -> None:
        """'Cannot determine the base' must never encode as 'nothing changed'."""
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        code, out = self._run(["--base-ref", "no-such-ref-anywhere"])
        self.assertEqual(1, code, out)
        self.assertIn("cannot resolve the review base", out)
        self.assertIn("Refusing to fall back to HEAD", out)

    def test_head_is_not_accepted_as_its_own_reference(self) -> None:
        """The defect itself, as a property.

        With HEAD as the base, merge-base(HEAD, HEAD) is HEAD, so the reference
        IS the change. A raise must still be caught -- and it is, because the
        guard compares against the merge-base with an explicit base ref, so
        passing HEAD deliberately is the one case that degenerates. Asserting it
        keeps the degeneracy visible rather than latent.
        """
        self._write_source("test_alpha.h", "REQUIRE_GPU_DEVICE();\n")
        self._seed_and_commit()
        self._set_allowance(_valid_allowance_entry(1))
        self._commit_all("allowance")
        self._git("branch", "-f", "base", "HEAD")
        self._set_allowance(_valid_allowance_entry(9999))
        self._commit_all("raise")

        base_code, base_out = self._run(["--base-ref", "base"])
        head_code, _ = self._run(["--base-ref", "HEAD"])
        self.assertEqual(1, base_code, base_out)
        self.assertEqual(
            0,
            head_code,
            "comparing against HEAD should degenerate to comparing the change with "
            "itself -- if this ever fails, the reference resolution changed and this "
            "test's premise needs revisiting",
        )


class RealTreeTests(unittest.TestCase):
    """A handful of assertions against the committed tree, so a refactor that
    makes the scan silently stop matching cannot hide behind synthetic fixtures.
    """

    def test_committed_scan_equals_the_committed_baseline_exactly(self) -> None:
        """Discriminating version of an earlier VACUOUS assertion.

        The previous test asserted `len(sources) > 50` and `sites > 100`, which
        would pass against almost any half-broken scanner. This asserts the scan
        reproduces the committed baseline fingerprint-for-fingerprint -- the only
        statement that actually pins the guard to its own recorded number.
        """
        real = _load_guard()
        baseline, failures = real.load_baseline()
        self.assertEqual([], failures)
        self.assertEqual(baseline, real.scan_fingerprints())

    def test_fingerprint_digest_is_pinned_to_its_committed_values(self) -> None:
        """Every one of the 384 baseline keys comes from fingerprint().

        Changing the hash algorithm, or the key SHAPE, re-keys the whole ratchet
        at once: every baselined entry reports stale, every real site reports
        new, and the tempting "fix" is to regenerate -- which resets the ratchet
        to zero while looking like routine maintenance. `usedforsecurity=False`
        is safe precisely because it does not move these bytes; sha256 would, and
        so would dropping the enclosing-case component.

        These literals are values actually committed in
        environment_skip_baseline.json.
        """
        real = _load_guard()
        diagnostics_case = (
            "[Gaussian Diagnostics] Production metrics preserve GPU timing "
            "capture semantics without GPU"
        )
        self.assertEqual(
            "message|2fdc85c6d5|d6920be551",
            real.fingerprint(
                "message", "Skipping test - ProjectSettings unavailable", diagnostics_case
            ),
        )
        self.assertEqual(
            "macro|REQUIRE_GPU_DEVICE|337f4fcc2b",
            real.fingerprint(
                "macro",
                "REQUIRE_GPU_DEVICE",
                "[GaussianSplatting][AsyncReadback] Batched readback tolerates "
                "per-request failures",
            ),
        )

        # The whole point of the round-4 re-key: the SAME shape in a DIFFERENT
        # case must not collide, or a swap nets zero and the inventory grows
        # silently.
        other = real.fingerprint(
            "macro",
            "REQUIRE_GPU_DEVICE",
            "[GaussianSplatting][AsyncReadback] Batched readback rejects fully "
            "failed batches cleanly",
        )
        self.assertNotEqual("macro|REQUIRE_GPU_DEVICE|337f4fcc2b", other)

        # And the committed baseline must still be exactly what the scan produces.
        baseline, failures = real.load_baseline()
        self.assertEqual([], failures)
        self.assertEqual(baseline, real.scan_fingerprints())

    def test_committed_inventory_is_the_declared_number(self) -> None:
        real = _load_guard()
        found = real.scan_all()
        self.assertEqual(383, sum(len(v) for v in found.values()))
        self.assertEqual(27, len(found))

    def test_derived_macro_set_matches_the_headers_actual_macros(self) -> None:
        """The list is derived, not hand-written -- pin what it derives TO."""
        real = _load_guard()
        self.assertEqual(
            (
                "GS_ENV_SKIP",
                "REQUIRE_GPU_DEVICE",
                "REQUIRE_LOCAL_GPU_DEVICE",
                "REQUIRE_STREAMING_CAPABLE",
                "REQUIRE_WORKER_THREAD_POOL",
            ),
            real.skip_macro_names()[0],
        )
        self.assertEqual((), real.skip_macro_names()[1])

    def test_a_new_wrapper_macro_is_picked_up_automatically(self) -> None:
        """The F3 hazard: a skip wrapper added to the ONE excluded file.

        With a hand-written name list this yields zero sites for a real skip.
        Derivation is what closes it, so derivation is what is tested.
        """
        real = _load_guard()
        header = VALID_MACRO_HEADER + (
            "\n#define REQUIRE_BRAND_NEW_THING()      \\\n"
            '    if (!thing()) {                     \\\n'
            '        GS_ENV_SKIP("thing missing");   \\\n'
            "        return;                         \\\n"
            "    }\n"
        )
        names = real.skip_macro_names(header)
        self.assertIn("REQUIRE_BRAND_NEW_THING", names[0])
        sites = real.scan_source("TEST_CASE(\"a\") { REQUIRE_BRAND_NEW_THING(); }\n", names)
        self.assertEqual([(1, "macro", "REQUIRE_BRAND_NEW_THING", "a")], sites)

    def test_an_object_like_wrapper_macro_is_picked_up(self) -> None:
        """The reviewer's counter-example, which the first derivation missed.

        `#define GS_SKIP_NO_GPU do { GS_ENV_SKIP("no gpu"); return; } while (0)`
        was invisible twice over: `_DEFINE_RE` required a `(` after the name, so
        it never entered the derived set, and the call pattern ended in `\\(`, so
        a bare `GS_SKIP_NO_GPU;` would not have matched even if it had. The
        result was `0 new, 0 stale` for real skips.
        """
        real = _load_guard()
        header = VALID_MACRO_HEADER + (
            '\n#define GS_SKIP_NO_GPU do { GS_ENV_SKIP("no gpu"); return; } while (0)\n'
        )
        function_like, object_like = real.skip_macro_names(header)
        self.assertIn("GS_SKIP_NO_GPU", object_like)
        self.assertNotIn("GS_SKIP_NO_GPU", function_like)
        sites = real.scan_source(
            'TEST_CASE("a") {\n    GS_SKIP_NO_GPU;\n    CHECK(true);\n}\n',
            (function_like, object_like),
        )
        self.assertEqual([(2, "macro", "GS_SKIP_NO_GPU", "a")], sites)

    def test_a_delegating_wrapper_macro_is_derived_transitively(self) -> None:
        """(C) The third variant of one root cause.

        `#define REQUIRE_RENDERER() REQUIRE_GPU_DEVICE()` contains neither the
        token nor skip prose, so a seed-only pass never sees it -- and because
        test_macros.h is the ONE file excluded from the site scan, both the
        delegation and every REQUIRE_RENDERER() call site were invisible, so new
        environment skips passed the shrink-only guard.

        Derivation is iterated to a FIXED POINT, not one level: a two-hop chain
        is no less real than a one-hop one, and "we handled the case we thought
        of" is precisely how this root cause has now produced three blind spots
        (object-like macros, delegating macros, and whatever is next).
        """
        real = _load_guard()

        one_hop = VALID_MACRO_HEADER + "\n#define REQUIRE_RENDERER() REQUIRE_GPU_DEVICE()\n"
        self.assertIn("REQUIRE_RENDERER", real.skip_macro_names(one_hop)[0])

        two_hop = one_hop + "#define REQUIRE_RENDERER_2() REQUIRE_RENDERER()\n"
        derived = real.skip_macro_names(two_hop)[0]
        self.assertIn("REQUIRE_RENDERER", derived)
        self.assertIn("REQUIRE_RENDERER_2", derived)

        # ... and an object-like macro three hops from the token.
        three_hop = two_hop + "#define GS_SKIP_ALL do { REQUIRE_RENDERER_2(); } while (0)\n"
        self.assertIn("GS_SKIP_ALL", real.skip_macro_names(three_hop)[1])

        # The call site of a delegated wrapper must actually be COUNTED, not
        # merely named in the derived set.
        sites = real.scan_source(
            'TEST_CASE("a") {\n    REQUIRE_RENDERER();\n}\n', real.skip_macro_names(one_hop)
        )
        self.assertEqual([(2, "macro", "REQUIRE_RENDERER", "a")], sites)

    def test_delegation_to_a_failing_macro_is_not_a_skip(self) -> None:
        """Transitivity must not drag in REQUIRE_RENDERING_DEVICE_SINGLETON,
        which FAILs rather than skipping."""
        real = _load_guard()
        header = VALID_MACRO_HEADER + (
            "\n#define REQUIRE_SINGLETON_ONLY() REQUIRE_RENDERING_DEVICE_SINGLETON()\n"
        )
        function_like, object_like = real.skip_macro_names(header)
        self.assertNotIn("REQUIRE_SINGLETON_ONLY", function_like + object_like)

    def test_a_regressed_wrapper_macro_is_also_picked_up(self) -> None:
        """Derivation must survive the wrapper reverting to free-form prose."""
        real = _load_guard()
        header = VALID_MACRO_HEADER + (
            "\n#define REQUIRE_LEGACY_THING()                     \\\n"
            '    if (!thing()) {                                  \\\n'
            '        MESSAGE("Skipping test - thing missing");    \\\n'
            "        return;                                      \\\n"
            "    }\n"
        )
        self.assertIn("REQUIRE_LEGACY_THING", real.skip_macro_names(header)[0])

    def test_failing_macro_is_never_derived_as_a_skip(self) -> None:
        real = _load_guard()
        header = VALID_MACRO_HEADER + (
            "\n#define REQUIRE_RENDERING_DEVICE_SINGLETON()          \\\n"
            '    if (!rd()) { FAIL("no singleton, skipping is wrong"); return; }\n'
        )
        derived = real.skip_macro_names(header)
        self.assertNotIn("REQUIRE_RENDERING_DEVICE_SINGLETON", derived[0] + derived[1])

    def test_committed_macros_satisfy_the_contract(self) -> None:
        real = _load_guard()
        self.assertEqual([], real.check_macro_contract())


if __name__ == "__main__":
    unittest.main(verbosity=2)
