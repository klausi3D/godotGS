# ADR: Phase-1 guard hardening — a closed cluster of remediations (#891–#910)

- **Status:** Proposed. **Filed before implementation**, which is what R3 asks for and what
  [`adr-gate-evaluates-pr-diff.md`](adr-gate-evaluates-pr-diff.md) could not claim. No code in
  this cluster has been written. Nothing here is a report of work already done.
- **Risk class:** this document is R0 (`docs/**`). The changes it covers classify R1–R3; §1
  records each member's class as **measured** by `scripts/agentic/classify_change.py` at
  `adcd6916dbd`, not as planned.
- **Scope of the discharge:** `.agentic/policy.json` requires "Design record (ADR or
  design-change issue) before implementation" for R3. This ADR discharges that requirement
  **for exactly the tasks in §1 and for nothing else.** §1's closure rule is not decoration.
- **Findings:** `GS-AUDIT-TEST-004` … `TEST-013`, `GS-AUDIT-BUILD-001`, `GS-AUDIT-SYS-011`,
  `GS-AUDIT-GPU-017`, `GS-AUDIT-DOC-001`, `GS-AUDIT-DOC-002`. Audit snapshot `55bd3953475`.
- **Verified against:** `origin/master` at `adcd6916dbd`. Every count, class and file
  reference in §§1, 4–8 was re-derived from that tree; the derivations are named inline so a
  reviewer can re-run them rather than trust them.
- **Landing:** its own R0 PR (`docs/**` only), merged **before** any implementation PR in §1
  opens. The PR also adds the entry to [`index.md`](index.md), whose ADR list is the only
  hand-maintained registry and has drifted before.
- **Precedent:** [`adr-gate-evaluates-pr-diff.md`](adr-gate-evaluates-pr-diff.md) (the R3
  disposition this ADR templates), [`adr-advisory-lane-ledger.md`](adr-advisory-lane-ledger.md)
  (measure-then-arm), [`adr-test-quarantine-manifest.md`](adr-test-quarantine-manifest.md)
  (declared, owned, expiring exclusions).

**Programme principle.**

> **Class follows the right design; design never follows class.** Scoping a fix down to dodge
> R3 is the same shape as weakening a gate to make it pass.

This governs every member of §1, and it is stated here rather than buried in a decision record
because it is the rule that produced two of the five decisions in §11. Where a member's class
was left open, the open thing was always the *design*; the class was then read off whatever
design turned out to be correct. The inverse move — picking a cheaper scope because it grades
R1, and calling that a scoping decision — is the failure this ADR's own subject matter exists
to catch, one level up. §11 Q1 (#895's scope) and §11 Q5 (T10's reach) are the two places the
principle was tested; both were settled by it, and Q1 was settled *against* the cheaper class.

## 1. Membership — this ADR covers exactly these tasks

Of the fifteen remaining Phase-1 sub-tasks, **eight measure R3 unconditionally** (#891, #892,
#893, #894, #896, #897, #898, #904); **#895 measures R3 on the scope the maintainer chose**
(§11 Q1); #903's class follows from its design and is not pre-decided (§11 Q5); and one
(#901) is deliberately floored at R3 by §7.
Read literally, each would need its own design record. They are one programme against one
defect shape — a guard that reports green while observing nothing — so they get one record.
The price of that economy is that the list is closed.

| Task | Issue(s) | Finding | Class (measured at `adcd6916dbd`) | Design |
| --- | --- | --- | --- | --- |
| T3 runtime completion marker | #891 | `TEST-007` | **R3** | §4 |
| T4 GPU retag + quarantine split | #892; children #906, #907, #908, #909, #910 | `TEST-006` | **R3** | §5 |
| T5 release waiver expiry | #893 | `TEST-005` | **R3** | §9 |
| T6 `validate_automation` wiring | #894 | `TEST-004` | **R3** | §9 |
| T7a synthetic-fixture floor | #895 | `TEST-009` | **R3** — full scope, enforcement at the consumer (§11 Q1) | §9 |
| T7b orphan runtime probes | #896 | `TEST-010` | **R3** | §9 |
| T7c GPU-evidence path filter | #897 | `TEST-011` | **R3** | §8 |
| T7d StringName orphan guard | #898 | `TEST-012` | **R3** (the task plan carried R1 — see note) | §9 |
| T8a metric provenance | #899 | `SYS-011` | **R1** alone; **R2** if the fix reaches `renderer/` | §9 |
| T8b frame-completion tracker | #900 | `GPU-017` | **R2** | §9 |
| T9a fork-delta truth + drift guard | #901 | `DOC-001` | **R1 → R3 by design** | §7 |
| T9b `nodes/README.md` truth | #902 | `DOC-002` | **R1** | §9 |
| T10 production-defaults pixel coverage | #903 | `TEST-008` | **Not pre-decided — the class follows the design** (§11 Q5). **R1** if the correct design stops at scenes + `qa_test_runner.gd`; **R3** if it needs `.github/workflows/baseline_qa.yml`. §6 covers T10 either way | §6 |
| T11a stale fork claim | #904 | `TEST-013` | **R3** | §9 |
| T11b dead shaders | #905 | `BUILD-001` | **R2** | §9 |

Deliberately **not** members, and not covered: **T1** (#887, #888, #889), which landed as
**#886** with its own ADR; and **T2** (#890), in flight as **PR #911** with its own
disposition. Both are cited here (§3, §10) but neither draws on this record.

**Note on #898.** `GS-AUDIT-TEST-012`'s defect is at `tests/ci/run_module_tests.py:2936-2987`,
and that exact path is one of the five entries in policy's "CI deterministic-check /
release-gate machinery" R3 rule. A diff that fixes it is R3 by path, regardless of how small
the edit is. PR #911 measured the same escalation from the other side and declined the edit
rather than accept the class; here the class is accepted. See §7.1.

**Closure rule.** Any R3 change **not** in the table above needs its own design record. This
document may not be cited as the design record for work it does not name. Adding a row is an
**amendment to this ADR** — a revision proposed, reviewed and approved as a document change —
never an implementation detail decided inside the PR that wants the coverage. An open-ended
cluster ADR becomes the artifact future R3 PRs point at to skip their own; a closed list
cannot be stretched.

## 2. Programme decisions that apply to every member

These are stated once, here, and are binding on every PR in §1.

1. **Fail-closed over fail-open.** Every member is an instance of
   [`evidence-integrity.md`](../governance/evidence-integrity.md) required practice 1: a
   guard that cannot run must report a state that is not `pass`, and a consumer of required
   evidence must fail closed on it.
2. **A positive completion marker beats inferring success from absence.** Where a member has
   the choice between "no bad signal seen" and "a good signal produced", it takes the latter.
   §4 is the largest instance; the principle is not confined to it.
3. **Declared limits live in the guard's own docstring.** Not in a PR body, not in an issue
   comment. A limit a future reader cannot find while reading the guard is a limit that will
   be rediscovered as a defect.
4. **Acceptance is mutation-proof, in two directions.** Every repaired guard must be shown
   RED under **(a)** the defect it guards, and **(b)** its own wiring deleted — the step
   removed, the call site dropped, the function stubbed to a passing return. A fix whose test
   cannot fail is rejected, and (b) is not optional: in #886 the deletion of the step that
   *ran* the new suite went unnoticed for three rounds while every assertion in the file
   stayed green.
5. **Baselines, quarantine manifests and fingerprints change only through their documented
   flows.** And a corollary that bites T4 specifically: **a retag is a rename.** It moves
   name-keyed digests while counts stay identical, so an unchanged count is not evidence that
   nothing moved. Where a member changes case names or tags, the acceptance evidence is the
   derived membership set before and after, never the count.

## 3. The R3 obligations disposition: cited, then instantiated

R3's evidence requirements include "Two independent reviews and CODEOWNER + human approval."
With a sole collaborator, two of those three are not achievable in form. **#886** established
the disposition; this ADR provides it as a **template**, not as a blanket.

The template, from #886:

> **R3 obligations disposition (owner).**
> The independent-review requirement is met in substance by two machine reviewers: *[an
> adversarial verifier with independently reproduced mutation evidence]* and *[a Codex review
> pass]*, which found *[N]* real defects, *[all fixed here with RED proofs / dispositioned as
> follows]*.
> The second-human-reviewer and CODEOWNER formalities are waived: structurally impossible
> with a sole collaborator.
> The ADR requirement is satisfied by `docs/architecture/adr-phase1-guard-hardening.md` §*[N]*,
> filed before implementation.
> The `streaming-gpu-ci` deterministic check *[was run / is waived: no runtime-reachable code
> in this diff]*.
> Known-open limits are enumerated in the guard docstring and tracked on *[#NNN]*.

**Every PR carries the pointer plus its own facts.** The bracketed slots are the point of the
template. A disposition that says *"Per #886 precedent"* and stops is a rubber stamp, and
rubber-stamping a waiver is the normalisation dynamic this programme exists to end. A
reviewer must reject a disposition that does not state, for that PR:

- **which** machine reviewers ran, and **how many rounds**;
- **how many findings** each produced, and the disposition of each — fixed with a RED proof,
  deferred with an issue, or rejected with a reason;
- whether the **`streaming-gpu-ci`** check genuinely did not apply to **this** diff. The
  waiver is available only where the diff contains no runtime-reachable code. Most members of
  §1 do not qualify: T3 edits the runtime harness itself, T4 retags C++ cases the harness
  executes, T10 adds QA scenes. For those, "waived" is wrong and "not run" is not "passed".

A correctly instantiated paragraph, for illustration (**the numbers below are placeholders** —
they are what a real PR replaces, not facts about any PR):

> **R3 obligations disposition (owner).** Independent review is met in substance by two
> machine reviewers, per the #886 precedent. The adversarial verifier ran 3 rounds and
> reproduced the mutation evidence independently; it found 2 real defects — the retag left
> `Stage-B instance depth culling toggles` outside the new `[Streaming]` filter because its
> name carries no `Streaming` token, and the manifest count stayed at 59 across the retag so
> the count check could not see the move — both fixed here, each with the RED transcript in
> the verification table. Codex ran 2 passes / 4 threads: 1 real defect (fixed, RED proof), 3
> dispositioned on the thread as no-change with reasons. Second-human-reviewer and CODEOWNER
> formalities are waived: structurally impossible with a sole collaborator. The ADR
> requirement is satisfied by `docs/architecture/adr-phase1-guard-hardening.md` §5, filed
> before implementation. **`streaming-gpu-ci` was run, not waived** — this diff retags C++
> cases the GPU harness executes, so the runtime-unreachability argument does not apply;
> result and runner recorded in the verification table above. Known-open limits are in
> `run_gpu_harness.py`'s docstring and tracked on #892.

## 4. T3 (#891) — the runtime completion-marker contract

`_classify_result` (`tests/runtime/run_runtime_validation.py:337-378`) treats `passed` as the
**fall-through**: exit 0, no `[RUNTIME_FAIL]`, no `[RUNTIME_SKIP]` ⇒ pass. A scenario that
prints nothing and exits 0 is indistinguishable from one that asserted everything. The argv
flags in the workflows defend only the *skip-marker* path; they do not touch this one.

### 4.1 The marker

**Decision.** A fourth marker, alongside `SKIP_MARKER` / `FAIL_MARKER` / `METRICS_MARKER` at
`run_runtime_validation.py:33-35`:

```
[RUNTIME_PASS] {"scenario": "<registry name>", "assertions": <int>}
```

Same grammar as `[RUNTIME_METRICS]` — a marker token followed by one JSON object on one line —
so the parser reuses the existing extraction shape rather than inventing a second one. Both
fields are required; a malformed or absent payload is a failure, not a missing-optional.

`_classify_result` gains one branch, evaluated after the existing failure and skip branches:
exit 0, no failure markers, no skip markers, **and no `[RUNTIME_PASS]`** ⇒ `status =
"failed"`, reason `no completion marker`. The fall-through to `passed` is deleted; `passed`
becomes reachable only from a marker.

### 4.2 Who emits it

**Decision: the scenario emits it; the harness only verifies it.** A harness that synthesises
the marker it checks is the tautology this programme exists to remove.

There is no shared GDScript test library today — 15 of the 16 `.gd` files under
`tests/runtime/` declare their own `const METRICS_MARKER := "[RUNTIME_METRICS]"` and friends.
Copying a fourth constant into 15 files repeats what the existing three already cost.
**Decision:** add one
shared `tests/runtime/gs_runtime_report.gd` that owns the marker constants and the assertion
counter, and port scenarios onto it as they are touched.

### 4.3 A scenario that legitimately asserts nothing

**Decision.** `assertions: 0` is a failure **unless** the scenario also emits a
`no_assertions_reason` string *and* its registry name appears in an explicit, expiring
allowlist in `runtime_scenarios.json`. Same posture as the quarantine manifest: an untracked
exemption is not available; a tracked one is, with an owner, an issue and an expiry.

This is not new doctrine in the repo. The GPU harness already treats "ran, asserted nothing"
as distinct from "passed" (`case_assert_audit_ok`, `zero_assertion_cases`, #695/#696), and
[`adr-advisory-lane-ledger.md`](adr-advisory-lane-ledger.md) §4a defines `zero_coverage` for
the same reason. T3 gives the runtime lane the concept it is missing.

### 4.4 `fail_on_skip` defaults moving into profiles

**Measured: this changes no CI lane, and one local path.**

Precedence is `--fail-on-skip` > `--allow-skips` > profile `fail_on_skip` > implicit
`resolved_gd_mode != "headless"` (`run_runtime_validation.py:1145-1153`). All three CI
invocations pass `--fail-on-skip` explicitly and therefore never reach the profile key at all
(`gaussian_production_gates.yml:344,356`; `release_ci_runtime.yml:117`). Under each profile's
own declared `gd_mode`, the effective value today is already `true` for all six profiles:
`headless-ci` sets `true` explicitly against a headless default of `false`; the other five
are `non-headless` or `windows-vulkan` and inherit `true`.

So making the defaults explicit is behaviour-preserving where it matters. What it removes is
the *mode-coupled implicit default*: today a local `--gd-mode headless` run of `release-ci`,
`stress-only` or `smoke` silently flips `fail_on_skip` to `false`, because those three carry
no explicit key. After the change the profile's declared value holds regardless of the mode
override. That is a behaviour change on the local path only, and it is the intended direction —
a flag that silently loosens a gate when you change an unrelated setting is the same
absence-as-success shape one level up.

`TEST-007`'s evidence line describes `stress-only`/`smoke` as setting no `fail_on_skip`, which
is true of the key and **not** true of the effective value under their declared modes. The
finding's direction stands; that sub-claim is narrowed here rather than repeated.

### 4.5 The renderer-proof probe fails closed when the probe is absent

`_rendered_content_ok()` (`test_canonical_node_asset_render.gd:343-347`) returns `true` when
`rendered_content_probe_available` is false — the repo's only runtime renderer-proof emitter
passes when it could not look. **Decision:** make it a conjunction — probe available **and**
content seen. A renderer without `has_rendered_content()` (`:180`) then fails the proof.

`_build_renderer_proof_summary(required=True)` already fails when *no* proof metrics were
emitted at all (`:795-797`); the uncovered case is precisely *present-but-unavailable*, which
is the one this closes.

### 4.6 The real decision: advisory first, then blocking

**Decision: the repo's established advisory-then-blocking ladder, not an immediate
fail-closed flip.**

An immediate flip turns every unported scenario RED in one PR, and the pressure that follows
is to relax the parser — which recreates the defect with a fresh justification. The ladder:

- **Step 1 (this cluster).** The parser learns the marker. A scenario without it is recorded
  as `no_completion_marker` in `runtime_validation_report.json` and printed, but does not
  fail. All 11 registered GDScript scenarios are ported in the same PR, and their measured
  per-scenario assertion counts are recorded. **Porting all 11 is not the same as observing all
  11 in one run**, and step 2's trigger turns on that distinction: every registered scenario
  gets the marker here, but no single profile *selects* all 11, so no single run can witness
  them. Step 1's obligation is over the registry; step 2's is over what each profile runs.
- **Step 2 (gated on step 1's numbers, and it stays in this cluster).** The parser flips to
  fail-closed, and the assertion counts measured in step 1 are pinned as a shrink-only floor.
  Arming against measured values is the [advisory-lane-ledger](adr-advisory-lane-ledger.md)
  pattern; arming against a hoped-for value is how a ratchet ends up pinning zero.

  **The trigger is a soak, not a date and not a phase boundary** (settled: §11 Q4). Deferring
  the flip to "Phase 2" makes it someone's forgotten TODO, and deferred enforcement that
  nobody is holding is exactly the rot this audit catalogued — an advisory lane with no
  arming condition is an advisory lane forever. So the flip has a condition it can actually
  meet, in this cluster:

  - **Proposed soak, as two distinct obligations.** They are separate on purpose; collapsing
    them into one is what made the first draft of this paragraph unsatisfiable (see the
    correction below).
    1. **Per-profile completeness.** 5 consecutive runs of each CI-invoked profile, with
       `[RUNTIME_PASS]` present for every scenario **that profile selects** — not every
       registered scenario — and no `no_completion_marker` record in that profile's report for
       any of those runs.
    2. **Union coverage.** The profiles in the soak set must, between them, select **all 11**
       registered `GDS_TESTS` scenarios. This is asserted separately, as a property of
       `runtime_scenarios.json` at the moment the flip lands, and it is what makes obligation 1
       add up to a statement about the whole registry.

    The profile set is the three profiles CI actually invokes — **`headless-ci`** and
    **`streaming-gpu-ci`** (`gaussian_production_gates.yml:346,358`) and **`release-ci`**
    (`release_ci_runtime.yml:119`) — not all six declared profiles. A profile no lane invokes
    cannot soak, and counting it as soaked would be the same absence-as-evidence shape one
    level up.

    **Both halves measured at `adcd6916dbd`**, by reading `gd_tests` out of
    `tests/runtime/runtime_scenarios.json` and the registry out of `GDS_TESTS`
    (`run_runtime_validation.py:107-119`):

    | Profile | Scenarios selected | Covers all 11? |
    | --- | --- | --- |
    | `headless-ci` | 3 | no |
    | `streaming-gpu-ci` | 3 | no |
    | `release-ci` | 10 | no |
    | **union of the three** | **11** | **yes** |

    The union holds, but only just, and on one scenario: **`Streaming GPU Tier Budget
    Contract` is selected by `headless-ci` alone** and is the single scenario `release-ci`
    omits. Drop `headless-ci` from the set, or edit its `gd_tests`, and the union silently
    stops covering the registry while every remaining obligation still passes.

    **So obligation 2 is a guard, not a note.** The flip PR asserts the union property in code
    — derived from `runtime_scenarios.json` and `GDS_TESTS`, never from a hand-written list of
    scenario names, per §2 and `evidence-integrity.md` practice 5 — and **a future profile edit
    that breaks it must fail loudly.** A registered scenario that no soaked profile selects is
    a coverage hole, and the failure must name the orphaned scenario. The forbidden outcome is
    the quiet one: the soak keeps passing over a shrinking share of the registry, and the
    fail-closed flip is armed by evidence about scenarios nobody ran.

  - **Correction, recorded because the mistake is instructive.** The first version of this
    paragraph required `[RUNTIME_PASS]` from *every registered scenario in every run*. Measured
    against the table above, **no profile selects all 11**, so no run could ever qualify, step 2
    could never arm, and the missing-marker path would have stayed advisory forever. That is
    precisely the outcome §11 Q4 exists to prevent — an unsatisfiable trigger is a forgotten
    TODO with a schedule attached — and it is a vacuous gate in the strictest sense this repo
    means it: a condition that can never fire can never discriminate, so it would have read as
    a rigorous trigger while being no trigger at all. It is recorded here rather than quietly
    corrected because the tightening felt like *strengthening* the requirement at the time. The
    next author revising this trigger should check satisfiability against real profile
    selections before tightening it again.
  - **This N and this profile set are a proposal, not a measurement.** They are chosen before
    step 1 has produced a single run. The flip PR must confirm them against real runs and say
    so: if 5 runs is too few to have seen the flaky path, or a profile turns out never to emit,
    the flip PR revises the trigger *and states the revision* rather than quietly meeting the
    letter of this paragraph. One cost is already visible and the flip PR must not be surprised
    by it: **`release-ci` is a nightly evidence lane, not a PR gate**
    (`release_ci_runtime.yml`, `cron: "0 6 * * *"`, "intentionally NOT a required PR gate"), so
    5 consecutive `release-ci` runs is 5 nights of wall-clock. If that latency is the reason to
    change the trigger, the flip PR says so and proposes the alternative — it does not silently
    drop `release-ci` from the set.
  - **Precondition: the reports the soak reads must survive the run. Today one of them does
    not.** Verified at `adcd6916dbd`: neither invocation in `gaussian_production_gates.yml`
    passes `--report-path` (there is **no** `--report-path` anywhere under `.github/workflows/`),
    so both fall back to the same default — `tests/runtime/runtime_validation_report.json`
    (`run_runtime_validation.py:217-221`, default at `:219`) — inside the **same job and the
    same workspace**. The write is truncating (`report_path.open("w")`, `:1205`), so the
    `streaming-gpu-ci` run at
    `:351-360` **overwrites the `headless-ci` report** written at `:339-349`, and the single
    upload step at `:362-367` ships only the survivor.

    The consequence is exactly the error this cluster exists to remove: the soak would try to
    confirm that no `no_completion_marker` appeared in the **headless** reports by reading a
    file that no longer contains them, and would read that silence as a pass. Absence of a
    signal is never a passing signal — and here the absence is not even a gap in the evidence,
    it is evidence that was produced and then destroyed. Note that `headless-ci` is the only
    profile selecting `Streaming GPU Tier Budget Contract`, so the lost report is the one
    carrying the union's single load-bearing scenario.

    **Therefore: distinct `--report-path` values per invocation, and both artifacts preserved,
    are a precondition of the soak counting at all.** No qualifying run exists before this
    lands. This is **T3 implementation work on `.github/workflows/gaussian_production_gates.yml`**,
    and it does not move T3's class — measured, not assumed:

    ```
    $ python scripts/agentic/classify_change.py --paths \
          tests/runtime/run_runtime_validation.py tests/runtime/gs_runtime_report.gd \
          tests/runtime/runtime_scenarios.json
    risk_class: R3
    $ python scripts/agentic/classify_change.py --paths \
          tests/runtime/run_runtime_validation.py tests/runtime/gs_runtime_report.gd \
          tests/runtime/runtime_scenarios.json .github/workflows/gaussian_production_gates.yml
    risk_class: R3
      R3  .github/workflows/gaussian_production_gates.yml  (Release / security / CI workflow surface)
    ```

    T3 is R3 on `run_runtime_validation.py` alone; the workflow edit adds a second R3 path and
    changes nothing about the obligations. **This ADR does not make that edit** — it is an R0
    design record, and the workflow change belongs to T3's implementation PR. A later reader
    must not assume it is already done: check the two invocations for `--report-path` before
    counting a single soak run.

    `release-ci` is unaffected — it runs in its own workflow and job and uploads under its own
    artifact name (`release_ci_runtime.yml:124-129`), so nothing overwrites it.

  - **If the soak exposes marker gaps, that is a finding to fix, not a reason to stay advisory
    silently.** A scenario that intermittently omits the marker has an intermittent
    completion path, which is the defect `TEST-007` describes. The available responses are:
    fix it, or give it a tracked, expiring `no_assertions_reason` allowlist entry per §4.3.
    "Extend the soak until it goes green" is not one of them, and neither is letting the
    advisory step run on with no one holding the trigger.

**Migration cost, stated honestly.** The registry holds **13** scenarios: 11 GDScript
(`GDS_TESTS`, `run_runtime_validation.py:107-119`) and 2 C++ (`CPP_TESTS`). Plus **4** orphan
probes (`GS-AUDIT-TEST-010` / #896) that are in no registry, no profile and no workflow.

- The 11 GDScript scenarios are ported by the T3 implementer, in step 1.
- The **2 C++ harnesses** must emit the marker too, and **nothing in CI will observe it**:
  `--skip-cpp` is universal across all three CI invocations, and `run_cpp_harnesses` hardcodes
  `fail_on_skip=False` (`:569`). Porting them is bookkeeping against a lane that does not run.
  It is in scope because leaving two registry members structurally exempt is how the next
  audit finds this again; it is not evidence, and the PR must not present it as evidence.
- The **4 orphans** are #896's business, not T3's. **Sequencing:** land #896 before step 2, so
  the fail-closed flip never has to reason about scenarios that may be deleted. If #896
  registers them, they are ported with it; if #896 deletes them, they never need the marker.

## 5. T4 (#892, #906–#910) — batch grouping and the hollow batch

### 5.1 The derived partition

Re-derived at `adcd6916dbd` by importing `BATCHES` from `tests/ci/run_gpu_harness.py`,
enumerating every `TEST_CASE` under `modules/gaussian_splatting/tests/` and `tests/`, and
matching with `doctest_wildcmp` from `tests/ci/check_gpu_sorting_order_coverage.py` — doctest
semantics (brackets literal, case-insensitive), each batch's `excludes` subtracted. **Not
hand-picked**, and re-derivable by any reviewer with the same three inputs.

- **155** `[RequiresGPU]` cases written; **8** linker-dropped (`test_gpu_sorting.cpp`,
  `KNOWN_UNLINKED`, tracked on #622/#631) ⇒ **147 registered**.
- **87** match at least one batch; **39** match a required batch; **60** match none.
- 14 batches, 8 required. `ComputeInfrastructure` and `Streaming` match **0** cases each.

The 60 partition as follows:

| Issue | Group | Cases | Source |
| --- | --- | --- | --- |
| #906 | bare-tagged renderer pipeline | **38** | `test_renderer_pipeline.h` |
| #907 | device ownership / buffer lifetime / leak detection | **12** | `test_render_device_manager_ownership.h` (7), `test_memory_leak_detection.h` (3), `test_gpu_buffer_manager_lifetime.h` (2) |
| #908 | streaming | **3** | `test_gpu_streaming.h` |
| #910 | singletons | **6** | composite-hazard (2), importer (1), sorting-perf (1), phase-1 integration (1), render validation (1) |
| — | *(not in the split)* | 1 | `test_tile_renderer.cpp` — inside the `TileRenderer` filter, subtracted by the honored #643 `excludes` |

38 + 12 + 3 + 6 = **59**, exactly the count the wildcard declares
(`tests/ci/quarantine_manifest.json`: `*][RequiresGPU]*`, `count: 59`, issue #820, expiry
`2026-10-15`). The 60th unbatched-at-runtime case is the #643 waiver — a filter exclusion with
its own `deferred_requires_gpu_waivers` entry, not a member of the split and not this task's
work. #909 contributes **0** cases to the partition, for the reason in §5.3.

### 5.2 `Streaming` is a retag, not a filter edit

The filter is `*Streaming*][RequiresGPU]*`, which requires the token to appear **before** the
literal `][RequiresGPU]`. All three cases in `test_gpu_streaming.h` are tagged
`[GaussianSplatting][RequiresGPU]` and mention streaming only in the descriptive tail, so the
filter matches none of them. This is a tag-**order** defect.

**Decision: add the `[Streaming]` subsystem tag before `[RequiresGPU]` on the three cases.**
Not a widened filter — one of the three, `Stage-B instance depth culling toggles`, carries no
`Streaming` token anywhere in its name, so no filter edit could reach it. A filter loose
enough to try (`*Streaming*`, matching the descriptive tail) would instead pull in cases from
`test_renderer_pipeline.h` that belong to #906's group and from
`test_scene_director_submission_scaffolding.h` that are already batched — it would move the
boundary rather than fix it, and it would make batch membership depend on prose.

Per §2.5, the acceptance evidence for this retag is the derived membership set before and
after — the count is identical on both sides by construction.

### 5.3 `ComputeInfrastructure` is deleted, and retagging is rejected

All **11** `[ComputeInfra]` cases live in `test_compute_infrastructure.h` and **none** carries
`[RequiresGPU]`. They are null-device CPU tests: they pass `nullptr` as the `RenderingDevice`
and assert the error paths that produces (`:134`, `:137` —
`CHECK(result.detail.contains("RenderingDevice is null"))`).

**Decision: delete the `ComputeInfrastructure` batch. Retagging is explicitly rejected.** Two
reasons, and the second is the decisive one:

1. Tagging a null-device test `[RequiresGPU]` is a **false claim about what it exercises**. The
   batch would then run green on a GPU runner while proving nothing about the GPU — the
   permanently-green catalogue entry replaced by a permanently-green batch.
2. It would **remove existing coverage**. `run_module_tests.py:104` runs
   `("GaussianSplatting [ComputeInfra]", ("*GaussianSplatting*][ComputeInfra]*",),
   ("*][RequiresGPU]*",), True)` — a **strict** lane that explicitly *excludes*
   `[RequiresGPU]`. Adding the tag would evict all 11 cases from the strict lane that runs
   them today.

Deleting the batch therefore loses nothing and removes a hollow entry. This rejection is
recorded here so it is not "restored" later by someone reading only the zero-match symptom.

### 5.4 #820 closes **by** the split; `REQUIRED_BATCHES` stays enumerated

- **#820 is closed by the split, as superseded by #906–#910.** Its subject — the 59-case
  wildcard declaration — ceases to exist, and an open issue pointing at a declaration that is
  gone is worse than no issue. The wildcard is replaced by **four** named declarations — one
  each for **#906 (38), #907 (12), #908 (3) and #910 (6)** — carrying the same required fields
  the manifest already demands (`UNLANED_REQUIRED_FIELDS`: `test_case`, `count`, `reason`,
  `issue_url`, `owner`, `expires_utc`), whose counts sum to 59 at the moment of the split.
- **#909 gets no manifest declaration at all.** Its batch is deleted outright (§5.3), so it
  contributes **0** cases to the partition (§5.1) — and a zero-count declaration cannot exist:
  `tests/ci/check_test_lane_coverage.py:380` rejects any `unlaned_tests` entry whose `count`
  is not a positive integer
  (`if not isinstance(entry.get("count"), int) or entry["count"] < 1`). Five declarations is
  therefore not merely redundant here, it is **unrepresentable**; an implementer who reads
  "five" and writes five either fails the guard or invents an undocumented split to fill the
  fifth. #909's work is the batch deletion, not a declaration.
- **`REQUIRED_BATCHES` stays policy-enumerated, not derived.** It records a *decision* ("this
  batch must be able to fail CI"), and that is precisely the fact a tree which has already
  lost the batch can no longer tell you — the same reasoning
  `check_test_lane_coverage.py`'s `STRICT_COVERAGE_CONTRACTS` docstring gives for its own
  enumeration. Everything else in T4 is derived: counts, memberships, the partition itself.

### 5.5 A proven-green GPU run precedes any case entering a required batch

**Decision:** the #724 bar, restated as a rule rather than a precedent. Before any batch
enters `REQUIRED_BATCHES`, it must have run on the self-hosted GPU runner from a real build
with `>0` cases, `>0` assertions, `case_assert_audit_ok=true`, `zero_assertion_cases=[]`, and
recorded wall-time headroom against its budget.

**Corollary: none of #906–#910 promotes anything.** A retag moves cases into an *advisory*
batch; promotion is a separate, separately-evidenced step. Landing a retag and a promotion in
one PR would mean requiring a batch nobody has yet watched run.

## 6. T10 (#903) — production-defaults pixel coverage and advisory expected-fail scenes

The QA project pins `gaussian_splatting/composite/depth_test=false`
(`tests/examples/godot/test_project/project.godot`, `[rendering]`) while the shipped default is
`true` (`gaussian_splat_manager.cpp:998`). Phase-0 runtime evidence, on a binary built from
`55bd3953475`:

| Config | `scaling_3d` | scale | `depth_test` | Splats |
| --- | --- | --- | --- | --- |
| default-default | Bilinear | 1.00 | **true** | **PRESENT** |
| scaled | Bilinear | **0.75** | **true** | **ABSENT** (`GS-AUDIT-GPU-001`) |
| temporal | **FSR2** | 1.00 | **true** | **ABSENT** (`GS-AUDIT-GPU-001`) |

### 6.1 The default-default scene lands blocking, now

**Decision.** scale 1.0 + `depth_test=true` is green today, so it joins `test_scenes` in
`qa_test_runner.gd` with a captured baseline. No ladder, no advisory step — a scene that
passes on the first run belongs in the blocking suite immediately, and this one closes the
single largest blind spot in the only per-PR pixel gate.

### 6.2 The two red scenes land as advisory expected-fail oracles

**Decision.** A new `EXPECTED_FAIL_SCENES` map beside `QUARANTINED_SCENES` in
`qa_test_runner.gd`, carrying the same fields the quarantine manifest requires (reason, issue,
owner, expiry) **plus a required `expected_failure` signature**. Five behaviours, all pinned:

| Observed | Outcome |
| --- | --- |
| scene runs, **RED, matching** the entry's `expected_failure` | recorded as `expected_fail`, counted in the summary and in `qa_results.json`, **suite does not fail** |
| scene runs, **RED, not matching** the entry's `expected_failure` | **suite fails**, reported as `unexpected_failure` with both the expected and the observed signature printed |
| scene runs, **GREEN** | **suite fails.** Either GPU-001 is fixed — remove the entry and promote — or the scene stopped asserting. Both need a human |
| scene produced **no result** | fails closed, as a missing quarantine entry does today |
| entry's **expiry passes** | fails, matching the quarantine manifest's clock-checked expiry |

**Why the signature is required, and not optional.** Without it the table classifies *any* red
as `expected_fail`, and the suite is then blind in the one direction that matters most: a scene
that still exhibits GPU-001 **and** develops a second, unrelated regression looks exactly like a
scene that only exhibits GPU-001, and stays advisory indefinitely. So does a scene that stops
failing for GPU-001's reason and starts failing for a brand-new one. The green-fails-the-suite
row catches *repair*; it cannot distinguish the known failure from a new one, and nothing else
in the table can either. Reading "it is red, as expected" off a red that was never checked
against what was expected is precisely the shape
[`evidence-integrity.md`](../governance/evidence-integrity.md) forbids — absence of a
distinguishing signal read as a passing signal — and it is the same shape as the `[RUNTIME_PASS]`
gap §4 exists to close, one lane over.

**What the signature is, and the judgement behind it.** **Pin a stable failure reason /
assertion identity, not a pixel fingerprint.** Concretely, the entry declares the identity of
the assertion the scene is expected to fail on — a stable machine token the scene emits
alongside its human message, in the same spirit as the existing `[QA_SKIP]` marker
(`qa_test_base.gd:29`) — optionally narrowed by a predicate over the metrics the runner already
carries through `get_result_metrics()` / `append_renderer_diagnostics()`
(`qa_test_base.gd:109,113-158`, which already surfaces `visible_splats` and `sorted_splats`).
GPU-001's signature is "**splats absent**", i.e. a splat-count-zero condition on the scene's own
content assertion.

The alternative — hashing the failing frame and matching the hash — is rejected. A pixel
fingerprint over a *visual mismatch* is brittle in exactly the wrong direction: driver
revisions, unrelated shader changes, tone-mapping tweaks and ordinary sampling noise all move
the bits without changing what failed, so the guard would emit false `unexpected_failure` reds
for changes that have nothing to do with GPU-001, and the pressure that follows is to delete the
signature rather than fix anything. An assertion identity is stable under everything except a
change to *why* the scene fails, which is the only event the signature is meant to detect.

**One thing the T10 implementer must measure rather than assume.** Which stage GPU-001's
zero-count is observable at is *not* settled here. The defect is conditioned on
`composite/depth_test=true`, so it may well be a composite-stage loss with `visible_splats > 0`
upstream — in which case the signature must key on the scene's own content assertion and not on
`cull_gpu_visible_count`. T10 records the observed metric values for both red configs and pins
the signature against what it measured, not against what this paragraph guessed.

**Why not quarantine.** A quarantined scene does not run and therefore cannot observe its own
repair. An expected-fail scene runs every time and is a live regression oracle for the fix.

**Why the suite cannot be mistaken for broken.** The summary distinguishes `failed` from
`expected_fail`, prints the issue reference for each, and a suite whose only red is
expected-fail exits 0 with one line naming the scenes and their issues. A reader who sees red
scene output and a zero exit code is told, in the same output, why.

### 6.3 The entries point at GPU-001's own issue, filed at T10 time

**Decision (settled: §11 Q2). `GS-AUDIT-GPU-001` gets its own GitHub issue, filed when T10 is
implemented, and the `EXPECTED_FAIL_SCENES` entries point at that issue — not at #903.** The
issue is **not** filed now; T10 files it, and this ADR records only that it must exist before
the entries do.

Two reasons, and the second is the operative one:

- **Filing it is inside the Phase-1 exclusion policy, not an exception to it.** Phase-2/3
  findings were kept out of the Phase-1 issue batch because they were not being worked. The
  moment an in-repo artifact — the expected-fail oracle — references the defect, GPU-001 *is*
  active work: something in the tree now depends on the defect's identity and on someone
  noticing when it changes. That is the condition the exclusion was drawing the line at. It
  also costs nothing to write: GPU-001 is the best-evidenced finding in the audit —
  runtime-confirmed with the pixel matrix reproduced above — so the issue body comes straight
  out of the ledger.
- **Pointing at #903 would tangle two different closure events.** **#903 closes when the gate
  covers production defaults.** **The oracles flip to blocking when the defect is fixed.**
  Those are different events with different owners and different evidence, and a tracker can
  only mean one of them. Under the "re-point later" option, #903 stays open purely as a
  placeholder for a defect it does not describe, or it closes and the manifest is left
  pointing at a closed issue — which is the same shape §5.4 rejects for #820. Different
  events need different trackers.

### 6.4 The flip to blocking is gated on GPU-001 being fixed, not on a date

Removing an `EXPECTED_FAIL_SCENES` entry is the whole flip — the same property
`QUARANTINED_SCENES` already documents ("Removing an entry is the whole fix for that scene").
No separate promotion machinery is built for a two-entry map. The trigger is GPU-001's issue
closing (§6.3), which is why that issue and #903 must be distinct: #903 is done long before
this flip is possible.

### 6.5 Every QA/visual change is A/B'd on both `depth_test` values

**Decision, and it is a rule for the whole programme, not just T10.** Phase 0 measured that
the QA pin does not merely narrow coverage — it **inverts the result**: with
`depth_test=false`, splats present in *every* tested configuration — scale 1.0, scale 0.75 and
FSR2 alike — including the two that are broken at the shipped default. A test run under the QA pin
therefore returns a false *refutation* of the entire `depth_test=true` defect class, not a
weaker confirmation of it. Any QA or visual change in this cluster records both values, and a
result reported for only one value is not evidence.

## 7. New guards register themselves — a programme-wide rule, worked through T9 (#901)

**Rule (maintainer ruling, recorded here because it is design, not scope). Every
new `tests/ci/check_*.py` guard this programme creates adds itself to policy's "CI
deterministic-check / release-gate machinery" enumeration in the same diff that creates it.**
Not T9's alone. The consequence is deliberate and is the point: touching `.agentic/policy.json`
floors the PR at R3 via `SELF_REFERENTIAL_PATHS`, so a PR that introduces guard machinery is
graded as guard machinery from its first commit rather than from the first time someone
notices.

The rule generalises because the *reason* does. A guard is release-gate machinery on the day it
is written, not on the day someone remembers to enumerate it; the enumeration is what makes the
class true, and deferring it means the guard's own introducing PR — the diff where every design
decision inside it is made — is reviewed one or two classes below the machinery it is adding.
Splitting registration into a follow-up is the same move viewed from a different angle: it lands
an R3-grade guard under an R1 review.

**Instances in this cluster.**

- **T9 (#901)** — `tests/ci/check_engine_delta.py`. The worked example, measured below.
- **T10 (#903)** — its new override-diff guard inherits the rule identically. Note that §6 as
  written does not yet specify that guard's shape; the rule attaches to it the moment T10's
  design introduces it, and T10's PR carries the `.agentic/policy.json` edit and the resulting
  R3 grade with it. This also interacts with §11 Q5: a T10 that introduces a guard script is
  R3 by this rule regardless of whether it reaches `baseline_qa.yml`.
- **No other member of §1 can be shown to introduce a new guard script from this ADR's text
  alone.** §9 records the remaining members as direct applications of their findings' remediation
  directions without naming files, so whether (for instance) T6's `validate_automation` wiring
  or T7b's orphan-probe work creates a new `tests/ci/check_*.py` is not determinable here. That
  is stated as a limit of this record, not as a finding that they do not. Each implementer
  checks the rule against their own diff; §10.1 is the guard that would make the check
  automatic.

### 7.1 T9 worked through, with the measurement

`tests/ci/check_engine_delta.py` does not exist yet (verified: no `*engine*` file under
`tests/ci/` at `adcd6916dbd`). As a new file under `tests/**` it matches only the R1 rule.
Measured:

```
$ python scripts/agentic/classify_change.py --paths ENGINE_PATCHES.md tests/ci/check_engine_delta.py
risk_class: R1
```

So the new guard that **polices R3 engine-delta drift** would itself be R1, because policy's
"CI deterministic-check / release-gate machinery" rule is a five-entry enumeration of
existing file paths and a new guard is not in it.

**Applying the rule: T9's PR adds its own new guard to that enumeration in the same diff.**
Adding it means editing `.agentic/policy.json`, and `SELF_REFERENTIAL_PATHS`
(`scripts/agentic/classify_change.py:57`, applied at `:129-133`, shipped in #886) forces any
diff touching that file to `ordering[-1]`. Measured:

```
$ python scripts/agentic/classify_change.py --paths ENGINE_PATCHES.md \
      tests/ci/check_engine_delta.py .agentic/policy.json
risk_class: R3
  R0  ENGINE_PATCHES.md                  (Docs / agentic governance)
  R1  tests/ci/check_engine_delta.py     (Local module or test change)
  R3  .agentic/policy.json               (risk policy change (self-referential; forced to the top class))
```

The PR that introduces the R3-policing guard is therefore graded R3 **by construction**, and
the gap closes in the same diff where it was found. The class is correct *because* the policy
edit rides along, not despite it — splitting the guard and its registration into two PRs would
land an R3-grade guard under an R1 review, which is the outcome this decision exists to
prevent.

The same mechanism, seen from the other side: **PR #911** declined to touch
`run_module_tests.py` precisely because that path alone flips a change from R1 to R3
"regardless of what the edit says", and its task was scoped R1. It documented the gap in the
guard's own docstring instead. Both decisions are right for their scope, and the pair is why
#898 is listed R3 in §1.

## 8. T7c (#897) — the GPU-evidence path filter is derived from policy's R2 globs

**Decision (settled: §11 Q3). T7c derives `renderStreamingPrefixes` from `.agentic/policy.json`'s
R2 `path_globs`, rather than continuing to hand-maintain it.** This is design content, so it is
made *here*, as the amendment §11 Q3 said it would need — not inside #897's PR.

### 8.1 The doctrine's own test, and how it answers

`.github/workflows/gaussian_production_gates.yml:165-169` hand-lists three prefixes and uses
them to decide whether the Windows GPU evidence lane runs at all
(`:198`, `path.startsWith(prefix)`). Policy's R2 rule lists eight globs.
[`evidence-integrity.md`](../governance/evidence-integrity.md) required practice 5 ("Derive
coverage; enumerate only policy") states the test verbatim: **would discovering a new item mean
the list is *stale*, or that someone must *decide*?** Applied here: when a new
`streaming_*.cpp` appears, is the prefix list stale, or does someone need to make a decision?

**Stale.** A file that policy already grades R2 is, by that grading, a change that requires
GPU/runtime evidence — the decision was made when the R2 glob was written. The prefix list is
not a second, independent judgement about which R2 paths deserve evidence; it is a copy of the
first judgement that nobody re-copies. `REQUIRED_BATCHES` (§5.4) stays enumerated for the exact
opposite reason: it records a decision the tree cannot reconstruct. This list records a decision
the tree *can* reconstruct, from the file that made it.

**This does not contradict §10.1**, which argues that policy's own R3 machinery rule must stay
enumerated. The two lists sit on opposite sides of practice 5's test. Policy's enumeration
*makes* the decision — which files are release-gate machinery — and deriving it from a filename
pattern would let a rename change a file's risk class. The workflow's prefix list makes no
decision at all; it *describes* the set policy already decided, which is why it can go stale
without anyone noticing. Derive the description, enumerate the decision.

**The drift is not hypothetical, and it is not small.** Measured at `adcd6916dbd` by expanding
policy's R2 `path_globs` over `git ls-files` and comparing against the workflow's three
prefixes:

- **197** tracked files are R2 by policy; the prefix list matches **152**.
- **45 R2 files are invisible to the evidence lane**: `core/` **32**, `compute/` **7**,
  `lod/` **4**, `asset_management/` **2**.
- The prefix set is a strict **subset** — **0** files match a prefix without also matching an
  R2 glob. So deriving only ever widens the lane; it cannot silently drop a path that is
  covered today.

The `core/` figure is the doctrine's point made concrete. Policy's glob is
`modules/gaussian_splatting/core/*streaming*`; the workflow's prefix is
`modules/gaussian_splatting/core/gaussian_streaming`. The prefix reaches
`gaussian_streaming.cpp/.h` and **nothing else**, while `streaming_atlas.cpp`,
`streaming_upload_pipeline.cpp`, `streaming_vram_regulator.cpp`,
`streaming_eviction_controller.cpp` and **28 more** sit in the same directory, are R2 by
policy, and change without ever arming the lane. Nobody decided that. The list was written when
`gaussian_streaming.*` was the streaming code, and the streaming code moved.

**Note the matcher difference.** Policy expresses globs (`**`, `*streaming*`); the workflow
tests `String.prototype.startsWith`. Deriving therefore means translating globs to a matcher,
not string-substituting a list — `core/*streaming*` has no prefix form. #897 owns that
translation, and its acceptance evidence is the derived path set compared against the 197-file
expansion above, per §2.5: a count that matches is not evidence that the membership matches.

### 8.2 Required: an explicit, reasoned exclusion list

**Deriving alone would make divergence impossible; that is the wrong goal. Divergence must be
*visible*.** Some R2 path may genuinely not need the GPU evidence lane — a header-only
constants file, a directory of pure CPU policy types — and a derivation with no escape hatch
either forces the lane onto it or invites someone to quietly narrow the R2 glob in
`policy.json`, which would be far worse: it would move the file's *risk class* to buy a cheaper
CI run. That is the "design follows class" inversion this ADR's programme principle names.

**Decision:** the derivation is `R2 globs − declared exclusions`, where each exclusion is a
committed entry carrying a **reason**, consistent with how this repo's other declared-exclusion
mechanisms are shaped. Both existing precedents are per-entry field maps, and the new one
follows them rather than inventing a shape:

| Mechanism | Per-entry required fields |
| --- | --- |
| `tests/ci/quarantine_manifest.json` `unlaned_tests` | `test_case`, `count`, `reason`, `issue_url`, `owner`, `expires_utc` (`check_test_lane_coverage.py:160-167`) |
| `renderer_release_gate_manifest.json` `deferred_requires_gpu_waivers` | `test_name`, `issue_url`, `owner`, `expires_utc`, `risk`, `mitigation`, `docs_path` (`check_renderer_release_gates.py:379-388`) |
| **T7c's exclusion list (new)** | `path_glob`, `reason`, `issue_url`, `owner`, plus `expires_utc` — see below |

Three properties the list must have, each taken from a precedent rather than argued from
scratch:

1. **Fail closed on a malformed or incomplete entry**, as both precedents do — a missing field
   is a validation failure, not a silently ignored exclusion.
2. **An exclusion that matches nothing is itself a failure.** A stale exclusion for a deleted
   path is a licence sitting unused, and the next R2 file that happens to match it inherits the
   exemption without anyone deciding. `deferred_requires_gpu_waivers`' `docs_path` existence
   check is the same instinct.
3. **Expiry.** `expires_utc` is carried, on the quarantine manifest's clock-checked model, so
   an exclusion is re-argued rather than inherited. **This is the one field where a case can be
   made either way** — an R2 path that genuinely never needs the lane is a permanent fact, and
   a permanent fact on a 90-day expiry is churn. The decision here is to carry it anyway,
   because "genuinely never" is exactly the claim that ages badly in this repo, and a re-argued
   exclusion costs one line. #897 may argue the other side in its PR; it may not simply omit
   the field.

**Where it lives is #897's call, not this ADR's** — beside the workflow, or as a key in an
existing manifest — with one constraint: it must **not** live in `.agentic/policy.json`. Putting
it there would let a CI-cost argument be settled by editing the risk-policy file, and would
floor every subsequent exclusion at R3 for reasons that have nothing to do with the exclusion.

**What it is not.** The exclusion list is not a place to park R2 paths whose lane is currently
red. That is what the quarantine and waiver mechanisms above are for, and routing a red lane
through a coverage exclusion instead would hide it in the one list nobody reads as a failure
record.

## 9. Members with no independent design content

Each of the following is a direct application of its finding's remediation direction. Listing
them explicitly is required by §1's closure rule: a member with no design section still has to
appear, so that the absence is a recorded judgement rather than an omission.

**T7c (#897) is no longer on this list.** It was, until §11 Q3 settled that it derives its path
filter from policy; that decision is design content, so T7c now has its own record at §8. A
member moving off this list is the expected outcome of an open question closing, and the move
is recorded here so the deletion is visible rather than silent.

- **T5 — release waiver expiry (#893).** No independent design content — direct application of
  `GS-AUDIT-TEST-005`'s remediation direction.
- **T6 — `validate_automation` wiring (#894).** No independent design content — direct
  application of `GS-AUDIT-TEST-004`'s remediation direction.
- **T7a — synthetic-fixture floor (#895).** No independent design content — direct application
  of `GS-AUDIT-TEST-009`'s remediation direction, at the *consumer*, per the finding's own
  recommendation. Its **scope** was open and is now settled (§11 Q1) at full scope, class R3;
  scope is not design, so it stays listed here.
- **T7b — orphan runtime probes (#896).** No independent design content — direct application of
  `GS-AUDIT-TEST-010`'s remediation direction.
- **T7d — StringName orphan guard (#898).** No independent design content — direct application
  of `GS-AUDIT-TEST-012`'s remediation direction.
- **T8a — metric provenance (#899).** No independent design content — direct application of
  `GS-AUDIT-SYS-011`'s remediation direction.
- **T8b — frame-completion tracker (#900).** No independent design content — direct application
  of `GS-AUDIT-GPU-017`'s remediation direction.
- **T9b — `nodes/README.md` truth (#902).** No independent design content — direct application
  of `GS-AUDIT-DOC-002`'s remediation direction.
- **T11a — stale fork claim (#904).** No independent design content — direct application of
  `GS-AUDIT-TEST-013`'s remediation direction.
- **T11b — dead shaders (#905).** No independent design content — direct application of
  `GS-AUDIT-BUILD-001`'s remediation direction.

### 9.1 Sequencing constraints (not design)

- **#896 before T3 step 2** (§4.6), so the fail-closed flip is not reasoning about scenarios
  that may be deleted.
- **#894 before the structural YAML port** that [`adr-gate-evaluates-pr-diff.md`](adr-gate-evaluates-pr-diff.md)
  §5 defers to it. PyYAML is deliberately not a dependency of the required gate today; #894 is
  what makes it mandatory and fail-closed. #894's PR must record **which lane carries the
  dependency**, because a module-scope `import yaml` in `tests/agentic/` would `ImportError`
  at unittest *discovery* and red the only required check on every PR.
- **The wildcard's removal and the four named declarations are one atomic change.** Removing
  `*][RequiresGPU]*` (count 59) before the #906/#907/#908/#910 declarations exist leaves the
  manifest under-declared against a corpus that has not moved; adding them first double-declares
  the same 59 cases. Either state is a guard reporting on a tree that never existed. (Four, not
  five: #909 declares nothing — §5.4.) #909's batch deletion is not part of this atomic change;
  it touches `run_gpu_harness.py`'s `BATCHES`, not the manifest.

## 10. Known gaps and carried follow-ups

### 10.1 The gap this ADR opens and does not close

Policy's R3 machinery rule is a hand-written enumeration of five paths. **Enumerate-only is
the correct doctrine here** — a derived list would make the class follow the *filename*
instead of the *decision*: every new `tests/ci/` script would silently become R3, and renaming
a guard out of the pattern would silently drop it out of R3 with nothing to notice. The gap is
not the enumeration; it is that **nothing notices when a new `tests/ci/check_*.py` appears that is
neither listed nor explicitly exempted.** §7 states the self-registration rule for every guard
*this programme* creates, and the rule is binding — but it is binding on people, enforced by
review. Nothing in the tree fails when someone forgets it, and nothing at all constrains a guard
written outside this programme.

**Decision:** T9 is building a drift guard anyway. Fold the enumeration check into it **if
cheap** — for each `tests/ci/check_*.py` and `tests/ci/run_*.py`, require membership either in
policy's R3 machinery list or in an explicit, reasoned exempt list committed beside it, and
fail closed on a file in neither. If that is not cheap inside T9's diff, it is a **one-line
follow-up on #887**, not scope growth for any member of §1. Landing it is what turns §7's rule
from a convention into a guard, which is the difference this whole cluster is about.

### 10.2 Carried by reference — open, not re-opened here

From **#886**, tracked on **#887**:

- `scripts/agentic/classify_change.py` is **not** in `SELF_REFERENTIAL_PATHS` (verified: the
  tuple holds `.agentic/policy.json` only), so a PR rewriting the classifier publishes a class
  derived by its own code. Published-not-enforced surface; the accepted remedy is the one-entry
  addition.
- The **`JOB` key vs `name:` value** gap: the workflow guard matches the job *key*, while the
  required-check context is the `name:` value, so renaming only `name:` bricks the repo with
  every test green.
- The **`shell_text()` expression residual**: GitHub expression spans are stripped before
  matching, so a `||` *delivered into* the shell by an expression would be invisible to the
  default-deny rule.

From **PR #911**, tracked on **#890** (T2 — not a member of §1):

- A **whole-file rename** of a baselined test file produces a **false-RED**. Safe direction —
  it blocks a legitimate change and licenses nothing — but it is a known cost, with git rename
  detection named as the likely remedy.
- The **regenerate path compares against the flattened baseline** (all fingerprints, any key),
  a deliberate loosening disclosed in the PR: a fingerprint that *moves between keys* is not
  seen as an addition. Related, from the same PR: `--regenerate-null-deref-baseline` has no
  coverage beyond the single refusal test added there.
- **`_blob_at_base`'s `ls-tree` pre-check is unpinned** — deleting it leaves every test green
  (measured twice, rounds 2 and 3).

## 11. Decision record — five questions that were open, and how they were settled

These five were **genuinely open** when this ADR was first filed: each was a fork the author
could see but had no standing to close, and each was written up as an unanswered question with
both branches argued rather than a recommendation dressed as an analysis. **All five have since
been settled by the maintainer, and each decision is now folded into its owning section.** This
section is kept — rather than deleted once the answers landed — because the reasoning is the
part that is worth anything later: a reader six months out needs to know *why* #895 is R3, not
merely that it is, and a decision with its alternatives erased is indistinguishable from a
decision nobody made.

Two of the five (Q1, Q5) were settled by the programme principle stated in the preamble:
**class follows the right design; design never follows class.**

**Q1 — #895's scope, and therefore its class. Settled: FULL SCOPE, R3 accepted.**

*What was open.* `GS-AUDIT-TEST-009` puts the fixture-count floor "at load in
`run_runtime_validation`". Measured at `adcd6916dbd`:

```
$ python scripts/agentic/classify_change.py --paths \
      tests/runtime/prepare_synthetic_assets.py tests/runtime/check_benchmark_asset_paths.py
risk_class: R1
$ python scripts/agentic/classify_change.py --paths \
      tests/runtime/prepare_synthetic_assets.py tests/runtime/check_benchmark_asset_paths.py \
      tests/runtime/run_runtime_validation.py
risk_class: R3
  R3  tests/runtime/run_runtime_validation.py  (CI deterministic-check / release-gate machinery)
```

The producer-side-only scope grades R1 and is cheaper; the consumer-side scope grades R3 and is
the finding's own recommendation.

*Settled.* **Enforcement goes at the consumer, `run_runtime_validation`, as
`GS-AUDIT-TEST-009` recommends. The R3 class is accepted, not worked around.** Producer-side
enforcement guards the paths that *build* fixtures; it leaves the manifest default-open at the
point of *load*, which is precisely the hole that produced **#669** — a missing fixture yielding
0 splats, ~2400 FPS, and a passing benchmark. A floor that the loader does not check is a floor
that a stale import cache, a hand-edited manifest or a new load path walks straight past. R1
here would have bought a cheaper review by declining to guard the place the defect actually
occurred.

*Recorded in.* §1's row (now **R3**); §9's T7a entry, which keeps T7a on the no-design list
because scope is not design. Q1 is the clearest instance of the programme principle: the R1
option was available, defensible on paper, and wrong.

**Q2 — the `issue_url` for T10's expected-fail entries. Settled: GPU-001 gets its own issue, at
T10 time.**

*What was open.* `GS-AUDIT-GPU-001` has no GitHub issue — Phase-2/3 findings were deliberately
excluded from the Phase-1 batch — while the manifest fields require a real target. Either
GPU-001 is filed, or the entries point at #903 and are re-pointed later.

*Settled.* **The entries point at GPU-001's own issue, which T10 files when it is implemented.
Not #903, and not filed now.** Full reasoning in §6.3: filing it is *within* the Phase-1
exclusion policy rather than an exception to it, because an in-repo artifact referencing the
defect makes it active work; and #903 cannot serve, because **#903 closes when the gate covers
production defaults while the oracles flip to blocking when the defect is fixed** — two events,
two trackers.

*Recorded in.* §6.3 (new), and §6.4's flip trigger.

**Q3 — whether T7c derives `renderStreamingPrefixes` from policy's R2 globs. Settled: derive,
with an explicit reasoned exclusion list.**

*What was open.* `GS-AUDIT-TEST-011` raises deriving as an option; the ADR's first revision did
not take it, and flagged that taking it would be design content that could not be decided inside
#897's PR.

*Settled.* **Derive.** The rationale is `evidence-integrity.md` practice 5 applied to its own
test case — when a new `streaming_*.cpp` appears, the prefix list is **stale**, not awaiting a
decision, because policy already decided that R2 paths need GPU evidence. Measured drift at
`adcd6916dbd`: **45 of 197** R2 files are invisible to the evidence lane, 32 of them in `core/`
alone. **Required addition: an explicit exclusion list carrying a reason per entry**, so an R2
path that genuinely does not need the lane can be exempted *visibly* rather than by quietly
narrowing an R2 glob in `policy.json` — which would be the programme principle inverted, moving
a file's risk class to buy a cheaper CI run.

*Recorded in.* §8 (new, T7c's own design section); removed from §9's list, with the move noted
there. This is the amendment §11 Q3 said would be needed — made here as a reviewed document
change, exactly as the closure rule requires.

**Q4 — whether T3's fail-closed flip is in this cluster or in Phase 2. Settled: in-cluster, on
an evidence-based trigger.**

*What was open.* §4.6 placed step 2 in the cluster but did not say what fires it; if the arming
ratchet needed GPU-runner numbers, step 2 might have to become Phase-2 work.

*Settled.* **In-cluster, triggered by a defined soak — not a date and not a phase boundary.**
Advisory until N consecutive runs show the marker present across the profile set, then the flip
PR lands *citing the soak evidence*. Deferring to "Phase 2" would make the flip somebody's
forgotten TODO, which is the deferred-enforcement rot the audit catalogued: an advisory lane
with no arming condition never arms. And explicitly: **if the soak exposes marker gaps, that is
a finding to fix, not a reason to stay advisory silently.**

*Recorded in.* §4.6 step 2, including a proposed N and profile set that the flip PR must
confirm against real runs rather than inherit.

*Two corrections found in review, both kept visible in §4.6 rather than silently patched.* The
first draft of the trigger was **unsatisfiable** — it demanded a marker from every registered
scenario in every run, and no CI profile selects more than 10 of the 11 — so the "evidence-based
trigger" would have armed nothing, which is the same forgotten-TODO outcome this answer was
chosen to avoid, reached by the opposite route. It is now two obligations: per-profile
completeness, plus a separately guarded union-coverage property. The second: the soak's evidence
is currently **destroyed before it is read** — both `gaussian_production_gates.yml` invocations
write the same default report path and the later one overwrites the earlier — so distinct
`--report-path` values and preserved artifacts are now a stated precondition, and T3
implementation work. Neither correction changes the answer to Q4; both change whether the answer
could have been carried out.

**Q5 — whether T10 reaches `baseline_qa.yml`. Settled: not pre-decided; the class follows the
design.**

*What was open.* On the minimal scope — QA scenes plus `qa_test_runner.gd` — T10 measures R1,
not the R3 its task plan carried; it reaches R3 only if the workflow is edited.

*Settled.* **The class is deliberately left to follow the design, and is not pinned in advance
either way.** The question "does T10 reach `baseline_qa.yml`?" is answered by whether the
correct design needs the workflow edited — for instance to surface the expected-fail count on
the job — and not by which answer is cheaper to review. Pre-deciding "keep it R1" would be a
scope decision made for a class reason, which is the move the programme principle forbids;
pre-deciding "make it R3" would be the same error with the sign flipped, buying ceremony
instead of avoiding it. §6 covers T10's design **either way**, so the ADR requirement is
satisfied whichever class the finished design measures. Note also that §7's self-registration
rule floors T10 at R3 independently if its design introduces a new guard script.

*Recorded in.* §1's T10 row, reframed from a scope caveat to "the class follows the design".

## 12. What would change this decision

- **A second reviewer.** A CODEOWNER or second human who is not the author makes §3's waiver
  unnecessary; the disposition paragraph shrinks to facts with no waiver in it, and the
  rubber-stamp risk this ADR is guarding against disappears with it.
- **GPU-001 being fixed.** §6's expected-fail scenes become ordinary blocking scenes and the
  `EXPECTED_FAIL_SCENES` mechanism loses its reason to exist. It should then be **deleted**,
  not kept in case something else needs it — a general-purpose expected-fail facility is a
  general-purpose place to park red.
- **A member leaving the cluster, or a new one wanting in.** Either is an amendment to §1's
  table, reviewed as a document change. This ADR does not cover R3 work it does not name, and
  that is the property that makes the single-record economy defensible.
