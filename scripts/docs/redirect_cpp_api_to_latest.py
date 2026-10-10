#!/usr/bin/env python3
"""Replace the staged Doxygen C++ API with a redirect to the `latest` docs version.

The C++ API (Doxygen output, docs/api/cpp) is about 3,800 files and 45-55 MB. It
carries no stability promise, so only the `latest` docs version publishes it. The
deploy job of .github/workflows/docs_pages.yml runs this script on the staged tree
before it builds and deploys a tagged version, so each mike version directory on
gh-pages does not carry its own copy.

The script removes <stage>/api/cpp and writes a single page at
<stage>/api/cpp/html/index.html that redirects to the same path in the `latest`
version. The docs link to `cpp/html/index.html` (docs/api/index.md) therefore
still resolves in a tagged version: it lands on latest's C++ API instead of a 404.
The redirect is relative: mike publishes a version at <site>/<version>/, so the
page at <site>/<version>/api/cpp/html/index.html reaches
<site>/latest/api/cpp/html/index.html through four `..` steps. It does not depend
on the site URL.

The script fails when the staged tree has no api/cpp directory. Staging always
copies the Doxygen output, so a missing directory means the generator or the stage
changed, and someone should look before a version is published without the
redirect.
"""
from __future__ import annotations

import argparse
import html
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CPP_API_RELATIVE = Path("api") / "cpp"
INDEX_RELATIVE = CPP_API_RELATIVE / "html" / "index.html"
# From <version>/api/cpp/html/index.html up to the site root that holds the versions.
TO_SITE_ROOT = "../../../../"


def redirect_target(version: str) -> str:
    return f"{TO_SITE_ROOT}{version}/{INDEX_RELATIVE.as_posix()}"


def redirect_page(version: str) -> str:
    target = html.escape(redirect_target(version), quote=True)
    label = html.escape(version)
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>C++ API reference ({label})</title>
<meta name="robots" content="noindex">
<link rel="canonical" href="{target}">
<meta http-equiv="refresh" content="0; url={target}">
</head>
<body>
<p>The C++ API reference is published only with the <code>{label}</code> docs version.
Continue to <a href="{target}">the {label} C++ API reference</a>.</p>
</body>
</html>
"""


def replace_with_redirect(stage_root: Path, version: str) -> Path:
    cpp_dir = stage_root / CPP_API_RELATIVE
    if not cpp_dir.is_dir():
        raise FileNotFoundError(f"no staged C++ API at {cpp_dir}; expected the Doxygen output to be staged there")
    shutil.rmtree(cpp_dir)
    index = stage_root / INDEX_RELATIVE
    index.parent.mkdir(parents=True)
    index.write_text(redirect_page(version), encoding="utf-8")
    return index


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stage", required=True, help="Staged docs root (the mkdocs docs_dir), e.g. .site/public-docs.")
    parser.add_argument("--version", default="latest", help="mike version that publishes the C++ API (default: latest).")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if not args.version or "/" in args.version or args.version in {".", ".."}:
        print(f"ERROR: invalid docs version {args.version!r}", file=sys.stderr)
        return 2
    stage_root = Path(args.stage)
    if not stage_root.is_absolute():
        stage_root = REPO_ROOT / stage_root
    try:
        index = replace_with_redirect(stage_root, args.version)
    except FileNotFoundError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"[docs] Replaced the staged C++ API with a redirect to {redirect_target(args.version)} ({index})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
