#!/usr/bin/env python3
"""Guard: `GPUSorterFactory::capture_radix_build_signature()` must cover every piece of live
sorting configuration the sorter build actually reads.

Why this exists (#586 round-9)
------------------------------
Round 7 gave the renderer a DETERMINISTIC sorter-creation latch: a build failure that cannot
change while the device and the configuration are fixed is not retried, because retrying it is
a shader recompile every saturated-backoff interval forever. Codex's round-9 review found the
other half of that sentence missing -- the latch also survived the user CORRECTING the
configuration, and worse, `TileGlobalSortResources::ensure_resources()` consumed the change
signal on the way past (`key_config = desired_key_config` in the no-retry branch), so the
correction could never take effect and every translucent frame stayed rejected until renderer
teardown.

The fix releases the latch when the BUILD INPUTS change:
`sorter_unavailable_build_signature` records what the failed build read, and a differing
signature drops the latch exactly once. That makes this guard's invariant load-bearing in both
directions:

  * a build input MISSING from the signature => the user corrects that setting, the signature
    does not move, the latch holds, and the dead end round 9 fixed is back -- silently.
  * a NON-input included in the signature => an unrelated project-settings edit costs a full
    create_sorter() and re-latch, i.e. the build storm rounds 2 and 7 refused.

Round 2 already rejected mirroring "capability-affecting settings" into the resource state,
because a hand-written list is correct only until the code grows a new input and rots silently
when it does. That objection applies to this signature too -- so the list is not trusted, it
is CHECKED against ground truth here.

What is derived
---------------
Everything.

  * **read** -- every `g_gpu_sorting_config.<field>` in gpu_sorter.cpp, including reads through
    the `const GPUSortingConfig &config = g_gpu_sorting_config;` aliases the probes use. Alias
    reads are bound to the alias's own enclosing block AND cross-checked against the real
    member list parsed from gpu_sorting_config.h, so an unrelated local called `config` (the
    AUTO-threshold struct in this same file has `config.radix_max_elements`) cannot be
    mistaken for a sorting-config read.
  * **captured** -- the fields `capture_radix_build_signature()` assigns into the signature.
  * **members** -- the fields of `GaussianSplatting::SorterBuildSignature`, parsed from
    sort_fallback_policy.h. Every member must be assigned, or the struct carries a field the
    capture leaves at its default and the comparison silently ignores it.

Whole-file scope is deliberate and is the safe direction. If a config read is added to a
function in gpu_sorter.cpp that the RADIX build path never reaches, this guard will still
demand it be captured; the cost of that over-inclusion is at most one extra build attempt per
configuration edit, whereas the cost of guessing which functions the build path reaches -- and
guessing wrong -- is the silent dead end above. Narrowing the scope to a list of "build-path
functions" would reintroduce exactly the hand-written list this guard exists to avoid.

Wiring
------
A signature nothing records or compares is a signature that guards nothing, so the guard also
requires `tile_render_resources.cpp` to still WRITE `sorter_unavailable_build_signature` and to
still COMPARE it. Both are single lines that a refactor can drop without any test noticing.

Fail-closed behaviour
---------------------
  * A file, function, struct or member list this guard cannot parse is a FAILURE, never
    "nothing to check".
  * The counts are PINNED: a derived guard whose input set has shrunk to nothing still prints
    PASSED, so a change in how many config fields are covered has to be a visible, reviewed
    edit here.

Do not silence this guard by narrowing the scope or lowering the pins to make a change pass.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / "modules" / "gaussian_splatting" / "renderer"
SORTER_SOURCE = RENDERER / "gpu_sorter.cpp"
CONFIG_HEADER = RENDERER / "gpu_sorting_config.h"
POLICY_HEADER = RENDERER / "sort_fallback_policy.h"
RESOURCES_SOURCE = RENDERER / "tile_render_resources.cpp"

CONFIG_GLOBAL = "g_gpu_sorting_config"
CONFIG_STRUCT = "GPUSortingConfig"
SIGNATURE_STRUCT = "SorterBuildSignature"
CAPTURE_ANCHOR = "GPUSorterFactory::capture_radix_build_signature("
SIGNATURE_FIELD = "sorter_unavailable_build_signature"

# Pins. Bump these only together with a real change to what the build reads or records.
# Seven today: radix_bits + workgroup_size (RadixSort::is_supported / initialize),
# subgroup_prefix_mode (_subgroup_prefix_forced_off), and the four key-layout fields
# SortKeyConfig::from_settings() reads. The last four are captured through the EFFECTIVE key
# config the renderer would build with rather than re-read raw, because
# TileRenderer::_get_effective_sort_key_config() can promote 32-bit keys back to 64-bit.
EXPECTED_CONFIG_FIELDS = 7
EXPECTED_SIGNATURE_MEMBERS = 7

_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")
_ALIAS_RE = re.compile(rf"const\s+{CONFIG_STRUCT}\s*&\s*(\w+)\s*=\s*{CONFIG_GLOBAL}\s*;")
# A member declaration in a plain-old-data struct: `<type> <name> = <init>;` or `<type> <name>;`
_MEMBER_RE = re.compile(
    r"^\s*(?:static\s+)?(?:const\s+)?(?:unsigned\s+)?(?:uint\d+_t|int\d+_t|uint32_t|uint8_t|"
    r"int|bool|float|double|String)\s+(\w+)\s*(?:=[^;]*)?;",
    re.MULTILINE,
)


def _strip_cpp_comments(text: str) -> str:
    text = _BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return _LINE_COMMENT_RE.sub("", text)


def _struct_body(text: str, anchor: str) -> str | None:
    """The `{...}` body of `anchor` (a struct header or a function signature)."""
    stripped = _strip_cpp_comments(text)
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


def _enclosing_block(text: str, index: int) -> str:
    """The remainder of the block that encloses `index`, i.e. the alias's scope."""
    depth = 0
    for i in range(index, len(text)):
        char = text[i]
        if char == "{":
            depth += 1
        elif char == "}":
            if depth == 0:
                return text[index:i]
            depth -= 1
    return text[index:]


def _config_members(header_text: str) -> set[str]:
    body = _struct_body(header_text, f"struct {CONFIG_STRUCT}")
    if body is None:
        return set()
    return set(_MEMBER_RE.findall(body))


def _signature_members(header_text: str) -> list[str]:
    body = _struct_body(header_text, f"struct {SIGNATURE_STRUCT}")
    if body is None:
        return []
    # Stop at the first member function: operator==/!= bodies mention the members again.
    cut = body.find("bool operator")
    if cut != -1:
        body = body[:cut]
    return _MEMBER_RE.findall(body)


def _config_reads(source_text: str, config_members: set[str]) -> set[str]:
    """Every GPUSortingConfig field gpu_sorter.cpp reads, directly or through an alias."""
    stripped = _strip_cpp_comments(source_text)
    reads: set[str] = set()

    def _collect(text: str, holder: str) -> None:
        for match in re.finditer(rf"\b{re.escape(holder)}\s*\.\s*(\w+)\s*(\()?", text):
            name, call = match.group(1), match.group(2)
            if call:
                continue  # a method, not a field
            if name in config_members:
                reads.add(name)

    _collect(stripped, CONFIG_GLOBAL)
    for alias in _ALIAS_RE.finditer(stripped):
        # Bound to the alias's own block, so a same-named local elsewhere in the file cannot
        # contribute reads that the sorting config never had.
        _collect(_enclosing_block(stripped, alias.end()), alias.group(1))
    return reads


def _captured_fields(source_text: str, config_members: set[str]) -> tuple[set[str], list[str], str]:
    """(config fields captured, signature members assigned, error)."""
    body = _struct_body(source_text, CAPTURE_ANCHOR)
    if body is None:
        return set(), [], (
            f"could not parse `{CAPTURE_ANCHOR}` in {SORTER_SOURCE.name}. The deterministic "
            f"sorter latch is released by comparing what this function returns, so a rename "
            f"must update CAPTURE_ANCHOR here -- do not drop the check."
        )
    assigned: list[str] = []
    captured: set[str] = set()
    for match in re.finditer(r"\bsignature\s*\.\s*(\w+)\s*=\s*([^;]*);", body):
        assigned.append(match.group(1))
        for read in re.finditer(r"\b(\w+)\s*\.\s*(\w+)\b", match.group(2)):
            if read.group(2) in config_members:
                captured.add(read.group(2))
    return captured, assigned, ""


def main() -> int:  # noqa: PLR0911, PLR0912 -- linear fail-closed checks
    prefix = "[sorter-build-signature-parity]"
    for path in (SORTER_SOURCE, CONFIG_HEADER, POLICY_HEADER, RESOURCES_SOURCE):
        if not path.is_file():
            print(f"{prefix} FAIL missing {path.relative_to(ROOT)}")
            return 1

    config_members = _config_members(CONFIG_HEADER.read_text(encoding="utf-8"))
    if len(config_members) < 10:
        print(
            f"{prefix} FAIL parsed only {len(config_members)} member(s) of {CONFIG_STRUCT} from "
            f"{CONFIG_HEADER.name}; the member parse has drifted, and with it every 'is this a "
            f"config read' decision below."
        )
        return 1

    policy_text = POLICY_HEADER.read_text(encoding="utf-8")
    signature_members = _signature_members(policy_text)
    if len(signature_members) != EXPECTED_SIGNATURE_MEMBERS:
        print(
            f"{prefix} FAIL parsed {len(signature_members)} member(s) of {SIGNATURE_STRUCT} "
            f"({', '.join(signature_members) or 'none'}), pinned at "
            f"{EXPECTED_SIGNATURE_MEMBERS}. Adding or removing a build input is a deliberate "
            f"change to what releases the deterministic latch: update the pin in the same diff."
        )
        return 1

    source_text = SORTER_SOURCE.read_text(encoding="utf-8")
    reads = _config_reads(source_text, config_members)
    captured, assigned, error = _captured_fields(source_text, config_members)
    if error:
        print(f"{prefix} FAIL {error}")
        return 1

    problems: list[str] = []

    missing = sorted(reads - captured)
    if missing:
        problems.append(
            f"{CAPTURE_ANCHOR}) does not capture {', '.join(missing)}, which "
            f"{SORTER_SOURCE.name} reads out of {CONFIG_GLOBAL}. A build input the signature "
            f"cannot see is a setting the user can correct without the deterministic sorter "
            f"latch ever noticing: the sorter stays disabled and every translucent frame stays "
            f"rejected until renderer teardown (#586 round-9). Capture it, or -- if it truly "
            f"cannot affect a build -- move the read out of this translation unit."
        )

    unassigned = sorted(set(signature_members) - set(assigned))
    if unassigned:
        problems.append(
            f"{SIGNATURE_STRUCT} member(s) {', '.join(unassigned)} are never assigned by "
            f"{CAPTURE_ANCHOR}), so they keep their default in every capture and the "
            f"comparison that releases the latch is blind to them."
        )
    duplicated = sorted({name for name in assigned if assigned.count(name) > 1})
    if duplicated:
        problems.append(
            f"{SIGNATURE_STRUCT} member(s) {', '.join(duplicated)} are assigned more than once "
            f"in {CAPTURE_ANCHOR}); only the last write survives, so one of the inputs is being "
            f"discarded silently."
        )

    if len(reads) != EXPECTED_CONFIG_FIELDS:
        problems.append(
            f"covered config-field count changed: {len(reads)} field(s) read "
            f"({', '.join(sorted(reads)) or 'none'}), pinned at {EXPECTED_CONFIG_FIELDS}. A "
            f"guard whose input set has shrunk still prints PASSED, so this has to be a "
            f"deliberate edit: update the pin in the same diff that adds or removes a read."
        )

    resources_text = _strip_cpp_comments(RESOURCES_SOURCE.read_text(encoding="utf-8"))
    if not re.search(rf"{SIGNATURE_FIELD}\s*=", resources_text):
        problems.append(
            f"{RESOURCES_SOURCE.name} never assigns `{SIGNATURE_FIELD}`, so nothing records "
            f"what a failed build read and the latch can never be released."
        )
    if not re.search(rf"!=\s*{SIGNATURE_FIELD}|{SIGNATURE_FIELD}\s*!=", resources_text):
        problems.append(
            f"{RESOURCES_SOURCE.name} never COMPARES `{SIGNATURE_FIELD}`, so the signature is "
            f"recorded and then ignored -- the #586 round-9 dead end with extra bookkeeping."
        )

    if problems:
        for problem in problems:
            print(f"{prefix} FAIL {problem}")
        print(f"{prefix} {len(problems)} problem(s).")
        return 1

    print(
        f"{prefix} PASSED - capture_radix_build_signature() captures all "
        f"{len(reads)} {CONFIG_STRUCT} field(s) {SORTER_SOURCE.name} reads "
        f"({', '.join(sorted(reads))}) and assigns all {len(signature_members)} "
        f"{SIGNATURE_STRUCT} member(s); {RESOURCES_SOURCE.name} both records and compares it."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
