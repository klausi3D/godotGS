#!/usr/bin/env python3
"""Export smoke test: build a game with the GS export template and run it (#825).

One scenario, not a matrix. It exports `tests/examples/godot/test_project`
against a locally built `target=template_release` binary and then *runs* the
exported game, asserting that Gaussian Splatting actually renders in it.

Why this exists
---------------
This fork publishes no export templates, and the step that makes an export work
-- pointing `custom_template/release` at a GS-enabled template -- is easy to
miss. When it is missed the export still succeeds and produces a binary with no
gaussian_splatting module in it, which silently renders nothing. This test is
the executable form of that check.

Why it is not `--headless`
--------------------------
`main.cpp` maps `--headless` to the dummy rendering driver, so a headless run
has no RenderingDevice and any "it rendered" assertion is vacuous. The exported
binary is therefore launched with the same non-headless mode arguments the rest
of the runtime harness uses (`run_runtime_validation._resolve_mode_args`,
added for #382). Only the editor-side `--import` / `--export-release` steps run
headless, because they render nothing.

Usage
-----
    python tests/runtime/run_export_smoke.py \
        --editor-binary bin/godot.windows.editor.x86_64.exe \
        --template bin/godot.windows.template_release.x86_64.exe

Both paths are auto-discovered from `bin/` and can also be supplied via the
`GODOT_EDITOR_BINARY` / `GODOT_EXPORT_TEMPLATE` environment variables. When a
prerequisite is missing the run reports `[EXPORT_SMOKE_SKIP]` and exits 0
unless `--require-binaries` is passed.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Optional, Sequence

ROOT = Path(__file__).resolve().parents[2]
RUNTIME_DIR = ROOT / "tests" / "runtime"
PROJECT_DIR = ROOT / "tests" / "examples" / "godot" / "test_project"
PRESET_TEMPLATE = RUNTIME_DIR / "export_smoke_preset.cfg.in"
PRESET_NAME = "Export Smoke"
PROBE_RES_PATH = "res://tests/export_smoke_probe.gd"
PROBE_FILE = PROJECT_DIR / "tests" / "export_smoke_probe.gd"
FIXTURE_FILE = PROJECT_DIR / "tests" / "fixtures" / "synthetic_cube.ply"
ASSET_PREP_SCRIPT = RUNTIME_DIR / "prepare_synthetic_assets.py"

SKIP_MARKER = "[EXPORT_SMOKE_SKIP]"
FAIL_MARKER = "[EXPORT_SMOKE_FAIL]"
PASS_MARKER = "[EXPORT_SMOKE_PASS]"
METRICS_MARKER = "[EXPORT_SMOKE_METRICS]"

# Byte needles that must be present in a GS-enabled export template. A stock
# Godot template contains neither; that is exactly the silent failure this test
# is built to catch (issue #825: GrandmasHouse exported on 2026-01-21 with
# custom_template/release="" and its .exe carries zero GS symbols).
REQUIRED_TEMPLATE_SYMBOLS: Sequence[bytes] = (b"GaussianSplat", b"gaussian_splatting")

EXIT_PASS = 0
EXIT_FAIL = 1

# Mirrors export_smoke_probe.gd. Exit code 4 means: the exported binary loaded
# the module, brought up a real RenderingDevice, and drove a successful GPU
# raster pass over the fixture -- but the window read back blank. That is what a
# session without a composited desktop looks like (the in-repo
# `Canonical Node Asset Render` proof fails the same way there). It is still a
# failure by default; `--allow-blank-viewport` downgrades ONLY this one code.
PROBE_EXIT_NO_VISUAL_EVIDENCE = 4

sys.path.insert(0, str(RUNTIME_DIR))
from run_runtime_validation import _resolve_mode_args  # noqa: E402


def _skip(reason: str, require: bool) -> int:
    if require:
        print(f"{FAIL_MARKER} {reason} (--require-binaries was passed)")
        return EXIT_FAIL
    print(f"{SKIP_MARKER} {reason}")
    return EXIT_PASS


def _fail(reason: str) -> int:
    print(f"{FAIL_MARKER} {reason}")
    return EXIT_FAIL


def _discover(candidates: Sequence[str]) -> Optional[Path]:
    for pattern in candidates:
        matches = sorted(
            p for p in (ROOT / "bin").glob(pattern) if p.is_file() and ".console." not in p.name
        )
        if matches:
            return matches[0]
    return None


def _resolve_editor(explicit: Optional[str]) -> Optional[Path]:
    raw = explicit or os.environ.get("GODOT_EDITOR_BINARY") or os.environ.get("GODOT_BINARY")
    if raw:
        candidate = Path(raw)
        return candidate if candidate.is_file() else None
    return _discover(
        (
            "godot.windows.editor.x86_64.exe",
            "godot.windows.editor*.x86_64.exe",
            "godot.linuxbsd.editor*.x86_64",
        )
    )


def _resolve_template(explicit: Optional[str]) -> Optional[Path]:
    raw = explicit or os.environ.get("GODOT_EXPORT_TEMPLATE")
    if raw:
        candidate = Path(raw)
        return candidate if candidate.is_file() else None
    return _discover(
        (
            "godot.windows.template_release.x86_64.exe",
            "godot.windows.template_release*.x86_64.exe",
            "godot.linuxbsd.template_release*.x86_64",
        )
    )


def _run(command: List[str], *, cwd: Path, timeout: int, label: str) -> subprocess.CompletedProcess:
    print(f"[export-smoke] {label}: {' '.join(command)}")
    try:
        return subprocess.run(
            command,
            cwd=str(cwd),
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout if isinstance(exc.stdout, str) else (exc.stdout or b"").decode("utf-8", "replace")
        stderr = exc.stderr if isinstance(exc.stderr, str) else (exc.stderr or b"").decode("utf-8", "replace")
        return subprocess.CompletedProcess(command, 124, stdout, stderr)


def _tail(text: str, lines: int = 30) -> str:
    kept = [line.rstrip() for line in text.splitlines() if line.strip()][-lines:]
    return "\n".join(kept)


def _template_has_gs_symbols(binary: Path) -> List[str]:
    """Return the required needles that are absent from `binary`.

    Read in chunks with an overlap so a needle straddling a chunk boundary is
    still found -- an unbounded read of a ~70 MB (Windows) or ~120 MB (Linux)
    binary is avoidable, and a false "missing" here would fail the run for the
    wrong reason.
    """
    longest = max(len(needle) for needle in REQUIRED_TEMPLATE_SYMBOLS)
    remaining = set(REQUIRED_TEMPLATE_SYMBOLS)
    chunk_size = 8 * 1024 * 1024
    with binary.open("rb") as handle:
        carry = b""
        while remaining:
            chunk = handle.read(chunk_size)
            if not chunk:
                break
            window = carry + chunk
            for needle in list(remaining):
                if needle in window:
                    remaining.discard(needle)
            carry = window[-(longest - 1) :] if longest > 1 else b""
    return [needle.decode("ascii") for needle in REQUIRED_TEMPLATE_SYMBOLS if needle in remaining]


def _write_preset(template_binary: Path) -> Path:
    preset_text = PRESET_TEMPLATE.read_text(encoding="utf-8")
    preset_text = preset_text.replace("@PRESET_NAME@", PRESET_NAME)
    # Godot stores/reads this as an absolute path with forward slashes.
    preset_text = preset_text.replace(
        "@CUSTOM_TEMPLATE_RELEASE@", template_binary.resolve().as_posix()
    )
    target = PROJECT_DIR / "export_presets.cfg"
    target.write_text(preset_text, encoding="utf-8")
    return target


def _parse_metrics(output: str) -> Optional[dict]:
    for line in reversed(output.splitlines()):
        marker_at = line.find(METRICS_MARKER)
        if marker_at == -1:
            continue
        payload = line[marker_at + len(METRICS_MARKER) :].strip()
        try:
            parsed = json.loads(payload)
        except json.JSONDecodeError:
            return None
        return parsed if isinstance(parsed, dict) else None
    return None


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--editor-binary", default=None, help="Godot editor binary used to import and export.")
    parser.add_argument("--template", default=None, help="target=template_release binary to export against.")
    parser.add_argument("--output-dir", default=None, help="Directory for the exported game (default: a temp dir).")
    parser.add_argument("--import-timeout", type=int, default=1800)
    parser.add_argument("--export-timeout", type=int, default=1800)
    parser.add_argument("--run-timeout", type=int, default=300)
    parser.add_argument("--skip-import", action="store_true", help="Reuse the project's existing .godot import cache.")
    parser.add_argument("--keep", action="store_true", help="Keep the exported artifacts and generated preset.")
    parser.add_argument(
        "--allow-blank-viewport",
        action="store_true",
        help=(
            "Accept a run where the module, the RenderingDevice and a successful GPU raster pass "
            "were all proven but the window read back blank (no composited desktop session). "
            "Every other failure mode still fails."
        ),
    )
    parser.add_argument(
        "--require-binaries",
        action="store_true",
        help="Treat missing editor/template/platform prerequisites as a failure instead of a skip.",
    )
    args = parser.parse_args(argv)

    if platform.system() != "Windows":
        return _skip(
            "Export smoke test currently covers the Windows Desktop preset only; "
            f"host is {platform.system()}.",
            args.require_binaries,
        )

    editor = _resolve_editor(args.editor_binary)
    if editor is None:
        return _skip("No Godot editor binary found (set --editor-binary or GODOT_EDITOR_BINARY).", args.require_binaries)
    template = _resolve_template(args.template)
    if template is None:
        return _skip(
            "No target=template_release binary found (set --template or GODOT_EXPORT_TEMPLATE).",
            args.require_binaries,
        )
    if not PROBE_FILE.is_file():
        return _fail(f"Export smoke probe missing: {PROBE_FILE}")

    print(f"[export-smoke] editor   = {editor}")
    print(f"[export-smoke] template = {template}")

    # Guard the template itself before spending minutes on an export. A stock
    # template would produce a green-looking export and a silently splat-free
    # game; refuse to proceed instead.
    missing = _template_has_gs_symbols(template)
    if missing:
        return _fail(
            f"Export template {template} contains no {', '.join(missing)} symbols -- "
            "it is not a GS-enabled template."
        )
    print("[export-smoke] template carries GaussianSplat symbols.")

    if not FIXTURE_FILE.is_file():
        prep = _run(
            [sys.executable, str(ASSET_PREP_SCRIPT)],
            cwd=ROOT,
            timeout=args.import_timeout,
            label="prepare fixtures",
        )
        if prep.returncode != 0 or not FIXTURE_FILE.is_file():
            print(_tail(prep.stdout + "\n" + prep.stderr))
            return _fail(f"Could not generate the smoke fixture {FIXTURE_FILE}.")

    owns_output_dir = args.output_dir is None
    output_dir = Path(args.output_dir) if args.output_dir else Path(tempfile.mkdtemp(prefix="gs-export-smoke-"))
    output_dir.mkdir(parents=True, exist_ok=True)
    exported = output_dir / "gs_export_smoke.exe"
    preset_path: Optional[Path] = None

    try:
        if not args.skip_import:
            imported = _run(
                [str(editor), "--headless", "--path", str(PROJECT_DIR), "--import"],
                cwd=ROOT,
                timeout=args.import_timeout,
                label="import project",
            )
            if imported.returncode != 0:
                print(_tail(imported.stdout + "\n" + imported.stderr))
                return _fail(f"Project import failed (exit {imported.returncode}).")

        preset_path = _write_preset(template)
        exported_result = _run(
            [
                str(editor),
                "--headless",
                "--path",
                str(PROJECT_DIR),
                "--export-release",
                PRESET_NAME,
                str(exported),
            ],
            cwd=ROOT,
            timeout=args.export_timeout,
            label="export project",
        )
        if exported_result.returncode != 0 or not exported.is_file():
            print(_tail(exported_result.stdout + "\n" + exported_result.stderr))
            return _fail(f"Export failed (exit {exported_result.returncode}); no binary at {exported}.")

        # Discriminating static check on the produced artifact, mirroring the
        # evidence in #825 (`grep -a GaussianSplat <exe>`). A stock-template
        # export lands here with zero matches.
        missing_in_export = _template_has_gs_symbols(exported)
        if missing_in_export:
            return _fail(
                f"Exported binary {exported} contains no {', '.join(missing_in_export)} symbols: "
                "the export used a stock template (custom_template/release unset or wrong)."
            )
        print("[export-smoke] exported binary carries GaussianSplat symbols.")

        run_result = _run(
            [str(exported), *_resolve_mode_args("windows-vulkan"), "--script", PROBE_RES_PATH],
            cwd=output_dir,
            timeout=args.run_timeout,
            label="run exported game",
        )
        combined = run_result.stdout + "\n" + run_result.stderr
        metrics = _parse_metrics(combined)
        if metrics is not None:
            print(f"{METRICS_MARKER} {json.dumps(metrics, sort_keys=True)}")

        blank_viewport = (
            run_result.returncode == PROBE_EXIT_NO_VISUAL_EVIDENCE
            and metrics is not None
            and metrics.get("status") == "failed_visual_evidence"
            and bool(metrics.get("pipeline_evidence_ok"))
        )
        if blank_viewport and args.allow_blank_viewport:
            print(
                f"{PASS_MARKER} (pipeline evidence only, --allow-blank-viewport) exported binary "
                f"loaded the module, held a RenderingDevice and rastered "
                f"{metrics.get('visible_splats_max')} splats; the window read back blank."
            )
            return EXIT_PASS

        if run_result.returncode != 0 or metrics is None or metrics.get("status") != "passed":
            print(_tail(combined))
            reason = metrics.get("reason") if metrics else "probe produced no metrics line"
            return _fail(f"Exported game run failed (exit {run_result.returncode}): {reason}")

        print(
            f"{PASS_MARKER} exported binary rendered "
            f"{metrics.get('visible_splats_max')} splats "
            f"({metrics.get('visual_non_background_samples_max')} non-background samples)."
        )
        return EXIT_PASS
    finally:
        if not args.keep:
            if preset_path is not None and preset_path.exists():
                preset_path.unlink()
            if owns_output_dir:
                shutil.rmtree(output_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
