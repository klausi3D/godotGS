#!/usr/bin/env python3
"""Guard: every ClassDB-registered Gaussian Splatting class has a documented
doc_classes XML (present, well-formed, non-empty brief_description).

The module registers its editor/scripting classes via GDREGISTER_CLASS /
GDREGISTER_ABSTRACT_CLASS in register_types.cpp. Each such class must ship a
doc_classes/<Class>.xml with a real brief so the class reference is not blank in
the editor and docs. This locks in G6 (docs true) against a new class landing
without documentation.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules" / "gaussian_splatting"
REGISTER_TYPES = MODULE / "register_types.cpp"
DOC_CLASSES = MODULE / "doc_classes"

# Capture the leaf class name, tolerating a namespace qualifier — ClassDB and the
# doc XML are keyed by the leaf, so GDREGISTER_CLASS(GaussianSplatting::Foo) -> Foo.
_REGISTER_RE = re.compile(r"\bGDREGISTER(?:_ABSTRACT|_INTERNAL|_RUNTIME)?_CLASS\(\s*(?:\w+::)*(\w+)\s*\)")
# The doc build (doc_data.gen.h generation + class.xsd validation) is the authority
# on XML well-formedness; this guard only needs the class name and the brief text,
# so it scans with targeted regexes instead of an XML parser (avoids Bandit B314 on
# untrusted-XML parsing, which does not apply to these in-repo files either way).
_CLASS_NAME_RE = re.compile(r'<class\s+name="([^"]+)"')
_BRIEF_RE = re.compile(r"<brief_description>(.*?)</brief_description>", re.DOTALL)


def _registered_classes(text: str) -> set[str]:
    return set(_REGISTER_RE.findall(text))


def main() -> int:
    if not REGISTER_TYPES.is_file():
        print(f"[doc-classes-check] FAIL missing {REGISTER_TYPES.relative_to(ROOT)}")
        return 1

    registered = _registered_classes(REGISTER_TYPES.read_text(encoding="utf-8"))
    if not registered:
        print("[doc-classes-check] FAIL found no GDREGISTER_CLASS registrations to check")
        return 1

    failures: list[str] = []
    for class_name in sorted(registered):
        xml_path = DOC_CLASSES / f"{class_name}.xml"
        if not xml_path.is_file():
            failures.append(
                f"registered class `{class_name}` has no doc_classes/{class_name}.xml"
            )
            continue
        text = xml_path.read_text(encoding="utf-8")
        name_match = _CLASS_NAME_RE.search(text)
        if not name_match:
            failures.append(f"doc_classes/{class_name}.xml has no `<class name=...>` root element")
            continue
        if name_match.group(1) != class_name:
            failures.append(
                f"doc_classes/{class_name}.xml declares class name `{name_match.group(1)}` != `{class_name}`"
            )
        brief_match = _BRIEF_RE.search(text)
        brief = (brief_match.group(1).strip() if brief_match else "")
        if not brief:
            failures.append(
                f"doc_classes/{class_name}.xml has an empty <brief_description> (write a real one-line summary)"
            )

    if failures:
        for failure in failures:
            print(f"[doc-classes-check] FAIL {failure}")
        print(
            f"[doc-classes-check] {len(failures)} of {len(registered)} registered classes are undocumented or malformed."
        )
        return 1

    print(
        f"[doc-classes-check] PASSED — all {len(registered)} registered classes have a doc_classes XML with a non-empty brief."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
