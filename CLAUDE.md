# CLAUDE.md

<!-- Agent instructions are maintained vendor-neutrally in AGENTS.md and the nested
AGENTS.md files; this file only bridges them into Claude Code. Block-level HTML
comments like this one are stripped before the file reaches the model, so keep
maintainer notes here rather than in prose. -->

@AGENTS.md

## Claude Code

- Before editing files in a directory that has its own `AGENTS.md`, read it. Examples:
  `modules/gaussian_splatting/`, `renderer/`, `shaders/`, `tests/`,
  `.github/workflows/`.
- Project skills, which load only when relevant:
  - `gs-codebase-map`: orientation and large-file reading
  - `gs-build-test`: lanes and output control
  - `gs-change-evidence`: risk class, evidence, PR notes
  - `gs-review`: independent review
- Subagents: `gs-explorer` (read-only search) and `gs-verifier` (runs checks, edits
  nothing).
- A PreToolUse hook condenses plain `scons` and `tests/{ci,runtime}/run_*.py` output.
  The full log goes to `tests/output/agent_logs/`. For raw output, prefix the command
  with `GS_RAW_OUTPUT=1`.
- Compact instructions are in `AGENTS.md` (imported above).
