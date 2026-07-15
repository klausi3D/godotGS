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
import xml.parsers.expat  # low-level well-formedness check; not the ElementTree API Bandit B314 warns about
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules" / "gaussian_splatting"
REGISTER_TYPES = MODULE / "register_types.cpp"
DOC_CLASSES = MODULE / "doc_classes"
CONFIG_PY = MODULE / "config.py"

# Capture the leaf class name, tolerating a namespace qualifier - ClassDB and the
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


def _well_formed_error(text: str) -> str | None:
    # Reject a malformed doc XML (e.g. an unclosed tag) before accepting it, even
    # though the editor doc build is the ultimate authority on validity.
    parser = xml.parsers.expat.ParserCreate()
    try:
        parser.Parse(text, True)
    except xml.parsers.expat.ExpatError as exc:
        return str(exc)
    return None


def _doc_class_list(text: str) -> list[str]:
    # The class names inside config.py's get_doc_classes() return [...] literal;
    # SConstruct embeds only these XMLs into the editor docs.
    match = re.search(r"def get_doc_classes\(\):.*?return\s*\[(.*?)\]", text, re.DOTALL)
    if not match:
        return []
    return re.findall(r'"(\w+)"', match.group(1))


def main() -> int:
    if not REGISTER_TYPES.is_file():
        print(f"[doc-classes-check] FAIL missing {REGISTER_TYPES.relative_to(ROOT)}")
        return 1

    registered = _registered_classes(REGISTER_TYPES.read_text(encoding="utf-8"))
    if not registered:
        print("[doc-classes-check] FAIL found no GDREGISTER_CLASS registrations to check")
        return 1

    doc_list = _doc_class_list(CONFIG_PY.read_text(encoding="utf-8")) if CONFIG_PY.is_file() else []
    doc_list_set = set(doc_list)

    failures: list[str] = []
    # Every registered class must be listed in config.py get_doc_classes() so its XML
    # is embedded into the editor docs - a present-but-unlisted XML is silently ignored.
    for class_name in sorted(registered):
        if class_name not in doc_list_set:
            failures.append(
                f"registered class `{class_name}` is not in config.py get_doc_classes(); its doc_classes XML would not be embedded"
            )
    # And every listed name must be a real registration (no stale entries).
    for listed in doc_list:
        if listed not in registered:
            failures.append(
                f"config.py get_doc_classes() lists `{listed}`, which is not registered via GDREGISTER*_CLASS"
            )

    for class_name in sorted(registered):
        xml_path = DOC_CLASSES / f"{class_name}.xml"
        if not xml_path.is_file():
            failures.append(
                f"registered class `{class_name}` has no doc_classes/{class_name}.xml"
            )
            continue
        text = xml_path.read_text(encoding="utf-8")
        wf_error = _well_formed_error(text)
        if wf_error is not None:
            failures.append(f"doc_classes/{class_name}.xml is not well-formed XML: {wf_error}")
            continue
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
        f"[doc-classes-check] PASSED - all {len(registered)} registered classes have a doc_classes XML with a non-empty brief."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
