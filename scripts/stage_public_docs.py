#!/usr/bin/env python3
"""Stage public docs content for MkDocs/GitHub Pages publishing."""
from __future__ import annotations

import argparse
import os
import re
import shutil
from pathlib import Path
from typing import Iterable, Sequence
from urllib.parse import quote, urlsplit, urlunsplit

REPO_ROOT = Path(__file__).resolve().parents[1]

# Public-stage exclusions: the single list of what never reaches the public site
# or its search index. Each pattern is a docs/-relative POSIX path matched on
# whole path components:
#   "reports"            -> docs/reports and everything beneath it (prefix match)
#   "architecture/x.md"  -> exactly that file
#   "**/archive"         -> a directory named `archive` at ANY depth
# Excluding here, not with mkdocs `exclude_docs`, is deliberate: an excluded file
# is never staged, so the link rewriter below turns every inbound link from a
# public page into a GitHub blob/tree URL instead of leaving it dangling.
# Every entry must still match a source path; a stale entry fails the stage
# (see find_unmatched_exclusions) so this list cannot rot silently.
DEFAULT_EXCLUSIONS: tuple[str, ...] = (
    # Legacy multi-agent coordination memory; AGENTS.md: "Not source of truth".
    "agent_memory",
    # Superseded material, wherever it lives (docs/archive/, docs/reports/archive/).
    "**/archive",
    # Point-in-time investigations, audits and doc-coverage dumps (incl. the
    # generated documentation-gaps.md); records, not current documentation.
    "reports",
    # Agent work programs; live status is in GitHub issues, not the public site.
    "programs",
    # Agent working journal of the completed renderer refactor ("dirty worktree"
    # branch context, per-slice logs); its own closeout says the refactor is done.
    "architecture/gaussian-renderer-refactor-memory.md",
    # How-to for scripts/refactor_phase_runner.py, which drives that finished
    # refactor's phases from a WSL checkout.
    "architecture/refactor-phase-runner.md",
    # Working plan for the pipeline unification; stages 0-3 landed and its
    # "current state" section is stale (e.g. ply_file_path is now removed).
    "architecture/gaussian-pipeline-unification-plan.md",
    # Post-refactor cleanup plan, overtaken by the code: ply_file_path is a
    # one-release migration shim and the duplicated source-path helper is gone.
    "architecture/gaussian-pipeline-deprecation-deletion-plan.md",
    # Design proposal marked "design, not implemented"; the shaders still carry
    # the sh_occlusion coupling the spec would remove.
    "architecture/resolve_lighting_redesign_spec.md",
    # Unimplemented tier-2 cluster-culling spec; no cluster-cull code exists and
    # the legacy ClusterCuller stack was deleted (#293).
    "architecture/tier2_cluster_culling_spec.md",
)
MARKDOWN_LINK_PATTERN = re.compile(r"(!?\[[^\]]+\]\()([^)]+)(\))")
HTML_ATTR_PATTERN = re.compile(
    r'(<(?:a|img|video|source)\b[^>]*?\s(?:href|src)=["\'])([^"\']+)(["\'])',
    flags=re.IGNORECASE,
)
REMOTE_URL_PATTERN = re.compile(r"^https?://", flags=re.IGNORECASE)
MEDIA_EXTENSIONS = {".avif", ".gif", ".jpeg", ".jpg", ".mp4", ".png", ".svg", ".webm", ".webp"}


def path_is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def parse_repo_coordinates(repo_url: str) -> tuple[str, str] | None:
    normalized = repo_url.rstrip("/")
    if normalized.endswith(".git"):
        normalized = normalized[:-4]
    match = re.match(r"^https?://github\.com/([^/]+)/([^/]+)$", normalized)
    if not match:
        return None
    return match.group(1), match.group(2)


def build_blob_url(repo_url: str, ref: str, relative_path: Path, is_dir: bool) -> str:
    encoded_path = quote(relative_path.as_posix())
    kind = "tree" if is_dir else "blob"
    return f"{repo_url.rstrip('/')}/{kind}/{quote(ref)}/{encoded_path}"


def build_raw_url(repo_url: str, ref: str, relative_path: Path) -> str:
    coords = parse_repo_coordinates(repo_url)
    if not coords:
        return build_blob_url(repo_url, ref, relative_path, is_dir=False)
    owner, repo = coords
    encoded_path = quote(relative_path.as_posix())
    return f"https://raw.githubusercontent.com/{owner}/{repo}/{quote(ref)}/{encoded_path}"


def parse_exclusion(pattern: str) -> tuple[bool, tuple[str, ...]]:
    """Split a pattern into (matches_at_any_depth, path components)."""
    normalized = pattern.strip().replace("\\", "/").strip("/")
    any_depth = normalized.startswith("**/")
    if any_depth:
        normalized = normalized[3:]
    parts = tuple(part for part in normalized.split("/") if part)
    if not parts or any(part in {".", "..", "**"} for part in parts):
        raise ValueError(f"Invalid public-docs exclusion pattern: {pattern!r}")
    return any_depth, parts


def exclusion_matches(relative_path: Path, pattern: str) -> bool:
    any_depth, pattern_parts = parse_exclusion(pattern)
    path_parts = relative_path.parts
    width = len(pattern_parts)
    starts = range(len(path_parts) - width + 1) if any_depth else range(1)
    return any(path_parts[start : start + width] == pattern_parts for start in starts)


def should_exclude(relative_path: Path, exclusions: Iterable[str]) -> bool:
    return bool(relative_path.parts) and any(
        exclusion_matches(relative_path, pattern) for pattern in exclusions
    )


def find_unmatched_exclusions(source_root: Path, exclusions: Iterable[str]) -> list[str]:
    """Patterns that match no path under source_root (a stale list entry)."""
    relative_paths = [path.relative_to(source_root) for path in source_root.rglob("*")]
    return sorted(
        pattern
        for pattern in set(exclusions)
        if not any(exclusion_matches(relative, pattern) for relative in relative_paths)
    )


def split_target_token(token: str) -> tuple[str, str]:
    stripped = token.strip()
    if not stripped:
        return token, ""
    # Support Markdown targets with optional title text.
    if stripped.startswith("<") and stripped.endswith(">"):
        return stripped[1:-1], ""
    match = re.match(r"^(\S+)(\s+.+)?$", stripped)
    if not match:
        return stripped, ""
    suffix = match.group(2) or ""
    return match.group(1), suffix


def rebuild_url(split_result, new_path: str) -> str:
    return urlunsplit(
        (
            split_result.scheme,
            split_result.netloc,
            new_path,
            split_result.query,
            split_result.fragment,
        )
    )


def rewrite_target(
    target: str,
    *,
    is_embed: bool,
    source_file: Path,
    staged_file: Path,
    source_root: Path,
    staged_root: Path,
    exclusions: Sequence[str],
    repo_url: str | None,
    ref: str | None,
) -> str:
    split_result = urlsplit(target)
    if split_result.scheme or split_result.netloc:
        return target
    if not split_result.path or split_result.path.startswith("#") or split_result.path.startswith("/"):
        return target

    decoded_path = split_result.path
    staged_candidate = (staged_file.parent / decoded_path).resolve()
    if path_is_within(staged_candidate, staged_root) and staged_candidate.exists():
        return target

    source_candidate = (source_file.parent / decoded_path).resolve()
    if not source_candidate.exists() or not path_is_within(source_candidate, REPO_ROOT):
        return target
    if path_is_within(source_candidate, source_root):
        rel_from_source = source_candidate.relative_to(source_root)
        if not should_exclude(rel_from_source, exclusions):
            return target

    if not repo_url or not ref:
        return target

    rel_from_repo = source_candidate.relative_to(REPO_ROOT)
    extension = rel_from_repo.suffix.lower()
    if source_candidate.is_dir():
        rewritten = build_blob_url(repo_url, ref, rel_from_repo, is_dir=True)
    elif is_embed or extension in MEDIA_EXTENSIONS:
        rewritten = build_raw_url(repo_url, ref, rel_from_repo)
    else:
        rewritten = build_blob_url(repo_url, ref, rel_from_repo, is_dir=False)

    return rebuild_url(split_result, rewritten)


def rewrite_markdown(
    text: str,
    *,
    source_file: Path,
    staged_file: Path,
    source_root: Path,
    staged_root: Path,
    exclusions: Sequence[str],
    repo_url: str | None,
    ref: str | None,
) -> str:
    def markdown_replacer(match: re.Match[str]) -> str:
        prefix, raw_target, suffix = match.groups()
        url_path, optional_suffix = split_target_token(raw_target)
        rewritten = rewrite_target(
            url_path,
            is_embed=prefix.startswith("!["),
            source_file=source_file,
            staged_file=staged_file,
            source_root=source_root,
            staged_root=staged_root,
            exclusions=exclusions,
            repo_url=repo_url,
            ref=ref,
        )
        return f"{prefix}{rewritten}{optional_suffix}{suffix}"

    def html_replacer(match: re.Match[str]) -> str:
        prefix, raw_target, suffix = match.groups()
        lower_prefix = prefix.lower()
        is_embed = not lower_prefix.startswith("<a")
        rewritten = rewrite_target(
            raw_target,
            is_embed=is_embed,
            source_file=source_file,
            staged_file=staged_file,
            source_root=source_root,
            staged_root=staged_root,
            exclusions=exclusions,
            repo_url=repo_url,
            ref=ref,
        )
        return f"{prefix}{rewritten}{suffix}"

    rewritten = MARKDOWN_LINK_PATTERN.sub(markdown_replacer, text)
    rewritten = HTML_ATTR_PATTERN.sub(html_replacer, rewritten)
    return rewritten


def copy_docs_tree(
    *,
    source_root: Path,
    output_root: Path,
    exclusions: Sequence[str],
    repo_url: str | None,
    ref: str | None,
) -> tuple[int, int]:
    if output_root.exists():
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True, exist_ok=True)

    copied_files = 0
    rewritten_markdown = 0

    for source_file in source_root.rglob("*"):
        if source_file.is_dir():
            continue
        relative_path = source_file.relative_to(source_root)
        if should_exclude(relative_path, exclusions):
            continue

        destination_file = output_root / relative_path
        destination_file.parent.mkdir(parents=True, exist_ok=True)

        if source_file.suffix.lower() in {".md", ".markdown"}:
            text = source_file.read_text(encoding="utf-8")
            rewritten = rewrite_markdown(
                text,
                source_file=source_file,
                staged_file=destination_file,
                source_root=source_root,
                staged_root=output_root,
                exclusions=exclusions,
                repo_url=repo_url,
                ref=ref,
            )
            destination_file.write_text(rewritten, encoding="utf-8")
            rewritten_markdown += 1
        else:
            shutil.copy2(source_file, destination_file)
        copied_files += 1

    return copied_files, rewritten_markdown


def default_repo_url() -> str | None:
    server = os.environ.get("GITHUB_SERVER_URL", "").rstrip("/")
    repository = os.environ.get("GITHUB_REPOSITORY", "").strip("/")
    if server and repository and REMOTE_URL_PATTERN.match(server):
        return f"{server}/{repository}"
    return None


def default_ref() -> str | None:
    return os.environ.get("GITHUB_SHA")


def parse_exclusions(value: Sequence[str]) -> tuple[str, ...]:
    exclusions: list[str] = []
    for item in value:
        for chunk in item.split(","):
            normalized = chunk.strip()
            if normalized and normalized not in exclusions:
                parse_exclusion(normalized)  # reject malformed patterns up front
                exclusions.append(normalized)
    return tuple(exclusions)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Stage docs/ into a publishable MkDocs docs_dir.")
    parser.add_argument("--source", default="docs", help="Source docs directory (relative to repo root).")
    parser.add_argument(
        "--output",
        default=".site/public-docs",
        help="Output directory for staged public docs (relative to repo root).",
    )
    parser.add_argument(
        "--exclude",
        "--exclude-dir",
        dest="exclude",
        action="append",
        default=list(DEFAULT_EXCLUSIONS),
        help=(
            "Extra docs-relative path pattern to exclude, added to DEFAULT_EXCLUSIONS "
            "(prefix match on path components; '**/name' matches at any depth). "
            "May be repeated or comma-separated."
        ),
    )
    parser.add_argument("--repo-url", default=default_repo_url(), help="GitHub repository URL for rewritten links.")
    parser.add_argument("--ref", default=default_ref(), help="Git ref/SHA used for rewritten GitHub links.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    source_root = (REPO_ROOT / args.source).resolve()
    output_root = (REPO_ROOT / args.output).resolve()
    try:
        exclusions = parse_exclusions(args.exclude)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc

    if not source_root.is_dir():
        raise SystemExit(f"Source directory does not exist: {source_root}")

    unmatched = find_unmatched_exclusions(source_root, exclusions)
    if unmatched:
        raise SystemExit(
            "Public-docs exclusion patterns match nothing under "
            f"{source_root} (stale entry? update DEFAULT_EXCLUSIONS): {unmatched}"
        )

    copied_files, rewritten_markdown = copy_docs_tree(
        source_root=source_root,
        output_root=output_root,
        exclusions=exclusions,
        repo_url=args.repo_url,
        ref=args.ref,
    )

    print(f"[docs-site] Source: {source_root}")
    print(f"[docs-site] Output: {output_root}")
    print(f"[docs-site] Excluded paths: {list(exclusions)}")
    if args.repo_url and args.ref:
        print(f"[docs-site] Out-of-scope links rewritten to: {args.repo_url}@{args.ref}")
    else:
        print("[docs-site] Out-of-scope links kept as-is (repo URL/ref not configured).")
    print(f"[docs-site] Copied files: {copied_files} (rewritten markdown files: {rewritten_markdown})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
