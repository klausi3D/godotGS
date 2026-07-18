# ADR: Decompose the `GaussianSplatNode3D` god-class by ownership, not friendship

- **Status:** Proposed — awaiting owner sign-off. Design-note-before-implementation;
  no production code changes in this PR.
- **Risk class:** R2 (design). The *implementation* it authorizes spans R1→R3: the
  PREDELETE-guard step (Step 3) is **R3** — it edits GPU-resource teardown ordering and
  must carry runtime lifetime evidence and a separate owner sign-off before merge.
- **Base SHA:** `237a4b1cc3965fdbd6f12dec825c0e2077b2e9ce` (`origin/master`).
- **Primary issue:** #552 (decompose the god-class). **Also closes / advances:** #551
  (PREDELETE ordering → structural), #550 (per-frame camera writers → one writer).
  **Coordinates with:** #578 / #516 / #517 (world-node lifecycle, in flight), #558
  (script-instantiable internals), #357 & #547 (settings ownership / config-loader dedup),
  #299 (missing node integration coverage), #592 (editor preview worker — adjacent).

> **Issue-number note for reviewers.** The task brief referred to the god-class as #548,
> the camera writes as #547, and "settings-ownership #544". The *actual* GitHub issues are
> **#552** (god-class), **#550** (camera writes), and **#357/#547** (settings). #548 is a
> `GaussianData` naming collision and #544 is the uint32 streaming-buffer cap — unrelated.
> This ADR uses the verified numbers.

## Context / problem

`nodes/gaussian_splat_node_3d.{h,cpp}` is 870 + 2,759 lines and delegates to a 1,499-line
`gaussian_splat_node_helpers.cpp` holding **six friend-helper objects**. Issue #552's fix
direction is precise: *"carve real ownership seams … as owned members, not friends. …
friend-decomposition widens state aliasing instead of shrinking it."*

That is exactly what exists today. The helpers are declared `friend` and each holds a bare
`GaussianSplatNode3D &owner` (`gaussian_splat_node_helpers.h:11-111`), and the node header
itself documents that they *"access 50+ private fields and methods"*
(`gaussian_splat_node_3d.h:118-128`). The six helpers are instantiated as value members
(`gaussian_splat_node_3d.h:258-263`) but every method reaches back into `owner.<private>`.
The result is one shared mutable-state blob split across two files — the surface area is not
reduced, only relocated. All ~50 private fields (`gaussian_splat_node_3d.h:130-256`) remain
co-owned by seven objects.

### Current responsibility map — every concern the node owns

| # | Concern | State (h) | Entry points | Helper (friend) |
|---|---------|-----------|--------------|-----------------|
| 1 | **Asset binding** — imported PLY/SPZ resource, connect/disconnect `changed`, reload with `CACHE_MODE_REPLACE` 3-failure handling, import-metadata flags, bounds recompute, `ply_file_path` legacy migration | `splat_asset`, `runtime_asset`, `asset_loading`, `asset_lod_enabled`, `asset_optimize_for_gpu` (`:131-143`) | `set_splat_asset` (`cpp:686`), `_load_asset` (`cpp:1792`), `_update_asset`, `_clear_asset`, `_on_asset_changed`, `_set` migration (`cpp:594`) | AssetHelper (`helpers.cpp:136-272`) |
| 2 | **Procedural / manual data** — validate 11 arrays, build `GaussianData`, synthesize opacity/SH from colors, compute bounds, build runtime asset | `renderer_data` (`:237`) | `set_splat_data` + 8 `_…splat…` privates (`cpp:723-971`) | — (on node) |
| 3 | **Quality / LOD / streaming policy** — 4 preset tables ×~28 keys, tier caps, clamps, effective-config snapshot + WARN-on-cap logging | `quality_preset`, `lod_bias`, `max_render_distance`, `max_splat_count`, `lod_config`, `streaming_config`, `effective_config_snapshot` (`:136-147`) | `set_quality_preset`/`set_lod_bias`/… | QualityHelper (`helpers.cpp:760-1207`, ~450 lines — the largest concern) |
| 4 | **Painterly** — brush params + `PainterlyManager` | `enable_painterly`, `edge_threshold`, `stroke_opacity`, `stroke_width`, `color_variation`, `temporal_blend`, `painterly_seed`, `painterly_manager` (`:150-157`) | `set_enable_painterly`/…, `_apply_painterly_settings` (`cpp:2077`) | — (on node) |
| 5 | **Renderer registration + settings ownership** — shared `Ref<GaussianSplatRenderer>`, director instance register/params/transform, the **global settings-owner lease** | `renderer`, `render_state_dirty`, `shared_renderer_multi_instance_state` (`:236-239`) | `_ensure_renderer`, `_apply_renderer_settings`, `_register_shared_renderer`, `_register_instance_in_director` (`cpp:2405`), `_update_instance_params_in_director` (`cpp:2499`) | RendererHelper (`helpers.cpp:1294-1499`) + free-function owner map (`helpers.cpp:35-80`) |
| 6 | **GPU base storage** — `gaussian_base` + `render_instance` RIDs against `GaussianSplatStorage` singleton | `render_instance`, `gaussian_base`, `last_known_scenario` (`:207-214`) | `_ensure_gaussian_base`/`_release_gaussian_base`/`_sync_gaussian_storage` (`cpp:2197-2252`), `_update_render_instance` | — (on node) |
| 7 | **Viewport bootstrap / render-target tracking** — 3-state machine, observers, deferred bootstrap + first-frame render | `cached_viewport_*`, `observed_viewport`, `viewport_texture_state`, `*_deferred` (`:219-234`) | `_update_cached_render_target`, `_deferred_viewport_bootstrap`, `_on_viewport_*` | ViewportHelper (`helpers.cpp:274-597`, ~320 lines) |
| 8 | **Per-frame update + camera publication** — update-mode gate, dirty upload, **`renderer->set_camera_transform/projection` every frame** | `render_state_dirty`, `shared_renderer_multi_instance_state` | `process_gaussian_render` (`cpp:1633`), `update_splats` + 8 privates, `_update_viewport_render_state` (**camera writes `cpp:1590-1591`**) | — (on node) |
| 9 | **Visibility** — CPU distance/frustum cull, parent-visibility tracking | `visible_in_viewport`, `parent_visible`, `parent_visibility_target`, `update_mode` (`:160,215-217`) | `_update_visibility` (`cpp:1974`) | VisibilityHelper (`helpers.cpp:1209-1292`) |
| 10 | **Color grading** — two-track replay state machine, per-instance director routing, bake/restore | `color_grading`, `grading_pushed_for_current_data`, `grading_explicit_pending` (`:177-192`) | `set_color_grading` (`cpp:2631`), `_push_color_grading_to_renderer`, `_replay_color_grading_if_pending` (`cpp:1914`) | — (on node) |
| 11 | **Debug overlays / HUD** — 13 flags, settings-manager persistence, child `CanvasLayer`+`Control` | `show_*`, `debug_draw_mode`, `debug_overlay_opacity`, `debug_hud_*` (`:242-256`) | `set_show_*`, `_update_debug_hud_visibility` (`cpp:2141`) | DebugHelper (`helpers.cpp:599-758`) |
| 12 | **Scene effectors / wind** — layer mask, scope-root resolution, ancestor collection, per-instance wind override | `scene_effector_*`, `wind_*`, `effect_*_scale` (`:165-173`) | `_get_instance_wind_*` (`cpp:2361`), `_resolve_scene_effector_scope_root` | — (on node) |
| 13 | **Compat properties / serialization** — `_set`/`_get`/`_get_property_list`/`_validate_property`, legacy fields, `stats/*` virtuals, inspector hiding | — | `cpp:517-684` | — (on node) |
| 14 | **Stats / monitoring** | `visible_splat_count`, `total_splat_count`, `last_update_time_ms`, `gpu_memory_mb` (`:195-199`) | `get_statistics` (`cpp:1361`), `get_effective_config_snapshot` | — (on node) |
| 15 | **Lifecycle / notifications** — ENTER/EXIT tree+world, PREDELETE prune dance, manager register | — | `_notification` (`cpp:432-515`) | — (on node) |
| 16 | **Editor drag-drop** | — | `_can_drop_data_fw`/`_drop_data_fw` (`cpp:2715`) | — (on node) |

The six friends cover concerns 1, 3, 5, 7, 9, 11; ten more concerns live directly on the
node. No concern owns its state — every field is reachable by all seven objects.

### Load-bearing comment invariants that must become structural

The audit (#551) calls out that correctness rests on prose. Four invariants are encoded only
in comments and are silently breakable by a reorder:

- **A — PREDELETE unref-then-prune (#551).** Two ~40-line mirrored comment blocks
  (`gaussian_splat_node_3d.cpp:460-500`, `gaussian_splat_world_3d.cpp:112-166`) explain that
  `NOTIFICATION_EXIT_TREE` already ran `unregister → _prune_world_if_unused` but observed
  `refcount>1` (the node still holds its `renderer` `Ref`), so PREDELETE must (1) `renderer.unref()`
  **first** (`node:494`, `world:160`), then (2) explicitly call `try_prune_world_if_unused`
  (`node:496-498`, `world:162-164`). The intervening `_unregister_shared_renderer()` is a
  no-op for pruning. Any reorder reintroduces the F6-reload renderer leak that motivated the
  fix (Codex review comments #3294797692 / #3294797697 on PR #387; director contract at
  `gaussian_splat_scene_director.h:266-276`). **Correct today, prose-guarded.**

- **B — settings gating. NOT one invariant — three distinct predicates.** (Corrected in
  review round 1; the earlier revision described this as a single "settings single-owner"
  rule, which would have licensed a decomposition that silently changed behavior. §B1 below
  is now the normative statement.)

  A file-static, mutex-guarded map (`g_renderer_settings_owner_mutex` /
  `g_renderer_settings_owner_lookup`, `helpers.cpp:35-36`, helpers at `:38-80`) elects one
  node as the settings owner per renderer. But **ownership is only one of three predicates
  in force**, and they are not equivalent:

  | | Predicate | Definition | Semantics when it denies |
  |---|---|---|---|
  | **P1** | `can_apply_renderer_settings()` — `helpers.cpp:1294-1325` | node holds the ownership lease for this renderer **and** is in tree/world **and** has local source data **and** the renderer's active scene data belongs to this node | write **dropped silently** |
  | **P2** | `_is_renderer_shared_with_other_content()` — `helpers.cpp:97-111` *and a duplicate* `cpp:41-58` | director reports `get_instance_count_for_renderer() > 1` **or** `has_world_submission_for_renderer()` | write **dropped, or the value forced to a safe default, for every node incl. the owner** |
  | **P3** | splat-count mismatch heuristic — `helpers.cpp:1385` | `renderer_splat_count > 0 && renderer_splat_count != local_splat_count` | write dropped + `WARN_PRINT_ONCE` (`:1387`) |

  Three consequences the decomposition **must** preserve, none of which follows from a
  single lease:

  1. **P2 is not P1.** The four `show_*` debug flags are not merely dropped when the
     renderer is shared — they are **actively forced to `false` even for the lease-holding
     owner** (`helpers.cpp:608-611`, `shared_renderer ? false : owner.show_*`). A
     lease-gated setter cannot express "forced off for the owner too."
  2. **The painterly setters gate on P2 alone, with no ownership check**, and they
     `return` **before writing the node-local member** (`cpp:1026-1029`, `1039-1042`,
     `1051-1054`, `1068-1071`, `1081-1084`) — so the property value is discarded, not just
     the renderer write. `set_enable_painterly` (`cpp:1011-1024`) is the asymmetric
     exception: no P2 gate at all.
  3. **`_validate_property` keys off P2 alone** (`cpp:518-536`), never P1. So on a shared
     renderer the *owner's* inspector rows are hidden too. Deriving hiding from
     `holds_settings_lease()` — as an earlier revision of this ADR proposed — would make
     the owner's rows **reappear**. That is a behavior change, not a refactor.

  Additionally, **P1's "check" is itself a mutation**: `can_apply_renderer_settings()` is
  declared `const` but releases (`:1319`) and claims (`:1324`) entries in the global map on
  every call. And color grading is deliberately **exempt** from all three
  (`helpers.cpp:1403-1410`: *"No shared-renderer gate needed: peers no longer share a
  single color_grading slot"*), with an independent ungated push path at `cpp:1884-1912`.

  The hiding/gating string lists are kept **manually** in sync — `_validate_property`'s own
  comment points at `gaussian_splat_node_helpers.cpp:1284-1378`, which is already ~10 lines
  off from the real `can_apply_renderer_settings()` range (`:1294-1325`). The drift is
  measurable today, and §B1 records it rather than pretending the two lists agree.

- **C — color-grading replay flags.** `grading_pushed_for_current_data` and
  `grading_explicit_pending` (`gaussian_splat_node_3d.h:184-192`) form a two-track replay
  machine whose clear/set sites are prose-specified across five locations and *must not* be
  reset on EXIT_TREE (`cpp:396-404`, `1914-1953`, `helpers.cpp:1347-1358`). A wrong
  clear clobbers a peer's grading on a shared renderer.

- **D — `last_known_scenario` caching.** Cached at register time
  (`cpp:2443-2450`) precisely because `get_world_3d()` returns null by PREDELETE
  (`gaussian_splat_node_3d.h:209-214`). The dependency is prose-only.

These are the invariants #551 wants encoded in *types*, not comments.

### B1 — the gating matrix that must be carried forward verbatim

This table is **normative**. It is the current, verified behavior at the base SHA, and it is
the acceptance criterion for every step that touches settings. A step may change *where* a
predicate is evaluated; it may **not** change *which* predicate governs a property, or what
denial does. Any intended change is a separate, owner-approved behavior PR — never a
side effect of decomposition.

| Property / write | Gated by | Denial effect | Site |
|---|---|---|---|
| `set_max_splats` | P1 **then** P3 | dropped + `WARN_PRINT_ONCE` | `helpers.cpp:1385-1390` |
| `set_lod_enabled`, `set_lod_bias`, `set_lod_max_distance`, `set_frustum_culling`, `set_async_upload_enabled` | P1 | dropped silently | `helpers.cpp:1392-1397` |
| `set_painterly_enabled` (from `apply_renderer_settings`) | P1 | dropped silently | `helpers.cpp:1398` |
| `set_painterly_edge_threshold/stroke_opacity/stroke_length/gamma` | P1 | dropped silently | `helpers.cpp:1399-1402` |
| `set_streaming_config_overrides` | P1 | dropped silently | `helpers.cpp:1454` |
| `debug/overlay_opacity`, `debug/debug_draw_mode`, `debug/runtime_preview` | P1 **only** | dropped silently; **not** hidden in inspector | `helpers.cpp:699`, `:712`, `:731` |
| `show_tile_grid`, `show_density_heatmap`, `show_performance_hud`, `show_residency_hud` | P1 **and** P2 | renderer write dropped; **and forced to `false` for the owner too** when P2 holds | gates `helpers.cpp:631-632`, `646-647`, `661-662`, `752-753`; forcing `:608-611` |
| node-local `show_*` member + `GaussianSplatSettingsManager` persistence | **ungated** — written before the gate | always applied | `helpers.cpp:627/629`, `642/644`, `657/659`, `748/750` |
| `edge_threshold`, `stroke_opacity`, `stroke_width`, `temporal_blend`, `painterly_seed` (setters) | P2 **only**, no ownership check | **node-local member write also skipped** | `cpp:1026-1089` |
| `enable_painterly` (setter) | **ungated at the setter** (asymmetric — deliberate today) | — | `cpp:1011-1024` |
| `color_variation` | no renderer control exists — explicit no-op | — | `cpp:1063-1066` |
| color grading | **deliberately exempt** from P1/P2/P3 | always applied per-instance | `helpers.cpp:1403-1410`; independent path `cpp:1884-1912` |
| camera transform / projection | **ungated**, every node, every frame | — | `cpp:1590-1591` (this is #550, and Step 6 is the *only* step licensed to change it) |
| `_validate_property` inspector hiding | P2 **only** | hides `painterly/*`, the four `debug/show_*`, `quality/lod_bias`, `quality/max_splat_count` — **including for the owner** | `cpp:518-536` |
| sharing-status change detection | per-frame poll, not an event | re-runs `_apply_renderer_settings()` + `notify_property_list_changed()` | `cpp:1677-1686` (`shared_renderer_multi_instance_state`, decl `h:239`) |

**Known pre-existing inconsistencies — preserve them, or fix them in a named separate PR.**
The decomposition must not silently "clean these up," because each is observable:

- Hidden but not P2-gated: `painterly/enabled`, `quality/lod_bias`, `quality/max_splat_count`
  (their setters have no P2 gate, so a shared-renderer owner can still set them from GDScript
  and the write lands).
- P1-gated but never hidden: `debug/overlay_opacity`, `debug/debug_draw_mode`,
  `debug/runtime_preview`, `quality/max_render_distance`, `rendering/frustum_culling`.
- `quality/preset` stays editable while the `lod_bias` / `max_splat_count` it drives are
  hidden — a hidden knob remains indirectly movable.
- `_is_renderer_shared_with_other_content` is **duplicated** with two signatures
  (`helpers.cpp:97-111` taking `GaussianSplatNode3D &`, `cpp:41-58` taking
  `const Ref<GaussianSplatRenderer> &`). Collapsing the duplicate to one function is in scope
  for Step 2 and is behavior-preserving; changing its *predicate* is not.
- Only **one** drop in the entire surface warns (`helpers.cpp:1387`). Every other denial —
  the whole `apply_renderer_settings` body, all eight debug setters, all five painterly
  setters — is completely silent. Making denials observable is desirable but is a
  **behavior change**: it belongs in its own PR with its own sign-off, not inside a
  decomposition step.

## Decision

Replace friend-decomposition with **owned collaborators**: small C++ objects that own their
own state (private members), expose a **narrow, explicit contract**, and are **not friends**
of the node. The node becomes a thin `Node3D` scene-facing shell that (a) registers the
GDScript API, (b) routes property/notification plumbing, and (c) wires collaborators together
by passing values across contracts — never by reaching into another object's privates.

### Target collaborators (owned members, no `friend`)

| Collaborator | Owns (moves off the node) | Contract (illustrative) | Closes concern(s) |
|---|---|---|---|
| **`AssetBinding`** | `splat_asset`, `runtime_asset`, `renderer_data`, `asset_loading`, import flags, local AABB | `bind(asset)`, `set_procedural(arrays…)`, `reload()`, `clear()`; returns `PayloadView{ count, aabb, is_2d, origin_label }`; signals `data_ready` / `data_cleared` to the shell via a callback interface | 1, 2 |
| **`QualityPolicy`** | `quality_preset`, `lod_bias`, `max_render_distance`, `max_splat_count`, `lod_config`, `streaming_config`, `effective_config_snapshot` | pure: `set_preset(...)`/`set_lod_bias(...)` → `ResolvedQuality{ RendererSettings, effective_snapshot }`. **No renderer, no GPU, no tree** — unit-testable in isolation | 3 (+ 4 painterly params fold in as inputs) |
| **`RendererRegistration`** | `renderer` `Ref`, `render_instance`, `gaussian_base`, `last_known_scenario`, director instance record, **the settings lease** | `attach(world)`, `detach()`, `publish_instance_params(InstanceParams)`, `try_apply(const RendererSettings&)` **gated by a held `RendererSettingsLease`**, `release_and_prune()` (see LifecycleGuard) | 5, 6 |
| **`ViewportRenderState`** | `cached_viewport_*`, `observed_viewport`, `viewport_texture_state`, deferral flags, observers | `track(viewport)`, `untrack()`, `is_ready()`, `render_target()`, `size()`; signals `became_ready`. **Camera publication removed** (see #550) | 7 |
| **`ColorGradingPolicy`** | `color_grading` + the two replay flags as a typed `GradingReplayState` | `set_grading(...)`, `on_resource_changed()`, `on_data_window_opened(director, id)`, `on_data_cleared()`, `on_renderer_ref_changed()` — named transitions replace prose (invariant C) | 10 |
| **`DebugOverlayController`** | 13 `show_*` flags, `debug_draw_mode`, HUD child nodes, settings-manager persistence | `set_show_*`, `sync_to_renderer(lease)`, `update_hud()` | 11 |

Concerns 9 (visibility), 12 (effectors/wind), 13 (property plumbing), 14 (stats), 15
(lifecycle dispatch), 16 (drag-drop) stay on the shell — they are genuinely the node's
scene-facing responsibility, and shrink to thin forwarders once 1/3/5/7/10/11 move out.

### Making the four invariants structural

- **Invariant A + D → `LifecycleGuard` / director atomic API (#551).** Add
  `GaussianSplatSceneDirector::release_renderer_and_prune(Ref<GaussianSplatRenderer> &r, const RID &scenario)`
  that performs `r.unref()` **then** `_prune_world_if_unused(scenario)` as one indivisible
  step, or an equivalent scoped `PruneAfterUnref` guard whose destructor enforces the order.
  Both node and world PREDELETE collapse to a single call; the ordering **cannot be expressed
  wrong** because the two operations are no longer separately callable at the site. `last_known_scenario`
  becomes a private of `RendererRegistration`, refreshed on attach. The mirrored 40-line
  comments shrink to a 3-line pointer at the guard.

- **Invariant B → `RendererSettingsLease` (P1) *plus* a preserved `SharingState` (P2) —
  two seams, not one.** (Corrected in review round 1. The earlier revision proposed a single
  lease and derived `_validate_property` hiding from `holds_settings_lease()`; per §B1 that
  would have unhidden the owner's inspector rows on a shared renderer and dropped the
  "forced to `false` even for the owner" semantics of the four `show_*` flags. Both are
  behavior changes and both are now forbidden.)

  - **P1 becomes `RendererSettingsLease`,** a typed token held by `RendererRegistration`,
    replacing the file-static map. Dead-owner stealing (`_settings_owner_is_live`,
    `helpers.cpp:38-44`) becomes `try_acquire`. Because today's `can_apply_renderer_settings()`
    mutates the map from a `const` method (`:1319`/`:1324`), the lease API must make the
    acquire/release **explicit and non-`const`**, with a separate pure
    `holds_lease()` query — this is the one place the refactor legitimately improves the
    shape without changing the predicate.
  - **P2 stays a distinct, separately-evaluated predicate** — a `SharingState` value
    (`instance_count > 1 || has_world_submission`) read from the director, exposed as
    `registration.sharing_state()`. The two duplicated implementations
    (`helpers.cpp:97-111`, `cpp:41-58`) collapse into this one function; the predicate is
    unchanged.
  - **P3 (the splat-count heuristic, `helpers.cpp:1385`) is carried forward verbatim** as a
    guard inside the `set_max_splats` path. It is *not* folded into the lease: it is a
    different question ("does the renderer already carry someone else's splat count?") and
    two peers with equal counts must continue not to trip it.
  - **`_validate_property` hiding continues to derive from P2 alone** — from
    `sharing_state()`, **never** from `holds_settings_lease()`. This keeps the owner's rows
    hidden on a shared renderer, exactly as today. The win is that the duplicated string
    list at `cpp:517-536` is replaced by one `constexpr` list shared with the gating sites,
    so drift becomes impossible; the *predicate* is untouched.
  - **The `show_*` "force to `false` when shared" semantics (`helpers.cpp:608-611`) is part
    of the contract**, not an implementation detail: `DebugOverlayController::sync_to_renderer`
    takes both the lease *and* the `SharingState` and reproduces the forcing exactly.
  - **Color grading stays exempt** from all three predicates, and the ungated per-instance
    push path (`cpp:1884-1912`) is preserved as-is.

- **Invariant C → `GradingReplayState`.** The two bools become a small enum-driven type with
  named transitions (`ArmedExplicit`, `PushedForWindow`, `Idle`) inside `ColorGradingPolicy`;
  the "do not reset on EXIT_TREE" rule is encoded by simply *not exposing* a reset there.

### One-writer-per-frame camera publication (#550)

`_update_viewport_render_state` writes `renderer->set_camera_transform/projection` every
frame (`cpp:1590-1591`); with N nodes on a shared renderer that is N redundant
last-write-wins writes each frame — the same "renderer-wide state, per-node writer" ambiguity
the settings lease exists to prevent. **Move camera publication to a single writer**: a
`CameraPublisher` owned by `GaussianSplatManager` / `GaussianSplatSceneDirector` that reads
the active camera once per renderer per frame and publishes it. Nodes stop writing camera
state entirely; `ViewportRenderState` keeps only render-target tracking. This closes #550 and
removes camera from the node's per-frame surface.

## Staged migration (each step compiles and is CI-green on its own)

Ordered to extract zero-lifetime-risk policy first, encode the structural invariants next,
and touch GPU/lifetime seams last. Every step keeps the public GDScript API and serialized
property names byte-identical; behavior is preserved and proven per step.

- **Step 0 — this ADR + a nested `nodes/AGENTS.md`** capturing the ownership seams and the
  four invariants, so new code does not re-widen the surface. *(R0, docs.)*
- **Step 1 — extract `QualityPolicy`** (the ~450-line QualityHelper + preset tables, 4, 5).
  It befriends nothing; takes inputs by value, returns `ResolvedQuality`. Biggest LOC drop,
  no GPU/lifetime. **Gate:** guard lane + new `tests/…/test_quality_policy.h` (preset tables,
  tier caps, `GS_LOD_BIAS_MIN/MAX` clamps). *(R1.)*
- **Step 2 — `RendererSettingsLease` (P1) + `SharingState` (P2) + preserved P3** (invariant B).
  Replace the free-function owner map with the lease; collapse the two duplicate
  `_is_renderer_shared_with_other_content` implementations into one `sharing_state()`;
  keep `_validate_property` hiding derived from **P2**; keep the P3 splat-count heuristic on
  the `set_max_splats` path; keep the `show_*` force-to-`false`-when-shared semantics.
  Behavior-preserving in the strong sense of §B1.
  **Gate (blocking):** a **gating matrix table test** that asserts the §B1 table cell-by-cell —
  for each property, on each of {sole node, shared-renderer owner, shared-renderer non-owner,
  dead-owner-stolen}, assert (a) whether the renderer write lands, (b) whether the node-local
  member is written, (c) whether the inspector row is hidden. This test must be written
  **against the base SHA first and pass unmodified there**, then still pass on the head —
  that is what makes "behavior-preserving" checkable rather than asserted. Plus the existing
  lifetime tests and a lease unit test (acquire / steal-dead-owner / release / peer-denied).
  *(R2.)*
- **Step 3 — `release_renderer_and_prune` / `PruneAfterUnref`** (invariant A, **#551**).
  Rewrite **both** node and world PREDELETE to the single atomic call; delete the mirrored
  comments. **Gate (blocking):** `tests/…/test_renderer_lifetime_proof.h` scenario_c (F6
  reload) stays green + a compile-time proof the two ops are not separately callable at the
  site; runtime lifetime evidence attached. **R3 — separate owner sign-off.**
- **Step 4 — `ColorGradingPolicy` / `GradingReplayState`** (invariant C, 10). Behavior-preserving;
  grading + shared-renderer peer tests. *(R2.)*
- **Step 5 — `ViewportRenderState` + `AssetBinding` as owned (non-friend) members** (7, 1, 2).
  Drop the `friend` declarations for the viewport/asset helpers. **Gate:** editor-preview
  smoke + new node+`splat_asset` integration coverage (closes the #299 gap). *(R2.)*
- **Step 6 — one-writer camera publication** (**#550**). Move `set_camera_transform/projection`
  into the manager/director `CameraPublisher`. **Gate:** visual validation on GrandmasHouse
  (shared renderer, multi-node) — rendering-math-adjacent, must validate on real-scan content.
  *(R2.)*
- **Step 7 — `DebugOverlayController` + delete the `friend` block** (11). Once all six helpers
  are owned collaborators, remove `friend class …` (`gaussian_splat_node_3d.h:123-128`); verify
  the header no longer exposes privates to helpers. Node header becomes a thin shell. *(R1.)*

## Issue-closure mapping

| Issue | Disposition | Closed by |
|---|---|---|
| **#552** god-class decompose | **Closed** by the full sequence (Steps 1–7): friends → owned collaborators, real ownership seams | Steps 1–7 |
| **#551** PREDELETE ordering structural | **Closed** — the ordering becomes an atomic director API / scoped guard | Step 3 |
| **#550** N per-frame camera writers | **Closed** — one writer per renderer per frame | Step 6 |
| **#558** script-instantiable internals | **Advanced, not regressed** — every new collaborator is a plain internal C++ type (never `GDREGISTER`'d), so decomposition adds **zero** script surface; the heavy-internals `GDREGISTER_CLASS` audit (`register_types.cpp:116-120`) remains its own follow-up | (non-regression note) |
| **#516 / #517** world-node lifecycle | **Not closed here** — owned by in-flight **PR #578**. Step 3's guard should be reused by the world node; sequence Step 3 to land before/after #578 so only one of them touches `gaussian_splat_world_3d.cpp:112-166` | coordination |
| **#357 / #547** settings ownership / loader dedup | **Adjacent** — Step 2's lease clarifies renderer-settings ownership; routing `QualityPolicy` (Step 1) through `gs::settings` helpers is the natural home for #547's dedup. Tracked separately | follow-up |
| **#299** node+asset integration gap | **Closed** as the Step 5 gate | Step 5 |
| **#592** editor preview worker hang | **Out of scope** — different file (`editor/gaussian_resource_preview_generator.cpp`); noted for adjacency only | — |

## Consequences

- **Benefit.** State aliasing shrinks from 7 co-owners of ~50 fields to one owner per seam
  with a narrow contract. Three prose-guarded invariants (A/B/C) become types that cannot be
  expressed wrong; the two mirrored 40-line comments are deleted. `QualityPolicy` becomes
  unit-testable without a tree or GPU. The node header stops leaking 50+ privates.
- **Cost / risk.** Step 3 is R3 (GPU teardown ordering) and needs runtime lifetime evidence;
  Step 6 needs GPU visual validation (agents cannot raster locally — must run on the GPU
  runner). The refactor is behavior-preserving, so its value is velocity + correctness
  durability, not user-visible change — reviewers should grade it on *seam quality*, not diff
  size.
- **Coordination.** Step 3 and PR #578 both touch the world node PREDELETE; whichever lands
  second rebases onto the other. No step bundles unrelated work; each is independently
  reversible.

## Invariant list — what every step is graded against

Checkable. A step that violates one is rejected even with green CI.

| # | Invariant | How it is checked |
| --- | --- | --- |
| **N1** | The §B1 gating matrix holds cell-for-cell. No property changes which predicate governs it; no denial changes its effect (dropped / forced-off / node-local-also-skipped). | The Step 2 gating matrix table test, written against the base and passing unmodified on head. |
| **N2** | `_validate_property` hiding derives from **P2 (`sharing_state()`) only** — never from lease-holding. The owner's rows stay hidden on a shared renderer. | Explicit case in the matrix test: shared-renderer **owner** → rows hidden. |
| **N3** | The `show_*` flags are forced to `false` when P2 holds, **including for the lease holder**; the node-local member and settings-manager persistence still receive the write. | Matrix test rows (a)+(b) for the four `show_*` flags. |
| **N4** | The painterly setters gated on P2 continue to skip the **node-local member write**, not just the renderer write. `set_enable_painterly` remains ungated at the setter. | Matrix test row (b) for the five setters + the asymmetry case. |
| **N5** | P3 (splat-count heuristic) remains a distinct guard: two peers with **equal** splat counts do not trip it. | Unit test with two equal-count peers asserting the write lands and no WARN fires. |
| **N6** | Color grading remains exempt from P1/P2/P3, and the independent push path stays ungated. | Shared-renderer peer grading test unchanged. |
| **N7** | Exactly one definition of the P2 predicate exists after Step 2 (the duplicate is deleted, not forked). | Grep guard: one `sharing_state()` definition; zero `_is_renderer_shared_with_other_content`. |
| **N8** | Exactly one source of truth for the hidden-property name list; the gating sites and `_validate_property` consume the same list. | Grep guard: the property-name string list appears once. |
| **N9** | The PREDELETE unref-then-prune ordering (invariant A) cannot be expressed wrong at the call site — the two operations are not separately callable there. | Step 3: compile-time proof (the separate ops are private/unavailable at the site) + `test_renderer_lifetime_proof.h` scenario_c green. |
| **N10** | The public GDScript API and every serialized property name are byte-identical across all seven steps. | Property-list snapshot test; doc_classes completeness guard stays green. |
| **N11** | No new script surface: every collaborator is a plain internal C++ type, never `GDREGISTER`'d. | Grep guard on `GDREGISTER_CLASS` count in `register_types.cpp` (unchanged). |
| **N12** | Friendship strictly decreases; zero `friend class` grants in `gaussian_splat_node_3d.h` after Step 7. | Grep guard, monotonically non-increasing. |
| **N13** | Camera publication changes **only** in Step 6. No earlier step alters `cpp:1590-1591` semantics. | Diff review per step. |

## Evidence a step must produce

1. **Gating matrix evidence (N1–N6):** the matrix test output from the base SHA and from the
   head, attached and diffed. This is the single most important artifact in this ADR — the
   defect it guards against (silently changing which predicate governs which property) is
   invisible to every other check.
2. **Guard lane:** `run_module_tests.py --guard-only` green, plus the N7/N8/N11/N12 grep
   guards, which land with the step that makes them true.
3. **Targeted tests:** existing node, lifetime, grading, and shared-renderer suites green
   **without modification**. Modifying an existing assertion is a review blocker absent a
   written reason.
4. **Step 3 (R3):** runtime lifetime evidence — RID counts across an F6 reload cycle, with
   `test_renderer_lifetime_proof.h` scenario_c green, plus the compile-time ordering proof.
   Separate owner sign-off.
5. **Step 6 (R2, rendering-adjacent):** visual validation on real-scan content
   (GrandmasHouse, shared renderer, multi-node) from the GPU runner. Agents cannot raster
   locally; absent runner output this reports "not run", never "passed".
6. **Step 5:** editor-preview smoke + the new node+`splat_asset` integration coverage (#299).
7. **Base anchoring:** base SHA recorded; `file:line` anchors re-verified against it.

No step weakens a guard, baseline, or threshold. A behavior change discovered to be
*necessary* is lifted out into its own PR with its own sign-off — it never rides inside a
decomposition step.

## Decisions the owner needs to make

- **D1 — Adopt "ownership seams, not friends" as the target** (owned collaborators, no
  `friend`), per #552's fix direction? (Y / N / amend.)
- **D2 — Approve the four invariants becoming structural** (LifecycleGuard, RendererSettingsLease,
  GradingReplayState, scenario-cache ownership)? In particular, **prefer the director-side
  atomic `release_renderer_and_prune` over a scoped `PruneAfterUnref` guard**, or vice versa?
- **D3 — Confirm the staged order and that Step 3 is R3** with its own sign-off + lifetime
  evidence gate, and Step 6 requires GPU visual validation.
- **D4 — Sequence Step 3 against PR #578** (which lands first for the shared world-node
  PREDELETE edit)?
- **D5 — Confirm the §B1 gating matrix is frozen for the decomposition.** The three
  predicates (P1 lease / P2 sharing / P3 splat-count heuristic) and the listed pre-existing
  inconsistencies are carried forward **verbatim**, and every fix to them is a separate,
  separately-approved behavior PR. In particular, confirm we do **not** take the tempting
  simplifications: collapsing P2 into the lease, deriving inspector hiding from
  lease-holding, or making the silent denials warn. (Recommended: yes, freeze — each of
  those is user-visible.)
