---
name: gs-build-test
description: Build GodotGS with SCons and run the repo's guard, module, runtime, GPU-harness and benchmark lanes without flooding the context with output. Use when you need to compile, run tests, pick which checks a change needs, or read a long build/test log.
---

# Build and test GodotGS (output-economical)

The canonical reference is `docs/reference/build-test-ci.md`. Read only the section
you need. Its "Per-lane result ledger" section is long; consult it only when you are
interpreting `[lane-result]` lines.

## Pick the smallest lane that can fail for your change

| Change | Minimum lane |
| --- | --- |
| Docs, `.agentic/`, `scripts/agentic/` (R0) | `python scripts/agentic/validate_repo_contract.py --strict-hierarchy`, `python -m unittest discover -s tests/agentic`, `python scripts/docs/check_links.py docs README.md BUILDING.md CONTRIBUTING.md` |
| Module or tests (R1) | `python tests/ci/run_module_tests.py --guard-only`, then targeted doctest cases |
| Renderer, shaders or streaming (R2) | R1, plus a module build and `python tests/runtime/run_runtime_validation.py --godot-binary <bin> --profile headless-ci` (GPU lanes as needed) |
| Workflows, persistence, engine (R3) | R2, plus `python tests/ci/validate_automation.py --contracts-only` |

To get the derived class for your diff, run
`python scripts/agentic/classify_change.py --base-ref <base-sha>`.

## Build

```bash
scons platform=windows target=editor dev_build=yes tests=yes -j<jobs>
```

The output is `bin/godot.windows.editor.dev.x86_64.exe` (`dev_build` adds `.dev`).
Module-only targets are in `modules/gaussian_splatting/docs/BUILD_AND_TEST.md`.

## Run targeted tests instead of whole suites

- Single doctest case:
  `bin/<binary> --test --test-case="*YourCase*"`
- GPU case: `bin/<binary> --gs-gpu-test --test-case="*YourCase*"`, or a harness
  batch with `python tests/ci/run_gpu_harness.py --batch <Batch> --godot <bin>`
- Single Python test module:
  `python -m unittest tests.ci.test_<name>`
- Runtime validation: always pass `--profile`. Without it, the runner falls back to the
  GPU-and-display `release-ci` profile.

## Keep output out of context

- Where the agent tool supports PreToolUse hooks, a repo hook rewrites plain
  `scons ...` and `python tests/{ci,runtime}/run_*.py ...` commands. The output is
  condensed to failure lines, a warning count and the tail, and the full log goes to
  `tests/output/agent_logs/`. The exit status passes through unchanged.
  - For raw output, prefix the command with `GS_RAW_OUTPUT=1`.
  - Commands that already pipe, redirect or chain are not rewritten.
- Without the hook, call the condenser directly from bash:
  `( set -o pipefail; <cmd> 2>&1 | python scripts/agentic/condense_output.py filter --kind runner )`.
  Use `--kind scons` for builds.
- To dig into a full log, search it for the failing case name. Do not read it whole.
- For long multi-lane runs, delegate to the `gs-verifier` subagent. It returns a short
  verdict with evidence.

## Report honestly

- Report the exact commands and their exit codes.
- If a lane could not run (no GPU, no binary), write "not run", never "passed".
- A condensed view with no matched lines is not success. Only the exit status is the
  verdict.
