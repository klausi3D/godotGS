#!/usr/bin/env python3
"""Fail a "painterly" GDScript test that cannot tell painterly from the baseline.

WHY THIS GUARD EXISTS (#997, #851)
----------------------------------
Every painterly test this repository shipped was vacuous. They enabled
painterly, asserted ``visible_splats > 0`` -- which is identical on the
baseline raster -- and passed. None of them supplied a ``PainterlyMaterial``,
so ``RasterStage`` rejected the painterly path with
``RenderFallbackReason::PAINTERLY_MATERIAL_UNAVAILABLE``
(``modules/gaussian_splatting/renderer/render_pipeline_stages.cpp:2561-2566``)
and rendered the baseline instead. Measured on a real scan at ``b915afc51c5``:
a painterly-enabled frame without a material was *bit-identical* to a
painterly-disabled frame (0 differing pixels of 518,400). No painterly defect
could have turned any of those tests red.

THE CONTRACT
------------
A GDScript test that turns painterly ON must **reference** the probe that shows
whether the painterly path actually ran.

Declared limit, stated because it is the difference between what this guard
checks and what a reader might assume: this is a REFERENCE test, not an
assertion test. A file that copies ``stage_raster_painterly`` into a metrics
dictionary, or merely names it in a comment, satisfies the rule. Statically
distinguishing "reads the probe and fails on it" from "mentions the probe" is
not something a regex can do; what this guard buys is that a painterly test
cannot be written in total ignorance of the path it is running, and that adding
one silently is impossible. The assertion itself is reviewed by humans.

The only probe that survives render-cache reuse is
``stage_raster_painterly`` (``StageMetrics::raster.painterly_active``, published
at ``renderer/render_diagnostics_orchestrator.cpp:520``); the ``raster_path``
string reads ``"compute"`` on the baseline and ``"cached"`` whenever the cache
serves the frame, so a test asserting that string fails on correct frames.

So: every discovered file either references ``stage_raster_painterly``, or is
named in ``ALLOWLIST`` with a reason.

FAIL-CLOSED RULES
-----------------
Each of these is a failure, not a warning, because each one is a way for this
guard to look green while checking nothing:

1. Discovery found no painterly-enabling file at all. The guard's own matcher
   has then broken (a renamed setter, a moved directory), and "nothing to
   check" must never read the same as "checked and clean".
2. A discovered file neither asserts the path nor is allowlisted.
3. An allowlist entry names a file that does not exist.
4. An allowlist entry names a file that no longer enables painterly.
5. An allowlist entry names a file that DOES assert the path. The list is
   shrink-only: once a test is repaired, its exemption has to go.
6. A search root does not exist.

DECLARED SCOPE LIMIT
--------------------
GDScript only. C++ doctest cases under ``modules/gaussian_splatting/tests/``
also call ``set_painterly_enabled(true)``, but they assert host-side state
plumbing (which node owns the renderer-wide flag) and run in lanes with no
RenderingDevice, so there is no painterly frame for them to observe. That is a
real blind spot, stated rather than silently designed around: a C++ test that
starts making claims about painterly *pixels* is not covered here.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[2]

#: Directories scanned for GDScript tests. A missing root fails the run (rule 6)
#: rather than quietly shrinking the search.
SEARCH_ROOTS: Tuple[str, ...] = (
    "tests",
    "modules/gaussian_splatting/tests",
    "scripts/tools",
)

#: Turning painterly on. Any of these makes a file painterly-behavioural.
#: The dynamic ``call("set_enable_painterly", true)`` form is listed explicitly.
#: It is how tests/examples/godot/test_project/scenes/painterly_test.gd writes
#: it, and a first cut of this guard that only matched the direct call silently
#: skipped that file -- the single most vacuous painterly test in the repo.
ENABLE_PATTERNS: Tuple[re.Pattern, ...] = (
    re.compile(r"set_enable_painterly\s*\(\s*true\s*\)"),
    re.compile(r"set_painterly_enabled\s*\(\s*true\s*\)"),
    re.compile(r"""\.\s*call\s*\(\s*["'](?:set_enable_painterly|set_painterly_enabled)["']\s*,\s*true\s*\)"""),
    re.compile(r"""["']painterly/enabled["']\s*,\s*true"""),
    re.compile(r"\bpainterly/enabled\s*=\s*true\b"),
    re.compile(r"\benable_painterly\s*=\s*true\b"),
)

#: The probe that distinguishes a painterly frame from a baseline frame.
PATH_ASSERTION_PATTERN = re.compile(r"stage_raster_painterly")

#: Repo-relative POSIX path -> why this test is allowed to enable painterly
#: without observing the painterly path. Shrink-only: rule 5 fails the run if an
#: entry here starts asserting the path, so a repaired test cannot keep its
#: exemption.
ALLOWLIST: Dict[str, str] = {
    "tests/test_gaussian_splat_visibility.gd": (
        "Toggle-safety regression sample, not a painterly render test: it asserts only "
        "that splats stay visible across a painterly on/off/on cycle and makes no claim "
        "about the painterly image. It runs with no PainterlyMaterial by design, because "
        "the regression it guards (#0 splats after a toggle) is on the baseline fallback."
    ),
    "tests/examples/godot/test_project/scenes/painterly_test.gd": (
        "API-surface smoke check invoked with --headless by "
        "modules/gaussian_splatting/tests/PAINTERLY_AUDIT_RUNBOOK.md. It asserts the thing "
        "that was actually broken and is checkable without a GPU -- that 'painterly/material' "
        "exists and round-trips -- and states in its own output that it does NOT verify the "
        "render path. It builds no camera or viewport, and the scene director refuses to "
        "create a renderer without a RenderingDevice, so a stage_raster_painterly assertion "
        "here would be permanently skipped or a false failure. The pixel proof is "
        "tests/runtime/test_painterly_material_render.gd on the GPU lanes."
    ),
    "tests/examples/godot/test_toggle_painterly.gd": (
        "Documentation sample of the node API, listed in tests/ci/validate_automation.py. "
        "It prints visibility counts and asserts nothing about the rendered frame."
    ),
}


def _iter_gd_files() -> List[Path]:
    files: List[Path] = []
    for root_name in SEARCH_ROOTS:
        root = ROOT / root_name
        if not root.is_dir():
            raise FileNotFoundError(
                f"search root '{root_name}' does not exist; the guard would have "
                f"scanned less than it claims"
            )
        files.extend(sorted(p for p in root.rglob("*.gd") if p.is_file()))
    # A file can sit under two roots only if one nests in the other; dedupe on
    # the resolved path so it is judged once.
    unique: Dict[Path, None] = {}
    for path in files:
        unique.setdefault(path.resolve(), None)
    return list(unique)


def _enables_painterly(text: str) -> bool:
    return any(pattern.search(text) for pattern in ENABLE_PATTERNS)


def _asserts_path(text: str) -> bool:
    return bool(PATH_ASSERTION_PATTERN.search(text))


def _rel(path: Path) -> str:
    return path.resolve().relative_to(ROOT).as_posix()


def check(root: Path = ROOT) -> Tuple[List[str], Dict[str, bool]]:
    """Return (failures, {relative_path: asserts_the_painterly_path})."""
    failures: List[str] = []
    enabling: Dict[str, bool] = {}

    for path in _iter_gd_files():
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:  # pragma: no cover - unreadable file is a real failure
            failures.append(f"{_rel(path)}: could not be read ({exc})")
            continue
        if not _enables_painterly(text):
            continue
        rel = _rel(path)
        enabling[rel] = _asserts_path(text)

    # Rule 1: discovery must find something.
    if not enabling:
        failures.append(
            "no GDScript file under "
            + ", ".join(SEARCH_ROOTS)
            + " was found enabling painterly. Either the enable-pattern list is stale "
            "(a renamed setter) or the search roots moved. A guard that finds nothing "
            "to check has not checked anything."
        )

    # Rule 2: an unexplained vacuous painterly test.
    for rel, asserts in sorted(enabling.items()):
        if asserts or rel in ALLOWLIST:
            continue
        failures.append(
            f"{rel}: enables painterly but never reads 'stage_raster_painterly', so it "
            f"cannot tell a painterly frame from the baseline fallback it would silently "
            f"get without a PainterlyMaterial (#997). Assert the path, or add an entry to "
            f"ALLOWLIST in {Path(__file__).name} saying why this test makes no claim about "
            f"the painterly image."
        )

    # Rules 3-5: the allowlist must stay honest and shrink.
    for rel, reason in sorted(ALLOWLIST.items()):
        if not reason.strip():
            failures.append(f"ALLOWLIST['{rel}'] has an empty reason.")
        target = root / rel
        if not target.is_file():
            failures.append(
                f"ALLOWLIST['{rel}'] names a file that does not exist. Remove the stale "
                f"entry; a stale exemption silently widens the guard's blind spot."
            )
            continue
        if rel not in enabling:
            failures.append(
                f"ALLOWLIST['{rel}'] names a file that no longer enables painterly. "
                f"Remove the entry."
            )
            continue
        if enabling[rel]:
            failures.append(
                f"ALLOWLIST['{rel}'] now asserts 'stage_raster_painterly' and no longer "
                f"needs an exemption. Remove the entry -- this list is shrink-only."
            )

    return failures, enabling


def _self_test() -> int:
    """Mutation-check the guard: every rule must be shown to fire."""
    cases: List[Tuple[str, bool]] = []

    with tempfile.TemporaryDirectory() as tmp:
        fake = Path(tmp)
        (fake / "tests").mkdir()

        def run_against(files: Dict[str, str], allowlist: Dict[str, str]):
            for name, body in files.items():
                target = fake / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(body, encoding="utf-8")
            global ROOT, SEARCH_ROOTS, ALLOWLIST
            saved = (ROOT, SEARCH_ROOTS, ALLOWLIST)
            ROOT, SEARCH_ROOTS, ALLOWLIST = fake, ("tests",), allowlist
            try:
                return check(fake)[0]
            finally:
                ROOT, SEARCH_ROOTS, ALLOWLIST = saved
                for name in files:
                    (fake / name).unlink()

        good = "func f():\n\tnode.set_enable_painterly(true)\n\tassert(stats.stage_raster_painterly)\n"
        vacuous = "func f():\n\tnode.set_enable_painterly(true)\n\tassert(visible > 0)\n"
        neutral = "func f():\n\tpass\n"

        cases.append((
            "rule 1: no painterly file discovered",
            bool(run_against({"tests/a.gd": neutral}, {})),
        ))
        cases.append((
            "rule 2: vacuous painterly test flagged",
            any("never reads" in f for f in run_against({"tests/a.gd": vacuous}, {})),
        ))
        cases.append((
            "rule 2 negative: asserting test accepted",
            not run_against({"tests/a.gd": good}, {}),
        ))
        cases.append((
            "rule 3: allowlist entry for a missing file",
            any("does not exist" in f for f in run_against({"tests/a.gd": vacuous}, {
                "tests/a.gd": "ok", "tests/gone.gd": "stale"})),
        ))
        cases.append((
            "rule 4: allowlist entry that no longer enables painterly",
            any("no longer enables" in f for f in run_against(
                {"tests/a.gd": vacuous, "tests/b.gd": neutral},
                {"tests/a.gd": "ok", "tests/b.gd": "stale"})),
        ))
        cases.append((
            "rule 5: allowlist entry that now asserts the path",
            any("shrink-only" in f for f in run_against(
                {"tests/a.gd": good, "tests/b.gd": vacuous},
                {"tests/a.gd": "redundant", "tests/b.gd": "ok"})),
        ))
        cases.append((
            "rule 6: missing search root",
            _missing_root_fires(fake),
        ))
        cases.append((
            "enable-pattern: painterly/enabled = true in a .tscn-style assignment",
            any("never reads" in f for f in run_against(
                {"tests/a.gd": 'func f():\n\tnode.set("painterly/enabled", true)\n'}, {})),
        ))
        cases.append((
            'enable-pattern: dynamic call("set_enable_painterly", true)',
            any("never reads" in f for f in run_against(
                {"tests/a.gd": 'func f():\n\tnode.call("set_enable_painterly", true)\n'}, {})),
        ))

    failed = [name for name, ok in cases if not ok]
    for name, ok in cases:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print(f"self-test: {len(cases) - len(failed)}/{len(cases)} mutations flagged")
    if failed:
        print("check_painterly_test_non_vacuity: guard failed self-test", file=sys.stderr)
        return 1
    return 0


def _missing_root_fires(fake: Path) -> bool:
    global ROOT, SEARCH_ROOTS
    saved = (ROOT, SEARCH_ROOTS)
    ROOT, SEARCH_ROOTS = fake, ("tests", "does/not/exist")
    try:
        check(fake)
    except FileNotFoundError:
        return True
    finally:
        ROOT, SEARCH_ROOTS = saved
    return False


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="mutation-check the guard itself and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    try:
        failures, scanned = check()
    except FileNotFoundError as exc:
        print(f"check_painterly_test_non_vacuity: guard failed: {exc}", file=sys.stderr)
        return 1

    print(f"check_painterly_test_non_vacuity: {len(scanned)} GDScript file(s) enable painterly")
    # Label from the value the guard actually computed, never from allowlist
    # membership: in the one run where rule 2 fires, an allowlist-derived label
    # would print "asserts path" for the very file stderr is reporting as vacuous.
    for rel in sorted(scanned):
        if scanned[rel]:
            mark = "asserts path"
        elif rel in ALLOWLIST:
            mark = "allowlisted, does NOT assert path"
        else:
            mark = "VACUOUS - neither asserts nor allowlisted"
        print(f"  - {rel} ({mark})")

    if failures:
        print("check_painterly_test_non_vacuity: guard failed", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check_painterly_test_non_vacuity: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
