# Documentation Style Guide

## Purpose

Keep documentation accurate, concise, and verifiable against code.

## Usage

| Step | Action | Reference |
| --- | --- | --- |
| Scope review | Read all Markdown files in the target docs area before editing. | `docs/index.md` |
| Implementation verification | Verify behavior claims against `modules/gaussian_splatting/` before publishing. | `modules/gaussian_splatting/register_types.cpp:69` |
| Style application | Prefer direct task-based writing and compact tables for inventories. | `docs/style/documentation-style-guide.md` |
| Evidence capture | Add `file:line` references for implementation claims. | `modules/gaussian_splatting/register_types.cpp:75` |
| Link validation | Run repository link checks after docs edits. | `scripts/docs/check_links.py:133` |

## API

| Rule area | Rule | Source |
| --- | --- | --- |
| Class names | Document class names only when they are registered with `GDREGISTER_CLASS` or `GDREGISTER_ABSTRACT_CLASS`. | `modules/gaussian_splatting/register_types.cpp:75` |
| Singleton names | Document singleton names exactly as registered. | `modules/gaussian_splatting/register_types.cpp:101` |
| Method/property exposure | Document methods/properties only when exposed in `_bind_methods()` and `ADD_PROPERTY`. | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:84` |
| Loader API exposure | Document loader methods only when bound through `ClassDB::bind_method`. | `modules/gaussian_splatting/io/ply_loader.cpp:60` |
| C++ docs source | Keep C++ API docs aligned with Doxygen input patterns. | `docs/Doxyfile:14` |
| GDScript docs source | Keep function docs in `##` comments directly above `func` signatures. | `scripts/extract_gdscript_docs.py:34` |
| Link paths | Use relative paths that resolve from the source file directory and stay inside repository boundaries. | `scripts/docs/check_links.py:99` |

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
