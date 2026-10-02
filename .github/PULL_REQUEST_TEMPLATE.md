<!--
  See AGENTS.md and docs/governance/review-policy.md. Keep the change small and
  reviewable. Fill in every section; delete a section only if it is genuinely N/A
  and say why.
-->

## Summary

<!-- What changed and why, in a few sentences. -->

## Linked task / issue

<!-- Task contract id and/or GitHub issue, e.g. GS-298 / #298. -->

## Risk class

<!-- R0 / R1 / R2 / R3 (see docs/governance/agentic-engineering.md).
     The Agentic PR Gate re-derives the risk class from this PR's diff
     (classify_change) and publishes it to the job summary; the class never
     fails the check. Your declared class is not read by CI, and path scope
     (owned/forbidden paths) is not enforced: check_pr_contract runs only
     against the shipped fixture. A higher computed class is review pushback. -->

- Declared risk class:

## Base / head

- Base SHA:
- Head SHA:
- Stacked on (if applicable): base PR #___ at base SHA ___

## Scope

- Owned paths touched:
- Explicit non-goals (what this PR does NOT do):

## Invariants

<!-- Properties that must remain true (e.g. no resource leak on failure paths,
     host/shader layout in sync). -->

## User-visible behavior

<!-- Any change a user/developer would observe, or "none". -->

## Godot upstream delta

<!-- Does this touch servers/scene/core/editor/platform? If yes, justify. -->

## Validation

<!-- Exact commands and their results. Mark unavailable hardware as "not run". -->

```text

```

## GPU / performance evidence (R2+)

<!-- Runtime/GPU harness or benchmark evidence vs the immutable base; VRAM and
     frame-time numbers for perf/memory claims; real-scan visual validation for
     rendering-math changes. "N/A" for non-R2 changes. -->

## Review artifacts

<!-- Links to review results (correctness / GPU-performance), if produced. -->

## Known blind spots

<!-- What you could not verify (e.g. no AMD/Intel GPU evidence). -->

## Rollback plan

<!-- How to revert safely if this regresses. -->

## Checklist

- [ ] Scope is focused and justified; no unrelated changes.
- [ ] No secrets or tokens added.
- [ ] No generated artifacts, transcripts, or session IDs committed.
- [ ] No baseline/threshold/gate weakened to make a check pass.
- [ ] Validation commands and results included above.
- [ ] Canonical docs updated if behavior changed.
