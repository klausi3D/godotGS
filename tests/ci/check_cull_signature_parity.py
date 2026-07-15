#!/usr/bin/env python3
"""Guard: every output-affecting GPUCuller::CullingConfig field is folded into the
render-cull cache signature, or is explicitly waived with a machine-checked reason.

Why this exists (standing owner rule): the OutputCompositor reuses a cached render
when its cache key is unchanged. `cull_config_signature` (computed by
`_compute_cull_config_signature` in renderer/render_pipeline_stages.cpp) is one
component of that key. If a new culling/threshold knob is added to
`GPUCuller::CullingConfig` but not hashed into the signature, changing that knob at
runtime silently reuses a stale render -- a "no silent degradation" (G4) violation.

This guard fails closed: it parses the struct's fields and the fields the signature
actually reads, and requires each field to be in exactly one of two sets:
  * hashed   -- read as `config.<field>` inside the signature function, or
  * WAIVERS  -- listed below with a reason. For a field whose effect is captured by
               another (already-hashed) field, the waiver names that field in
               `requires_hashed`, and the guard verifies it really is hashed -- so a
               waiver can never mask a genuine gap.

A field that is neither hashed nor waived fails the guard: add it to the signature
(preferred) or, if it truly cannot affect the rendered/culled output, waive it here
with a justification. Do not silence this guard by deleting a field from the check.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules" / "gaussian_splatting"
CULLER_HEADER = MODULE / "interfaces" / "gpu_culler.h"
SIGNATURE_SOURCE = MODULE / "renderer" / "render_pipeline_stages.cpp"

SIGNATURE_FN = "_compute_cull_config_signature"
STRUCT_NAME = "CullingConfig"

# A plain-old-data member declaration inside the struct body:
# "<type...> <name> = <default>;" or "<type...> <name>;". Methods ("void f();")
# are excluded because a "(" follows the name instead of "=" or ";".
_FIELD_RE = re.compile(r"^\s*[\w:]+(?:\s*[*&])?\s+(\w+)\s*(?:=|;)")
# `config.<field>` reads inside the signature function body.
_CONFIG_READ_RE = re.compile(r"\bconfig\.(\w+)\b")
# A statement that actually folds a value into the running hash: `seed = _hash*(`.
# A field counts as hashed only if it appears in such a statement, so a debug/temp
# read like `const float x = config.knob;` or a log line does not mark it hashed.
_HASH_STMT_RE = re.compile(r"\bseed\s*=\s*_hash\w*\s*\(")
# `#if 0` / `#if false` ... `#endif` regions are compiled out, so a hash line or a
# member declared inside one is not active and must not be counted.
_IF_ZERO_RE = re.compile(r"^#\s*if\s+(?:0|false)\b")
_IF_RE = re.compile(r"^#\s*if")
_ELSE_RE = re.compile(r"^#\s*el(?:se|if)\b")
_ENDIF_RE = re.compile(r"^#\s*endif\b")
# An access specifier is the only struct-body line that is unambiguously not a
# data member. Everything else that does not parse as a field is reported as
# unrecognized (fail closed) rather than guessed at, so an attributed or
# macro-wrapped member (e.g. `alignas(16) float knob = 0;`) cannot be silently
# dropped by a broad "looks like a method" heuristic.
_ACCESS_SPECIFIER_RE = re.compile(r"^(?:public|private|protected)\s*:")
# `//` line comments and `/* */` block comments. Stripped before parsing so a
# commented-out hash line or a `config.<field>` mention in a comment is not
# miscounted as a real signature read (nor a commented field as a struct member).
# Block comments collapse to their newlines so line boundaries are preserved and
# a multi-line comment can never join two declaration lines.
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def _strip_cpp_comments(text: str) -> str:
    text = _BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return _LINE_COMMENT_RE.sub("", text)


def _strip_disabled_blocks(text: str) -> str:
    """Blank out `#if 0` / `#if false` ... `#endif` regions (nesting-aware; a
    top-level `#else`/`#elif` re-enables) so compiled-out members or hash lines are
    not parsed as active. Line count is preserved so no lines merge."""
    out: list[str] = []
    disabled = False
    nest = 0
    for line in text.split("\n"):
        stripped = line.lstrip()
        if not disabled:
            if _IF_ZERO_RE.match(stripped):
                disabled = True
                nest = 0
                out.append("")
            else:
                out.append(line)
            continue
        if _IF_RE.match(stripped):
            nest += 1
        elif _ENDIF_RE.match(stripped):
            if nest == 0:
                disabled = False
            else:
                nest -= 1
        elif _ELSE_RE.match(stripped) and nest == 0:
            disabled = False
        out.append("")
    return "\n".join(out)


# Fields deliberately NOT hashed. Each maps to (reason, requires_hashed) where
# requires_hashed names the already-hashed field(s) that capture this field's
# output effect (empty when the field genuinely has no effect on the cached render).
WAIVERS: dict[str, tuple[str, tuple[str, ...]]] = {
    # Override flags only select the *source* of a resolved knob; the resolved knob
    # is hashed, so the flag itself carries no additional cache-relevant state
    # (see gpu_culler.cpp: `if (!<knob>_override) <knob> = <setting>;`).
    "lod_bias_override": ("override flag; the resolved lod_bias is hashed", ("lod_bias",)),
    "lod_min_screen_size_override": (
        "override flag; the resolved lod_min_screen_size is hashed",
        ("lod_min_screen_size",),
    ),
    "lod_max_distance_override": (
        "override flag; the resolved lod_max_distance is hashed",
        ("lod_max_distance",),
    ),
    "importance_cull_override": (
        "override flag; the resolved importance_cull_threshold is hashed",
        ("importance_cull_threshold",),
    ),
    "opacity_aware_culling_override": (
        "override flag; the resolved opacity_aware_culling is hashed",
        ("opacity_aware_culling",),
    ),
    "visibility_threshold_override": (
        "override flag; the resolved visibility_threshold is hashed",
        ("visibility_threshold",),
    ),
    # Memoized projections of already-hashed LOD inputs; recomputed from them.
    "lod_cached_min_screen_threshold": ("derived cache of lod_min_screen_size", ("lod_min_screen_size",)),
    "lod_cached_max_distance": ("derived cache of lod_max_distance", ("lod_max_distance",)),
    "lod_cached_max_distance_sq": ("derived cache (square) of lod_max_distance", ("lod_max_distance",)),
    # Recompute bookkeeping; no effect on the rendered/culled output.
    "lod_cache_dirty": ("recompute bookkeeping flag; no output effect", ()),
    "cull_params_dirty": ("recompute bookkeeping flag; no output effect", ()),
    # Auto-tuner reference baseline: set equal to the hashed importance_cull_threshold
    # (gpu_culler.cpp) and otherwise only mutated as transient overflow-auto-tune
    # state; not an independent input knob.
    "importance_cull_baseline": (
        "auto-tuner baseline tracking the hashed importance_cull_threshold",
        ("importance_cull_threshold",),
    ),
    # Viewport size is an independent OutputCompositor cache-key component
    # (can_reuse_cached_render compares cached_render_viewport_size directly,
    # interfaces/output_compositor.cpp); hashing it here would double-count it.
    "last_cull_viewport_size": (
        "viewport size is keyed independently in the OutputCompositor cache",
        (),
    ),
    # Dead field: set from the rendering/gaussian_splatting/lod/bias project setting
    # (gpu_culler.cpp) but never read anywhere; no output effect. Tracked for
    # wire-or-retire (production-readiness charter F6).
    "lod_project_bias": ("dead field; set from the lod/bias setting but never read", ()),
}


def _brace_body(text: str, anchor: str) -> str | None:
    """Return the `{...}` body (contents only) that follows `anchor` in `text`.

    Comments and `#if 0` regions are stripped from the whole text *before* the
    brace scan, so a `{` or `}` inside a comment or compiled-out block cannot
    mis-bound the body. Returns None if the anchor or a balanced body is not found.
    """
    stripped = _strip_disabled_blocks(_strip_cpp_comments(text))
    start = stripped.find(anchor)
    if start == -1:
        return None
    brace = stripped.find("{", start)
    if brace == -1:
        return None
    depth = 0
    for i in range(brace, len(stripped)):
        char = stripped[i]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return stripped[brace + 1 : i]
    return None


def _has_top_level_comma(line: str) -> bool:
    """True if `line` has a comma outside any (), [], or {} -- i.e. a second
    declarator (`float a = 0, b = 1;`), not a comma inside an initializer such as
    `Size2i(1280, 720)`."""
    depth = 0
    for char in line:
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth = max(0, depth - 1)
        elif char == "," and depth == 0:
            return True
    return False


def _has_content_after_first_statement(line: str) -> bool:
    """True if `line` has non-blank content after its first top-level `;` -- i.e. a
    second full declaration on the same line (`float a = 0; float b = 1;`) whose
    tail _FIELD_RE would not see. Semicolons inside (), [], {} do not count."""
    depth = 0
    for i, char in enumerate(line):
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth = max(0, depth - 1)
        elif char == ";" and depth == 0:
            return line[i + 1 :].strip() != ""
    return False


def _extract_struct_fields(text: str, struct_name: str) -> tuple[list[str], list[str]]:
    """Return (field_names, unrecognized_lines).

    Fail closed: any non-blank line in the struct body that is neither a parsed
    data member nor an access specifier is reported as unrecognized, so a member in
    unsupported syntax (attribute/macro/templated type, a method, static_assert,
    etc.) cannot be silently dropped -- it must be classified deliberately by
    extending this parser.
    """
    body = _brace_body(text, f"struct {struct_name}")
    if body is None:
        return [], []
    fields: list[str] = []
    unrecognized: list[str] = []
    for line in body.splitlines():
        stripped = line.strip()
        if not stripped or stripped in ("{", "}"):
            continue
        match = _FIELD_RE.match(line)
        if match:
            # _FIELD_RE only captures the first declarator up to the first `;`.
            # Reject a line that packs more than one member -- a second declarator
            # after a top-level comma (`float a = 0, b = 1;`) or a second statement
            # after a top-level `;` (`float a = 0; float b = 1;`) -- so the extra
            # members are not silently dropped. One member per line.
            if _has_top_level_comma(stripped) or _has_content_after_first_statement(stripped):
                unrecognized.append(stripped)
            else:
                fields.append(match.group(1))
            continue
        if _ACCESS_SPECIFIER_RE.match(stripped):
            continue
        unrecognized.append(stripped)
    return fields, unrecognized


def _extract_hashed_fields(text: str, fn_name: str) -> set[str] | None:
    body = _brace_body(text, fn_name)
    if body is None:
        return None
    # A field counts as hashed only when it appears in a statement that folds a
    # value into the running hash (`seed = _hash*(... config.<field> ...)`). A bare
    # read that never reaches `seed` (a debug/temp local, a log line) does not.
    hashed: set[str] = set()
    for line in body.splitlines():
        if _HASH_STMT_RE.search(line):
            hashed.update(_CONFIG_READ_RE.findall(line))
    return hashed


def _collect_failures(fields: list[str], field_set: set[str], hashed_fields: set[str]) -> list[str]:
    failures: list[str] = []

    # 1. Every field is hashed or waived.
    for field in fields:
        if field not in hashed_fields and field not in WAIVERS:
            failures.append(
                f"CullingConfig.{field} is neither hashed in {SIGNATURE_FN} nor waived; "
                f"add `config.{field}` to the signature (or waive it with a reason if it "
                f"cannot affect the cached render)"
            )

    # 2. No field is both hashed and waived (stale waiver).
    for field in WAIVERS:
        if field in hashed_fields:
            failures.append(
                f"CullingConfig.{field} is both hashed and listed in WAIVERS; remove it from WAIVERS"
            )

    # 3. No waiver names a field that no longer exists.
    for field in WAIVERS:
        if field not in field_set:
            failures.append(
                f"WAIVERS lists `{field}`, which is not a CullingConfig field; remove the stale waiver"
            )

    # 4. Each waiver's claimed resolved field(s) really are hashed.
    for field, (_reason, requires_hashed) in WAIVERS.items():
        for required in requires_hashed:
            if required not in hashed_fields:
                failures.append(
                    f"WAIVERS[{field}] claims its effect is captured by `{required}`, but "
                    f"`{required}` is not hashed in {SIGNATURE_FN}"
                )

    return failures


def main() -> int:
    if not CULLER_HEADER.is_file():
        print(f"[cull-signature-parity] FAIL missing {CULLER_HEADER.relative_to(ROOT)}")
        return 1
    if not SIGNATURE_SOURCE.is_file():
        print(f"[cull-signature-parity] FAIL missing {SIGNATURE_SOURCE.relative_to(ROOT)}")
        return 1

    fields, unrecognized = _extract_struct_fields(CULLER_HEADER.read_text(encoding="utf-8"), STRUCT_NAME)
    if not fields:
        print(f"[cull-signature-parity] FAIL could not parse struct {STRUCT_NAME} in {CULLER_HEADER.name}")
        return 1
    # Fail closed on any struct-body line the parser does not recognize, so a new
    # member in unsupported syntax cannot slip past the parity check unseen.
    if unrecognized:
        for line in unrecognized:
            print(
                f"[cull-signature-parity] FAIL unrecognized declaration in {STRUCT_NAME}: `{line}` "
                f"-- extend the parser (tests/ci/check_cull_signature_parity.py) to classify it"
            )
        return 1
    field_set = set(fields)

    hashed = _extract_hashed_fields(SIGNATURE_SOURCE.read_text(encoding="utf-8"), SIGNATURE_FN)
    if hashed is None:
        print(f"[cull-signature-parity] FAIL could not find {SIGNATURE_FN} in {SIGNATURE_SOURCE.name}")
        return 1
    # Only count reads of real struct fields (ignore any incidental config.* helper).
    hashed_fields = hashed & field_set

    failures = _collect_failures(fields, field_set, hashed_fields)
    if failures:
        for failure in failures:
            print(f"[cull-signature-parity] FAIL {failure}")
        print(
            f"[cull-signature-parity] {len(failures)} problem(s); "
            f"{len(hashed_fields)} hashed, {len(WAIVERS)} waived, {len(fields)} fields total."
        )
        return 1

    print(
        f"[cull-signature-parity] PASSED - all {len(fields)} CullingConfig fields are hashed "
        f"({len(hashed_fields)}) or explicitly waived ({len(WAIVERS)})."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
