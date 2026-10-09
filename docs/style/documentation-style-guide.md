# Documentation Style Guide

## Purpose

Keep documentation accurate, short and written for the person who reads the page.
These rules apply to every new or changed paragraph. Existing pages are brought in
line page by page.

## Page Audiences

A page's audience is the tab it sits under in `docs/.pages`. The audience decides
how the page names things in the code.

| Tab | Reader | How to name things |
| --- | --- | --- |
| Get Started, Guides | People using the editor | UI labels, written as a path: Inspector › Quality › Preset. Project setting paths such as `rendering/gaussian_splatting/quality/tier_preset`. Node and resource names as the editor shows them, such as `GaussianSplatNode3D`. Link to the Reference page for methods. No C++ symbols, no source paths, no "Implementation reference" columns. |
| Reference, Performance | People looking up exact behaviour | Stable symbols: a bound method or property, a signal, a project setting path, `Class::method` for C++. No line numbers. |
| Contributing | Contributors and reviewers | As Reference, plus source file paths. No line numbers. |
| Reports (`docs/reports/`, `docs/archive/`), PR descriptions, issue comments | Reviewers checking one change | `file:line` pinned to a commit SHA, such as `modules/gaussian_splatting/register_types.cpp:93` at `dbc0f49aa8b`. |

Line numbers change on unrelated edits, so a `file:line` on a published page goes
stale without anyone touching the page.

Every claim is checked against the code at your base commit, whatever the page
cites. A UI label is checked against the property it shows (`ADD_GROUP` and
`ADD_PROPERTY` in the class's `_bind_methods()`), a symbol with
`rg -n "<symbol>" modules/gaussian_splatting`.

## Rules

1. **Name things for the page's audience.** Follow the table above.
2. **No process history on Get Started and Guides pages.** Leave out
   maintainer-decision dates, `§` references, commit SHAs, how a guard or gate
   works, and "an earlier revision said…". Say what is true now. History belongs in
   ADRs, reports and PR descriptions.
3. **Link an issue only when the reader can act on it**, for example to follow a
   known limitation until it is fixed. Do not cite issues as the source of a
   statement.
4. **Show a limitation on the page where it bites.** One or two sentences, then a
   link to its section in
   [Known Public Alpha Limitations](../development/known-public-alpha-limitations.md),
   which holds the full write-up.
5. **One name per concept.** Use the term from the
   [glossary](../user/manual/concepts.md#glossary) and none of the names it lists
   under "Not". A new concept gets a glossary entry in the same PR.
6. **Procedures are numbered steps**, not table rows. One action per step. Add the
   expected result when the reader needs to check it.
7. **Link text says where the link goes**: "see
   [Release Channels](../development/release-channels.md)", not "here", "this page"
   or a bare URL.
8. **Diagram alt text gives the steps in order**, such as "Copy the `.ply` into the
   project, Godot imports it, assign the splat asset to a `GaussianSplatNode3D`".
   "Diagram of the import flow" is not alt text. A caption does not replace it. A
   Mermaid chart or a video gets the same steps as text next to it.
9. **State a limit once, in plain words.** No self-labels such as "honest",
   "candid" or "to be clear", and no repeated caveats.
10. **Keep internal vocabulary on Contributing pages and in reports.** On other
    pages, name the thing itself: "the Windows build", not "the Windows lane".

| Term | Meaning |
| --- | --- |
| lane | One CI job or test group that runs and reports on its own: a platform build in `.github/workflows/release_builds.yml`, or a doctest group in `MODULE_TEST_FILTERS` in `tests/ci/run_module_tests.py`. Per-splat storage in `GaussianData` is a field, not a lane. |
| strict / advisory | Whether a lane's ordinary failure (a nonzero exit or a crash) fails the run (strict) or is only reported (advisory). A harness anomaly, such as exit 0 with no doctest summary, fails the run for either kind; see [Build, test and CI](../reference/build-test-ci.md). |
| guard | An automated check of a repository invariant, such as a file format, a settings manifest or a layout mirror, that fails CI when the invariant breaks. Most are `check_*.py` scripts under `tests/ci/`; `tests/ci/run_module_tests.py --guard-only` runs the full set, including checks built into the runner and one that needs a Godot binary. |
| gate | A required check that blocks a merge or a release, such as `agentic-pr-gate` or the renderer release gate. |

## Before You Publish

| Step | Action | Reference |
| --- | --- | --- |
| Scope review | Read all Markdown files in the target docs area before editing. | `docs/index.md` |
| Link validation | Run the link checker on the changed area. | `scripts/docs/check_links.py` |
| Snippet and API check | Run the snippet checker (see below). | `tests/ci/check_doc_snippets.py` |
| Generated pages | Regenerate generated pages and commit the result; the `docs-build` CI job fails when a committed generated page is stale. | `docs/development/docs-site.md` |

## API

| Rule area | Rule | Source |
| --- | --- | --- |
| Class names | Document class names only when they are registered with `GDREGISTER_CLASS` or `GDREGISTER_ABSTRACT_CLASS`. | `initialize_gaussian_splatting_module()` in `modules/gaussian_splatting/register_types.cpp` |
| Singleton names | Document singleton names exactly as registered. | `initialize_gaussian_splatting_module()` in `modules/gaussian_splatting/register_types.cpp` |
| Method/property exposure | Document methods/properties only when exposed in `_bind_methods()` and `ADD_PROPERTY`. | For example `GaussianSplatNode3D::_bind_methods()` |
| Loader API exposure | Document loader methods only when bound through `ClassDB::bind_method`. | `PLYLoader::_bind_methods()` |
| C++ docs source | Keep C++ API docs aligned with Doxygen input patterns. | `INPUT` and `FILE_PATTERNS` in `docs/Doxyfile` |
| GDScript docs source | Keep function docs in `##` comments directly above `func` signatures. | `extract_docs()` in `scripts/extract_gdscript_docs.py` |
| Link paths | Use relative paths that resolve from the source file directory and stay inside repository boundaries. | `validate_link()` in `scripts/docs/check_links.py` |

## Checked Examples and API References

`tests/ci/check_doc_snippets.py` runs on every pull request and checks `docs/` against the module's ClassDB bindings (`_bind_*` bodies and `register_types.cpp`):

- GDScript blocks: members of module classes must be bound (not C++-only), class names must be registered, argument counts must match `D_METHOD` plus `DEFVAL`, and bare calls must be GDScript functions or members of the script's base.
- Grouped properties such as `rendering/opacity` are one name: use the setter or `set("rendering/opacity", value)`, never `node.rendering.opacity`.
- Prose: `Class.member` is the script spelling and must be bound. Cite C++ symbols as `Class::member`.
- API pages (`docs/api/`, H1 names the class): every `method(...)` in a table row's API cell must be bound, unless the row says `C++`.
- `docs/architecture/` and ADRs only warn; `docs/agent_memory/`, `docs/archive/` and `docs/reports/` are not checked.

For a fragment that relies on variables from elsewhere, or for a block that is intentionally not runnable, put a marker on the line before the fence:

```markdown
<!-- snippet: prelude="var splat_node: GaussianSplatNode3D" -->
<!-- snippet: pseudo reason="shows the call shape, not runnable code" -->
```

A `prelude` is checked like code (separate declarations with `;`). A `pseudo` marker needs a reason. Never add a marker to hide an API that does not exist.

## Examples

```bash
python3 tests/ci/check_doc_snippets.py
rg -n "GDREGISTER_CLASS\(|GDREGISTER_ABSTRACT_CLASS\(" modules/gaussian_splatting/register_types.cpp
python3 scripts/docs/check_links.py docs/style
```

## Troubleshooting

| Symptom | Cause | Action |
| --- | --- | --- |
| Documented class/singleton does not exist | Registration changed. | Re-audit `register_types.cpp` before publishing. |
| Documented method is missing at runtime | Binding removed or renamed. | Re-check `_bind_methods()` and property bindings. |
| `check_doc_snippets.py` reports `MEMBER_CPP_ONLY` | The member is declared in C++ but not bound. | Use a bound alternative. On a Reference or Contributing page you may cite it as `Class::member` in prose; on a Get Started or Guides page, describe the behaviour and link to Reference. |
| Heading anchor link fails | Anchor text does not match checker slug normalization. | Rename heading or update anchor target. |
| Relative link is reported missing | Path resolved from wrong source directory. | Recompute path relative to the current document and rerun checker. |
| `docs-build` reports stale generated docs | A generator input (settings, shader comments, GDScript doc comments, compatibility YAML, benchmark data) changed without regenerating. | Run `python3 scripts/build_documentation.py --all --no-engine-patch` and commit the files the `docs-build` log lists as stale. |
