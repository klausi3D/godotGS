#!/usr/bin/env python3
"""Resolve built export-template binaries and the names Godot expects (#825).

Both export-template jobs in `.github/workflows/release_builds.yml` call this
script instead of carrying their own inline shell globbing. That is deliberate:
the first version of those jobs did the matching inline with
`godot.windows.template_release*.x86_64.exe`, which cannot match
`godot.windows.template_release.x86_64.console.exe` -- SConstruct appends
`.console` AFTER the architecture, so the console wrapper does not end in
`.x86_64.exe`. The ambiguity check downstream was correct; the filter feeding it
silently excluded the file it was meant to find, and the Windows job could never
upload an artifact. Keeping the matching here makes it unit-testable against the
file names SConstruct actually produces (see test_resolve_export_template.py).

Two separate name spaces are involved and must not be conflated:

* the BUILT binary in `bin/`, named by SConstruct:
      godot.windows.template_release.x86_64.exe
      godot.windows.template_release.x86_64.console.exe
      godot.linuxbsd.template_release.x86_64
* the EXPORT TEMPLATE name Godot's exporter looks up, from
  `EditorExportPlatform*::get_template_file_name()`:
      windows_release_x86_64.exe   ("windows_" + target + "_" + arch + ".exe")
      linux_release.x86_64         ("linux_" + target + "." + arch)
  Note the '_' vs '.' separator difference. It is upstream Godot behaviour, not
  a typo; renaming either makes the template invisible to the exporter.

Usage:
    python tests/ci/resolve_export_template.py \
        --bin-dir bin --platform windows --scons-target template_release --arch x86_64

Prints `key=value` lines and, when `GITHUB_OUTPUT` is set, appends the same
lines to it. Exits non-zero (fail-closed) when the expected binaries are absent
or ambiguous.
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional

SUPPORTED_PLATFORMS = ("windows", "linuxbsd")
CONSOLE_SUFFIX = ".console.exe"


class ResolutionError(RuntimeError):
    """Raised when the bin directory does not hold exactly what is required."""


@dataclass(frozen=True)
class ResolvedTemplate:
    main_path: Path
    main_name: str
    template_name: str
    console_path: Optional[Path] = None
    console_name: Optional[str] = None
    console_template_name: Optional[str] = None

    def as_outputs(self) -> Dict[str, str]:
        outputs = {
            "main_path": str(self.main_path),
            "main_name": self.main_name,
            "template_name": self.template_name,
        }
        if self.console_path is not None:
            outputs["console_path"] = str(self.console_path)
            outputs["console_name"] = self.console_name or ""
            outputs["console_template_name"] = self.console_template_name or ""
        return outputs


def export_target_from_scons_target(scons_target: str) -> str:
    """`template_release` -> `release`; the exporter's word, not SCons'."""
    if not scons_target.startswith("template_"):
        raise ResolutionError(
            f"Expected a template SCons target (template_release/template_debug), got {scons_target!r}."
        )
    return scons_target[len("template_") :]


def expected_template_name(platform: str, scons_target: str, arch: str) -> str:
    """Mirror of EditorExportPlatform*::get_template_file_name()."""
    target = export_target_from_scons_target(scons_target)
    if platform == "windows":
        # platform/windows/export/export_plugin.cpp
        return f"windows_{target}_{arch}.exe"
    if platform == "linuxbsd":
        # platform/linuxbsd/export/export_plugin.cpp
        return f"linux_{target}.{arch}"
    raise ResolutionError(f"Unsupported platform {platform!r}.")


def expected_console_template_name(platform: str, scons_target: str, arch: str) -> Optional[str]:
    """The `.console.exe` sibling Godot resolves next to a Windows template.

    `EditorExportPlatformWindows::modify_template()` derives it as
    `<template basename> + ".console.exe"`, so it must sit beside the template
    under the matching name or a console-wrapper export ships without one.
    """
    if platform != "windows":
        return None
    return expected_template_name(platform, scons_target, arch)[: -len(".exe")] + CONSOLE_SUFFIX


def _built_prefix(platform: str, scons_target: str) -> str:
    return f"godot.{platform}.{scons_target}"


def is_built_console_wrapper(name: str, platform: str, scons_target: str, arch: str) -> bool:
    """`godot.windows.template_release.x86_64.console.exe`.

    The architecture comes BEFORE `.console`, which is exactly what the original
    inline `*.x86_64.exe` glob got wrong.
    """
    if platform != "windows":
        return False
    return name.startswith(_built_prefix(platform, scons_target)) and name.endswith(
        f".{arch}{CONSOLE_SUFFIX}"
    )


def is_built_main_binary(name: str, platform: str, scons_target: str, arch: str) -> bool:
    """`godot.windows.template_release.x86_64.exe` / `godot.linuxbsd.template_release.x86_64`.

    Explicitly excludes the console wrapper so the two predicates are disjoint by
    construction rather than by the order they happen to be applied in.
    """
    if not name.startswith(_built_prefix(platform, scons_target)):
        return False
    if platform == "windows":
        return name.endswith(f".{arch}.exe") and not name.endswith(CONSOLE_SUFFIX)
    return name.endswith(f".{arch}")


def resolve(bin_dir: Path, platform: str, scons_target: str, arch: str) -> ResolvedTemplate:
    if platform not in SUPPORTED_PLATFORMS:
        raise ResolutionError(f"Unsupported platform {platform!r}; expected one of {SUPPORTED_PLATFORMS}.")
    if not bin_dir.is_dir():
        raise ResolutionError(f"Bin directory does not exist: {bin_dir}")

    entries = sorted(p for p in bin_dir.iterdir() if p.is_file())
    listing = ", ".join(p.name for p in entries) or "<empty>"

    mains: List[Path] = [p for p in entries if is_built_main_binary(p.name, platform, scons_target, arch)]
    consoles: List[Path] = [
        p for p in entries if is_built_console_wrapper(p.name, platform, scons_target, arch)
    ]

    if len(mains) != 1:
        raise ResolutionError(
            f"Expected exactly one {platform} {scons_target} {arch} binary in {bin_dir}, "
            f"found {len(mains)}: {[p.name for p in mains]}. Directory contains: {listing}"
        )

    template_name = expected_template_name(platform, scons_target, arch)
    console_template_name = expected_console_template_name(platform, scons_target, arch)

    if platform == "windows":
        if len(consoles) != 1:
            raise ResolutionError(
                f"Expected exactly one Windows {scons_target} console wrapper "
                f"(*.{arch}{CONSOLE_SUFFIX}) in {bin_dir}, found {len(consoles)}: "
                f"{[p.name for p in consoles]}. Godot's Windows export resolves the console "
                f"binary as <template basename>.console.exe, so it must ship with the "
                f"template. Directory contains: {listing}"
            )
        console = consoles[0]
        return ResolvedTemplate(
            main_path=mains[0].resolve(),
            main_name=mains[0].name,
            template_name=template_name,
            console_path=console.resolve(),
            console_name=console.name,
            console_template_name=console_template_name,
        )

    return ResolvedTemplate(
        main_path=mains[0].resolve(),
        main_name=mains[0].name,
        template_name=template_name,
    )


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin-dir", default="bin")
    parser.add_argument("--platform", required=True, choices=list(SUPPORTED_PLATFORMS))
    parser.add_argument("--scons-target", default="template_release")
    parser.add_argument("--arch", default="x86_64")
    args = parser.parse_args(argv)

    try:
        resolved = resolve(Path(args.bin_dir), args.platform, args.scons_target, args.arch)
    except ResolutionError as exc:
        print(f"resolve_export_template: {exc}", file=sys.stderr)
        return 1

    outputs = resolved.as_outputs()
    for key, value in outputs.items():
        print(f"{key}={value}")

    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as handle:
            for key, value in outputs.items():
                handle.write(f"{key}={value}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
