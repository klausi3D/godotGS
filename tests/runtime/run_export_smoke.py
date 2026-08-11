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

What actually proves the module is in there
-------------------------------------------
The byte-string scan of the template and of the exported binary is a **fast
pre-check, not proof**: it rules a stock template OUT (a stock template
contains none of those literals), but any file containing them would pass it,
so it cannot rule a GS template IN. The proof is downstream and runs inside the
exported binary: `export_smoke_probe.gd` resolves the GS types through ClassDB
and then requires a live RenderingDevice, a renderer, visible splats and a
successful GPU raster pass. Do not quote the symbol scan on its own as evidence
that an export is GS-enabled.

Usage
-----
    python tests/runtime/run_export_smoke.py \
        --editor-binary bin/godot.windows.editor.x86_64.exe \
        --template bin/godot.windows.template_release.x86_64.exe

Both paths are auto-discovered from `bin/` and can also be supplied via the
`GODOT_EDITOR_BINARY` / `GODOT_EXPORT_TEMPLATE` environment variables. When a
prerequisite is missing the run reports `[EXPORT_SMOKE_SKIP]` and exits 0
unless `--require-binaries` is passed.

The negative control (`--expect-stock-template-failure`)
-------------------------------------------------------
A smoke test that has only ever been run in its passing configuration proves
that one configuration worked once, not that it still detects the failure. So
this script also runs itself backwards: with `--expect-stock-template-failure`
it writes the preset with an EMPTY `custom_template/release` -- the #825
misconfiguration verbatim -- and requires the run NOT to end in a working GS
export. See `negative_control_outcome` for the four outcomes and what each of
them says about the machine it ran on.

A control that passes on *any* failure is not a control. "The export did not
succeed" is far weaker than "the export was refused because no template could be
resolved": a timeout, a crash, a disk-full error and an unrelated resource error
all satisfy the first and establish nothing about the second. Only the expected
rejection passes -- classified from the editor's own diagnostics and from the
verified absence of the binary on disk, never inferred from a nonzero exit code.
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
PRESET_BACKUP_NAME = "export_presets.cfg.smoke-backup"
PROBE_RES_PATH = "res://tests/export_smoke_probe.gd"
PROBE_FILE = PROJECT_DIR / "tests" / "export_smoke_probe.gd"
FIXTURE_FILE = PROJECT_DIR / "tests" / "fixtures" / "synthetic_cube.ply"
ASSET_PREP_SCRIPT = RUNTIME_DIR / "prepare_synthetic_assets.py"

SKIP_MARKER = "[EXPORT_SMOKE_SKIP]"
FAIL_MARKER = "[EXPORT_SMOKE_FAIL]"
PASS_MARKER = "[EXPORT_SMOKE_PASS]"
METRICS_MARKER = "[EXPORT_SMOKE_METRICS]"

# Byte needles absent from a stock Godot template, which is the silent failure
# this test exists to catch (issue #825: GrandmasHouse exported on 2026-01-21
# with custom_template/release="" and its .exe carries zero GS symbols).
#
# NECESSARY, NOT SUFFICIENT. This is a cheap pre-check, not proof that a binary
# is GS-enabled: any file containing these literals passes it, so on its own it
# is trivially defeatable. Its job is to fail fast before a multi-minute export,
# and to catch the specific stock-template case. The actual proof that the
# module is present and working is downstream, in the exported binary itself:
# the ClassDB lookup in export_smoke_probe.gd and the render evidence it
# gathers on a live RenderingDevice.
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

# Exit code this module fabricates for a subprocess it had to kill. It is NOT a
# reliable timeout signal on its own -- 124 is also a perfectly ordinary exit
# code for a real process -- which is why `CommandResult.timed_out` carries the
# fact separately.
TIMEOUT_RETURNCODE = 124

# --- Negative-control evidence -----------------------------------------------
#
# What the editor prints when it refuses an export because it cannot resolve a
# template. Both halves matter, and both are chosen to be LOCALE-INDEPENDENT:
# the editor runs with the host's language, and a German or Chinese runner would
# translate every TTR() string, so matching the English message text would make
# the control machine-dependent.
#
#   * `editor/editor_node.cpp` emits
#     `Cannot export project with preset "%s" due to configuration errors:\n%s`
#     through plain `vformat()`, NOT `TTR()`, so this prefix survives any locale.
#     It says "the editor refused because `can_export()` returned false".
#   * The reason itself IS translated, but the *path* interpolated into it is
#     not: `editor/export/editor_export_platform.cpp` appends
#     `<data dir>/export_templates/<version>/windows_release_x86_64.exe` after
#     the translated "No export template found at the expected path:" line
#     (`export_templates_folder` in `editor/file_system/editor_paths.h`,
#     `EditorExportPlatformWindows::get_template_file_name()` in
#     `platform/windows/export/export_plugin.cpp`). Matching the path narrows
#     "some configuration error" to "the template could not be resolved", which
#     is the only reason this control is allowed to pass.
EXPORT_REJECTION_MARKER = "Cannot export project with preset"
EXPORT_REJECTION_REASON_MARKER = "due to configuration errors"
MISSING_TEMPLATE_MARKERS: Sequence[str] = (
    "export_templates",
    "windows_release_x86_64.exe",
    # Set when `custom_template/release` names a file that does not exist. Not
    # this control's own configuration (it writes ""), but it is still a
    # template-resolution refusal, so it is recognised rather than misfiled as
    # an unrelated failure. English-only by nature -- it carries no path -- so it
    # is a bonus signal, never the sole one required.
    "Custom release template not found",
)

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
        # Resolve against the CALLER's cwd, at the moment we validate it. The
        # editor is later spawned with cwd=ROOT, so a relative path that exists
        # here would be looked up under the repository there -- accepted as
        # valid and then not found. Same shape as --output-dir.
        candidate = Path(raw).resolve()
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
        # Absolute for the same reason as the editor: validated in the caller's
        # cwd, embedded into the preset and used from another one.
        candidate = Path(raw).resolve()
        return candidate if candidate.is_file() else None
    return _discover(
        (
            "godot.windows.template_release.x86_64.exe",
            "godot.windows.template_release*.x86_64.exe",
            "godot.linuxbsd.template_release*.x86_64",
        )
    )


class CommandResult(subprocess.CompletedProcess):
    """`CompletedProcess` plus an unambiguous "we killed it" flag.

    A fabricated exit code cannot carry this fact: `TIMEOUT_RETURNCODE` is a
    legal exit code for a real process, so a caller that inferred "timed out"
    from `returncode == 124` would also fire on a program that simply exited
    124. The negative control has to tell a hang apart from a refusal, so the
    distinction is recorded rather than reconstructed.
    """

    def __init__(
        self,
        args: Sequence[str],
        returncode: int,
        stdout: str,
        stderr: str,
        *,
        timed_out: bool = False,
    ) -> None:
        super().__init__(args, returncode, stdout, stderr)
        self.timed_out = timed_out

    @property
    def combined_output(self) -> str:
        return f"{self.stdout or ''}\n{self.stderr or ''}"


def _run(command: List[str], *, cwd: Path, timeout: int, label: str) -> CommandResult:
    print(f"[export-smoke] {label}: {' '.join(command)}")
    try:
        completed = subprocess.run(
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
        return CommandResult(command, TIMEOUT_RETURNCODE, stdout, stderr, timed_out=True)
    return CommandResult(
        command,
        completed.returncode,
        completed.stdout or "",
        completed.stderr or "",
        timed_out=False,
    )


def _tail(text: str, lines: int = 30) -> str:
    kept = [line.rstrip() for line in text.splitlines() if line.strip()][-lines:]
    return "\n".join(kept)


def _missing_gs_symbol_needles(binary: Path) -> List[str]:
    """Return the byte needles that are absent from `binary`.

    An empty result means only "these literals appear somewhere in the file" --
    it does NOT establish that the binary is a GS-enabled Godot template. See
    REQUIRED_TEMPLATE_SYMBOLS: this is a fast, defeatable pre-check whose value
    is catching the stock-template case before a multi-minute export. Treat a
    non-empty result as conclusive (the module is definitely not in there) and
    an empty one as merely "keep going".

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


def preset_paths() -> tuple[Path, Path]:
    """(preset, backup). Read through this so tests can repoint PROJECT_DIR."""
    return PROJECT_DIR / "export_presets.cfg", PROJECT_DIR / PRESET_BACKUP_NAME


def preset_state_error(overwrite_allowed: bool) -> Optional[str]:
    """Refusal reason for the current on-disk preset state, or None to proceed.

    The backup file is *state*, and it has three possible starting conditions.
    Enumerating them explicitly is the whole point of this function -- an
    earlier version handled only "absent", and its `backup_path.unlink()` for
    the other cases deleted the developer's original in exactly the crash the
    backup existed to survive.

      1. no backup                -> normal; only the preset itself gates the run
      2. backup + generated preset -> a previous run died mid-flight
      3. backup + no preset        -> a previous run died between the two moves

    In (2) and (3) the tool cannot tell which file is the developer's, so it
    refuses in both and says so. Silently preferring either one is how the
    original gets destroyed.
    """
    preset, backup = preset_paths()

    if backup.exists():
        if preset.exists():
            situation = (
                f"both {preset.name} and {backup.name} are present, which means a previous run "
                f"was interrupted between backing your preset up and restoring it. One of those "
                f"two files is your original and this tool cannot tell which -- deleting or "
                f"overwriting either could lose uncommitted config"
            )
        else:
            situation = (
                f"{backup.name} is present with no {preset.name}, which means a previous run was "
                f"interrupted after it moved your preset aside. That backup is most likely your "
                f"original, and this run will not consume a file it did not create"
            )
        return (
            f"Refusing to run: {situation}. Look in {PROJECT_DIR}, keep the file you want as "
            f"{preset.name}, delete {backup.name}, then re-run. --overwrite-preset does not "
            "override this."
        )

    if preset.exists() and not overwrite_allowed:
        return (
            f"{preset} already exists and this run did not create it. "
            "It is gitignored, so overwriting it would be an unrecoverable, invisible edit to "
            "your config. Move it aside, or pass --overwrite-preset to have the run move it to "
            f"{backup.name} and restore it afterwards."
        )

    return None


def _write_preset(
    template_binary: Path,
    backup_path: Path,
    custom_template_release: Optional[str] = None,
) -> Path:
    """Write the generated preset, first moving any pre-existing one aside.

    The caller has already established (via `preset_state_error`) that
    overwriting is allowed and that no backup exists. The existing file is
    *moved* (not copied-then-truncated), so a crash between here and the restore
    leaves the developer's original on disk under `backup_path`.

    `custom_template_release` defaults to `template_binary`'s absolute path,
    which is the normal run. The negative control passes `""` to reproduce the
    #825 preset exactly; note that `""` is a *deliberate value*, not "unset", so
    the check below is `is None` rather than a truth test.
    """
    target = PROJECT_DIR / "export_presets.cfg"
    if target.exists():
        # Defense in depth: never clobber a backup, whatever the caller thinks.
        # If this fires, the pre-flight check was bypassed.
        if backup_path.exists():
            raise RuntimeError(
                f"Refusing to overwrite an existing backup at {backup_path}; it may hold the "
                "only copy of a developer's preset."
            )
        target.replace(backup_path)
        print(f"[export-smoke] moved your existing export_presets.cfg to {backup_path.name}")

    if custom_template_release is None:
        # Godot stores/reads this as an absolute path with forward slashes.
        custom_template_release = template_binary.resolve().as_posix()

    preset_text = PRESET_TEMPLATE.read_text(encoding="utf-8")
    preset_text = preset_text.replace("@PRESET_NAME@", PRESET_NAME)
    preset_text = preset_text.replace("@CUSTOM_TEMPLATE_RELEASE@", custom_template_release)
    target.write_text(preset_text, encoding="utf-8")
    return target


def _restore_preset(generated: Optional[Path], backup_path: Path) -> None:
    """Remove the generated preset and put the developer's original back."""
    if generated is not None and generated.exists():
        generated.unlink()
    if backup_path.exists():
        backup_path.replace(PROJECT_DIR / "export_presets.cfg")
        print(f"[export-smoke] restored your original {PROJECT_DIR.name}/export_presets.cfg")


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


NEGATIVE_CONTROL_OUTCOMES = (
    "export_rejected",
    "stock_template_detected",
    "undetected",
    "unrelated_failure",
)
# Pass-LIST, not a fail-list. The earlier form was `if outcome == "undetected":
# fail`, so every outcome anyone added later -- including ones that establish
# nothing -- passed by default. An unrecognised outcome must be red.
NEGATIVE_CONTROL_PASSING_OUTCOMES = ("export_rejected", "stock_template_detected")


def missing_template_rejection_evidence(export_output: str) -> List[str]:
    """The markers proving the editor refused because it could not resolve a template.

    Empty means "not proven", which is the only thing this control is entitled to
    conclude from an export that merely failed. Requires BOTH halves:

      * the untranslated refusal prefix, which says `can_export()` returned false
        rather than the export dying somewhere downstream, and
      * at least one template-resolution marker, which narrows that refusal from
        "some configuration error" (a bad texture-format combination also lands
        there) to the missing template this control is about.

    See the marker constants for why each one is locale-independent.
    """
    text = export_output or ""
    if EXPORT_REJECTION_MARKER not in text or EXPORT_REJECTION_REASON_MARKER not in text:
        return []
    template_markers = [marker for marker in MISSING_TEMPLATE_MARKERS if marker in text]
    if not template_markers:
        return []
    return [EXPORT_REJECTION_MARKER, *template_markers]


def negative_control_outcome(
    export_returncode: int,
    exported: Optional[Path],
    export_output: str,
    *,
    timed_out: bool = False,
) -> tuple[str, str]:
    """Classify a `--expect-stock-template-failure` run. Returns (outcome, detail).

    The preset named NO custom template. Exactly two outcomes are acceptable, and
    both of them are *positive findings* -- the control never passes on the mere
    absence of success.

    ``export_rejected`` (passes)
        The editor refused the export **because it could not resolve a template**,
        and no binary exists at the export path -- checked on disk, not inferred
        from the exit code. Today that means no stock export template is installed
        on this machine, so the exporter had nothing to silently fall back to.
        After the engine-side fix (milestone 1 PR 5) this is the outcome to expect
        everywhere, and the reason this control has to exist *before* that change:
        it is the assertion that provably passes for a different reason on the
        pre-change tree.

    ``stock_template_detected`` (passes)
        The export SUCCEEDED and produced a binary with no GS symbols in it.
        That is #825 reproduced: a stock upstream template is installed on this
        machine and Godot silently fell back to it. The byte-scan caught it and
        nothing upstream of the byte-scan did. Worth reporting loudly -- it is a
        fact about the runner, not about the change under test.

    ``undetected`` (fails)
        A binary carrying GS symbols came out of a preset that named no custom
        template. Either the preset substitution did not take or something is
        feeding a GS template in behind the flag's back; either way the positive
        run above is not discriminating and its green is worth nothing.

    ``unrelated_failure`` (fails)
        The export did not succeed, but nothing establishes that the empty
        `custom_template/release` is why. A timeout, a crash, a disk-full error,
        a missing project, an unreadable fixture -- every one of them produced a
        nonzero exit and no binary, which the earlier version of this function
        classified as `export_rejected` and reported green. A control that passes
        whenever anything at all goes wrong has stopped being a control.
    """
    binary_present = exported is not None and exported.is_file()
    where = str(exported) if exported is not None else "the export path"
    binary_state = f"a binary IS present at {where}" if binary_present else f"no binary exists at {where}"

    if timed_out:
        return (
            "unrelated_failure",
            f"the export TIMED OUT (killed after the export timeout, reported as exit "
            f"{export_returncode}) instead of being refused; a hang says nothing about whether "
            f"the empty custom_template/release was detected ({binary_state})",
        )

    if binary_present:
        missing = _missing_gs_symbol_needles(exported)
        if not missing:
            # Worst case, and it does not become acceptable because the editor
            # also happened to exit nonzero.
            return (
                "undetected",
                f"the export produced {where} (exit {export_returncode}) and it carries GS symbols "
                "even though the preset named no custom template, so this run cannot tell a "
                "correct export from the #825 one",
            )
        if export_returncode != 0:
            return (
                "unrelated_failure",
                f"the export failed (exit {export_returncode}) and LEFT A BINARY at {where} that is "
                f"missing {', '.join(missing)}. A partial artifact from a failed export is not a "
                "clean missing-template rejection, and the byte-scan cannot say which of the two "
                "this was",
            )
        return (
            "stock_template_detected",
            f"the export SUCCEEDED and produced a binary missing {', '.join(missing)}. A stock "
            "upstream export template is installed on this machine, so #825's exact condition is "
            "reproducible here; the byte-scan is what caught it",
        )

    if export_returncode == 0:
        return (
            "unrelated_failure",
            f"the export reported SUCCESS (exit 0) yet {binary_state}. That is neither a rejection "
            "nor a produced artifact, so there is nothing to scan and nothing was established",
        )

    evidence = missing_template_rejection_evidence(export_output)
    if not evidence:
        return (
            "unrelated_failure",
            f"the export failed (exit {export_returncode}) and {binary_state}, but its output "
            f"carries no missing-template rejection: expected {EXPORT_REJECTION_MARKER!r} together "
            f"with one of {list(MISSING_TEMPLATE_MARKERS)}. A timeout, a crash or an unrelated "
            "resource error fails exactly like this and would prove nothing about the empty "
            "custom_template/release",
        )

    return (
        "export_rejected",
        f"the editor refused the export (exit {export_returncode}) because it could not resolve an "
        f"export template -- evidence: {', '.join(repr(item) for item in evidence)} -- and "
        f"{binary_state} (checked on disk)",
    )


def _negative_control_verdict(
    export_returncode: int,
    exported: Optional[Path],
    export_output: str,
    *,
    timed_out: bool = False,
) -> int:
    outcome, detail = negative_control_outcome(
        export_returncode, exported, export_output, timed_out=timed_out
    )
    print(
        f"{METRICS_MARKER} "
        + json.dumps(
            {
                "probe": "export_smoke_negative_control",
                "outcome": outcome,
                "export_returncode": export_returncode,
                "timed_out": bool(timed_out),
                "binary_present": bool(exported is not None and exported.is_file()),
                "rejection_evidence": missing_template_rejection_evidence(export_output),
            },
            sort_keys=True,
        )
    )
    if outcome not in NEGATIVE_CONTROL_PASSING_OUTCOMES:
        return _fail(
            f"Negative control did not establish that the empty custom_template/release was "
            f"detected ({outcome}): {detail}."
        )
    print(f"{PASS_MARKER} negative control ({outcome}): {detail}.")
    return EXIT_PASS


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--editor-binary", default=None, help="Godot editor binary used to import and export.")
    parser.add_argument("--template", default=None, help="target=template_release binary to export against.")
    parser.add_argument("--output-dir", default=None, help="Directory for the exported game (default: a temp dir).")
    parser.add_argument("--import-timeout", type=int, default=1800)
    parser.add_argument("--export-timeout", type=int, default=1800)
    parser.add_argument("--run-timeout", type=int, default=300)
    parser.add_argument("--skip-import", action="store_true", help="Reuse the project's existing .godot import cache.")
    parser.add_argument(
        "--keep",
        action="store_true",
        help=(
            "Keep the exported binary and pack. The generated export_presets.cfg is removed "
            "either way so a displaced original is always put back."
        ),
    )
    parser.add_argument(
        "--overwrite-preset",
        action="store_true",
        help=(
            "Allow the run to take over an existing "
            "tests/examples/godot/test_project/export_presets.cfg. Without this the run refuses "
            "rather than touching a preset it did not create. Even with it, the original is moved "
            "aside to export_presets.cfg.smoke-backup and restored afterwards."
        ),
    )
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
    parser.add_argument(
        "--expect-stock-template-failure",
        action="store_true",
        help=(
            "Negative control: write the preset with an EMPTY custom_template/release (the #825 "
            "misconfiguration) and require that the run does NOT end in a working GS export. "
            "Passes ONLY on a proven missing-template rejection, or when the export succeeds and "
            "the produced binary is detected as stock. A GS-enabled binary from a preset that "
            "named no template fails, and so does any other failure -- a timeout, a crash or an "
            "unrelated error establishes nothing. Does not run the probe."
        ),
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

    # Never destroy a preset this run did not create, and never assume a leftover
    # backup is disposable. Both files are gitignored, so `git status` would not
    # warn a developer that their own config had been taken.
    refusal = preset_state_error(args.overwrite_preset)
    if refusal is not None:
        return _fail(refusal)

    print(f"[export-smoke] editor   = {editor}")
    print(f"[export-smoke] template = {template}")
    if args.expect_stock_template_failure:
        # The template is still resolved and still pre-checked below, even
        # though the preset will not name it. That is on purpose: it keeps the
        # two invocations symmetric, so a negative control cannot "pass" merely
        # because the template download failed.
        print(
            "[export-smoke] NEGATIVE CONTROL: writing the preset with an EMPTY "
            "custom_template/release; this run must NOT produce a working GS export."
        )

    # Cheap pre-check before spending minutes on an export: a stock template
    # would produce a green-looking export and a silently splat-free game.
    # Absence of the needles is conclusive; presence proves nothing on its own.
    missing = _missing_gs_symbol_needles(template)
    if missing:
        return _fail(
            f"Export template {template} contains no {', '.join(missing)} symbols -- "
            "it cannot be a GS-enabled template."
        )
    print("[export-smoke] pre-check: template contains GS symbol strings (not yet proof).")

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
    # Resolve to an absolute path against the CALLER's cwd before anything runs.
    # The export subprocess runs with cwd=ROOT, so a relative --output-dir would
    # otherwise silently land somewhere the caller never named.
    output_dir = (
        Path(args.output_dir).resolve()
        if args.output_dir
        else Path(tempfile.mkdtemp(prefix="gs-export-smoke-")).resolve()
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    exported = output_dir / "gs_export_smoke.exe"
    preset_backup = PROJECT_DIR / PRESET_BACKUP_NAME
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

        preset_path = _write_preset(
            template,
            preset_backup,
            custom_template_release="" if args.expect_stock_template_failure else None,
        )
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
        if args.expect_stock_template_failure:
            # The export tail is printed either way: on `export_rejected` it is
            # the evidence of *why* it was rejected, and on the failing outcomes
            # it is the only place the misconfiguration could have been noticed.
            # The verdict is classified from the FULL output, not the tail: the
            # refusal diagnostic is several lines long and a tail cut could drop
            # the half that distinguishes it from an unrelated failure.
            print(_tail(exported_result.combined_output))
            return _negative_control_verdict(
                exported_result.returncode,
                exported,
                exported_result.combined_output,
                timed_out=exported_result.timed_out,
            )

        if exported_result.returncode != 0 or not exported.is_file():
            print(_tail(exported_result.stdout + "\n" + exported_result.stderr))
            return _fail(f"Export failed (exit {exported_result.returncode}); no binary at {exported}.")

        # Static screen on the produced artifact, mirroring the evidence in #825
        # (`grep -a GaussianSplat <exe>`): a stock-template export lands here
        # with zero matches. One-directional -- it rules a stock template OUT,
        # it does not rule a GS template IN, since any binary containing these
        # literals would pass. The proof that the module is actually there and
        # working is the ClassDB lookup plus the render evidence in
        # export_smoke_probe.gd, which runs next inside this very binary.
        missing_in_export = _missing_gs_symbol_needles(exported)
        if missing_in_export:
            return _fail(
                f"Exported binary {exported} contains no {', '.join(missing_in_export)} symbols: "
                "the export used a stock template (custom_template/release unset or wrong)."
            )
        print("[export-smoke] pre-check: exported binary contains GS symbol strings; "
              "running it now for the ClassDB + render proof.")

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
        # The preset teardown is NOT gated on --keep: --keep is about keeping the
        # exported artifacts, and leaving someone else's config displaced is not
        # a debugging convenience.
        _restore_preset(preset_path, preset_backup)
        if not args.keep and owns_output_dir:
            shutil.rmtree(output_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
