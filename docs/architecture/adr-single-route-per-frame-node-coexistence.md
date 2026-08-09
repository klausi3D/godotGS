# ADR: `GaussianSplatWorld3D` + `GaussianSplatNode3D` in one scene — warn now, route later (#788)

- **Status:** Accepted. **Option 1 (editor configuration warning) is accepted as the
  interim** and is specified implementably in §6 below. **Option 2 (genuine per-submission
  routing) is accepted as the target direction and deferred** — it needs its own ADR and
  its own evidence. **Option 3 (hard-reject the combination) is rejected**; §5.3 records
  why, including the cost it would impose on scenes that already exist.
- **Risk class:** this document is R0 (`docs/**`). The Option 1 implementation it specifies
  lands in `modules/gaussian_splatting/nodes/**` and `modules/gaussian_splatting/tests/**`,
  which `.agentic/policy.json` classifies as **R1** — no ADR is *required* there, so this
  record exists because the decision is a product-behaviour decision, not because a gate
  demanded it. Option 2 will touch `modules/gaussian_splatting/renderer/**` and
  `core/*streaming*` (**R2**) and must not be started against this ADR alone.
- **Tracking:** #788 (this decision). **Split from:** #785 (`qa_visual_diff` /
  `qa_sh_rotation` were built on the combination this ADR describes; fixed by PR #854).
  **Blocked-on / related:** #855 (world route drops content the instance route draws).
  **Waiting on Option 2:** `qa_stream_multi_asset.tscn`.
- **Verified against:** `origin/master` @ **`a04472a82cf`**. Every file:line in this
  document was read at that commit. §2 records which of the issue's original citations had
  drifted.

## 1. What a user sees

Place a `GaussianSplatWorld3D` and a `GaussianSplatNode3D` in the same scene. One of the
two renders nothing. In the configuration reported on #788 the *world* renders nothing,
there is no error, and the only signal is a single `WARN_PRINT_ONCE`:

```text
[GaussianSplatRenderer] Resident route rejected
(reason=submission_hint_resident:instance_submission:resident_no_instances);
frame skipped to preserve single-route-per-frame contract.
```

Hiding either node does not hand the frame back to the other. The two node types are never
independently observable in one scene.

For a renderer aiming at a public alpha this is the wrong failure shape: the behaviour is
contract-consistent, but "I added a second node and my scene went black, with one line in a
log most users never open" is not a diagnosable outcome.

## 2. Citation audit — what had drifted

The issue's mechanism paragraph was written against an earlier tree. Re-checked at
`a04472a82cf`:

| Claim in #788 | Cited as | Actual at `a04472a82cf` | Verdict |
| --- | --- | --- | --- |
| "world submissions only" scope doc | `core/gs_project_settings.h:234-252` | doc block `:235-246`, function `get_streaming_route_policy()` at `:247` | **drifted** (off by one at the start, overshoots the end); claim itself accurate |
| world derives its hint from `route_policy` | `nodes/gaussian_splat_world_3d.cpp:505-517` | `:510-514` (`has_desired_residency_hint = true` at `:510`, policy read at `:511`, hint ternary `:512-514`) | **drifted**; claim accurate |
| `resident_no_instances` | `renderer/resident_instance_contract_publisher.cpp:374` | `:400` (guard `:398-404`) | **drifted by 26 lines**; claim accurate |
| the frame skip | `renderer/gaussian_splat_renderer.cpp:2478` | reason string built at `:2474`, `WARN_PRINT_ONCE` at `:2508`, `return` at `:2513` | **drifted by 30 lines**; claim accurate |
| repo already calls the combination unproven, in two places | `qa_test_runner.gd`, `tests/runtime/test_mixed_residency_routing.gd:3-5` | `qa_test_runner.gd:53-55` and `test_mixed_residency_routing.gd:3-5` | **both confirmed verbatim** |

One claim is **not** merely drifted but **imprecise**, and the correction changes what a fix
has to handle — see §3.2.

### 2.1 PR #854 (issue #785) is open, not merged

At `a04472a82cf`, PR #854 has **not** landed. It rewrites `qa_test_runner.gd` and replaces
`qa_visual_diff.tscn` / `qa_sh_rotation.tscn` with four route-separated scenes. Two
consequences for this ADR:

- The `qa_test_runner.gd` line numbers above will move when #854 merges. The
  **`qa_stream_multi_asset.tscn` entry that #788 cites is not one of the entries #854
  removes** — #854 deletes only the two `#785` entries. The "disabled until the runtime
  surface can prove true resident/streaming coexistence" reason survives #854 and remains
  the repo's standing statement that this combination is unproven.
- #854 does **not** make the combination work. It removes the two QA scenes that depended on
  it. Nothing in the repo after #854 exercises world+instance coexistence.

## 3. Mechanism, as the code actually reads

### 3.1 The route is chosen once per frame, per renderer

`GaussianSplatRenderer::should_prefer_resident_backend()`
(`renderer/gaussian_splat_renderer.cpp:2842-2875`) resolves one backend for the whole frame:

1. `route_policy == GS_ROUTE_RESIDENT` → resident, reason `requested_resident_policy`
   (`:2849`).
2. otherwise ask the scene director for a single submission residency hint; no hint →
   streaming.
3. hint `RESIDENT` → resident, reason `submission_hint_resident:<source>` (`:2864`).

`build_frame_backend_plan()` (`:1600-1657`) turns that into the frame's plan, and
`render_scene_instance()` commits: if the resident route is chosen and the resident contract
cannot be published, the frame is **skipped**, deliberately, rather than falling through to
the other backend — `single_route_per_frame` and `alternate_backend_fallback_forbidden` are
read at `:2505-2506` and drive the skip at `:2508-2513`. Resident publication fails with
`resident_no_instances` when the instance list is empty
(`renderer/resident_instance_contract_publisher.cpp:398-404`).

The project default is `route_policy = 1` = `GS_ROUTE_STREAMING`
(`core/gaussian_splat_manager.cpp:1008`; enum at `core/gs_project_settings.h:196-197`).

### 3.2 Correction: the world hint has *precedence*, and the failure means it was absent

#788 says the instance node "always publishes RESIDENT" and that therefore "with both
present the renderer commits to RESIDENT". The precedence in
`GaussianSplatSceneDirector::get_submission_residency_hint_for_renderer()`
(`core/gaussian_splat_scene_director.cpp:2811-2859`) is the other way round:

- If a world submission is **active** for this renderer *and*
  `record_has_renderable_payload()` is true *and* it carries a hint, the **world's** hint
  wins and the source is `"world_submission"` (`:2817-2827`).
- Only if that test fails does it fall back to scanning the instance records
  (`:2830-2852`), whose hint is `RESIDENT` (source `"instance_submission"`, `:2849`) unless
  the instance records disagree with each other (`"mixed_instance_submissions"`, `:2841`,
  which returns *no* hint).

The reason string recorded on #788 is `submission_hint_resident:instance_submission`. That
string is only reachable through the **fallback** branch, and only when
`route_policy != RESIDENT`. So the measured failure proves something sharper than the issue
states: **the world submission did not supply the hint even though the world node was
present in the scene** — it was either not yet active for that renderer or its payload was
not yet renderable at the moment the route was decided.

Which of those two it was is **not determined here** and cannot be without a runtime run
(see §8). It matters, because it means the mixed scene is *doubly* broken:

- if the world payload is not ready at hint time, the instance hint decides and the world
  is skipped (`resident_no_instances` when the instance content is also unavailable, e.g.
  hidden or empty);
- if the world payload *is* ready, the world hint decides and the **instance** node's
  content is the one silently dropped — which is exactly the symmetry #788 reports
  ("forcing `route_policy=1` makes the instance node's capture render the *world's*
  splats").

**Which node type disappears therefore depends on load timing.** That is worse than a fixed
precedence and is a first-class reason not to leave the combination silent.

### 3.3 Why hiding a node does not help

Two independent reasons, both structural:

- The instance-hint fallback loop at `core/gaussian_splat_scene_director.cpp:2830-2846`
  iterates `world->instance_store.records()` and filters only on
  `record.has_desired_residency_hint`. It **does not check `record.visible`**, although the
  same store's `visible` flag is honoured elsewhere in the same file (`:1603`, `:1754`,
  `:1929`, `:2075`, `:2923`). A hidden `GaussianSplatNode3D` still steers the global route.
- Even if it did, the route decision is per-frame and global. `visible` decides whether the
  shared composite draws, not which submission owns the frame.

The first point is a defect in its own right and is recorded as follow-up work in §9. It is
**not** fixed by this ADR, and the Option 1 warning is deliberately specified to be
visibility-independent so that it does not quietly depend on it.

### 3.4 `route_policy` does not resolve the conflict either

Setting `route_policy = 0` (`GS_ROUTE_RESIDENT`) makes `should_prefer_resident_backend()`
return true at step 1 (`:2847-2851`). The resident route then renders whatever instances
the director builds — the `GaussianSplatNode3D` content — and the world's streaming payload
is not drawn. Setting it to `1` gives the world the frame and drops the instance content.
**Neither value renders both.** The conflict is `route_policy`-independent, which is why
the warning specified in §6 does not read `route_policy` at all.

## 4. Why this is a decision and not just a bug

The renderer is doing what its contract says. `core/gs_project_settings.h:235-246` states
the scope explicitly and even anticipates the question ("If per-node backend steering is
ever needed, introduce a narrow per-world-group setting rather than broadening this one").
The repo has said in two places that coexistence is unproven
(`tests/examples/godot/test_project/scripts/qa_test_runner.gd:53-55`,
`tests/runtime/test_mixed_residency_routing.gd:3-5`). What is missing is not correctness —
it is that **nothing stops a user from authoring the scene, and nothing tells them.**

## 5. Options

### 5.1 Option 1 — make it loud (**accepted, interim**)

Surface the conflict as an editor configuration warning on both node types, at author time,
in the scene tree, where a user is already looking. Cheap (R1), reversible, and orthogonal
to Options 2 and 3. It changes no render behaviour and breaks no existing scene: a warning
triangle is additive.

Accepted. Specified in §6.

### 5.2 Option 2 — make it work (**accepted as target, deferred**)

Genuine per-submission routing. This is the real feature and the one
`qa_stream_multi_asset.tscn` is explicitly waiting on. Deferred, not declined, for three
reasons:

1. **The choke point is a single-value API.** `get_submission_residency_hint_for_renderer()`
   returns *one* `int32_t` for a renderer (`:2811`), `FrameBackendPlan` carries *one*
   backend (`renderer/gaussian_splat_renderer.cpp:1600-1657`), and `RenderRouteDecision`
   carries `single_route_per_frame` / `alternate_backend_fallback_forbidden` as invariants
   that the skip path *reads* (`:2505-2506`). Per-submission routing is not a hint change;
   it is a change to what a frame *is*.
2. **Compositing is the hard part, not routing.** The module composites once at end of
   frame with one colour and one depth per pixel. Two routes that cull, sort and raster
   independently and composite independently cannot produce correct inter-route occlusion
   or blending for transparent splats — they would need a merged sort or an explicitly
   documented ordering restriction. That is a design question with its own ADR, not a
   follow-up commit.
3. **Ordering constraint: #855 first.** #855 measured the world route dropping content the
   instance route draws (SSIM 0.8813 at one orbit angle, reproduced bit-identically at two
   settle times). Building a coexistence A/B on top of two routes that already disagree
   would produce a gate that cannot distinguish a routing defect from the known divergence.
   **#855 must be root-caused before Option 2 is scheduled.**

### 5.3 Option 3 — make it impossible (**rejected**)

Hard-reject the combination with an error naming both nodes.

Evaluated honestly, and rejected:

- **It breaks scenes that exist, on upgrade.** Today the combination is not uniformly
  black: per §3.2, one of the two node types *does* render, and which one depends on load
  timing. A user whose scene currently shows their instance content would, on upgrading,
  get a hard error and nothing at all. A diagnostic that turns partial output into no
  output is a regression, and the module has no scene-migration mechanism to soften it.
- **There is nothing to reject at the point it would matter.** The nodes are already in the
  user's saved `.tscn`. A runtime `ERR_FAIL` cannot un-author the scene; it can only make
  the same failure louder and less recoverable than the warning Option 1 already gives.
- **It forecloses Option 2.** Any implementation of per-submission routing must stand up
  exactly this combination, in tests and in `qa_stream_multi_asset.tscn`. A rejection in the
  node layer becomes the first thing Option 2 has to delete, and every intermediate slice
  has to work around it.
- **It is not the same as the module's existing fail-closed precedent.** The module does
  hard-reject an unsupported configuration elsewhere (rejecting no-manager sorters at init,
  #764) — but that is an internal, opt-in, non-default code path, not a user-authored scene
  composition. `AGENTS.md`'s "do not weaken a guard" rule protects measured invariants; it
  does not argue for converting a product limitation into a crash.

**The honest counterpoint, recorded:** a hard error would guarantee nobody ships a silently
black scene, and Option 1 does not — a user can ignore a warning triangle. This is accepted
as a residual risk, mitigated by the warning naming the observable consequence explicitly
(§6.2) and by the documented workaround. If evidence later shows users shipping the broken
combination anyway, Option 3 can be revisited as an *export-time* check rather than a
runtime error, which would break no editing session.

## 6. Implementation specification — Option 1

Implementable as written. No part of this section was executed or built; §8 lists what
remains unverified.

### 6.1 Classes and detection

**Both** node classes get the warning, symmetrically. A user who clicks either node must see
it; a warning on only one is a coin-flip on which node they inspect first.

- `GaussianSplatNode3D` already overrides `get_configuration_warnings()`
  (`modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:1810-1851`, bound at
  `:234`). Append the new warning there.
- `GaussianSplatWorld3D` has **no** override today (confirmed: no
  `get_configuration_warnings` in `nodes/gaussian_splat_world_3d.h`). Add
  `PackedStringArray get_configuration_warnings() const override;`, chain to
  `Node3D::get_configuration_warnings()`, and mirror the `ClassDB::bind_method` that
  `GaussianSplatNode3D` uses at `:234` so GDScript-side tests can read it.
- `GaussianSplatDynamicInstance3D` needs **no change**: it derives from
  `GaussianSplatNode3D` and its override already chains
  (`nodes/gaussian_splat_dynamic_instance_3d.cpp:8`).

**Discovery mechanism — SceneTree groups, scoped by `World3D`.** Do not scan the scene tree.

- Two non-persistent group names, declared once as shared constants (e.g. in
  `nodes/gaussian_splat_node_3d.h` or a small shared header):
  `"__gs_world_submission_nodes"` and `"__gs_instance_submission_nodes"`.
- Each class calls `add_to_group(<its own group>, /*persistent=*/false)` **once, in its
  constructor**. Godot registers grouped nodes with the `SceneTree` on tree entry
  (`scene/main/node.cpp:338`) and removes them on tree exit (`:425`), so
  `SceneTree::get_nodes_in_group()` returns exactly the in-tree nodes with no bookkeeping of
  our own. `persistent = false` keeps the group out of the saved `.tscn`.
- The condition, evaluated only when `get_configuration_warnings()` is called:

  ```text
  is_inside_tree()
    AND get_world_3d().is_valid()
    AND this node would submit content
    AND ∃ a node in the *other* group with
          the same get_world_3d()->get_scenario() RID
          that would also submit content
  ```

  Scoping by scenario RID — not by edited-scene root — is what makes this correct: the
  director keys its `world_registry` by scenario
  (`core/gaussian_splat_scene_director.cpp:2865-2870`), so two nodes in different `World3D`s
  (a `SubViewport` with `own_world_3d`) genuinely do not conflict and must not warn.

- **"would submit content"** means, deliberately, *has renderable content assigned* — not
  *is visible*:
  - `GaussianSplatWorld3D`: `get_world().is_valid()` and the world reports a non-zero splat
    count, mirroring `SubmissionStore::record_has_renderable_payload()`
    (`core/gaussian_splat_scene_director.cpp:843-851`): valid `gaussian_data` with
    `get_count() > 0`, **or** a valid `payload_source` with `get_count() > 0`.
  - `GaussianSplatNode3D`: not all of `splat_asset` / `renderer_data` / `runtime_asset` are
    null — the same triple the existing "No Gaussian splat asset or runtime data assigned"
    warning uses at `nodes/gaussian_splat_node_3d.cpp:1813`.

- **Do not gate on director/renderer state** (`_is_renderer_shared_with_other_content()`,
  `nodes/gaussian_splat_node_3d.cpp:42-59`). It is the right question at runtime and the
  wrong one at author time: it depends on registration order and on the world resource
  having loaded, so the warning would blink. Structural presence is what the user can act
  on. The director query may be used as an *additional* runtime log, never as the condition.

### 6.2 Warning text

Exact strings. Both name **both** class names and the observable consequence.

On `GaussianSplatWorld3D`:

```text
A GaussianSplatNode3D shares this node's World3D. The renderer commits to a single
render route per frame, so only one of the two node types is drawn and the other
renders nothing — which one depends on load timing. Hiding either node does not
restore the other. Until per-submission routing lands (issue #788), keep
GaussianSplatWorld3D content and GaussianSplatNode3D content in separate scenes or
separate viewports (a SubViewport with its own World3D).
```

On `GaussianSplatNode3D`:

```text
A GaussianSplatWorld3D shares this node's World3D. The renderer commits to a single
render route per frame, so only one of the two node types is drawn and the other
renders nothing — which one depends on load timing. Hiding either node does not
restore the other. Until per-submission routing lands (issue #788), keep
GaussianSplatNode3D content and GaussianSplatWorld3D content in separate scenes or
separate viewports (a SubViewport with its own World3D).
```

The phrase **"renders nothing"** and both literal class names are load-bearing: §6.4 asserts
on them, and the assertion is specified here rather than derived from the implementation
constant, so the test cannot become a tautology against the code it guards.

### 6.3 When the warning is re-evaluated

`get_configuration_warnings()` is pull-based — the editor calls it only after
`update_configuration_warnings()`. A node must refresh **itself and its peers**, because
when node A enters the tree, node B's warning is the one that goes stale.

Add a small shared helper (e.g. `_notify_route_conflict_peers()`) that walks the *other*
group, filters by matching scenario RID, and calls `update_configuration_warnings()` on each
peer. Call it, plus `update_configuration_warnings()` on self, from:

| Trigger | `GaussianSplatWorld3D` | `GaussianSplatNode3D` |
| --- | --- | --- |
| `NOTIFICATION_ENTER_TREE` / `NOTIFICATION_EXIT_TREE` | add (`:79`, `:125`) | add (`:441`, `:469`) |
| `NOTIFICATION_ENTER_WORLD` / `NOTIFICATION_EXIT_WORLD` | **add both cases** (the class handles neither today) — the scoping key is the `World3D` and a viewport's `own_world_3d` can change it without tree churn | add (cases exist at `:445`, `:449`) |
| content assigned/cleared | `set_world()`, `clear_world()`, `apply_world()` | the sites that already call `update_configuration_warnings()` (`:738`, `:827`, `:1063`, `:1283`, `:1293`, `:1303`, `:2969`) — add the *peer* notification there |

**Two triggers named in the brief are deliberately excluded, and each is pinned by a test:**

- **Visibility change is not a trigger.** The condition is visibility-independent by design
  (§3.3): hiding a node does not restore the other, so a warning that disappeared on hide
  would be a lie. `NOTIFICATION_VISIBILITY_CHANGED` therefore stays untouched
  (`gaussian_splat_world_3d.cpp:207`, `gaussian_splat_node_3d.cpp:526`). Test T5 pins that
  the warning survives a `set_visible(false)` on either node.
- **`route_policy` change is not a trigger.** Per §3.4 the conflict exists at both values.
  Test T6 pins the warning at `route_policy = 0` and `= 1`.

If a later change makes the condition depend on either input, that change must also add the
trigger *and* flip the corresponding test — the tests are what make the omission deliberate
rather than forgotten.

### 6.4 Mutation proof

A configuration warning is exactly the kind of change that can be added and never fire. The
following cases go in `modules/gaussian_splatting/tests/test_gaussian_splat_node.h`, which
already has `[SceneTree]` warning coverage and the `ScopedTestNode` helper (see the existing
warning assertion at `:5436`).

**Build note for the implementer:** this build is compiled with `disable_exceptions=True`,
so `REQUIRE`/`CHECK` **do not abort**. Never write `REQUIRE(ptr)` followed by a dereference.
Use `if (!x) { FAIL("..."); return; }` at every early exit.

Shared helper for the assertions — a warning *counts as present* only if a single entry
contains all three of `"GaussianSplatWorld3D"`, `"GaussianSplatNode3D"` and
`"renders nothing"`. Substring-on-one-entry, not "any entry mentions the other class",
otherwise the pre-existing "No Gaussian splat asset..." warning could satisfy it.

| # | Case | Setup | Assertion |
| --- | --- | --- | --- |
| **T1** | **the RED-without / GREEN-with case** | `GaussianSplatWorld3D` with a world holding ≥1 splat **and** `GaussianSplatNode3D` with runtime splat data, both under one `SceneTree` root, same `World3D` | **both** nodes report the warning. `if (!found) { FAIL(...); return; }` |
| **T2** | control — world alone | only the `GaussianSplatWorld3D` | warning **absent** |
| **T3** | control — instance alone | only the `GaussianSplatNode3D` | warning **absent** |
| **T4** | control — two instance nodes | two `GaussianSplatNode3D`s, no world node | warning **absent**. This is the control that stops "more than one GS node in the scene" from passing as an implementation; multiple instance nodes coexist correctly, they all publish `RESIDENT` |
| **T5** | control — visibility is irrelevant | T1 setup, then `set_visible(false)` on the instance node, then on the world node | warning **still present in both configurations** (pins §6.3) |
| **T6** | control — `route_policy` is irrelevant | T1 setup, run once with `route_policy = 0` and once with `= 1`, restoring the setting via the existing `ProjectSettingGuard` pattern (`tests/test_node_surface_cleanup.h:184`) | warning **present at both values** (pins §3.4/§6.3) |
| **T7** | control — different `World3D` | world node in the main tree, instance node inside a `SubViewport` with `own_world_3d = true` | warning **absent in both**. This is the control that proves the scenario scoping is real rather than a global "is there a world node anywhere" check |
| **T8** | control — no content | both node types present but neither has content assigned | warning **absent** — the user's actionable problem is "no asset", which the existing warning already states |
| **T9** | re-evaluation actually happens | T1 setup, then `root->remove_child(instance_node)` | the world node's warning is **gone** on the next `get_configuration_warnings()` — proves group deregistration plus the peer `update_configuration_warnings()` on `EXIT_TREE` |

**The mutation run is part of the deliverable, not optional.** Before the PR is opened, run
T1 with the two `get_configuration_warnings()` additions reverted (keep everything else) and
record that **T1 fails and T2–T9 pass**. A run in which T1 still passes means the assertion
is matching something else — most likely a pre-existing warning — and the test proves
nothing. Paste the two transcripts (mutant and fixed) into the PR; do not describe them.

## 7. Consequences

- **Immediately:** the conflict becomes visible at author time on both node types, on a
  scene the user is editing, naming the consequence and a workaround. No render behaviour
  changes; no existing scene stops working.
- **The warning is a stopgap with an expiry condition.** It is deleted, not amended, when
  Option 2 lands. Both the warning text and this ADR name #788 so the removal is findable.
- **`qa_stream_multi_asset.tscn` stays quarantined** (`qa_test_runner.gd:53-55`). Option 1
  does not change what the runtime can prove; un-quarantining it is Option 2's job, and
  doing it earlier would re-create the #785 defect class.
- **Nothing in the repo exercises coexistence after #854 merges.** That is the correct state
  for now — a scene that cannot work should not be gating — but it means the first Option 2
  slice must bring its own coverage rather than inheriting any.
- **Cost:** an editor-only code path in two node classes, plus nine test cases. The peer
  notification is the only non-trivial part, and it runs on tree/world transitions and
  content assignment, never per frame.

## 8. What is not verified here

This ADR was written under a documentation-only constraint: **no build was run**, so nothing
below was executed.

- **The static reading of the route decision (§3) is verified by source citation only.** The
  runtime attribution — the reason string, the blank captures, the symmetry under
  `route_policy` — is #785's and #788's measured evidence, reproduced here as reported, not
  re-measured.
- **Unresolved (§3.2): why the world submission failed the
  `is_active() && record_has_renderable_payload()` test** in the reported run — whether the
  world submission was not yet active for that renderer, or its payload was not yet
  renderable when the route was decided. Both are consistent with the recorded reason
  string. Distinguishing them needs a run with the director state logged at hint time. It
  does not change this decision (the conflict exists either way, §3.4) but it does determine
  whether Option 2 also has to fix a startup ordering race.
- **The Option 1 spec compiles/behaves as written is unproven.** Group registration from a
  constructor, `get_world_3d()` validity inside the editor's edited-scene viewport, and the
  peer-notification ordering are all read from source, not run.

## 9. Follow-up work this ADR surfaces but does not do

- **Hidden instance nodes steer the global route.** The instance-hint fallback loop at
  `core/gaussian_splat_scene_director.cpp:2830-2846` ignores `record.visible` while the same
  store's `visible` flag is honoured at `:1603`, `:1754`, `:1929`, `:2075` and `:2923`.
  Worth filing as its own issue: a hidden node influencing route selection is surprising
  independently of #788, and it is a small, testable change. **Not** to be folded into the
  Option 1 PR — it is a behaviour change to the director, not a diagnostic.
- **#855 must be root-caused before Option 2 is scheduled** (§5.2, reason 3).

## 10. What would change this decision

- **Option 2 lands** → the warning and this ADR's §6 are deleted; the ADR stays as the
  record of why the interim existed.
- **Evidence that users ship the broken combination despite the warning** → revisit Option 3
  as an *export-time* check (§5.3), never as a runtime error.
- **A measurement showing which node disappears is in fact deterministic**, not
  load-timing-dependent (§3.2) → the warning text's "which one depends on load timing" must
  be corrected to say which one, because a vaguer warning than the truth is its own defect.
