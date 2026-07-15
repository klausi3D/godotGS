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
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules" / "gaussian_splatting"
REGISTER_TYPES = MODULE / "register_types.cpp"
DOC_CLASSES = MODULE / "doc_classes"

_REGISTER_RE = re.compile(r"\bGDREGISTER(?:_ABSTRACT|_INTERNAL|_RUNTIME)?_CLASS\(\s*(\w+)\s*\)")


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
        try:
            root = ET.parse(xml_path).getroot()
        except ET.ParseError as exc:
            failures.append(f"doc_classes/{class_name}.xml is not well-formed XML: {exc}")
            continue
        if root.get("name") != class_name:
            failures.append(
                f"doc_classes/{class_name}.xml declares class name `{root.get('name')}` != `{class_name}`"
            )
        brief = (root.findtext("brief_description") or "").strip()
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
