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
  reference in §§1, 4–7 was re-derived from that tree; the derivations are named inline so a
  reviewer can re-run them rather than trust them.
- **Landing:** its own R0 PR (`docs/**` only), merged **before** any implementation PR in §1
  opens. The PR also adds the entry to [`index.md`](index.md), whose ADR list is the only
  hand-maintained registry and has drifted before.
- **Precedent:** [`adr-gate-evaluates-pr-diff.md`](adr-gate-evaluates-pr-diff.md) (the R3
  disposition this ADR templates), [`adr-advisory-lane-ledger.md`](adr-advisory-lane-ledger.md)
  (measure-then-arm), [`adr-test-quarantine-manifest.md`](adr-test-quarantine-manifest.md)
  (declared, owned, expiring exclusions).

## 1. Membership — this ADR covers exactly these tasks

Of the fifteen remaining Phase-1 sub-tasks, **eight measure R3 unconditionally** (#891, #892,
#893, #894, #896, #897, #898, #904); two more are R3 or R1 depending on scope decisions the
maintainer has not made yet (#895, #903); and one (#901) is deliberately floored at R3 by §7.
Read literally, each would need its own design record. They are one programme against one
defect shape — a guard that reports green while observing nothing — so they get one record.
The price of that economy is that the list is closed.

| Task | Issue(s) | Finding | Class (measured at `adcd6916dbd`) | Design |
| --- | --- | --- | --- | --- |
| T3 runtime completion marker | #891 | `TEST-007` | **R3** | §4 |
| T4 GPU retag + quarantine split | #892; children #906, #907, #908, #909, #910 | `TEST-006` | **R3** | §5 |
| T5 release waiver expiry | #893 | `TEST-005` | **R3** | §8 |
| T6 `validate_automation` wiring | #894 | `TEST-004` | **R3** | §8 |
| T7a synthetic-fixture floor | #895 | `TEST-009` | **R1 or R3 — scope decides** (§10 Q1) | §8 |
| T7b orphan runtime probes | #896 | `TEST-010` | **R3** | §8 |
| T7c GPU-evidence path filter | #897 | `TEST-011` | **R3** | §8 |
| T7d StringName orphan guard | #898 | `TEST-012` | **R3** (the task plan carried R1 — see note) | §8 |
| T8a metric provenance | #899 | `SYS-011` | **R1** alone; **R2** if the fix reaches `renderer/` | §8 |
| T8b frame-completion tracker | #900 | `GPU-017` | **R2** | §8 |
| T9a fork-delta truth + drift guard | #901 | `DOC-001` | **R1 → R3 by design** | §7 |
| T9b `nodes/README.md` truth | #902 | `DOC-002` | **R1** | §8 |
| T10 production-defaults pixel coverage | #903 | `TEST-008` | **R1** on the minimal scope (scenes + `qa_test_runner.gd`); **R3** only if it reaches `.github/workflows/baseline_qa.yml` (§10 Q5) | §6 |
| T11a stale fork claim | #904 | `TEST-013` | **R3** | §8 |
| T11b dead shaders | #905 | `BUILD-001` | **R2** | §8 |

Deliberately **not** members, and not covered: **T1** (#887, #888, #889), which landed as
**#886** with its own ADR; and **T2** (#890), in flight as **PR #911** with its own
disposition. Both are cited here (§3, §9) but neither draws on this record.

**Note on #898.** `GS-AUDIT-TEST-012`'s defect is at `tests/ci/run_module_tests.py:2936-2987`,
and that exact path is one of the five entries in policy's "CI deterministic-check /
release-gate machinery" R3 rule. A diff that fixes it is R3 by path, regardless of how small
the edit is. PR #911 measured the same escalation from the other side and declined the edit
rather than accept the class; here the class is accepted. See §7.

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
  per-scenario assertion counts are recorded.
- **Step 2 (gated on step 1's numbers).** The parser flips to fail-closed, and the assertion
  counts measured in step 1 are pinned as a shrink-only floor. Arming against measured values
  is the [advisory-lane-ledger](adr-advisory-lane-ledger.md) pattern; arming against a
  hoped-for value is how a ratchet ends up pinning zero.

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
  gone is worse than no issue. The wildcard is replaced by five named declarations carrying
  the same required fields the manifest already demands (`UNLANED_REQUIRED_FIELDS`:
  `test_case`, `count`, `reason`, `issue_url`, `owner`, `expires_utc`), whose counts sum to 59
  at the moment of the split.
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
owner, expiry). Four behaviours, all pinned:

| Observed | Outcome |
| --- | --- |
| scene runs, **RED** | recorded as `expected_fail`, counted in the summary and in `qa_results.json`, **suite does not fail** |
| scene runs, **GREEN** | **suite fails.** Either GPU-001 is fixed — remove the entry and promote — or the scene stopped asserting. Both need a human |
| scene produced **no result** | fails closed, as a missing quarantine entry does today |
| entry's **expiry passes** | fails, matching the quarantine manifest's clock-checked expiry |

The green-fails-the-suite row is the load-bearing one. Without it, an expected-fail entry is
just a quarantine with extra steps, and the day GPU-001 is fixed nobody notices.

**Why not quarantine.** A quarantined scene does not run and therefore cannot observe its own
repair. An expected-fail scene runs every time and is a live regression oracle for the fix.

**Why the suite cannot be mistaken for broken.** The summary distinguishes `failed` from
`expected_fail`, prints the issue reference for each, and a suite whose only red is
expected-fail exits 0 with one line naming the scenes and their issues. A reader who sees red
scene output and a zero exit code is told, in the same output, why.

### 6.3 The flip to blocking is Phase 2, gated on GPU-001, not on a date

Removing an `EXPECTED_FAIL_SCENES` entry is the whole flip — the same property
`QUARANTINED_SCENES` already documents ("Removing an entry is the whole fix for that scene").
No separate promotion machinery is built for a two-entry map.

### 6.4 Every QA/visual change is A/B'd on both `depth_test` values

**Decision, and it is a rule for the whole programme, not just T10.** Phase 0 measured that
the QA pin does not merely narrow coverage — it **inverts the result**: with
`depth_test=false`, splats present in *every* tested configuration — scale 1.0, scale 0.75 and
FSR2 alike — including the two that are broken at the shipped default. A test run under the QA pin
therefore returns a false *refutation* of the entire `depth_test=true` defect class, not a
weaker confirmation of it. Any QA or visual change in this cluster records both values, and a
result reported for only one value is not evidence.

## 7. T9 (#901) — the guard that would misclassify itself

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

**Decision: T9's PR adds its own new guard to that enumeration in the same diff.** Adding it
means editing `.agentic/policy.json`, and `SELF_REFERENTIAL_PATHS`
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

## 8. Members with no independent design content

Each of the following is a direct application of its finding's remediation direction. Listing
them explicitly is required by §1's closure rule: a member with no design section still has to
appear, so that the absence is a recorded judgement rather than an omission.

- **T5 — release waiver expiry (#893).** No independent design content — direct application of
  `GS-AUDIT-TEST-005`'s remediation direction.
- **T6 — `validate_automation` wiring (#894).** No independent design content — direct
  application of `GS-AUDIT-TEST-004`'s remediation direction.
- **T7a — synthetic-fixture floor (#895).** No independent design content — direct application
  of `GS-AUDIT-TEST-009`'s remediation direction. (Its *scope* is an open question, §10 Q1;
  scope is not design.)
- **T7b — orphan runtime probes (#896).** No independent design content — direct application of
  `GS-AUDIT-TEST-010`'s remediation direction.
- **T7c — GPU-evidence path filter (#897).** No independent design content — direct application
  of `GS-AUDIT-TEST-011`'s remediation direction. (The finding's optional "consider deriving
  from `policy.json` R2 globs" leg is **not** taken here; §10 Q3.)
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

### 8.1 Sequencing constraints (not design)

- **#896 before T3 step 2** (§4.6), so the fail-closed flip is not reasoning about scenarios
  that may be deleted.
- **#894 before the structural YAML port** that [`adr-gate-evaluates-pr-diff.md`](adr-gate-evaluates-pr-diff.md)
  §5 defers to it. PyYAML is deliberately not a dependency of the required gate today; #894 is
  what makes it mandatory and fail-closed. #894's PR must record **which lane carries the
  dependency**, because a module-scope `import yaml` in `tests/agentic/` would `ImportError`
  at unittest *discovery* and red the only required check on every PR.
- **The wildcard's removal and the five named declarations are one atomic change.** Removing
  `*][RequiresGPU]*` (count 59) before #906–#910's declarations exist leaves the manifest
  under-declared against a corpus that has not moved; adding them first double-declares the
  same 59 cases. Either state is a guard reporting on a tree that never existed.

## 9. Known gaps and carried follow-ups

### 9.1 The gap this ADR opens and does not close

Policy's R3 machinery rule is a hand-written enumeration of five paths. **Enumerate-only is
the correct doctrine here** — a derived list would make the class follow the *filename*
instead of the *decision*: every new `tests/ci/` script would silently become R3, and renaming
a guard out of the pattern would silently drop it out of R3 with nothing to notice. The gap is
not the enumeration; it is that **nothing notices when a new `tests/ci/check_*.py` appears that is
neither listed nor explicitly exempted.** §7's decision closes the case for one file, by hand,
because that file happens to be the one we are writing.

**Decision:** T9 is building a drift guard anyway. Fold the enumeration check into it **if
cheap** — for each `tests/ci/check_*.py` and `tests/ci/run_*.py`, require membership either in
policy's R3 machinery list or in an explicit, reasoned exempt list committed beside it, and
fail closed on a file in neither. If that is not cheap inside T9's diff, it is a **one-line
follow-up on #887**, not scope growth for any member of §1.

### 9.2 Carried by reference — open, not re-opened here

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

## 10. Open questions for the maintainer

**Q1 — #895's scope decides its class, and the two answers differ in substance.**
`GS-AUDIT-TEST-009` puts the fixture-count floor "at load in `run_runtime_validation`", which
is an R3 path (measured R3). Enforcing it only in `prepare_synthetic_assets.py` and
`check_benchmark_asset_paths.py` measures R1 — but leaves the *consumer* unguarded, and the
consumer is where the 10×-thinned asset is actually loaded. The R1 scope is cheaper and
weaker; the R3 scope is the finding's own recommendation. Which, and does §1's row change?

**Q2 — what `issue_url` do T10's expected-fail entries point at?** `GS-AUDIT-GPU-001` has
**no GitHub issue**: Phase-2/3 findings were deliberately excluded from the Phase-1 batch. The
manifest fields require a real target. Either GPU-001 gets an issue before T10 lands, or the
entries point at #903 and are re-pointed when GPU-001 is filed. The first is cleaner; the
second is free.

**Q3 — should T7c derive `renderStreamingPrefixes` from `policy.json`'s R2 globs?**
`GS-AUDIT-TEST-011` raises it as an option and §8 does not take it. Deriving couples a workflow
to the policy file and removes one hand-maintained list; enumerating repeats exactly the drift
that produced the finding. This is a genuine fork with no decision behind it. **If the answer
is "derive", that is design content and needs either its own record or an amendment to this
ADR — it may not be decided inside #897's PR.**

**Q4 — is T3's fail-closed step (§4.6 step 2) in this cluster or in Phase 2?** This ADR places
it in the cluster, one step after the advisory step. If the arming ratchet must be pinned
against assertion counts measured on the GPU runner rather than locally, step 2 becomes a
separate PR with its own run, and T3 ships advisory-only here.

**Q5 — does T10 reach `baseline_qa.yml`?** On the minimal scope — QA scenes plus
`qa_test_runner.gd` — T10 measures **R1**, not the R3 the task plan carried, so R3's ADR
requirement would not formally apply to it. It reaches R3 only if the workflow is edited, for
instance to surface the expected-fail count on the job. §6 records T10's design **either way**,
because the expected-fail semantics are a product-behaviour decision rather than something a
gate demanded — but the maintainer should know that one member of §1 may not need this record
at all.

## 11. What would change this decision

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
