#!/usr/bin/env python3
"""BV-C2 codec probe lane: GSQ ratio and decode/pack throughput per core.

ADR: docs/architecture/adr-beyond-vram-worlds.md, PR row BV-C2 ("Ratio + decode/pack
throughput probe on real scans + generator subsets"), sections 5.3 and 5.4, risks K3
and K9.

Runs the doctest case "[GaussianSplatting][WorldIO][GSQ] BV-C2 codec probe: ratio ..."
(modules/gaussian_splatting/tests/test_gsq_codec_probe.h) of an optimized tests=yes
binary in full mode::

    python3 tests/runtime/flythrough/run_codec_probe.py --godot-binary B --out OUT \\
        [--scan a.ply ...] [--scan-dir DIR]

and writes OUT/codec_probe.json (the case's report plus host facts), OUT/codec_probe.log
and a Markdown table (stdout and $GITHUB_STEP_SUMMARY when set).

Real scans: every --scan path and every *.ply in --scan-dir (or $GS_REAL_SCAN_DIR when
--scan-dir is not given). A scan that is not there is MISSING in the report, never a
number. A --scan-dir that does not exist is passed on as a path that does not exist, so it
too ends up MISSING with its name.

Report-only: nothing gates. Exit codes: 0 the probe ran and every synthetic data set is
OK (real scans may be MISSING; the verdict then says PARTIAL); 1 the case failed, crashed
or timed out, or a synthetic data set is not OK; 3 NOT_RUN (no binary, or no report).

Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List

TEST_CASE = "*BV-C2 codec probe: ratio*"
REPORT_NAME = "codec_probe.json"


def _cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def collect_scans(scans: List[str], scan_dir: str) -> List[str]:
    found = [str(Path(s)) for s in scans if s]
    if scan_dir:
        directory = Path(scan_dir)
        plys = sorted(str(p) for p in directory.glob("*.ply")) if directory.is_dir() else []
        # An absent or empty directory stays visible as one MISSING entry.
        found.extend(plys or [str(directory / "*.ply")])
    return found


def _fmt(value: Any, digits: int = 2) -> str:
    return f"{value:.{digits}f}" if isinstance(value, (int, float)) else "-"


def markdown(report: Dict[str, Any]) -> str:
    rows = [
        "| data set | kind | status | splats | B/splat | fixed B/splat | zstd gain | vs plain v3 | "
        "decode MB/s (sha) | decode Msplat/s (sha) | decode Msplat/s (no sha) | pack Msplat/s |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for d in report.get("data_sets", []):
        status = d.get("status", "?")
        if status != "OK":
            status = f"{status}: {d.get('reason', '')}"
        rows.append(
            "| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
                d.get("name", "?"), d.get("kind", "?"), status, d.get("splats", "-"),
                _fmt(d.get("bytes_per_splat")), _fmt(d.get("fixed_width_bytes_per_splat")),
                _fmt(d.get("zstd_ratio_vs_fixed_width")), _fmt(d.get("ratio_vs_plain_v3_sh3")),
                _fmt(d.get("decode_verified_stored_mb_per_s_one_core"), 1),
                _fmt(d.get("decode_verified_msplats_per_s_one_core")),
                _fmt(d.get("decode_unverified_msplats_per_s_one_core")),
                _fmt(d.get("pack_msplats_per_s_one_core")),
            )
        )
    host = report.get("host", {})
    lines = [
        "## BV-C2 GSQ codec probe",
        "",
        f"Verdict: **{report.get('verdict', '?')}**. One thread; runner `{host.get('runner_name', '?')}`, "
        f"CPU `{host.get('cpu_model', '?')}`, {host.get('logical_cpus', '?')} logical CPUs.",
        "",
        *rows,
        "",
    ]
    for kind in ("synthetic", "real_scan"):
        agg = report.get(kind, {})
        if agg.get("status") == "OK":
            lines.append(
                f"- {kind}: {_fmt(agg.get('bytes_per_splat'))} B/splat over {agg.get('splats')} splats "
                f"(fixed width {_fmt(agg.get('fixed_width_bytes_per_splat'))}, zstd gain "
                f"{_fmt(agg.get('zstd_ratio_vs_fixed_width'))}x)"
            )
        else:
            lines.append(f"- {kind}: {agg.get('status', 'MISSING')} ({agg.get('reason', 'no report')})")
    return "\n".join(lines) + "\n"


def verdict(report: Dict[str, Any], exit_code: int) -> tuple[str, int]:
    if exit_code != 0:
        return f"FAIL: test case exit code {exit_code}", 1
    bad = [
        d.get("name", "?") for d in report.get("data_sets", [])
        if d.get("kind") != "real_scan" and d.get("status") != "OK"
    ]
    if bad or report.get("synthetic", {}).get("status") != "OK":
        return f"FAIL: synthetic data sets not OK: {bad}", 1
    if report.get("real_scan", {}).get("status") != "OK":
        return "PARTIAL: synthetic OK, real scans MISSING", 0
    # The aggregate counts only OK scans; one measured scan must not hide another that is
    # MISSING or INVALID (Codex on #1363).
    not_ok = [
        f"{d.get('name', '?')}={d.get('status')}" for d in report.get("data_sets", [])
        if d.get("kind") == "real_scan" and d.get("status") != "OK"
    ]
    if not_ok:
        return f"PARTIAL: synthetic OK, some real scans not measured: {not_ok}", 0
    return "OK", 0


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--godot-binary", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--scan", action="append", default=[])
    parser.add_argument("--scan-dir", default=None)
    parser.add_argument("--timeout", type=int, default=1800, help="seconds")
    args = parser.parse_args(argv)

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = Path(args.godot_binary)
    if not binary.is_file():
        print(f"NOT_RUN: Godot binary {binary} does not exist")
        return 3
    scan_dir = args.scan_dir if args.scan_dir is not None else os.environ.get("GS_REAL_SCAN_DIR", "")
    scans = collect_scans(args.scan, scan_dir)

    report_path = out / REPORT_NAME
    if report_path.exists():
        report_path.unlink()
    env = dict(os.environ)
    env["GS_GSQ_PROBE_OUT"] = str(report_path)
    env["GS_GSQ_PROBE_SCANS"] = ";".join(scans)
    cmd = [str(binary), "--headless", "--test", f"--test-case={TEST_CASE}"]
    print("running:", " ".join(cmd))
    print("scans:", scans or "none")
    try:
        result = subprocess.run(
            cmd, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=args.timeout
        )
        code, log = result.returncode, (result.stdout or "") + (result.stderr or "")
    except subprocess.TimeoutExpired as exc:
        code, log = 124, f"timeout after {args.timeout} s\n{exc.stdout or ''}{exc.stderr or ''}"
    (out / "codec_probe.log").write_text(log, encoding="utf-8")
    print("\n".join(log.splitlines()[-40:]))

    if not report_path.is_file():
        print(f"NOT_RUN: the probe wrote no report (exit code {code}); see codec_probe.log")
        return 3 if code == 0 else 1
    report = json.loads(report_path.read_text(encoding="utf-8"))
    report["host"] = {
        "runner_name": os.environ.get("RUNNER_NAME", platform.node()),
        "cpu_model": _cpu_model(),
        "logical_cpus": os.cpu_count(),
        "platform": platform.platform(),
        "binary": str(binary),
        "binary_sha256": _sha256(binary),
        "git_sha": os.environ.get("GITHUB_SHA", ""),
        "scans_requested": scans,
    }
    text, rc = verdict(report, code)
    report["verdict"] = text
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    table = markdown(report)
    print(table)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as handle:
            handle.write(table)
    print(f"verdict: {text}")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
