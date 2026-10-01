# Versioned Docs Site

## Purpose

Define the GitHub Pages publication pipeline for versioned Gaussian Splatting documentation.

## Stack

| Component | Tool |
| --- | --- |
| Static site generator | MkDocs + Material |
| Version publishing | mike |
| Source docs root | `docs/` |
| Public staging dir | `.site/public-docs/` |
| Pages target | `gh-pages` branch |
| Navigation order | `docs/.pages` via `mkdocs-awesome-nav` |

Canonical site URL defaults to `https://klausi3d.github.io/godotGS/` and can be overridden for forks or custom domains with `DOCS_SITE_URL`.

## Versioning Model

| Source ref | Published version path |
| --- | --- |
| `master` / `main` push | `/latest/` |
| `v*` tag push | `/<tag>/` (for example `/v1.2.0/`) |

Default root points to `latest` via `mike set-default latest`.

Repository setting requirement:

- GitHub Pages source must be configured to deploy from the `gh-pages` branch.

## Navigation Rules

- Keep `docs/.pages` as the source of truth for top-level section order.
- Use `index.md` for section landing pages. Do not use `README.md` for published section indexes.
- Root-level `docs/README.md` is excluded from the published site so `docs/index.md` remains the single docs homepage.
- Published URLs stay anchored to the folder path (`section/`), so renaming a section landing page from `README.md` to `index.md` does not require a redirect by itself.
- Add `redirect_maps` entries only when a page's published URL changes.

## Local Commands

```bash
python3 -m pip install -r docs/requirements.txt -r docs/requirements-site.txt
ENABLE_GIT_DATES=false python3 scripts/build_docs_site.py --strict
python3 scripts/docs/release_acceptance.py
```

CI (`docs_pages.yml`) does not install the loose `>=` ranges above. It installs
`docs/requirements-lock.txt`, the full transitive set compiled from those two files,
exact-pinned and hashed, with `pip install --require-hashes`. To reproduce the CI
stack locally, install the lock the same way. After you change either input file,
regenerate the lock with the pip-tools command recorded in its header. Run it
with `PIP_CONFIG_FILE` set to the null device (`/dev/null`, or `nul` on
Windows), so a local extra index is never consulted. A lock compiled on Windows also pins `tzdata`, which
`mkdocs-git-revision-date-localized-plugin` requires only on Windows.

Equivalent Make targets:

```bash
make docs-site
```

## CI Pipeline

`.github/workflows/docs_pages.yml` has two jobs. Both run on GitHub-hosted
`ubuntu-latest`, install `doxygen` and install `docs/requirements-lock.txt` with
`pip install --require-hashes`.

### `docs-build`: the docs check

Runs on every pull request (no path filter), on the merge queue, on pushes and on
manual dispatch. It has a read-only token and never deploys. Steps, in order:

1. Generate docs artifacts:
   - `python3 scripts/build_documentation.py --engine-patch --engine-patch-summary-only || true`
   - `python3 scripts/build_documentation.py --all --no-engine-patch`
2. Fail if a committed generator output is stale (`git diff --exit-code`, plus a
   check for uncommitted new files) for `docs/api/gdscript_reference.md`,
   `docs/api/shader_reference.md`, `docs/reference/compatibility-matrix.md`,
   `docs/reference/project-settings.md`, `docs/assets/data/benchmark_latest.json`
   and `docs/assets/benchmarks/`. Fix a failure by running the second command
   above and committing the result. The project-settings generator takes its
   key set from `modules/gaussian_splatting/config/project_settings_manifest.json`
   and refuses to write the page when the manifest and the source registrations
   disagree. Separately, `tests/ci/check_project_settings_reference.py` (run by
   `run_module_tests.py --guard-only`, so also in the required
   `agentic-pr-gate`) fails when the page and the manifest list different keys.
   Not checked: the engine patch report,
   `docs/reports/documentation-*.md`, and the Doxygen output (not committed).
3. Stage public docs:
   - `python3 scripts/stage_public_docs.py --source docs --output .site/public-docs --repo-url https://github.com/<owner>/<repo> --ref <sha>`
4. Enforce media budgets:
   - `python3 scripts/check_docs_media_budget.py --root .site/public-docs --max-file-mb 25 --max-total-mb 250`
5. Validate the site:
   - `ENABLE_GIT_DATES=false mkdocs build --strict --config-file mkdocs.yml`
6. Run the release acceptance script **report-only** (`continue-on-error`):
   - `python3 scripts/docs/release_acceptance.py`

   It currently fails on public orphan pages
   ([#1099](https://github.com/klausi3D/godotGS/issues/1099)). Its result does not
   block the job until that is fixed.

`docs-build` is not a required status check. Only `agentic-pr-gate` is required on
`master`, and making `docs-build` required is a branch-protection decision for the
maintainer.

### `deploy`: publication

Runs only on pushes to `master`/`main` and `v*` tags that touch the docs, the
generators, the theme or the module sources (see the workflow's `paths:`), and on
manual dispatch. It has the only write token. It repeats steps 1, 3, 4 and 5 above,
without the freshness check and the release acceptance script, and then publishes:

- `mike deploy --push latest` and `mike set-default --push latest` from `master`/`main`
- `mike deploy --push <tag>` from a `v*` tag

`deploy` does not wait for `docs-build`. A stale committed generator output makes
`docs-build` fail, but the site still publishes the freshly regenerated output.

## Release Acceptance

Run these checks before every publish candidate:

1. Stage and build the public docs tree:
   - `ENABLE_GIT_DATES=false python3 scripts/build_docs_site.py`
2. Run the repository markdown link check:
   - `python3 scripts/docs/check_links.py docs README.md BUILDING.md CONTRIBUTING.md`
3. Run the release acceptance script:
   - `python3 scripts/docs/release_acceptance.py`

The script currently verifies the items below. CI runs it report-only until
[#1099](https://github.com/klausi3D/godotGS/issues/1099) is fixed, so a failure
does not block a PR or a publish:

- broken internal links
- public orphan pages in `.site/public-docs/`
- redirect target correctness from `mkdocs.yml`
- media budget across images and video
- missing alt text in staged public docs

## Manual QA Checklist

Usability paths:

- From the homepage, a new user can find `Installation`.
- From the homepage, a new user can find `First Run`.
- From the homepage, a contributor can find `Build / Test / CI Command Reference`.
- From the homepage, a reviewer can see the project-status box and reach `Compatibility Matrix`.

Visual QA:

- Desktop light mode: homepage and section cards look intentional and readable.
- Desktop dark mode: homepage still has correct contrast and hierarchy.
- Mobile: the drawer opens, the top-level nav is usable, and pages do not introduce page-level horizontal overflow.
- Tables: review `Compatibility Matrix` and `Build from Source` at mobile width for readable overflow handling.
- Cards: review homepage and section landing pages for wrapping at narrow widths.

Search QA:

- `install` should surface `Installation` first.
- `first run` should surface `First Run` first.
- `build test` should surface `Build / Test / CI Command Reference` first.
- `compatibility` should surface `Compatibility Matrix` first.

Release spot-check pages:

- Homepage
- Start Here landing page
- `Installation`
- `First Run`
- `Build from Source`
- `Compatibility Matrix`
- Reference landing page

## Engine patch report automation

- Generator: `python3 scripts/generate_engine_patch_report.py`
- Baseline config: `docs/reference/engine_patch_sources.yaml`
- Outputs (committed):
  - `docs/reference/engine-patch.md`
  - `docs/reference/engine-patch.json`

Operational policy:

- Keep `upstream_ref` pinned to a stable commit/tag in `engine_patch_sources.yaml`.
- Update that pin only as an explicit maintenance change.
- Generation is non-blocking in docs CI (`|| true` in workflow step).
- Use `--strict` locally/CI only when intentionally gating on report freshness.

## Public Scope

The staged docs copy excludes internal docs directories:

- `docs/agent_memory/`
- `docs/archive/`

Out-of-scope relative links are rewritten to GitHub `blob`/`tree` URLs so public pages remain navigable.

The published MkDocs config enables instant navigation, top tabs, sticky tabs, section indexes, navigation path breadcrumbs, and footer next/previous links on the staged docs tree.

## Doxygen Inclusion

`docs/Doxyfile` writes C++ API HTML for `modules/gaussian_splatting` (excluding
`tests/`) to `docs/api/cpp`. Its paths are relative to the directory doxygen runs
in, which is the repository root when `scripts/build_documentation.py` runs it. The
output is gitignored, generated in both CI jobs, copied into the staged docs and
shipped with each docs version. The Doxygen warning log goes to
`doxygen-warnings.log` in the repository root (gitignored), outside the published
tree. When `doxygen` is not
installed locally, `build_documentation.py` skips this step.
