# ADR: Test quarantine manifest (production-readiness C3 / exit criterion G5)

- **Status:** Accepted (owner sign-off 2026-07-16 — Slice 1 "empty-dark mechanism"
  approved; the design below, incl. the two-manifest split and the per-entry human-approval
  rule, is adopted as written; each future quarantine entry remains individually human-gated).
- **Risk class:** R3 — the core change edits `tests/ci/run_module_tests.py` lane-execution
  logic (`.agentic/policy.json` classifies that file as deterministic-check machinery),
  and quarantine *approvals* are human-gated per the program charter §6. This ADR is the
  required design-note-before-implementation.
- **Program ledger:** #458. **Related:** #329 (26–29 GPU `[SceneTree]`/`[Importer]` tests
  deferred), #395 (missing `history_artifact_audit.py`).

## Context / problem

G5 ("testing enforced, not just present") requires: *no excluded test category without an
in-repo tracked waiver (file with issue links); known failures live in a quarantine
manifest, not in memory.*

Today:
- Advisory (`strict=False`) lanes in `run_module_tests.py` **swallow failures untracked** —
  `_report_failed_lane` prints "(advisory lane, continuing)" and returns success, so any
  regression there is invisible and linked to no issue.
- The oft-cited "4 pre-existing GaussianSplatting test failures" is **unverified stale
  memory** — `project_test_baseline.md` does not exist in-repo or git history; the only
  in-repo trace is an April CI-audit note that explicitly could not confirm which cases
  failed. **The real failing set is currently unknown** and must be measured, not assumed.
- The GPU `[SceneTree]`/`[Importer]` deferrals (#329) are hardcoded excludes with the
  release-gate manifest's `deferred_requires_gpu_waivers` array **empty**, despite its
  `closure_policy` requiring an explicit waiver per deferred test.

## Decision

Introduce a JSON quarantine manifest that the CI harness consults explicitly, plus a schema
guard. Specifics:

1. **Format — JSON**, `tests/ci/quarantine_manifest.json`, mirroring the field vocabulary
   already validated for `renderer_release_gate_manifest.json:deferred_requires_gpu_waivers`
   (`test_name`/`issue_url`/`owner`/`expires_utc`/`risk`/`mitigation`), plus
   `lane` (must equal a real `MODULE_TEST_FILTERS` name) and `base_sha_proven_failing`.
   A human-readable mirror `docs/reference/test-quarantine.md` is explicitly non-authoritative.
   *Rationale:* the harness must consult it programmatically and the anti-rot logic needs
   typed fields; a Markdown table cannot be parsed reliably and would drift.

2. **Semantics (no gate weakened):**
   - A quarantined lane's failure is **tolerated but loudly reported and counted**
     (`[QUARANTINE] ... failed as expected (issue ...)`), not silently swallowed.
   - A quarantined lane that **passes** **fails the run** (`[QUARANTINE-STALE] ... delete
     its entry`) — the manifest is self-cleaning and cannot rot.
   - A lane **absent** from the manifest keeps today's exact behavior (strict still blocks).
   - A schema guard (runs in the fast `--guard-only` lane) rejects any entry missing
     `issue_url`/`base_sha_proven_failing`/`owner`/`reason`/`risk`, naming an unknown lane,
     or past its `expires_utc`.

3. **Two non-overlapping manifests:** headless-lane quarantine → the new
   `quarantine_manifest.json`; the GPU `[SceneTree]`/`[Importer]` deferred set → populate the
   existing `renderer_release_gate_manifest.json:deferred_requires_gpu_waivers` (link #329),
   satisfying its closure policy. Documented in `test-quarantine.md`.

4. **Rollout — ships dark:**
   - **Slice 1 (mechanism):** all harness changes + schema guard + a committed **empty**
     manifest (`entries: []`) → byte-for-byte today's behavior; plus a `unittest` covering
     tolerate / stale-pass-fails / expired-fails / unknown-lane-fails / empty-noop.
   - **Slice 2 (populate — needs evidence + per-entry human approval):** re-establish the
     real failing set on the self-hosted **GPU runner** (`run_module_tests.py` +
     `run_gpu_harness.py`), open one issue per reproduced failure (proven on the base SHA
     per charter §2.7), then add manifest entries. **Every quarantine entry is a human-gated
     approval (charter §6).**

## Decisions the owner needs to make

- **D1 — Approve this R3/ADR-first process and the design above?** (Y/N or amendments.)
- **D2 — Confirm "empty-manifest-ships-dark" Slice 1 first** (mechanism only, zero behavior
  change), before any lane is quarantined? (Recommended: yes.)
- **D3 — Confirm the two-manifest split** (headless → new manifest; GPU-deferred → existing
  release-gate waiver array)? (Recommended: yes — avoids a third overlapping source.)
- **D4 — Standing rule for entries:** each quarantine entry requires (a) a reproduced
  failure on a named base SHA, (b) a tracking issue, (c) your explicit approval. Agents may
  draft entries but never self-approve. (Recommended: adopt as written.)

## Consequences

- Regressions in currently-advisory lanes become visible + tracked; the manifest cannot rot
  (stale-pass fails CI). No existing gate is loosened — quarantine is purely additive and
  explicit. The unverified "4 failures" claim is retired in favor of measured evidence.
- Cost: Slice 2 needs GPU-runner time to establish the real failing set (agents cannot
  raster locally).
