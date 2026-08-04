# Evidence Integrity

How this repository distinguishes *"this was checked and passed"* from *"this was
not checked"*. It is the single most common defect class found in the module's
guards, tests and CI — not because the work was careless, but because the two
states are easy to encode identically and almost impossible to tell apart
afterwards.

This page is normative for guards, tests, CI configuration and any artifact
presented as evidence. See also [contribution standards](contribution-standards.md),
[review policy](review-policy.md) and [`tests/AGENTS.md`](../../tests/AGENTS.md).

## The rule

> **Absence of a signal is never a passing signal.**
>
> "Did not run", "could not run", "found nothing to check" and "ran and passed"
> must have four distinguishable encodings. Collapsing any of them into "pass"
> is a defect even when the collapsed value happens to be correct today.

## The question to ask before reporting a check as green

> **What would a failure have to look like for this check to see it?**
>
> If the answer is *"it couldn't"*, the check is not evidence yet — regardless
> of whether it passed.

Applied honestly this catches the whole class before review does. Every example
below was green at the moment it was found.

## Recurring shapes

Each row is a real defect found in this repository. The point is not the
individual bugs — it is that they are one bug wearing different clothes.

| Shape | What it looks like | Real instance |
| --- | --- | --- |
| **Fail-open on absence** | An empty, missing or unreadable input yields the *most permissive* result. | `classify_change.py` returned `R0` — the lowest risk class — for an empty changed-path set, while `policy.json` declares `default_unclassified: R3` (#812). A skipped guard emitted `passed: true` with a `fail_reason` nobody reads (#811). |
| **Skip encoded as pass** | An environment guard returns early; the runner scores it as success. | `REQUIRE_GPU_DEVICE()` expanded to `MESSAGE(...); return;` and doctest scored the bare return **PASS** (#595). A test opened with an *unconditional* `return` before 63 lines of body and passed with zero assertions, inside a strict lane (#813). |
| **Guard wired to nothing** | The check exists, is correct, and is never invoked. | The skip-marker gate was enforced at the lane level but its regex could never match doctest's real output, so it counted 0 for its entire life (#595). A 515-line pin test ran in no lane. A helper was unit-tested directly and nothing called it. |
| **Self-certifying fixture** | The test manufactures input in a format the real producer never emits. | The skip gate's only test fabricated the marker at column 0 — a shape doctest never produces — under a comment asserting that doctest emits it (#595). |
| **Compared against itself** | A ratchet reads its "reference" from the same commit or document it is checking. | A shrink-only guard resolved its base with `git show HEAD:<file>`; in CI `HEAD` *is* the proposed commit, so it compared the change with itself and could never see an increase (#817). An earlier guard "read the allowed backlog out of the manifest and compared the manifest against itself" (`test_gpu_harness_deferred_contract.py`). |
| **Fixture cannot represent the property** | The test harness structurally cannot express what is being guaranteed. | A single-commit fixture cannot distinguish `HEAD` from the review base, so every test written in it passed against a broken ratchet. A single-environment run cannot detect environment dependence. Checking one workflow cannot answer "are the gates unblocked?" |
| **Hand-written list guarding an invariant** | Coverage is enumerated by a human and drifts. | A five-name macro list missed object-like macros, then delegating wrappers (#595). Field lists, event lists and file lists have all drifted the same way. |
| **Net-zero change** | A count-based check passes because a removal offsets an addition. | Fix-one/add-one netted zero and was written into a baseline. A same-size declaration swap, and a doctest-equivalent pattern rewrite, both passed every pre-existing guard (#650). |
| **Summarising across a boundary the data does not cross** | A field claims more than its inputs support. | `zero_coverage` derived from *passed* counts reported an all-failing lane as "nothing ran". `strict_failures` derived from an outcome mis-attributed an advisory anomaly to a strict lane. A field named `executed` only meant "a summary was printed" (#705). |
| **Green where the gate cannot fire** | The check runs in a mode that structurally cannot fail. | Enforcement sat behind `_is_ci()`, so every local run was green and said nothing about CI (#595). A stacked PR runs 2 of 17 checks because `branches: [master]` filters on the base (#823). |
| **Stale tracking presented as live** | A pointer to accountability outlives the accountability. | 9 of 10 quarantine declarations — covering 85 of 86 stranded cases — cited **closed** issues (#650). |
| **A fix that relocates the defect** | The primitive survives under a new name, and the test moves with it. | A laundering path closed in `--write-baseline` reappeared behind `--bootstrap-baseline`; the test covered only the old flag (#595). |

## Required practices

These are mandatory; a reviewer should treat a violation as a finding.

1. **Encode absence distinctly.** A guard that cannot run must report a state
   that is not `pass`, and consumers must fail closed on it. If a sentinel means
   "not measured", something must require a real measurement before that value
   can back a release claim.

2. **Ratchet against an immutable reference outside the change under review.**
   Never `HEAD`, never the document being checked. Fail closed when the
   reference cannot be resolved — "cannot determine the base" and "nothing
   changed" must not share an encoding.

3. **Mutation-prove every guard.** Revert the fix, show the test goes RED,
   restore, show GREEN. A test that passes with its fix reverted proves nothing.
   Include a mutation that **deletes the wiring**, not only the logic — a guard
   invoked by nothing is the most common way a check becomes decorative.

4. **Prefer property-shaped assertions to mechanism-shaped ones.** Assert *"no
   invocation by any route can increase the set"*, not *"this flag refuses"*.
   The first survives a refactor; the second moves with the defect it was
   written against.

5. **Derive, never enumerate.** Any list that defines coverage — macro names,
   fields, lanes, workflow events, files — must be derived from the source of
   truth. A hand-maintained list guarding an invariant is already broken.

6. **Verify the legal route, not only the illegal one.** Confirming that a
   guard blocks the bad case says nothing about whether the good case still
   works. A guard with no passing route for a legitimate operation gets bypassed,
   not obeyed.

7. **State the mode and the scope of every result.** "Green" is meaningless
   without *which environment, which event, which commit range*. Where hardware
   or a mode was unavailable, write **"not run"**, never "passed".

8. **Declare the limits.** When a check cannot cover a known shape, say so
   explicitly and name the shape. A guard documented as a ratchet over an
   *enumerated* set of shapes is honest; the same guard implying corpus-wide
   proof is not.

## Why this page exists

Twenty-two distinct instances of the shapes above were found in a single
review pass across the test harness, CI guards and release-evidence layer. Every
one was green. None were caught by the checks that existed, because in each case
the check could not observe the thing it claimed to rule out.

The individual fixes are in the git history. The rule is here so the next one is
recognised as a member of a known family rather than rediscovered as a novelty.
