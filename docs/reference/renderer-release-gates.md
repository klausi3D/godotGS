# Renderer Release Gates

This page defines the release evidence contract for renderer evidence and the
public-alpha gate. The source of truth is
[`renderer_release_gate_manifest.json`](renderer_release_gate_manifest.json);
the checker is `tests/ci/check_renderer_release_gates.py`.

## Predicate

A public alpha is not satisfied by a nightly, a manual workflow choice, a green
advisory report, or a path-filtered subset of renderer checks. Candidate mode
machine-enforces that a candidate declares `release_channel=public-alpha` or
matches a `v*-alpha*` release tag, then passes the manifest predicate:

- an issue snapshot is required, either through `--issues-json` or an embedded
  candidate evidence `issue_snapshot`;
- open `priority:P0`, `priority:P1`, and `release blocker` issues in that
  snapshot must be classified as `blocking`, `accepted_alpha_limitation`, or
  `deferred`;
- if the snapshot is open-only, omitted manifest-tracked `blocking` issues must
  be proven closed through candidate evidence `resolved_manifest_issues`;
- any `accepted_alpha_limitation` must have a user-facing entry in
  `docs/development/known-public-alpha-limitations.md`;
- Linux and Windows artifacts, runtime validation, GPU harness output,
  production evidence, benchmark reports, compatibility sources, docs release
  acceptance, and open-world proof must all be present;
- missing GPU runner availability, manual inputs, path filters, or advisory
  open-world proof cannot downgrade a public-alpha candidate to pass.

## Public Alpha Checklist

The manifest owns the machine-readable checklist. Public-alpha signoff requires
all blocking rows below to be closed by code, evidence, or an explicit accepted
limitation before candidate mode can pass.

| Gate | Classification | Evidence Required |
| --- | --- | --- |
| #351 route/fallback/stage contracts | Blocking | #351 closed or split to non-alpha follow-up; resident, streaming, and serial route failure-injection tests; candidate benchmark rows include `route_uid`, `stage_statuses`, and `fallback_counters`. |
| #352 GPU resource lifetime | Blocking | #352 closed or split to non-alpha follow-up; GPU/RID accounting fails on retained owned resources; required GPU batches report `rid_leak_bytes=0`. |
| #360 public-alpha acceptance gates | Blocking | Candidate evidence bundle passes `--mode candidate`; issue snapshot classifies every open P0/P1/release-blocker issue; known limitations page contains each accepted alpha limitation. |
| #369 opaque `qlty check` | Deferred external signal | `master` branch protection has no required status checks; no repo-owned qlty config is tracked; `qlty check` is documented as non-blocking unless branch protection later requires it. |

Closed umbrella issues #350 and #353 stay closed only because their remaining
release risk is represented by open child blockers and follow-up issues. They
must not be used as evidence that renderer correctness, GPU lifetime, streaming,
or public-alpha acceptance is complete.

## Issue Classifications

`blocking` means the public-alpha candidate fails while the issue remains open.

`accepted_alpha_limitation` means the candidate may pass only when the evidence
bundle points to the canonical known-limitations page and that page explains
user impact, platform/workflow scope, mitigation, and proof that the limitation
does not hide renderer correctness failures.

`deferred` means the issue is explicitly outside the public-alpha gate or is an
external/non-required signal. A deferred issue still needs a rationale and
evidence requirements in the manifest; an unclassified P0/P1 issue is a gate
failure, not an implicit deferral.

## External Status Checks

The local renderer release gate only treats repo-owned checks and required
branch-protection contexts as release blockers. As of 2026-05-21, GitHub branch
protection for `master` reports `required_status_checks=null`, and the repo does
not track a qlty configuration file. Therefore `qlty check` is an advisory
external signal for this gate. A failing, pending, absent, or login-gated qlty
result must not fail `tests/ci/check_renderer_release_gates.py`.

If qlty later becomes branch-protection-required or gets a repo-owned actionable
configuration/log contract, update `external_status_check_policy` in the
manifest and reclassify #369 before cutting a candidate.

## Workflow Policy Scope

The contract checker currently validates that required workflow files exist and
contain the required job markers. It does not yet parse GitHub Actions YAML
deeply enough to prove manual-input bypasses, path-filter bypasses, or
open-world advisory-only behavior. Those no-downgrade rules remain documented
review policy in the manifest until a workflow-behavior parser is added.

## Stable Release Candidate Gate

`release_builds.yml` maps a `v*` tag push (and a manual stable dispatch) to
`channel=stable, publish=true`. That publish path is gated by the
`release_candidate_gate` job, which the manifest tracks as a required job marker
for `release_builds.yml` (issue #593):

- **No Linux-only stable release.** For the stable/candidate channel the gate
  fails unless both `build_linux` and `build_windows` succeeded, so a Windows
  runner outage blocks a stable publish instead of silently shipping a
  Linux-only release. Nightly prereleases keep the Linux-only tolerance.
- **No unmatched release files.** `publish_release` sets
  `fail_on_unmatched_files: true` for the stable channel, so a missing asset
  fails the publish rather than shipping a partial release. Nightly keeps the
  relaxed behavior.
- **Candidate validation is required.** For the stable/tag path the gate runs
  `check_renderer_release_gates.py --mode candidate` against a public-alpha
  evidence bundle and **fails closed** when that bundle is absent, so a tag can
  never publish without passing candidate validation. `publish_release` hard
  depends on the gate's success.

**Remaining scoped gap (issue #360).** No CI lane yet produces the public-alpha
candidate evidence bundle (candidate GPU-harness report, benchmark-lane report,
artifact SHA/commit-parity, and issue snapshot). Until that pipeline is wired,
the gate fails closed on every real `v*` tag; a maintainer cutting a candidate
supplies the bundle out-of-band by pointing the `RELEASE_CANDIDATE_EVIDENCE`
(and optional `RELEASE_CANDIDATE_ISSUES`) repo/environment variable at a
produced bundle. Generating the bundle in-workflow so a green stable tag is
self-proving is tracked as follow-up under #360/#593.

### Evidence is bound to the commit and to the shipped bytes

Candidate validation on its own only proves the evidence bundle is *internally*
consistent: `--mode candidate` re-hashes the files the bundle points at and
checks them against the bundle's own recorded `godot_binary_commit`. A bundle
produced for an older, fully validated commit stays internally consistent
forever, so without further binding it would authorize publishing a *different*,
never-validated commit's binaries. Three checks close that hole:

1. **Commit binding.** The gate passes `--expected-commit ${{ github.sha }}`.
   The bundle's `commit` must equal the commit being published, exactly.
2. **Built-bytes binding.** The gate downloads the archives `build_linux` and
   `build_windows` produced *in this run*, hashes them, and passes
   `--artifact-sha linux_release_archive=<sha256>` (and the Windows equivalent).
   The bundle must record exactly those digests, so the validated artifact and
   the built artifact are provably the same bytes. The gate also requires each
   published `.sha256` sidecar to describe its archive, and requires the Linux
   `BUILD-INFO.txt` to record the commit being published — the latter applies on
   the nightly channel too, which has no evidence bundle at all.
3. **Shipped-bytes binding.** `publish_release` runs as a separate job that
   re-downloads the artifacts, so the gate never saw what it uploads. The gate
   therefore writes a `release-attestation.json` (commit + sha256 of every file
   it validated) via `tests/ci/release_attestation.py generate`, and
   `publish_release` re-hashes its downloaded payload and requires an exact
   match with `release_attestation.py verify` before calling
   `softprops/action-gh-release`. A missing attestation, a changed digest, a
   missing asset, an unattested extra file, or a commit mismatch all fail the
   publish.

Together these make the release chain closed: `github.sha` == evidence commit ==
BUILD-INFO commit; evidence artifact digests == digests of the archives built in
this run == digests in the attestation == digests of the bytes uploaded to the
GitHub release.

The contract checker verifies the `release_candidate_gate` job marker is present
but does not parse the job's runtime both-platform / candidate / binding logic;
those specifics remain YAML-level enforcement recorded under
`workflow_policy.workflow_enforced_rules` in the manifest. The binding logic
itself is unit-tested in `tests/ci/test_renderer_release_gates.py`
(`CandidateSupplyChainBindingTests`) and `tests/ci/test_release_attestation.py`.

## Renderer Evidence

The current foundation profile requires the compositor hazard visual gate and
tracks every `[RequiresGPU]` doctest by a checked snapshot. The snapshot is
deliberately strict: adding, renaming, or deleting a GPU test without updating
the manifest fails contract validation.

`[SceneTree][RequiresGPU]` and `[Importer][RequiresGPU]` tests remain deferred
because the current offscreen GPU harness does not bootstrap those integration
paths. They are still release blockers. Public-alpha signoff and closure of
#350/#360 require either zero deferred tests or explicit waivers in the manifest
with owner, date, issue URL, product risk, mitigation, and a known-limitations
docs path.

## Visual And Benchmark Rules

Visual and benchmark evidence is machine-checkable before it becomes release
evidence:

- required GPU batches must match at least one test, parse doctest summaries,
  avoid timeouts, avoid skips unless explicitly allowed, and report zero RID
  leak bytes;
- blocking visual lanes need committed reference PNGs, nonzero captures, all
  expected references matched, and non-null SSIM/PSNR fields;
- benchmark rows used for public alpha need hardware, driver, OS, binary,
  route/stage, fallback, queue-pressure, timing, and proof metadata;
- timeout-written reports fail;
- `gpu_time_frame_ms` can be positive evidence only when GPU timing is available
  and provenance proves the source. Otherwise the row must explicitly mark GPU
  timing as unavailable and use CPU-observed labels.

The current public performance dashboard remains a non-authoritative snapshot
until a complete candidate evidence bundle exists.

## Lifetime Accounting Proof

The `lifetime_accounting_proof` manifest section gates the renderer-lifetime
hazards tracked by #352. It consumes structured `[GS-LIFETIME]` JSON lines
written to stdout by the renderer lifetime-proof fixture (PR #386) and the
orphan-`StringName` CI guard (PR #389). Each scenario emits one self-contained
JSON object after the marker prefix declared in the manifest
(`stdout_marker`, currently `"[GS-LIFETIME] "`).

A passing run reports every required scenario exactly once with `passed=true`,
its scenario-specific byte counter below the manifest threshold, and either a
real orphan-`StringName` delta below the count threshold or the
`advisory_sentinel_value` (`-1`, "not measured this run").

### Required scenarios

| Scenario | Source | Metric (`scenario_metric_fields`) | Byte threshold |
| --- | --- | --- | --- |
| `renderer_instance` | PR #386 fixture | `rd_bytes_leaked` | 4194304 |
| `failed_init` | PR #386 fixture | `rd_bytes_leaked` | 4194304 |
| `scene_director_reload` | PR #386 fixture | `rd_bytes_leaked` (growth) | 262144 |
| `asset_attach_detach` | PR #386 fixture | `rd_bytes_leaked` (growth) | 65536 |
| `stringname_orphans` | PR #389 guard | `stringname_orphan_delta` (advisory) | `stringname_orphans_max=5` |

The advisory counter `stringname_orphan_delta` is reported by every PR #386
scenario as the sentinel `-1` ("not measured this run") and only carries a real
number from the PR #389 scenario. The gate skips the count check when the
sentinel is present, then enforces `stringname_orphans_max` when the
`stringname_orphans` scenario supplies the real delta.

The manifest also declares `advisory_fields_strict_for` — a map from scenario
name to the list of advisory fields that scenario MUST report (even if the
value is the sentinel). This closes a hole where a strict scenario could drop
the advisory field entirely and silently bypass its count threshold; the
`stringname_orphans` scenario is the canonical strict consumer of
`stringname_orphan_delta`.

### Example stdout line

```text
[GS-LIFETIME] {"scenario":"renderer_instance","passed":true,"rd_bytes_leaked":131072,
"rdm_owned_leaked":0,"rdm_tracked_leaked":0,"teardown_sync":true,
"threshold_bytes":4194304,"stringname_orphan_delta":-1,"fail_reason":""}
```

### Running the check

`--mode contract` only validates that the manifest section exists and is
well-formed:

```bash
python3 tests/ci/check_renderer_release_gates.py --mode contract
```

`--mode lifetime` consumes a captured stdout artifact and runs the full
scenario-by-scenario check:

```bash
python3 tests/ci/check_renderer_release_gates.py \
  --mode lifetime \
  --lifetime-stdout artifacts/lifetime_stdout.log
```

### Adding a new scenario

1. Add the scenario name to `required_scenarios` in the manifest section.
2. If the scenario reports a byte counter, register the field name in
   `scenario_metric_fields` and the threshold in `thresholds_bytes`.
3. If the scenario reports a new advisory counter, add the field name to
   `advisory_fields` and the corresponding cap in `thresholds_counts`. The
   validator's advisory-mapping currently routes `stringname_orphan_delta` to
   `stringname_orphans_max`; new advisory fields look up their cap by their own
   name in `thresholds_counts`. If the scenario is the canonical producer of
   that advisory counter (so the field MUST always appear in its entry), also
   list the scenario -> [field] pair under `advisory_fields_strict_for`.
4. Have the producing fixture/guard print `[GS-LIFETIME] {...}` lines that
   include `scenario`, `passed`, `fail_reason`, and the metric/advisory fields
   referenced above.

## CI Hook

Run the direct contract check locally:

```bash
python3 tests/ci/check_renderer_release_gates.py --mode contract
```

CI enforces the same contract through the existing module guard entry point:

```bash
python3 tests/ci/run_module_tests.py --guard-only
```

Candidate release validation requires an evidence bundle and an issue snapshot
unless the evidence bundle embeds `issue_snapshot`:

```bash
python3 tests/ci/check_renderer_release_gates.py \
  --mode candidate \
  --candidate-evidence artifacts/public-alpha/evidence_bundle.json \
  --issues-json artifacts/public-alpha/open_issues.json
```

The issue JSON is intended to come from `gh issue list` or the GitHub API with
labels included. When that snapshot is open-only, any manifest-tracked
`blocking` issue omitted from it needs explicit closure proof in
`resolved_manifest_issues` inside the evidence bundle. This PR does not require
live network access for the deterministic contract check.
