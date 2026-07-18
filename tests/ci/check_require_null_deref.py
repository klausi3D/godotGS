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
* A "symbol" may be a member chain or a no-arg getter call, so
  `state.hierarchical_structure` and `loaded->get_gaussian_data()` are each one
  symbol. Calls WITH arguments are not: matching those textually would be
  comparing expressions, not tracking a symbol.
* Dereference: `x->`, `*x`, `x[`. Note `x.foo()` is NOT treated as a
  dereference - on a `Ref<T>` it is a safe call on the handle, and on a value
  type it is not a dereference at all.
* The forward scan stops at anything that changes reachability or the symbol:
  `if` / `for` / `while` / `switch` / `return` / `else`, a block boundary, or a
  reassignment of the symbol. But it checks that statement's **header** before
  stopping, because a control-flow statement guards its body, never its own
  condition:
    - `REQUIRE(x); if (x) { x->f(); }`        -> NOT flagged, the `if` makes it safe;
    - `REQUIRE(x); if (x->is_ready()) { … }`  -> FLAGGED, the condition dereferences
      before any guarding can happen, and a non-aborting `REQUIRE` did not stop
      us reaching it.
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
3. **Dereferences further away than the scan window.**
4. **Dereferences inside a control-flow BODY guarded by an unrelated condition**:
   `REQUIRE(ptr != nullptr); if (other) { ptr->f(); }` is a real crash, but the
   guard stops at the `if` because it cannot tell which conditions protect the
   symbol. Only the control-flow HEADER is checked - `if (ptr->is_ready())`
   evaluates the dereference before any guarding, so that IS flagged.
5. **Dereferences inside a macro body** that expands to one, and
   dereferences of a container's *element* (`v[0]->f()` after
   `REQUIRE(!v.is_empty())`).
6. **Any REQUIRE whose failure is harmful for a non-dereference reason** - e.g.
   `REQUIRE(count == 3);` followed by code that indexes past the end.

Reliable detection of (2), (4) and (6) needs real type and dataflow information, i.e.
a compiler plugin or a clang-tidy check, not a source scan. This guard is scoped
to the highest-confidence shape on purpose. It is a ratchet against the pattern
spreading, not a proof that the corpus is free of it: of the ~800 `REQUIRE*`
usages in the module tests, the null-ish subset alone is ~460, and most of those
are followed by something this guard cannot and should not judge.

## Scope boundary

Scanned: `modules/gaussian_splatting/tests/*.{h,cpp}` and the top level of
`tests/test_*.cpp`. **Not** scanned: the rest of the engine test tree
(`tests/core/`, `tests/servers/`, ...). Those are upstream Godot's tests; they run
under the same no-exceptions configuration and the same crash is possible there,
but policing upstream is out of this module's scope and would bury the module
signal under an unownable baseline. If a module-owned test is ever added under a
nested engine test directory, widen `_test_sources()` rather than assuming it is
covered.

## Baseline

The pattern predates the guard: 325 sites across 32 files match it today. #656 is
explicit that they must not be mass-rewritten, so `require_null_deref_baseline.json`
records a **fingerprint per site** and the guard fails on any change to that set.

A count-only baseline is not enough. It licenses a swap: fix one site the
prescribed way and add a brand-new one in the same file, and the count is
unchanged, so the guard reports "0 new" and the new crash ships. The fingerprint
set reports both the removed and the added site.

The fingerprint is (symbol, predicate form, hash of the dereferencing statement) -
deliberately NOT the line number, which would go stale on every unrelated edit
above it and train people to regenerate without reading, which is how a guard
becomes a formality. The FULL statement is hashed: hashing a truncation made
sites differing only past the cut collapse into one identity, silently weakening
the ratchet. Truncation is a display concern only (see `_elide`).

The ratchet only turns one way. A **removed** fingerprint also fails, with an
instruction to delete it from the baseline - so fixing sites tightens the guard
permanently instead of leaving slack for new ones to occupy.
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_TESTS_DIR = ROOT / "modules" / "gaussian_splatting" / "tests"
ENGINE_TESTS_DIR = ROOT / "tests"

# Pre-existing violations, so the guard can land without the 325-site rewrite
# #656 explicitly rules out. Tracking issue:
# https://github.com/klausi3D/godotGS/issues/656
#
# The baseline records a FINGERPRINT PER SITE, not a count per file. A count-only
# baseline licenses a swap: fix one site the prescribed way and add a brand-new
# one in the same file, and the count is unchanged, so the guard reports "0 new"
# and the new crash ships. A fingerprint set catches that -- the removed
# fingerprint and the added one are both reported.
#
# The fingerprint is (symbol, predicate form, hash of the dereferencing
# statement). Deliberately NOT the line number: a line-keyed baseline goes stale
# on every unrelated edit above it and trains people to regenerate without
# reading, which is how a guard becomes a formality. Renaming a variable does
# re-fingerprint its site; that surfaces as one removed + one added, which is
# accurate.
#
# This is a RATCHET: an added fingerprint fails (new violation), and a removed
# one also fails, telling you to drop it from the baseline. Never add a
# fingerprint to make a check pass - that is the one edit this file exists to
# prevent.
BASELINE_PATH = Path(__file__).resolve().parent / "require_null_deref_baseline.json"
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

# A C++ identifier, or a member chain reached through '.' / '->' that we treat as
# a single symbol (e.g. `state.hierarchical_structure`,
# `resource_state.buffer_manager`). Segments may end in a NO-ARG call, so a
# getter form like `loaded->get_gaussian_data()` is one symbol too - that shape
# occurs in the corpus (test_gaussian_splat_world_io.h:711) and was previously
# skipped entirely (Codex, PR #659). Arguments are deliberately not supported:
# matching `f(a, b)` textually would start comparing expressions, not symbols.
# The chain is matched greedily; regex backtracking peels the trailing
# `.is_valid()` / `.is_null()` back off in the predicate patterns below.
_SYMBOL = r"[A-Za-z_]\w*(?:\s*\(\s*\))?(?:\s*(?:\.|->)\s*[A-Za-z_]\w*(?:\s*\(\s*\))?)*"

# Null-ish REQUIRE forms. Each yields the symbol asserted to be non-null.
_NULLISH_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("!= nullptr", re.compile(rf"^\s*REQUIRE(?:_MESSAGE)?\s*\(\s*({_SYMBOL})\s*!=\s*nullptr\b")),
    ("!= NULL", re.compile(rf"^\s*REQUIRE(?:_MESSAGE)?\s*\(\s*({_SYMBOL})\s*!=\s*NULL\b")),
    ("nullptr !=", re.compile(rf"^\s*REQUIRE(?:_MESSAGE)?\s*\(\s*nullptr\s*!=\s*({_SYMBOL})\b")),
    ("is_valid()", re.compile(rf"^\s*REQUIRE(?:_MESSAGE|_UNARY)?\s*\(\s*({_SYMBOL})\s*(?:\.|->)\s*is_valid\s*\(\s*\)")),
    # doctest exposes REQUIRE_FALSE, REQUIRE_FALSE_MESSAGE and REQUIRE_UNARY_FALSE.
    # An earlier `REQUIRE_FALSE(?:_MESSAGE|_UNARY_FALSE)?` put the alternation on
    # the wrong side of the prefix: it accepted the NONEXISTENT
    # REQUIRE_FALSE_UNARY_FALSE and missed the real REQUIRE_UNARY_FALSE
    # (Codex, PR #659).
    ("!is_null()", re.compile(rf"^\s*(?:REQUIRE_FALSE(?:_MESSAGE)?|REQUIRE_UNARY_FALSE)\s*\(\s*({_SYMBOL})\s*(?:\.|->)\s*is_null\s*\(\s*\)")),
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


def _symbol_regex(symbol: str) -> str:
    """Build a regex matching `symbol`, tolerating whitespace and `.`/`->` in a chain.

    The two accessors are treated as interchangeable: a chain asserted as `a.b`
    and dereferenced as `a->b` is the same object either way, and refusing to
    match across them would be an under-report.
    """
    parts = [part for part in re.split(r"\s*(?:\.|->)\s*", symbol) if part]
    rendered = []
    for part in parts:
        if part.endswith(")"):
            name = part.split("(", 1)[0].strip()
            rendered.append(re.escape(name) + r"\s*\(\s*\)")
        else:
            rendered.append(re.escape(part))
    return r"\s*(?:\.|->)\s*".join(rendered)


def _derefs(symbol: str, statement: str) -> bool:
    """True when `statement` dereferences `symbol`.

    `symbol->`, `*symbol` and `symbol[` count. A trailing `symbol.` does NOT: on a
    Ref<T> that is a call on the handle itself, which is exactly what is safe.
    """
    sym = _symbol_regex(symbol)
    if re.search(rf"(?<![\w.>]){sym}\s*->", statement):
        return True
    if re.search(rf"(?<![\w)\]]){sym}\s*\[", statement):
        return True
    if re.search(rf"(?<![\w)\]])\*\s*{sym}\b", statement):
        return True
    return False


def _reassigns(symbol: str, statement: str) -> bool:
    """True when the statement looks like it rebinds the symbol."""
    sym = _symbol_regex(symbol)
    return re.search(rf"(?<![\w.>]){sym}\s*=(?!=)", statement) is not None


def _same_line_rest(line: str, line_no: int) -> list[tuple[int, str]]:
    """Statements sharing the REQUIRE's own line, after its terminating ';'.

    A doctest assertion macro cannot contain a bare ';', so splitting on the
    first one reliably ends the REQUIRE. A control-flow remainder is kept whole
    rather than split, since `for (a; b; c)` would otherwise be shredded.
    """
    if ";" not in line:
        return []
    rest = line.split(";", 1)[1]
    if not rest.strip():
        return []
    if _CONTROL_FLOW_RE.match(rest):
        return [(line_no, rest.strip())]
    return [
        (line_no, f"{fragment.strip()};") for fragment in rest.split(";") if fragment.strip()
    ]


def _statements(lines: list[str], start_index: int) -> list[tuple[int, str]]:
    """Yield (line_number, statement_text) for statements after start_index.

    A statement is accumulated until a ';' at depth 0, or until a line that opens
    or closes a block, which is emitted on its own so the caller can stop there.
    """
    statements: list[tuple[int, str]] = []
    buffer = ""
    buffer_line = 0
    depth = 0
    for offset in range(start_index, min(start_index + 60, len(lines))):
        raw = lines[offset]
        stripped = raw.strip()
        if not stripped:
            continue
        if not buffer:
            buffer_line = offset + 1
        buffer = f"{buffer} {stripped}".strip()
        # Depth tracking so the two ';' inside a MULTI-LINE `for (a; b; c)` header
        # do not each look like a statement terminator. Without it only the
        # initializer was emitted, the `for` matched as control flow, and the scan
        # broke before ever reading the condition - missing a dereference that is
        # evaluated before the loop body can guard anything (Codex, PR #659).
        # Literals are already blanked by _strip_comments, so no parenthesis here
        # can come from inside a string.
        depth = max(0, depth + stripped.count("(") - stripped.count(")"))
        if depth == 0 and (
            ";" in stripped or stripped.endswith("{") or stripped.endswith("}")
        ):
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
        # What follows the REQUIRE may be on the SAME line
        # (`REQUIRE(ptr != nullptr); ptr->method();`), so the remainder of this
        # line is scanned before moving on. Starting at index + 1 skipped it
        # entirely - and that one-liner is exactly the shape tests/AGENTS.md uses
        # to describe the bug (Codex, PR #659).
        for stmt_line, statement in _same_line_rest(line, index + 1) + _statements(
            lines, index + 1
        ):
            if _CONTROL_FLOW_RE.match(statement):
                # A control-flow statement guards its BODY, never its own
                # header. `if (ptr) { ptr->f(); }` is safe, but
                # `if (ptr->is_ready())` evaluates the dereference before any
                # guarding can happen - and a non-aborting REQUIRE did not stop
                # us getting here. So test the header, then stop either way
                # (the body is out of scope: we cannot tell what guards it).
                header = statement.split("{", 1)[0]
                if _derefs(symbol, header):
                    violations.append((index + 1, symbol, form, header.strip()))
                break
            if _derefs(symbol, statement):
                violations.append((index + 1, symbol, form, statement.strip()))
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


def _multiset_difference(left: list[str], right: list[str]) -> list[str]:
    """Elements of `left` not covered by `right`, honouring duplicates."""
    remaining = list(right)
    out: list[str] = []
    for item in left:
        if item in remaining:
            remaining.remove(item)
        else:
            out.append(item)
    return out


def _elide(text: str, limit: int) -> str:
    """Shorten for DISPLAY only. Never feed this to fingerprint()."""
    return text if len(text) <= limit else text[: limit - 1] + "…"


def fingerprint(symbol: str, form: str, statement: str) -> str:
    """Stable identity for one violation site, independent of its line number.

    Hashes the FULL statement. An earlier version hashed a 120-character
    truncation, so two sites differing only past column 120 collapsed to one
    fingerprint and the ratchet silently stopped distinguishing them (Codex,
    PR #659) - two test_lod_system.cpp query sites did exactly that. Truncation
    is a display concern; see _elide().
    """
    normalized = re.sub(r"\s+", " ", statement).strip()
    digest = hashlib.sha1(normalized.encode("utf-8")).hexdigest()[:10]
    return f"{symbol}|{form}|{digest}"


def scan_fingerprints() -> dict[str, list[str]]:
    """Basename -> sorted fingerprints of every violation in that file."""
    return {
        name: sorted(fingerprint(sym, form, stmt) for _, sym, form, stmt in violations)
        for name, violations in scan_all().items()
    }


def load_baseline() -> tuple[dict[str, list[str]], list[str]]:
    """Read the fingerprint baseline. A missing or malformed file is a FAILURE, never a pass."""
    if not BASELINE_PATH.is_file():
        return {}, [
            f"Baseline file missing: {BASELINE_PATH.name}. Refusing to treat an absent "
            f"baseline as 'nothing to report'."
        ]
    try:
        data = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return {}, [f"Baseline file is not valid JSON: {exc}"]
    if not isinstance(data, dict) or not isinstance(data.get("files"), dict):
        return {}, ["Baseline file must be an object with a 'files' object."]
    out: dict[str, list[str]] = {}
    for name, prints in data["files"].items():
        if not isinstance(prints, list) or not all(isinstance(p, str) for p in prints):
            return {}, [f"Baseline entry '{name}' must be a list of fingerprint strings."]
        out[name] = sorted(prints)
    return out, []


def main() -> int:
    files = _test_sources()
    if not files:
        print("[require-null-deref] FAIL no test sources found - the scan is broken.")
        return 1

    found = scan_all()
    found_prints = scan_fingerprints()
    baseline, failures = load_baseline()
    total = sum(len(v) for v in found.values())
    # line lookup so a report can point at the source even though the baseline
    # itself is line-independent.
    where = {
        name: {
            fingerprint(sym, form, stmt): (line_no, sym, form, stmt)
            for line_no, sym, form, stmt in violations
        }
        for name, violations in found.items()
    }

    for name in sorted(set(found_prints) | set(baseline)):
        actual = found_prints.get(name, [])
        allowed = baseline.get(name, [])
        added = _multiset_difference(actual, allowed)
        removed = _multiset_difference(allowed, actual)
        if added:
            failures.append(f"{name}: {len(added)} NEW REQUIRE-then-dereference site(s):")
            for print_ in added:
                line_no, symbol, form, statement = where[name][print_]
                failures.append(
                    f"    line {line_no}: REQUIRE({symbol} {form}) then `{_elide(statement, 90)}`"
                )
            failures.append(
                f"    REQUIRE does not abort in this build (DOCTEST_CONFIG_NO_EXCEPTIONS): on "
                f"failure it reports and CONTINUES, so the dereference runs on null and crashes "
                f"the whole test binary, taking every later case with it. Write instead: "
                f"if (!<symbol>) {{ FAIL(\"...\"); return; }}  ({BASELINE_ISSUE})"
            )
        if removed:
            failures.append(
                f"{name}: {len(removed)} baselined site(s) no longer found - the ratchet must "
                f"tighten. Remove these from {BASELINE_PATH.name} so the slack cannot be "
                f"reoccupied by a new violation:"
            )
            for print_ in removed:
                failures.append(f"    {print_}")

    if failures:
        print(f"[require-null-deref] FAIL {total} site(s) found across {len(found)} file(s).")
        for failure in failures:
            print(f"  - {failure}" if not failure.startswith("    ") else failure)
        return 1

    print(
        f"[require-null-deref] PASS {len(files)} test source(s) scanned; "
        f"{total} baselined site(s) across {len(found)} file(s), 0 new, 0 stale."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
