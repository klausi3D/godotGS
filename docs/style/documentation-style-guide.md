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

## Examples

```bash
rg -n "GDREGISTER_CLASS\(|GDREGISTER_ABSTRACT_CLASS\(" modules/gaussian_splatting/register_types.cpp
python3 scripts/docs/check_links.py docs/style
```

## Troubleshooting

| Symptom | Cause | Action |
| --- | --- | --- |
| Documented class/singleton does not exist | Registration changed. | Re-audit `register_types.cpp` before publishing. |
| Documented method is missing at runtime | Binding removed or renamed. | Re-check `_bind_methods()` and property bindings. |
| Heading anchor link fails | Anchor text does not match checker slug normalization. | Rename heading or update anchor target. |
| Relative link is reported missing | Path resolved from wrong source directory. | Recompute path relative to the current document and rerun checker. |
| `docs-build` reports stale generated docs | A generator input (settings, shader comments, GDScript doc comments, compatibility YAML, benchmark data) changed without regenerating. | Run `python3 scripts/build_documentation.py --all --no-engine-patch` and commit the files the `docs-build` log lists as stale. |
