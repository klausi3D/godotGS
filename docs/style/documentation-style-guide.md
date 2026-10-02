# Documentation Style Guide

## Purpose

Keep documentation accurate, concise, and verifiable against code.

## Usage

| Step | Action | Reference |
| --- | --- | --- |
| Scope review | Read all Markdown files in the target docs area before editing. | `docs/index.md` |
| Implementation verification | Verify behavior claims against `modules/gaussian_splatting/` before publishing. | `initialize_gaussian_splatting_module()` in `modules/gaussian_splatting/register_types.cpp` |
| Style application | Prefer direct task-based writing and compact tables for inventories. | `docs/style/documentation-style-guide.md` |
| Evidence capture | Cite stable symbols in published pages; put `file:line` evidence in reports and PR descriptions. See [Citing code](#citing-code). | This page |
| Link validation | Run repository link checks after docs edits. | `scripts/docs/check_links.py` |
| Generated pages | Regenerate generated pages and commit the result; the `docs-build` CI job fails when a committed generated page is stale. | `docs/development/docs-site.md` |

## Citing code

Line numbers change on unrelated edits, so a `file:line` in a page that stays
published goes stale without anyone touching the page.

| Where | Cite | Examples |
| --- | --- | --- |
| User-facing and reference pages (all of `docs/` except `docs/reports/`, `docs/archive/` and `docs/agent_memory/`) | Stable symbols: `Class::method`, a bound property or signal name, a project setting path, or a file path when no symbol fits. No line numbers. | `GaussianSplatNode3D::_bind_methods()`, `rendering/gaussian_splatting/lod/enabled`, `modules/gaussian_splatting/config.py` |
| Reports, investigations, audits (`docs/reports/`, `docs/archive/`), PR descriptions, issue comments | `file:line` pinned to a commit SHA, so the reader can open the exact line that was read. | `modules/gaussian_splatting/register_types.cpp:93` at `dbc0f49aa8b` |

A symbol citation is still an implementation claim: verify it exists at your base
commit (`rg -n "<symbol>" modules/gaussian_splatting`) before you publish it.

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
| `check_doc_snippets.py` reports `MEMBER_CPP_ONLY` | The member is declared in C++ but not bound. | Use a bound alternative, or cite it as `Class::member` in prose. |
| Heading anchor link fails | Anchor text does not match checker slug normalization. | Rename heading or update anchor target. |
| Relative link is reported missing | Path resolved from wrong source directory. | Recompute path relative to the current document and rerun checker. |
| `docs-build` reports stale generated docs | A generator input (settings, shader comments, GDScript doc comments, compatibility YAML, benchmark data) changed without regenerating. | Run `python3 scripts/build_documentation.py --all --no-engine-patch` and commit the files the `docs-build` log lists as stale. |
