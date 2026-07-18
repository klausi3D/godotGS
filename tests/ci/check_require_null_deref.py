#!/usr/bin/env python3
"""Guard: no `REQUIRE(<null-ish>)` is followed by a dereference of the same symbol (#656).

## The failure this guards against

`REQUIRE` does **not** abort a test case in this build. It reports and execution
continues into the next statement.

The mechanism is explicit in the build, not incidental. Both `tests/SCsub` and
`modules/gaussian_splatting/SCsub` define
`DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS` when `disable_exceptions` is
set (it defaults to `True` in `SConstruct`). That macro makes doctest define
`DOCTEST_CONFIG_NO_EXCEPTIONS`, under which:

    // thirdparty/doctest/doctest.h
    #else // DOCTEST_CONFIG_NO_EXCEPTIONS
        void throwException() {}

`REQUIRE`'s abort path is that `throwException()`. Compiled to nothing, `REQUIRE`
degrades into a louder `CHECK`. (The `_BUT_WITH_ALL_ASSERTS` half is what keeps
`REQUIRE` compiling at all: without it doctest `#undef`s `REQUIRE` and replaces it
with a `static_assert(false)`, so the alternative to "silently does not abort" is
"does not build".)

So this:

    REQUIRE(ptr != nullptr);
    ptr->method();

does not fail one test case. It segfaults the whole test binary, and every case
after it never runs. doctest reports no result for them, so the run ends up
*shorter* rather than *red* - and "fewer cases ran" is not a signal anyone alarms
on, especially alongside lanes that already silently skip (#520, #329).

The correct pattern is an explicit guard:

    if (!ptr) {
        FAIL("<what was missing and why the case cannot continue>");
        return;
    }

## What this guard flags (deliberately narrow)

Precision over recall. A null-ish `REQUIRE*` on a symbol, followed - within a
short forward window - by a statement that **dereferences that same symbol**,
where nothing in between could have made the dereference safe.

* Null-ish predicates: `x != nullptr`, `x != NULL`, `x.is_valid()`,
  `x->is_valid()`, `REQUIRE_FALSE(x.is_null())`, `REQUIRE_NE(x, nullptr)`.
* Dereference: `x->`, `*x`, `x[`. Note `x.foo()` is NOT treated as a
  dereference - on a `Ref<T>` it is a safe call on the handle, and on a value
  type it is not a dereference at all.
* The forward scan stops at anything that changes reachability or the symbol:
  `if` / `for` / `while` / `switch` / `return` / `else`, a block boundary, or a
  reassignment of the symbol. `REQUIRE(x); if (x) { x->f(); }` is therefore not
  flagged - the `if` makes it safe.
* The scan crosses other assertion macros (the real corpus writes
  `REQUIRE(a); REQUIRE(b); a->f();`), and flags them if they themselves
  dereference the symbol.

## What this guard deliberately does NOT catch

Stated plainly, because a guard whose blind spots are undocumented invites
exactly the false confidence #656 is about:

1. **Bare `REQUIRE(x);`** with no comparison. It is indistinguishable from a
   boolean assertion without type information, and the corpus uses it for both.
2. **Dereferences through an alias.** `REQUIRE(a != nullptr); T *b = a; b->f();`
   is a real crash this guard does not see - it tracks one symbol, not
   assignment flow.
3. **Dereferences further away than the scan window**, or after a control-flow
   statement that does not actually make the dereference safe.
4. **Dereferences inside a macro body** that expands to one, and
   dereferences of a container's *element* (`v[0]->f()` after
   `REQUIRE(!v.is_empty())`).
5. **Any REQUIRE whose failure is harmful for a non-dereference reason** - e.g.
   `REQUIRE(count == 3);` followed by code that indexes past the end.

Reliable detection of (2) and (5) needs real type and dataflow information, i.e.
a compiler plugin or a clang-tidy check, not a source scan. This guard is scoped
to the highest-confidence shape on purpose. It is a ratchet against the pattern
spreading, not a proof that the corpus is free of it: of the ~800 `REQUIRE*`
usages in the module tests, the null-ish subset alone is ~460, and most of those
are followed by something this guard cannot and should not judge.

## Baseline

The pattern predates the guard: 315 sites across 32 files match it today. #656 is
explicit that they must not be mass-rewritten, so `BASELINE` records a per-file
**count** and the guard fails when a file exceeds it.

The baseline is a count per file, not a list of line numbers, on purpose: a
line-keyed baseline would go stale on every unrelated edit to a test file and
train people to regenerate it without reading it, which is how a guard becomes a
formality.

The ratchet only turns one way. A file **below** its baseline also fails, with an
instruction to lower the number - so fixing sites tightens the guard permanently
instead of leaving slack for new ones to occupy.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_TESTS_DIR = ROOT / "modules" / "gaussian_splatting" / "tests"
ENGINE_TESTS_DIR = ROOT / "tests"

# Pre-existing violations per file (basename -> count), so the guard can land
# without the 315-site rewrite #656 explicitly rules out. Tracking issue:
# https://github.com/klausi3D/godotGS/issues/656
#
# This is a RATCHET: a file over its number fails (new violation), and a file
# under its number also fails (lower the number). Never raise an entry to make a
# check pass - that is the one edit this file exists to prevent.
BASELINE: dict[str, int] = {
    "test_batched_async_readback.h": 2,
    "test_config_validation.h": 3,
    "test_data_authority_hardening.h": 3,
    "test_debug_hud_lifecycle.h": 6,
    "test_diagnostics.h": 3,
    "test_gaussian_data.h": 1,
    "test_gaussian_importer.h": 19,
    "test_gaussian_splat_asset_prune.h": 2,
    "test_gaussian_splat_container.h": 6,
    "test_gaussian_splat_node.h": 66,
    "test_gaussian_splat_world_io.h": 7,
    "test_gaussian_streaming_lifecycle.cpp": 4,
    "test_gpu_culler_hierarchy.h": 1,
    "test_gpu_sorting_pipeline_readback.h": 3,
    "test_gpu_streaming.cpp": 3,
    "test_gpu_streaming.h": 1,
    "test_lod_system.cpp": 10,
    "test_node_bootstrap.h": 2,
    "test_node_surface_cleanup.h": 4,
    "test_output_compositor_composite_hazard.h": 3,
    "test_painterly_material.cpp": 2,
    "test_ply_importer.h": 29,
    "test_renderer_lifetime_proof.h": 3,
    "test_renderer_pipeline.h": 6,
    "test_scene_director_asset_id_collision.h": 8,
    "test_scene_director_submission_scaffolding.h": 85,
    "test_sentinel_tier_defaults.h": 14,
    "test_shadow_instance_subset.h": 6,
    "test_shadow_pass_isolation.h": 2,
    "test_spz_importer.h": 4,
    "test_vram_budget_regulator.h": 6,
    "visual_compare.h": 1,
}
BASELINE_ISSUE = "https://github.com/klausi3D/godotGS/issues/656"

# How many statements to look ahead after the REQUIRE before giving up.
_SCAN_STATEMENTS = 6

# A doctest assertion macro that we scan THROUGH (it does not change reachability).
_ASSERT_MACRO_RE = re.compile(
    r"^\s*(?:REQUIRE|CHECK|WARN|INFO|MESSAGE|CAPTURE)\w*\s*\(", re.IGNORECASE
)
# Statements that change reachability or scope: stop the scan (fail-safe: we
# would rather miss a violation than report a guarded dereference).
_CONTROL_FLOW_RE = re.compile(
    r"^\s*(?:\}|\{|if\b|else\b|for\b|while\b|switch\b|case\b|default\s*:|return\b|"
    r"break\b|continue\b|do\b|try\b|catch\b|SUBCASE\b|TEST_CASE\b)"
)

# A C++ identifier, optionally reached through a member chain we treat as one
# symbol (e.g. `node->renderer`). Kept simple on purpose.
_SYMBOL = r"[A-Za-z_]\w*"

# Null-ish REQUIRE forms. Each yields the symbol asserted to be non-null.
_NULLISH_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("!= nullptr", re.compile(rf"^\s*REQUIRE(?:_MESSAGE)?\s*\(\s*({_SYMBOL})\s*!=\s*nullptr\b")),
    ("!= NULL", re.compile(rf"^\s*REQUIRE(?:_MESSAGE)?\s*\(\s*({_SYMBOL})\s*!=\s*NULL\b")),
    ("nullptr !=", re.compile(rf"^\s*REQUIRE(?:_MESSAGE)?\s*\(\s*nullptr\s*!=\s*({_SYMBOL})\b")),
    ("is_valid()", re.compile(rf"^\s*REQUIRE(?:_MESSAGE|_UNARY)?\s*\(\s*({_SYMBOL})\s*(?:\.|->)\s*is_valid\s*\(\s*\)")),
    ("!is_null()", re.compile(rf"^\s*REQUIRE_FALSE(?:_MESSAGE|_UNARY_FALSE)?\s*\(\s*({_SYMBOL})\s*(?:\.|->)\s*is_null\s*\(\s*\)")),
    ("REQUIRE_NE nullptr", re.compile(rf"^\s*REQUIRE_NE\s*\(\s*({_SYMBOL})\s*,\s*nullptr\s*\)")),
    ("REQUIRE_NE nullptr", re.compile(rf"^\s*REQUIRE_NE\s*\(\s*nullptr\s*,\s*({_SYMBOL})\s*\)")),
)


def _strip_comments(text: str) -> str:
    """Remove comments and blank out string/char literals, LINE BY LINE.

    Deliberately line-oriented: every input line maps to exactly one output line,
    so a reported line number cannot drift no matter what the file contains. (An
    earlier character-stream version of this function silently lost a newline on
    an unterminated char literal and shifted every subsequent report by one -
    precisely the kind of quiet miscount this guard exists to prevent.)

    Literals are replaced by empty ones rather than deleted so a `->` inside a
    message string cannot read as a dereference.
    """
    lines_out: list[str] = []
    in_block = False
    for line in text.split("\n"):
        out: list[str] = []
        i = 0
        n = len(line)
        while i < n:
            if in_block:
                end = line.find("*/", i)
                if end == -1:
                    i = n
                else:
                    in_block = False
                    i = end + 2
                continue
            if line.startswith("//", i):
                break
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            ch = line[i]
            if ch in ('"', "'"):
                # Find the closing quote on THIS line, honouring backslash escapes.
                j = i + 1
                closed = False
                while j < n:
                    if line[j] == "\\":
                        j += 2
                        continue
                    if line[j] == ch:
                        closed = True
                        break
                    j += 1
                if closed:
                    out.append(ch * 2)
                    i = j + 1
                else:
                    # Not a literal (digit separator like 1'000, stray apostrophe,
                    # or a raw/multi-line string). Keep the character verbatim
                    # rather than swallowing the rest of the line.
                    out.append(ch)
                    i += 1
                continue
            out.append(ch)
            i += 1
        lines_out.append("".join(out))
    return "\n".join(lines_out)


def _derefs(symbol: str, statement: str) -> bool:
    """True when `statement` dereferences `symbol`.

    `symbol->`, `*symbol` and `symbol[` count. `symbol.` does NOT: on a Ref<T>
    that is a call on the handle itself, which is exactly what is safe.
    """
    sym = re.escape(symbol)
    if re.search(rf"(?<![\w.>]){sym}\s*->", statement):
        return True
    if re.search(rf"(?<![\w)\]]){sym}\s*\[", statement):
        return True
    if re.search(rf"(?<![\w)\]])\*\s*{sym}\b", statement):
        return True
    return False


def _reassigns(symbol: str, statement: str) -> bool:
    """True when the statement looks like it rebinds the symbol."""
    sym = re.escape(symbol)
    return re.search(rf"(?<![\w.>]){sym}\s*=(?!=)", statement) is not None


def _statements(lines: list[str], start_index: int) -> list[tuple[int, str]]:
    """Yield (line_number, statement_text) for statements after start_index.

    A statement is accumulated until a ';' at depth 0, or until a line that opens
    or closes a block, which is emitted on its own so the caller can stop there.
    """
    statements: list[tuple[int, str]] = []
    buffer = ""
    buffer_line = 0
    for offset in range(start_index, min(start_index + 60, len(lines))):
        raw = lines[offset]
        stripped = raw.strip()
        if not stripped:
            continue
        if not buffer:
            buffer_line = offset + 1
        buffer = f"{buffer} {stripped}".strip()
        if ";" in stripped or stripped.endswith("{") or stripped.endswith("}"):
            statements.append((buffer_line, buffer))
            buffer = ""
            if len(statements) >= _SCAN_STATEMENTS:
                break
    return statements


def _scan_file(path: Path) -> list[tuple[int, str, str, str]]:
    """Return (line, symbol, form, statement) for each violation in the file."""
    text = _strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    lines = text.splitlines()
    violations: list[tuple[int, str, str, str]] = []

    for index, line in enumerate(lines):
        symbol = None
        form = ""
        for form_name, pattern in _NULLISH_PATTERNS:
            match = pattern.match(line)
            if match:
                symbol = match.group(1)
                form = form_name
                break
        if symbol is None:
            continue
        # The REQUIRE itself may dereference nothing; scan what follows it.
        for stmt_line, statement in _statements(lines, index + 1):
            if _CONTROL_FLOW_RE.match(statement):
                break
            if _derefs(symbol, statement):
                violations.append((index + 1, symbol, form, statement.strip()[:120]))
                break
            if _ASSERT_MACRO_RE.match(statement):
                continue
            if _reassigns(symbol, statement):
                break
    return violations


def _test_sources() -> list[Path]:
    return sorted(
        list(MODULE_TESTS_DIR.glob("*.h"))
        + list(MODULE_TESTS_DIR.glob("*.cpp"))
        + list(ENGINE_TESTS_DIR.glob("test_*.cpp"))
    )


def scan_all() -> dict[str, list[tuple[int, str, str, str]]]:
    """Basename -> violations, for every test source that has any."""
    results: dict[str, list[tuple[int, str, str, str]]] = {}
    for path in _test_sources():
        violations = _scan_file(path)
        if violations:
            results[path.name] = violations
    return results


def main() -> int:
    files = _test_sources()
    if not files:
        print("[require-null-deref] FAIL no test sources found - the scan is broken.")
        return 1

    found = scan_all()
    failures: list[str] = []
    total = sum(len(v) for v in found.values())

    for name in sorted(set(found) | set(BASELINE)):
        actual = len(found.get(name, []))
        allowed = BASELINE.get(name, 0)
        if actual > allowed:
            failures.append(
                f"{name}: {actual} REQUIRE-then-dereference site(s), baseline allows "
                f"{allowed}. New site(s):"
            )
            for line_no, symbol, form, statement in found[name][: actual - allowed + 2]:
                failures.append(
                    f"    line {line_no}: REQUIRE({symbol} {form}) then `{statement[:90]}`"
                )
            failures.append(
                f"    REQUIRE does not abort in this build (DOCTEST_CONFIG_NO_EXCEPTIONS): on "
                f"failure it reports and CONTINUES, so the dereference runs on null and crashes "
                f"the whole test binary, taking every later case with it. Write instead: "
                f"if (!<symbol>) {{ FAIL(\"...\"); return; }}  ({BASELINE_ISSUE})"
            )
        elif actual < allowed:
            failures.append(
                f"{name}: {actual} site(s) remain but the baseline still allows {allowed}. "
                f"Sites were fixed - lower BASELINE[\"{name}\"] to {actual} "
                f"({'remove the entry' if actual == 0 else 'tighten the ratchet'}) so the "
                f"slack cannot be reoccupied."
            )

    if failures:
        print(f"[require-null-deref] FAIL {total} site(s) found across {len(found)} file(s).")
        for failure in failures:
            print(f"  - {failure}" if not failure.startswith("    ") else failure)
        return 1

    print(
        f"[require-null-deref] PASS {len(files)} test source(s) scanned; "
        f"{total} baselined site(s) across {len(found)} file(s), 0 new."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
