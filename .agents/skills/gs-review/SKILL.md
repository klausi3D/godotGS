---
name: gs-review
description: Review a GodotGS diff as an independent correctness or GPU/performance reviewer. Works on a fixed base..head diff, reports structured findings and changes no code. Use when asked to review a PR, branch or commit range in this repo.
---

# Independent review (GodotGS)

Policy source: `docs/governance/review-policy.md`. It is about 90 lines; the
"Findings" and "Rules" sections are enough. The role prompts are in
`.agentic/roles/correctness-reviewer.md` and `gpu-performance-reviewer.md`.

## Ground rules

- Fix the range first: `git diff --stat <base>..<head>`, then read the diff file by
  file with `git diff <base>..<head> -- <path>`. Don't load the whole diff of a large
  PR at once.
- Read surrounding code only in windows around the hunks: search with line numbers
  first, then read a bounded range.
- **Reviewers do not implement.** Report findings. Never edit files.
- Don't chase style. Flag only issues that affect correctness, the stated requirements,
  evidence integrity, or scope creep.

## Checklist

- **Correctness:**
  - threading and locking
  - resource lifetime on every failure path
  - error and abort paths
  - back-compat of formats
  - test quality: can each test fail?
  - scope creep
- **GPU/performance (R2+):**
  - host↔shader layout contract
  - buffer bounds and the zero-element path
  - device-generation ownership
  - sync and async readback
  - timing honesty
  - VRAM
  - benchmark methodology against the immutable base
- **Evidence:** do the claimed commands match the class's checks in
  `.agentic/policy.json`? Are "not run" lanes labelled as such?

## Output

Use the findings format of `.agentic/schemas/review.schema.json`. Include:

- `base_sha` and `head_sha`
- the evidence you reviewed
- blind spots
- one entry per finding with severity `blocker|high|medium|low|note`. `blocker` and
  `high` entries need a concrete `required_action`.

Keep it compact: cite `path:line` and quote at most a few lines per finding.
