#!/usr/bin/env python3
"""D3-6 paired T0 diagnostic comparison -- PHASE 1 STUB.

The comparison-owned experiment envelope is specified in
test_compare_shared_layers_t0.py. Native probe summaries remain the output of
run_probe.summarise_path / probe_summary.route_telemetry_summary. Readbacks and
queue/publication observations must be collected independently by phase 2;
their absence must never become a successful diagnostic or a Tier A pass.

CLI: --experiment FILE --out DIR writes comparison.json and comparison.md.
Final workflow step: --require-report FILE --run-id ID --control CONTROL
requires a current, usable report, even when comparison execution was skipped.
No comparison, report writing, validation or workflow is implemented here yet.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Optional, Sequence


def compare(experiment: dict) -> dict:
    """Return verdict, exit_code, reasons and per-route metrics (stub only)."""
    return {
        "verdict": "NOT_RUN",
        "exit_code": 3,
        "report_only": True,
        "reasons": [{"code": "not_implemented", "detail": "D3-6 phase 1 stub"}],
        "metrics": {},
    }


def render_markdown(report: dict) -> str:
    """Render the diagnostic, preserving explicit not-measured values (stub)."""
    return "D3-6 phase 1 stub: NOT_RUN\n"


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--experiment", type=Path)
    mode.add_argument("--require-report", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--run-id")
    parser.add_argument("--control", choices=("none", "corrupt", "saturate", "skip_compare"), default="none")
    parser.parse_args(argv)
    # Intentionally wrong: the phase-1 CLI tests must expose missing report
    # writing and fail-open finalization, without import/argument errors.
    return 0


if __name__ == "__main__":
    sys.exit(main())
