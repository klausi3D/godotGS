#!/usr/bin/env python3
"""Derive the agentic risk class (R0-R3) for a set of changed paths.

Classification rules live in ``.agentic/policy.json``. For each changed path the
highest matching class is taken; the overall result is the maximum class across
all paths. Paths that match no rule fall back to ``default_unclassified`` (R3) so
that unrecognized, potentially sensitive paths fail closed.

An **empty** changed-path set falls back the same way. An empty diff is an
absence of information, not evidence that nothing risky changed, so it is
classified as ``default_unclassified`` rather than as the lowest class
(GS-AUDIT-TEST-002; the "fail-open on absence" shape in
``docs/governance/evidence-integrity.md``).

Base-ref resolution fails closed for the same reason: an unresolvable base is an
error, never a degraded diff and never an empty result.

Examples
--------
    python scripts/agentic/classify_change.py --paths modules/gaussian_splatting/renderer/foo.cpp
    python scripts/agentic/classify_change.py --base-ref master --format json
    python scripts/agentic/classify_change.py --base-ref master --github-step-summary
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_POLICY = ROOT / ".agentic" / "policy.json"

_GLOB_REGEX_CACHE: dict[str, "re.Pattern[str]"] = {}


def load_policy(policy_path: Path) -> dict[str, Any]:
    with open(policy_path, encoding="utf-8") as handle:
        return json.load(handle)


def _norm(path: str) -> str:
    text = path.replace("\\", "/")
    if text.startswith("./"):
        text = text[2:]
    return text


def _glob_to_regex(glob: str) -> "re.Pattern[str]":
    """Path-aware glob: ``**`` matches across ``/``, ``*`` matches within one path
    segment, ``?`` matches one non-``/`` char. Case-sensitive."""
    pattern = _GLOB_REGEX_CACHE.get(glob)
    if pattern is None:
        out: list[str] = []
        i, n = 0, len(glob)
        while i < n:
            if glob[i : i + 2] == "**":
                out.append(".*")
                i += 2
            elif glob[i] == "*":
                out.append("[^/]*")
                i += 1
            elif glob[i] == "?":
                out.append("[^/]")
                i += 1
            else:
                out.append(re.escape(glob[i]))
                i += 1
        pattern = re.compile("^" + "".join(out) + r"\Z")
        _GLOB_REGEX_CACHE[glob] = pattern
    return pattern


def _matches(path: str, glob: str) -> bool:
    return _glob_to_regex(glob).match(path) is not None


def classify_paths(paths: list[str], policy: dict[str, Any]) -> tuple[str, list[dict[str, str]]]:
    """Return (overall_class, per_path_detail)."""
    classification = policy["classification"]
    ordering = classification["ordering"]
    rank = {cls: index for index, cls in enumerate(ordering)}
    default = classification["default_unclassified"]
    rules = classification["rules"]

    per_path: list[dict[str, str]] = []
    overall: str | None = None

    for raw in paths:
        path = _norm(raw)
        best: str | None = None
        best_reason = ""
        for rule in rules:
            cls = rule["class"]
            for glob in rule["path_globs"]:
                if _matches(path, _norm(glob)):
                    if best is None or rank[cls] > rank[best]:
                        best = cls
                        best_reason = rule["reason"]
                    break
        if best is None:
            best = default
            best_reason = "unclassified path (fail-closed)"
        per_path.append({"path": path, "class": best, "reason": best_reason})
        if overall is None or rank[best] > rank[overall]:
            overall = best

    if overall is None:
        # No changed paths at all. This is an ABSENCE of information -- a
        # mis-resolved base, a filtered path list, a diff form that reported
        # nothing -- not a demonstration that nothing risky changed. Fail closed to
        # the policy's default_unclassified, exactly as an unmatched path does.
        # Returning ordering[0] (R0) here was the fail-open of GS-AUDIT-TEST-002.
        overall = default
    return overall, per_path


def _git(args: list[str]) -> "subprocess.CompletedProcess[str]":
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True)


def resolve_diff_base(base_ref: str) -> str:
    """Resolve ``base_ref`` to its merge base with ``HEAD``, or fail closed.

    Returns the merge-base commit SHA. Raises ``SystemExit`` (non-zero) when the
    ref, or the shared history, cannot be resolved.

    There is deliberately **no** fallback here. The previous implementation
    retried an unresolvable ``base...HEAD`` as a two-dot ``git diff base``, which
    is a different question: a two-dot diff also reports commits that are only on
    the base, and where the two refs share no history it succeeds where the
    three-dot form fails. Either way the caller got a path set whose meaning was
    not the PR's own diff, and an under-reported risk class followed. An empty
    result is not offered as a success path either -- see ``classify_paths``.
    """
    probe = _git(["rev-parse", "--verify", "--quiet", f"{base_ref}^{{commit}}"])
    if probe.returncode != 0 or not probe.stdout.strip():
        raise SystemExit(
            f"cannot resolve base ref {base_ref!r} to a commit in {ROOT}. Refusing to "
            f"classify against an unknown base: any degraded diff would under-report "
            f"the risk class. If this is a shallow checkout, fetch the base first "
            f"(actions/checkout with 'fetch-depth: 0', or "
            f"'git fetch --depth=<n> origin {base_ref}')."
        )
    base_sha = probe.stdout.strip()

    merge_base = _git(["merge-base", base_sha, "HEAD"])
    if merge_base.returncode != 0 or not merge_base.stdout.strip():
        raise SystemExit(
            f"no merge base between base ref {base_ref!r} ({base_sha}) and HEAD. "
            f"Refusing to fall back to a two-dot diff, which answers a different "
            f"question. Deepen the checkout so the shared history is present."
        )
    return merge_base.stdout.strip()


def git_changed_paths(base_ref: str) -> list[str]:
    # --no-renames so a rename reports BOTH the deleted source and the added
    # destination; otherwise moving a sensitive R3 path onto an R0 docs path would
    # hide the high-risk source and defeat the fail-closed classification.
    #
    # Diffing merge_base..HEAD is the two-dot spelling of `base...HEAD`; it is
    # written out so the base resolution can fail closed on its own (above)
    # instead of being hidden inside a single git invocation's exit code.
    base_sha = resolve_diff_base(base_ref)
    result = _git(["diff", "--name-only", "--no-renames", base_sha, "HEAD"])
    if result.returncode != 0:
        raise SystemExit(
            f"git diff failed for base-ref {base_ref!r} (merge base {base_sha}): "
            f"{result.stderr.strip()}"
        )
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


# How many per-path rows the markdown summary prints before truncating. The class
# itself is always complete; the table is context.
SUMMARY_PATH_LIMIT = 40

# Stated on every rendered summary. The gate enforces the *derivation* of the risk
# class; it does not yet verify that the class's evidence was produced, because no
# per-PR contract source exists in this repository (Phase-2 contract-source ADR).
SUMMARY_SCOPE_NOTE = (
    "> **What this gate enforces.** The risk class above is derived from this PR's own "
    "diff, and that derivation is enforced: an unresolvable base fails the check, and an "
    "empty changed-path set fails closed to the policy default rather than to R0. The "
    "evidence requirements and deterministic checks above are published for the human "
    "merging this PR; CI does **not** yet verify that they were produced, and no per-PR "
    "task contract (`owned_paths` / `forbidden_paths` / evidence) is consumed, because the "
    "repository has no per-PR contract source. Closing that gap is the Phase-2 "
    "contract-source ADR."
)


def render_markdown_summary(
    risk_class: str,
    per_path: list[dict[str, str]],
    policy: dict[str, Any],
    base_ref: str | None = None,
) -> str:
    """Render the risk class plus that class's policy obligations as markdown.

    Both lists are read out of ``policy`` rather than restated here, so a policy
    edit cannot drift from what the required check publishes.
    """
    class_policy = policy.get("risk_classes", {}).get(risk_class, {})
    title = class_policy.get("title", "")

    lines: list[str] = []
    lines.append(f"## Agentic risk class: {risk_class}" + (f" — {title}" if title else ""))
    lines.append("")
    if base_ref:
        lines.append(f"Derived from {len(per_path)} changed path(s) against base `{base_ref}`.")
    else:
        lines.append(f"Derived from {len(per_path)} changed path(s).")
    lines.append("")

    for heading, key, code in (
        (f"Evidence required for {risk_class}", "evidence_requirements", False),
        (f"Deterministic checks for {risk_class}", "deterministic_checks", True),
    ):
        lines.append(f"### {heading}")
        lines.append("")
        items = class_policy.get(key) or []
        if items:
            lines.extend(f"- `{item}`" if code else f"- {item}" for item in items)
        else:
            lines.append(f"- _none declared for {risk_class} in `.agentic/policy.json`_")
        lines.append("")

    lines.append(SUMMARY_SCOPE_NOTE)
    lines.append("")

    if not per_path:
        lines.append(
            "_No changed paths were reported for this base, so the class above is the "
            "fail-closed default rather than a measurement._"
        )
        lines.append("")
        return "\n".join(lines)

    shown = per_path[:SUMMARY_PATH_LIMIT]
    lines.append("<details><summary>Changed paths and their class</summary>")
    lines.append("")
    lines.append("| Class | Path | Rule |")
    lines.append("| --- | --- | --- |")
    for detail in shown:
        lines.append(f"| {detail['class']} | `{detail['path']}` | {detail['reason']} |")
    if len(per_path) > len(shown):
        lines.append(f"| … | _{len(per_path) - len(shown)} further path(s) not shown_ | |")
    lines.append("")
    lines.append("</details>")
    lines.append("")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--paths", nargs="*", help="Explicit changed paths to classify.")
    parser.add_argument("--base-ref", help="Git ref to diff HEAD against to discover changed paths.")
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY, help="Path to policy.json.")
    parser.add_argument("--format", choices=["text", "json"], default="text", help="Output format.")
    parser.add_argument(
        "--github-output",
        action="store_true",
        help="Also write risk_class to the file named by $GITHUB_OUTPUT.",
    )
    parser.add_argument(
        "--github-step-summary",
        action="store_true",
        help="Render the risk class together with that class's policy evidence "
        "requirements and deterministic checks as markdown, and append it to the file "
        "named by $GITHUB_STEP_SUMMARY. Falls back to stdout when that variable is "
        "unset, so the summary is never silently dropped.",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    policy = load_policy(args.policy)

    if args.paths is not None:
        paths = args.paths
    elif args.base_ref:
        paths = git_changed_paths(args.base_ref)
    else:
        print("error: provide --paths or --base-ref", file=sys.stderr)
        return 2

    risk_class, per_path = classify_paths(paths, policy)

    if args.format == "json":
        print(json.dumps({"risk_class": risk_class, "paths": per_path}, indent=2))
    else:
        print(f"risk_class: {risk_class}")
        for detail in per_path:
            print(f"  {detail['class']}  {detail['path']}  ({detail['reason']})")

    if args.github_output:
        output_file = os.environ.get("GITHUB_OUTPUT")
        if output_file:
            with open(output_file, "a", encoding="utf-8") as handle:
                handle.write(f"risk_class={risk_class}\n")

    if args.github_step_summary:
        summary = render_markdown_summary(risk_class, per_path, policy, base_ref=args.base_ref)
        summary_file = os.environ.get("GITHUB_STEP_SUMMARY")
        if summary_file:
            with open(summary_file, "a", encoding="utf-8") as handle:
                handle.write(summary + "\n")
        else:
            # Never a silent no-op: a summary flag that quietly writes nowhere is
            # the "guard wired to nothing" shape this file is being repaired for.
            print(summary)

    return 0


if __name__ == "__main__":
    sys.exit(main())
