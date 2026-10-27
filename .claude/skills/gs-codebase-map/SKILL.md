---
name: gs-codebase-map
description: Orientation for the GodotGS Gaussian Splatting module. Covers where the subsystems live, which files are too large to read whole, and how to find code without reading whole 3000-line files. Use before exploring modules/gaussian_splatting/ or when unsure where something lives.
---

# GodotGS codebase map (read cheaply)

Project work lives in `modules/gaussian_splatting/`. Everything else is upstream Godot.

## Where to look first

- Subsystem map: `modules/gaussian_splatting/ARCHITECTURE.md` (≈90 lines). Read only
  the section for your subsystem.
- `modules/gaussian_splatting/READING_ORDER.md` is an **onboarding map, not a
  checklist**. Its entries add up to more than 1 MB of source. Open only the entries
  your task touches.
- Directory → topic:
  - `core/`: data, streaming, manager, scene director, world
  - `renderer/`: pipeline, sorter, tiles, orchestrators
  - `shaders/`: GLSL plus `compile_shaders.py`
  - `interfaces/`: DI contracts, compositor, culler
  - `nodes/`: scene nodes
  - `io/`: importers and loaders
  - `persistence/`: on-disk formats (R3)
  - `lod/`, `painterly/`, `editor/`, `logger/`
  - `tests/`: doctest headers

## Large files: never read whole

Many sources and test headers are 1.5k–7k lines (for example
`tests/test_renderer_pipeline.h`, `core/gaussian_streaming.cpp`,
`renderer/tile_renderer.cpp`, `renderer/gpu_sorter.cpp`). To get a current ranking,
run:

```bash
python scripts/agentic/largest_files.py modules/gaussian_splatting --top 25
```

How to work with them:

1. Find the symbol: search (`rg -n`) for `ClassName::method` or the identifier.
2. Read only a window of about 80–200 lines around the hit.
3. Read headers (`*.h`) for the API before reading `*.cpp` for implementation.
4. Delegate broad "how does X work" questions to the explorer subagent
   (`gs-explorer` in Claude Code, `gs_explorer` in Codex). It returns file:line
   summaries instead of file contents.

## Never a source of truth / not worth reading

- `docs/agent_memory/` is legacy.
- `docs/reports/`, `docs/assets/data/` and `docs/architecture/generated/` are
  generated or archived.
- `*.gen.h` files are generated shader headers. Read the `.glsl` instead.
- `thirdparty/`, `doc/translations/`. Search `thirdparty/<lib>` by explicit path if
  you need a vendored header.

The repo's `.ignore` already keeps these out of default searches. Use
`rg --no-ignore-dot` to override.
