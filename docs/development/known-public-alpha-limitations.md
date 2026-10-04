# Known Public Alpha Limitations

This page is intentionally required by the renderer release-gate manifest. A
public-alpha candidate may carry an accepted limitation only when the candidate
evidence bundle points to a section here with:

- the issue URL;
- user-facing impact;
- affected platform or workflow;
- mitigation or unsupported-path statement;
- evidence proving the limitation does not hide renderer correctness failures.

## How to read this page

Every entry is a defect that is real, reachable in a supported configuration, and in
the alpha as it ships, with a workaround where one exists. Each entry carries a status:

| Status | Meaning |
| --- | --- |
| **Accepted** | A named human accepted it as a public-alpha limitation, and the release-gate manifest's `public_alpha_issue_ledger` has an entry for it. Only these entries can be cited by a release candidate. |
| **Active** | Real and open, disclosed so you are not surprised by it. It is **not accepted**: nobody has formally accepted it, and nothing in the gate tracks it. Listing it here does not decide whether it should block the alpha; that is the maintainer's call, recorded in the acceptance bar. |
| **Mitigated** | The defect this page used to describe was fixed. A narrower remainder is still open, and the entry describes only that remainder. |
| **Engine limitation** | The cause is in upstream Godot, not in this module, and the module cannot fix it alone. |

Fixed defects are removed from the list. The [Recently resolved](#recently-resolved)
section at the bottom records what this page used to say about them and which change
fixed them.

### Summary

| Limitation | Issue | Status |
| --- | --- | --- |
| Splats trail by about 1.5 px under TAA in motion | [#1025](https://github.com/klausi3D/godotGS/issues/1025) | Accepted |
| The bottom of the frame can go empty when overlap records run out | [#54](https://github.com/klausi3D/godotGS/issues/54) | Accepted |
| Far views of dense content can show 16 px black tiles | [#1137](https://github.com/klausi3D/godotGS/issues/1137) | Active |
| Transparent viewports are opaque under TAA or FSR2 | [#989](https://github.com/klausi3D/godotGS/issues/989) | Engine limitation |
| Shadow-casting splats darken themselves; the starter template renders dark | [#1089](https://github.com/klausi3D/godotGS/issues/1089) | Active |
| Painterly's GPU path has one nightly, non-gating test | [#997](https://github.com/klausi3D/godotGS/issues/997) (closed) | Mitigated |
| Painterly ignores sphere effectors and per-splat depth clipping | [#1079](https://github.com/klausi3D/godotGS/issues/1079) | Active |
| The painterly composite's `blend_strength` is a no-op | [#1001](https://github.com/klausi3D/godotGS/issues/1001) | Active |
| `get_statistics()` can crash when polled every frame | [#1030](https://github.com/klausi3D/godotGS/issues/1030) | Active |
| The starter template prints a render-thread error at shutdown (the crash is fixed) | [#1077](https://github.com/klausi3D/godotGS/issues/1077) | **Alpha blocker** (§11) |
| A world and a `GaussianSplatNode3D` in one scene: one of them renders nothing | [#788](https://github.com/klausi3D/godotGS/issues/788) | Active |
| A world payload change costs a full resubmit | [#1008](https://github.com/klausi3D/godotGS/issues/1008) | Active |
| An emptied world does not reach the renderer | [#1002](https://github.com/klausi3D/godotGS/issues/1002) | Active |
| World bounds are never re-derived once set | [#1003](https://github.com/klausi3D/godotGS/issues/1003) | Active |
| `strict_identity_transform` is bypassed on resubmit | [#1006](https://github.com/klausi3D/godotGS/issues/1006) | Active |
| Applying a world writes `world_path` into your resource | [#1007](https://github.com/klausi3D/godotGS/issues/1007) | Active |
| A world resource replaced mid-load can stay connected | [#1005](https://github.com/klausi3D/godotGS/issues/1005) | Engine limitation |
| A failed tile-sorter grow can lose both sorters | [#983](https://github.com/klausi3D/godotGS/issues/983) | Active |
| Linux is smoke-tested, not editor-tested | — | Active |
| macOS is build-supported and unvalidated | — | Active |
| Nightly Linux editors are unoptimized | — | Active |
| No Linux export template is attached to releases | [#1015](https://github.com/klausi3D/godotGS/issues/1015) | Active |
| Nothing is code-signed | [#1015](https://github.com/klausi3D/godotGS/issues/1015) | Active |

**Blocking defects are not listed as limitations here** — they are in the
[acceptance bar](../governance/release-acceptance-bar.md)'s §11 list. This page is for
what we ship knowing about. One §11 blocker has an entry here so that a user who sees its
symptom can find the issue: #1077, the shutdown error (see its entry). It is a blocker, not
an accepted limitation. Before it, the last blocker this page named was #851 (painterly
ignoring scene lighting), which closed on 2026-10-01, fixed by #1078; see
[Recently resolved](#recently-resolved).

The other exception is the clearly-fenced **"Proposed, not yet accepted"** section near
the bottom. A defect lands there when someone has proposed shipping it but no named human has
accepted it yet, so it is **still a blocker**. It is written up in advance only so the
disclosure and the disposition are drafted together and cannot drift; nothing in that
section may be cited as an `accepted_alpha_limitation`. If you are looking for what the
alpha actually ships with, read everything *above* that heading. The "Recently resolved"
section after it lists only fixed defects.

> That list is **human-maintained, and the machine gate cannot see all of it.** The
> candidate gate's population is issues labelled `priority:P0`, `priority:P1` or
> `release blocker`. The §11 blockers this page has named carried none of those, so nothing
> automated would have stopped a release on them. Four have left that list: #929 when it
> closed on 2026-09-20, carrying only `program:prod-ready`; #54 when it was accepted on
> 2026-09-25 (it is now listed under [Rendering](#rendering)); #833 when it was struck from
> §11 (item 9) on 2026-09-30, fixed by #1027, #1031 and #1032; and #851, which is
> `priority:P2`, when it closed on 2026-10-01, fixed by #1078 (§11 item 8). #1077, a §11
> blocker since 2026-10-03 (item 11), carries none of those labels either. The streaming
> items §11 still lists (#320, #786, #883) are `priority:P1` and visible to the gate; its
> 50M-asset lane item has no issue at all. Read "it is in the blocker set" as "a human has to hold the
> release for it", not as a guarantee the tooling enforces. Labelling them is tracked as
> an obligation on the bar.

**Where an entry has not been reproduced on hardware, it says so.** "Unproven" means
nobody has seen it happen, not that it does not happen; the code reading that produced it
is cited so you can check it yourself.

### Disclosure here is not the same as admission by the gate

The five bullets at the top of this page are the bar a limitation must clear to be cited
by a release candidate as an `accepted_alpha_limitation` — including the last one,
evidence that the limitation does not hide a renderer correctness failure. **Most entries
below do not clear it yet**. Some are code-reading findings with no reproduction, and the
entries for #983 and #1005 say so in their own Evidence lines.

Those two things are deliberately kept apart. This page's job as a *user* document is to
disclose everything real that we know about, reproduced or not; an entry with no
reproduction is still worth a user's time. Its job as a *gate* input is narrower, and an
entry that has not been reproduced **must not** be pointed at by a candidate's
`docs_path` until it has been. Nothing here should be read as having pre-cleared that
check. Do not relax the criterion at the top of this page to make an entry admissible —
produce the evidence, or leave the issue in the blocker set.

**And clearing it is still not sufficient.** A candidate bundle may cite an entry here only
if the issue *also* appears in the manifest's `public_alpha_issue_ledger`
(`_validate_candidate_issues` in `tests/ci/check_renderer_release_gates.py`). #1025 and #54
are the two entries on this page that have all of it — a named human acceptance, hardware
evidence behind all five bullets, and a ledger entry (#1025's since
[#1038](https://github.com/klausi3D/godotGS/issues/1038), and #54's since its 2026-09-25 acceptance). For #54 the hardware evidence covers the lasting
band; the brief band at defaults is derived from the code and not captured, and its entry
says so. **Every other entry here is still machine-invisible**, and adding a ledger entry is
an R3 manifest edit, not a docs change. Read an entry on this page as a disclosure to a reader, not as
something a release candidate may cite, unless the ledger says otherwise.

**Every entry was re-verified against `eff00db450c` (2026-10-01)**: the issue's state, the
merged changes that reference it, and the code it describes. Measurements were not retaken
for that revision. They are as reported, on the binaries they were taken on. The #1025 figures come from the pre-fix and
post-#1026 binaries (the entry was first verified at `bc77ce31e9c`). The #54 figures come from
`6f4552076c7`, and its import-route and node figures follow the code at `673f9c709f8`.

## Rendering

### Splats trail by about 1.5 px under TAA while the camera or the content is moving ([#1025](https://github.com/klausi3D/godotGS/issues/1025))

**Status: Accepted** (2026-09-20).

With TAA enabled and the camera or the content in motion, splat detail is drawn behind its
true position by **≈1.5 px** — 0.27–0.87 frames stale, and 6–26× an in-frame geometry control
measured in the same frames on the two pans. (On a forward dolly it is smaller than that:
2× the control in frames, and 0.36 px against the control's 0.54 px.) The trail does **not**
grow with camera speed: **1.48 px** at a
15°/s pan and **1.65 px** at 56°/s, because TAA's variance clip is computed from the current
frame. With a *static* camera there is nothing to see — the splat projection has carried the
engine's temporal jitter since #929/#1026.

**Under FSR2 there is no ghosting, at either scale.** **0.000 frames** of staleness at scale
1.0 across a slow pan and a normal pan — exactly the floor the same in-frame control sets —
and **0.04 px** on a dolly; at scale 0.5 the splat trail is at or below the control's in two
of three motions. The reason is a mechanism nothing had written down: the composite already
feeds FSR2's reactive mask through the destination alpha, so FSR2 reconstructs splat pixels
from the current frame instead of accumulating them. **Do not avoid FSR2 on account of this
entry.**

**Affects:** Forward+, single view, with `use_taa = true`. Measured on Windows, RTX 3090,
Vulkan 1.4.325, on a real scan at 960×540.

**Workaround:** `use_taa = false` — already Godot's own default. FSR2 needs no workaround at
either scale, and neither does a still camera.

**Accepted** as a public-alpha limitation on 2026-09-20 under the acceptance bar's §10.1
exception
([disposition](https://github.com/klausi3D/godotGS/issues/1025#issuecomment-5752837553)).
The disposition record and the mechanism live in the bar's
[§8.1](../governance/release-acceptance-bar.md); this entry is the user-facing half.

**Evidence: measured, and the rig proved it could fail.** The
[measurement](https://github.com/klausi3D/godotGS/issues/1025#issuecomment-5751997279) ran on
both the pre-fix and post-#1026 binaries, which agree to within 0.01 frames in every cell, so
the jitter fix neither causes nor cures this. Three gates ran before any figure was reported:
two independent walks of each camera path with no temporal stage were bit-identical over all
16,588,800 pixels; the region masks were re-derived per camera pose by hiding each subject, so
they follow the content across the screen; and **every temporal config had to move a control
region relative to the no-temporal capture**, so a viewport that had silently refused TAA or
FSR2 could not have been reported as a clean result. That last gate is what this page's fifth
admission bullet asks for — evidence that the limitation is not hiding a renderer correctness
failure rather than merely not showing one.

**The determinism gate was clean in the matrix these figures come from, and not always.** An
earlier matrix on the same binaries failed it in 5 of 6 runs, by 3–9 LSB over 49–963 of those
16,588,800 pixels — four to five orders of magnitude below the signal, and it moves none of
the numbers above, but it means the GS pipeline is very nearly, and not exactly,
frame-deterministic under a moving camera. Anyone extending #1026's **exact-zero**
determinism assertion to a moving camera should expect it to be flaky, and decide what to do
about that before landing it rather than after.

**One honest caveat.** The trail is ≈1.5 px whatever the content, but its *visibility* is not
content-independent. On the soft real scan measured it is not visible without roughly 10×
amplification; a sharper, higher-contrast scan would show more, and that has not been
measured.

Fixing it means the module publishing a velocity field — which is *not* an engine-boundary
change, since the module already holds the buffers and the previous-frame camera — but a wrong
velocity smears confidently and reads as a renderer bug, which is why it is not alpha work.

### The bottom of the frame can go empty when a frame needs more overlap records than are allocated ([#54](https://github.com/klausi3D/godotGS/issues/54))

**Status: Accepted** (2026-09-25).

Every splat that touches a tile costs one **overlap record**. If a frame needs more records
than the renderer has currently allocated, the tiles with
the highest tile index get nothing — and because tiles are numbered row by row, that is a
**hard horizontal line across the frame with only background below it**, plus one
partially-truncated tile row at the seam. The line sits wherever the budget ran out, so it
moves as your camera moves.

Two limits matter, and they are not the same number:

- **The allocated capacity**, which the renderer sizes and resizes itself. It starts at
  roughly 50 records per visible splat, shrinks toward the measured demand when the scene
  needs less, and grows after a drop. On the default path it learns the demand from a
  readback of an earlier frame, so it is always a little behind.
- **The configured cap**, `rendering/gaussian_splatting/gpu_sorting/max_overlap_records`
  (default **100,000,000**). The allocated capacity never grows past it. In Project
  Settings this reads `0`, which means "use the `gpu_sorting/gpu_preset` budget", and the
  default `high` preset's budget is 100,000,000. Any positive value overrides the preset.

**At defaults you can see a brief band, not a lasting one.** A sudden jump in demand, such
as the camera moving quickly into a dense close-up, can outrun the allocated capacity. The
band then shows for a frame or a few and goes away as the capacity catches up. This case
follows from the code; it has **not** been captured on hardware, and how many frames it
lasts is not measured. A **lasting** band needs demand above the configured cap, or a
capacity grow that keeps failing while VRAM is short (the third case below). The
measurements below produced it only by forcing that setting 200–1000× below its default.

**Affects:** any scene whose overlap-record demand in one frame outruns the capacity
allocated for it — in practice, dense content seen close up. Measured on an RTX 3090
(Vulkan).

**Accepted** as a public-alpha limitation on 2026-09-25
([disposition](https://github.com/klausi3D/godotGS/issues/54#issuecomment-5838456717)), with
three risks put to the maintainer: the brief band at defaults is derived from the code and
has not been captured, the headroom at defaults is an extrapolation, and automatic import
applies no splat limit. **The acceptance lapses** if a default-configured, in-envelope real
scene is measured above the cap; #54 then blocks the alpha again. The disposition record and
the mechanism live in the bar's [§8.1](../governance/release-acceptance-bar.md); this entry is
the user-facing half. #54 stays open as the tracking issue for the engineering fix.

**Evidence: reproduced on hardware 2026-09-22** — RTX 3090, Vulkan, `dev_build=yes` editor
binary at `6f4552076c7`. 100,000 splats filling a 512×512 viewport at `tile_size = 16`
demand **708,814 records**, 7.09 per splat. With `max_overlap_records` forced down:

| `max_overlap_records` | Last lit scanline (of 512) | Empty tiles (of 1024) |
| --- | --- | --- |
| 100,000 | 95 | 837 |
| 250,000 | 191 | 651 |
| 500,000 | 351 | 340 |
| 1,000,000 | 511 — nothing dropped | 0 |
| 100,000,000 (default) | 511 — nothing dropped | 0 |

**What this does *not* do, also measured.** The part of the image above the line is
**pixel-identical** to a clean render of the same scene (0 differing pixels in all three
overflowing cases), the line does **not** flicker — it sat on the same scanline across four
consecutive frames of a static camera — and nothing crashes. Splats are not
mis-sorted or mis-coloured; the ones that survive are exactly right, and the rest are
simply absent.

**You are told when it happens.** The log carries a one-shot warning
(`Overlap-record overflow: the tile-binning pass dropped overlap records …`) whenever any
record is dropped; use it to tell whether you are hitting this at all. The profiler monitor
`gaussian_splatting/overflow_tile_count` counts only tiles that were **emptied completely**.
A tile cut off partway, such as the seam row, loses splats without being counted, so a
small overflow confined to the last tiles can leave the monitor at 0. Both signals fired
in the run above.

**Is it transient or permanent?** That depends on which side of the *configured cap* your
demand is on. When the renderer sees a drop it grows its capacity toward 1.5×
the demand it measured — but it will not grow past `max_overlap_records`, whatever you have set
that to. So:

- **Demand below your `max_overlap_records`, capacity merely behind it** — a spike, and it
  fixes itself as the capacity catches up. This is the case you can meet at defaults.
- **Demand above your `max_overlap_records`** — **permanent**. The renderer cannot grow past
  the setting, and the band stays. Every measurement in the table above is this case: the frame
  was still truncated on the 24th frame, on exactly the same scanline as the first.
- **The renderer cannot allocate a bigger buffer** (VRAM is short) — **lasting while memory is
  short**, even below your `max_overlap_records`. It keeps the old capacity and retries, and
  the log says so (`Global composite sort grow to … could not build its replacement`). Raising
  `max_overlap_records` does not help here and asks for more memory; free VRAM or reduce
  density instead.

At the 100,000,000 default the second case needs a frame demanding more than 100 million
records. The 100,000-splat scene above demands 708,814.

**Workaround:** reduce splat density, or move the camera back, so fewer splats cover each
tile. If you lowered `max_overlap_records`, raise it back — do not set it below your
scene's demand. Raising it above the default costs VRAM as demand grows: each record needs
24 bytes with the default 64-bit keys (key and value, plus an equal-sized radix-sort copy),
so a scene that actually uses 100M records holds about **2.4 GB** in these buffers alone.
Prefer reducing density.

**Not measured:** whether a real scene at default settings can exceed 100,000,000 records.
Scaling the numbers above to 1080p gives about 56 records per splat, so the cap is reached
at roughly **1.8 million visible splats**.

Whether a splat limit keeps you below that depends on how the asset was imported:

- **Imported automatically** (a `.ply` or `.spz` dropped into the project and imported with
  Godot's default options): this uses the Ultra import preset, which sets no splat-count cap
  and full density. The renderer treats such an asset as full-fidelity and budgets for its whole splat
  count, not the node's `max_splat_count`. On this route no splat limit applies, so a scene
  with more than about 1.8 million splats in view can reach the cap on either node.
- **Imported through the module's import dialog** with its preselected settings: this uses
  the Desktop preset, which keeps at most 750,000 splats at 0.7 density. The node's
  `max_splat_count` then also applies. It is 500,000 on a default `GaussianSplatNode3D`,
  because its Balanced quality preset sets that when the node is created. That comes to about
  28 million records, or 3.6× under the cap. It is 1,000,000 on `GaussianSplatWorld3D` or on
  the node's Quality preset, so the asset's 750,000 is the binding limit. That comes to about
  42 million records, or 2.4×.

Any other import preset or node quality setting changes these figures. The condition that
matters is the one above: more than about 1.8 million splats in view at 1080p.

Whether a real scene at 1080p has that many splats in view at once has not been measured.
All of this is arithmetic on a synthetic grid, not a capture of real content. If you hit the
warning above at default settings, that is worth reporting on #54 as a data point. It does
not by itself end this acceptance: at defaults the warning also fires on the brief band
accepted above and on a failed grow while VRAM is short. What would end it is a
default-configured, in-envelope real scene **measured with demand above the cap** — a band
that persists at the default `max_overlap_records`, rather than one that clears as the
capacity catches up, with no failed-grow warning (`… could not build its replacement`) in
the log.

### Far views of dense content can show 16 px black tiles ([#1137](https://github.com/klausi3D/godotGS/issues/1137))

**Status: Active — shipping by maintainer decision** (2026-10-02, recorded on
[#1138](https://github.com/klausi3D/godotGS/pull/1138)) until hierarchical LOD
([#1136](https://github.com/klausi3D/godotGS/issues/1136)) lands. It is not yet a
ledger-accepted limitation: there is no acceptance-bar §8.1 row and no
`public_alpha_issue_ledger` entry, so a release candidate cannot cite it.

The rasterizer draws at most a fixed number of overlap records per 16 px tile. Records are
sorted near to far, so in a tile with more records than that, **the farthest ones are not
drawn**. When an object is far enough away to collapse into a few tiles, those tiles can
exceed the limit, and what was behind the front-most records shows as a **tile-aligned
black (background) rectangle**. The limit is
`rendering/gaussian_splatting/gpu_sorting/max_raster_splats_per_tile`; in Project Settings
it reads `0`, which means "use the `gpu_sorting/gpu_preset` limit". The default `high`
preset's limit is **12,288** (low 4,096, medium 8,192, ultra 16,384).

**Affects:** dense real scans viewed from a distance at the default settings. Measured on an
RTX 3090 (Vulkan) at 960×540 on four scans from GrandmasHouse: up to 2,295 missing pixels
per view, starting between about 6 m and 23 m depending on the scan, and in one large scan
already at its fitted view (14 m). At
1920×1080 the same object covers more tiles, so the holes start further away (holzbank:
from about 47 m instead of 23 m).

**How you know:** the first time it happens, a renderer logs
`Per-tile raster cap reached: …` once. `get_overflow_stats()` reports
`raster_tile_cap_drop_events` (intervals with at least one truncated tile) and
`sampled_dropped_records` / `sampled_dropped_tiles` for the most recently sampled frame.

**Workaround:** set `max_raster_splats_per_tile` to a positive value, up to `65536`. A
positive value overrides any preset. This costs **frame time, not memory**: one workgroup
walks all records of its tile, so the densest tile sets the length of the raster pass. At
`65536` the measured scans lost nothing, at **+33% to +75% GPU time** in the affected views
(+2.3 to +10.9 ms on the 3090); `32768` cost up to +60% and still left holes in two views
(lamp at 19 m, weg-gartenanlage at 14 m). Near views are unaffected either way. The measurements and images are on
[#1138](https://github.com/klausi3D/godotGS/pull/1138).

### Splats cast no shadows ([#1089](https://github.com/klausi3D/godotGS/issues/1089), [#1095](https://github.com/klausi3D/godotGS/issues/1095))

A `GaussianSplatNode3D` casts no shadow, from any light type (directional, spot, omni),
onto meshes, onto other splats or onto itself. `rendering/cast_shadow` has no visible
effect until the caster below lands. Splats still **receive** shadows: a mesh between a
directional light and the splats darkens them.

**Why.** The splat shadow pass never rasterized a single splat from the light's view. It
then copied whatever depth image the splat rasterizer still held, which was the main
camera's, into every shadow-map region it was given. That produced shadows that were wrong
everywhere: they followed the camera instead of the light, every directional cascade held
the same footprint, a hidden node kept casting, and the starter template rendered its cloud
almost black. Since #1095 slice 1 the pass is fail-closed: it writes nothing unless it
rasterized splats from the light's view into a target of the shadow region's size, and
today it never does. A dedicated depth-only splat caster is designed in #1095 and lands in
slices: directional first, then spot, then omni.

**Workaround:** for a shadow in the shape of a splat object, add a mesh proxy with
`cast_shadow = Shadows Only` (for example a `SphereMesh` or a low-poly hull of the scan).

**Evidence:** the starter template, 1280×720, Vulkan Forward+, RTX 3090, dev builds of the
#1095 slice 1 branch and its base `eff00db450c`. Mean splat luma at the shipped camera went
from 0.31 to 0.88, equal to the same frame with the light's shadows off (0.88); the
splat-caused floor shadow went from 23,248 px to 0, a hidden node's from 26,566 px to 0, and
the directional shadow atlas no longer changes when the node's shadow casting is toggled
(33,507 px to 0). A mesh occluder still darkens the splats (0.88 to 0.85). The GPU case
`Shadow pass leaves the atlas untouched when no splat is in the light's view` fails on the
base and passes with the guard. Not verified: other GPU vendors, real scans, and whether
splats receive spot and omni shadows correctly.

### Transparent viewports are opaque under TAA or FSR2 ([#989](https://github.com/klausi3D/godotGS/issues/989))

**Status: Engine limitation.**

A viewport with `transparent_bg` renders fully opaque when TAA or FSR2 is enabled. Alpha
is hardcoded to 1.0 in Godot's `taa_resolve.glsl` and in the FSR2 callbacks; a mesh-only
control shows identical loss with no splats present, so this is not splat-specific and
cannot be fixed from this module.

**Workaround:** do not combine `transparent_bg` with TAA or FSR2.

> **A warning for whoever fixes this.** The destination alpha #989 is about is **not** just
> an alpha channel — it is also, by accident, FSR2's reactive mask. Godot has no dedicated
> reactive texture: `params.reactive` is an alpha-swizzled view of the internal colour
> buffer, and the composite writes splat coverage into that alpha. That coupling is why
> splats do **not** ghost under FSR2 (see #1025). **Changing this channel changes FSR2's
> temporal behaviour silently**, with no test that would notice. Measure FSR2 ghosting
> before and after any #989 fix.

### Shadow-casting splats darken themselves, and the starter template renders dark ([#1089](https://github.com/klausi3D/godotGS/issues/1089))

**Status: Active — not accepted, pending a maintainer decision.** Nobody has accepted this
as a public-alpha limitation, and it is not in the acceptance bar's §11 list either. Whether
it blocks the alpha is undecided. It is disclosed here because it is live on master and the
default template shows it.

When a `GaussianSplatNode3D` has `rendering/cast_shadow` on (it is off by default) and a
`DirectionalLight3D` has shadows on, the splats do not cast a real shadow. The splat shadow
pass, `GaussianSplatRenderer::render_shadow_depth_map`, runs `render_sorted_splats` for the
light view. That call gives the raster stage a fresh `StageMetrics`, and
`RasterCompositeStage::execute` reads the sorted-splat count from those metrics in preference
to the frame snapshot, so the shadow raster sees zero splats and draws nothing. The shadow
pass then blits whatever depth texture the rasterizer holds into the shadow atlas, with no
check that this pass rendered it. That texture is the **main camera's** splat depth, so
every cascade receives a stretched copy of the camera view.

What you see: splats shadow themselves and go dark, a detached dark shape can appear on the
ground, and the effect changes as the camera moves, because it follows the camera rather
than the light. A node that is hidden can keep casting the last frame's shadow.

The shipped starter template (`templates/gaussian_splat_template`, `scenes/main.tscn`) hits
this out of the box. Its `GaussianSplatNode3D` sets `rendering/cast_shadow = true` and its
`DirectionalLight3D` sets `shadow_enabled = true`. Measured on #1089 (RTX 3090, 1280×720),
before #1093: from the camera position the template settles on, the white splat cloud
rendered at a mean luma of 0.06–0.10 instead of about 0.8. #1093 then made the template's
`[rendering]` settings take effect, including its default environment, whose ambient light
lifts the cloud. Measured on #1093 in the same framing, the cloud is at **0.31**, against
**0.88** with the light's shadows off. It is still dark.

**Workaround:** turn off `rendering/cast_shadow` on the splat node, or shadows on the light.
Either restores full brightness. Splats then cast no shadow, which is also what they
effectively do today.

### Painterly's GPU path is covered by one nightly test that does not gate anything ([#997](https://github.com/klausi3D/godotGS/issues/997))

**Status: Mitigated.** The original defect was fixed by #1028 on 2026-09-20, and #997
closed on 2026-10-01 (see [Recently resolved](#recently-resolved)). This entry covers a
remainder that no open issue tracks as a whole; #1034 tracks the CPU-only
`painterly_scenes` harness listed below.

Painterly needs two things, and `painterly/enabled` is only one of them: a valid
`PainterlyMaterial` must also reach the renderer, or the raster stage reports
`RenderFallbackReason::PAINTERLY_MATERIAL_UNAVAILABLE` and renders the baseline pipeline
instead. `GaussianSplatNode3D` binds `painterly/material`, so you can assign the material
in the inspector or in a scene file, and the node pushes it to the renderer.

Exactly one test renders the painterly GPU path and can fail when painterly does not run:
the `Painterly Material Render` runtime scenario (`tests/runtime/test_painterly_material_render.gd`).
It is in the `release-ci` runtime profile. The only workflow that runs that profile is the
nightly **Release-CI Runtime Evidence** lane (`release_ci_runtime.yml`), which is not a PR
gate. A change that breaks painterly can therefore merge green and only show up in the
next nightly. The rest of the painterly coverage does not touch the GPU path:

- The C++ `[GaussianSplatting][Painterly]` suite (`test_painterly_pipeline.h`) compiles the
  painterly shader permutations, but it draws its frames with a CPU software rasterizer
  (`render_scene_headless`).
- `test_painterly_material.cpp` checks the material's validation, serialization and shader
  compilation. It renders no frame.
- The `modules/gaussian_splatting/tests/painterly_scenes/` harness builds its images on the
  CPU (`painterly_scene_util.gd`) and references no `GaussianSplatNode3D`.
- The GDScript painterly tests are held by `tests/ci/check_painterly_test_non_vacuity.py`
  to read `stage_raster_painterly`, so they can no longer pass on a baseline frame. That
  guard checks what a test reads, not that the test ran on a GPU.

Painterly also ignores sphere effectors and per-splat depth clipping. See the next entry.

**Workaround:** treat painterly as experimental. Confirm you are actually on the painterly
path before drawing conclusions: `get_renderer().get_render_stats()["stage_raster_painterly"]`
is `true` on a painterly frame. The flag survives render-cache reuse, so it can be stale
after you change settings. Check it against the image as well. Validate visually in your own
scene.

### Painterly ignores sphere effectors and per-splat depth clipping ([#1079](https://github.com/klausi3D/godotGS/issues/1079))

**Status: Active.**

The painterly path fills its render parameters in `PainterlyRenderer::populate_painterly_gbuffer`.
The baseline path fills its own in `RenderPipelineStages::RasterStage::render_tile_fallback`.
The painterly producer sets only a subset of the fields, and the rest keep their struct
defaults, so controls that work on the baseline path do nothing under painterly. This is a
class of defect, not a single bug. The instances below were verified in code at
`eff00db450c`, and they are the ones found so far, not a complete list.

- **`SphereEffector3D` does nothing under painterly** (#1079, item 1). The baseline fills the
  `sphere_effector_*` fields from scene effectors and project settings. The painterly
  producer assigns none of them.
- **Per-splat depth clipping is off under painterly.** The baseline fills the
  `scene_depth_*` fields and enables `scene_depth_clip_enabled` when
  `composite/per_splat_depth_clip` asks for it. The painterly producer does not, and the
  baseline's code comment lists painterly among the paths that keep the defaults. Occlusion
  against meshes then relies only on the painterly composite's depth test. No open issue
  tracks this part.

Wind and lighting used to be on this list. Wind was fixed by #1033 and lighting by #1078
(see [Recently resolved](#recently-resolved)). Each added one shared writer that both
producers call: `apply_wind_to_render_params` and `apply_lighting_to_render_params`. The
guard `tests/ci/check_render_param_family_producers.py` fails if a producer stops calling
the wind writer. It does not cover the lighting writer yet; #1079 (item 3) tracks that.

**Workaround:** none. Do not rely on effectors or per-splat depth clipping while painterly is
active.

### The painterly composite's `blend_strength` is a no-op ([#1001](https://github.com/klausi3D/godotGS/issues/1001))

**Status: Active.**

`PainterlyRenderer::composite_painterly_output` assigns `push_constant.blend_strength` the
literal `1.0f`, with nothing feeding it. The shader does consume it
(`params.blend_strength` in `shaders/painterly_composite.glsl`), so the multiply happens, but
it multiplies by one. There is no user-facing control to set.

This is unrelated to `PainterlyMaterial.palette_blend_strength`, which is a different
quantity and does work.

**Workaround:** none needed; there is nothing to set. Do not expect a blend control.

## Scripting API

### `get_statistics()` can crash the engine when polled every frame ([#1030](https://github.com/klausi3D/godotGS/issues/1030))

**Status: Active.**

`GaussianSplatNode3D.get_statistics()` is ClassDB-bound and is the natural call for a
per-node HUD. Called **once per rendered frame** from GDScript it intermittently takes the
process down with a `CrashHandlerException` inside the call. It builds a large `Dictionary`
that includes everything `GaussianSplatRenderer::get_render_stats()` returns, read from live
metrics structures that the render thread is mutating concurrently under
`rendering/driver/threads/thread_model=2` (a separate render thread), which the starter
template ships. Godot's own default is `1`.

**Workaround:** poll it at **4 Hz or slower** — roughly every 15th frame, which is the rate
the starter template's performance overlay refreshes at (`update_interval = 0.25`). A
444-frame run at that rate did not crash. The starter template itself does not keep to that
rate everywhere. Its `_focus_camera()` calls `get_statistics()` once per frame until the
splat bounds arrive, for at most 5 seconds after the scene starts.

**Evidence:** **reproduced, not diagnosed** — 2 crashes in 5 runs on an optimized editor
build; the C++ backtrace was unsymbolized, so the faulting function is not named and no
root cause is established. 2-of-5 is a rate, not a mechanism.

**And the rate itself is suspect.** Those runs were taken on 2026-09-17, inside a window
when a stray `godot.head.exe` left over from 2026-09-16 was holding this machine's GPU
continuously; it was not killed until 2026-09-19. A crash the issue attributes to
main-thread/render-thread contention is exactly the kind of defect a third process
competing for the GPU would make *more* likely, so **2-of-5 is plausibly an overestimate**
of what a user on a quiet machine would hit.

The same premise cuts the *other* way for the workaround: a 444-frame run that stayed clean
**while the hazard was elevated** is **stronger** evidence that 4 Hz is safe, not weaker.
(An earlier revision of this entry said "correspondingly weaker", which was simply the wrong
direction — both figures move the same way from the same premise.)

**The defect is real either way, and both numbers still need retaking on a quiet machine** —
an overestimate is not a measurement, and one clean run is not a safety proof.

### Color grading cannot be baked on a node that renders a `splat_asset` ([#1105](https://github.com/klausi3D/godotGS/issues/1105))

`GaussianSplatNode3D.bake_color_grading()`, `bake_color_grading_snapshot()` and
`restore_color_grading()` work only on data supplied through `set_splat_data()`. On a node
that renders a `splat_asset` (the normal import path) they return `ERR_UNAVAILABLE`, print
why, and change nothing. The inspector shows the Bake section only for a node that holds
its own bakeable data.

**Impact:** none on the rendered result. The live `rendering/color_grading` grade already
applies to an asset-backed node, per instance on the GPU, so keep the resource enabled.
What you lose is only a bake's saving of about 20 ALU operations per splat
(`shaders/includes/color_grading_binning.glsl:8`).

**Why it is unsupported, not fixed:** the asset can be shared by several nodes and is
sealed once handed to the runtime, so it must not be rewritten. A node-owned copy would
duplicate the payload, take the node off the shared path, and would not survive a scene
reload. The bake math also does not yet match the live grade
([#1124](https://github.com/klausi3D/godotGS/issues/1124), suspected).

**Status:** **reproduced** on an RTX 3090 (Vulkan) before the change. On the same binary,
the asset-backed node's bake returned `ERR_UNCONFIGURED` ("no gaussian data loaded") and a
`set_splat_data()` node's bake returned `OK`. The unsupported-path error is now explicit
and covered by tests.

## Separate render thread (`thread_model=2`)

Only the starter template ships `rendering/driver/threads/thread_model=2`. The repository's
test project uses `1`. One CI case runs with a separate render thread: the
`Render-Thread Dispatch Characterization` case in `tests/ci/run_baseline_qa.py`
(`render_thread: "separate"`, in the self-hosted `baseline_qa.yml` lane). It drives
`GaussianSplatRenderer` test hooks directly and creates no `GaussianSplatNode3D`, so no CI
lane renders a splat node or the starter template in this mode. That case is judged by its
verdict markers, not its exit code, because its own comment records that shutdown in this
mode is unstable.

The per-frame render-thread syncs that #1092 reported are fixed by #1094 (see
[Recently resolved](#recently-resolved)).

### The starter template prints a render-thread error at shutdown ([#1077](https://github.com/klausi3D/godotGS/issues/1077))

**Status: public-alpha blocker** (maintainer decision 2026-10-03,
[record](https://github.com/klausi3D/godotGS/issues/1077#issuecomment-5972710739); acceptance
bar §11 item 11). It is listed here only so its symptom can be found, and is not an accepted
limitation. The shutdown **crash** is fixed by
[#1133](https://github.com/klausi3D/godotGS/pull/1133) (merged 2026-10-02). One error line
remains.

**What was fixed.** Before #1133, every windowed run of the starter template ended
abnormally **after** the scene had quit, under its `thread_model=2`: 6 of 6 runs on
2026-09-27, and 3 of 3 on 2026-10-03 on a build from before #1133. The runs logged
`This function (free) can only be called from the render thread` from
`RenderingDevice::free`, and sometimes a `SafeRefCount` misuse error. The cause was a
dispatcher argument-order bug that let the renderer's teardown run off the render thread.
The [independent review of #1133](https://github.com/klausi3D/godotGS/pull/1133#pullrequestreview-5389066529)
measured the base crashing (`0xC0000409`) in 3 of 3 runs and #1133 exiting 0 in 3 of 3,
with the `free` / `SafeRefCount` errors gone.

**What remains.** Every fixed run prints one
`This function (finalize) can only be called from the render thread` at quit and still exits
0. The review attributes it, by inference and not by trace, to the manager destroying its
local rendering devices on the main thread once the render loop has stopped. The
maintainer has decided that this error blocks the public alpha until it is fixed.

**Evidence:** the #1133 numbers are 3+3 windowed runs on one NVIDIA GPU. Release or
exported builds, a `GaussianSplatWorld3D` scene and `thread_model=1` have not been run
against the residual.

**Workaround:** none needed for the exit code. The remaining line is printed only at quit.
A non-zero exit at shutdown on a build that includes #1133 is not explained by this entry
and should be treated as a new failure.

## GaussianSplatWorld3D

### With a `GaussianSplatWorld3D` and a `GaussianSplatNode3D` in one scene, one of them renders nothing ([#788](https://github.com/klausi3D/godotGS/issues/788))

**Status: Active.**

The renderer picks one route per frame for the whole scene. Unless `route_policy` forces the
resident route, the route follows a single residency hint, chosen by
`GaussianSplatSceneDirector::get_submission_residency_hint_for_renderer`:

- If the world submission is active, has a renderable payload and carries a hint, the
  **world's** hint wins (source `world_submission`). The world node derives it from
  `route_policy`. In this case the `GaussianSplatNode3D` content can be the part that is not
  drawn. #788 reports this direction too: with `route_policy` set to streaming, the node's
  capture showed the world's splats instead of its own.
- Otherwise the hint comes from the instance records, and a `GaussianSplatNode3D` always
  publishes `SUBMISSION_RESIDENCY_HINT_RESIDENT` (source `instance_submission`). The frame
  goes resident, and if the resident contract then has no instances to publish, the frame is
  skipped with `resident_no_instances`. This is the configuration measured on #788: the
  **world** rendered nothing, with a one-shot warning
  `Resident route rejected (reason=submission_hint_resident:instance_submission_not_feasible:resident_no_instances) ... frame skipped to preserve single-route-per-frame contract`.

**Which of the two disappears therefore depends on the world submission's state when the
route is decided**, and that state is not visible to you. The accepted ADR
[single-route-per-frame node coexistence](../architecture/adr-single-route-per-frame-node-coexistence.md)
(§3.2) records this precedence and does not establish why the world submission failed its
gate in the measured case. Hiding either node does not help: the instance-hint scan ignores
`visible`, and the route is chosen per frame for the whole renderer. No configuration warning
is shown on this build.

**Workaround:** run content of only one of the two node types in a scene at a time.

### Any payload change costs a full resubmit — about 2.1 s at 1M splats ([#1008](https://github.com/klausi3D/godotGS/issues/1008))

**Status: Active.**

Changing `gaussian_data`, `bounds`, `metadata`, `lod_bias`, `max_render_distance` or
`max_splat_count` all converge on `_register_shared_renderer()`, which rebuilds the entire
submission from scratch. There is no dirty-field tracking and no equality early-out, so a
one-field nudge pays the whole cost: a blocking render-thread round trip, then
`clear_gaussian_data()` followed by a full GPU re-upload of every splat buffer.

**Two bounds on that.** The re-upload is the **resident** branch — a world whose payload is
file-backed and has no resident data takes `set_file_backed_payload_source()` instead
(`GaussianSplatRenderer::apply_world_submission_contract`), which does not re-upload the splat
buffers the same way. And a world that holds no registered submission resubmits nothing.
The cost below is the resident path, which is the one the alpha's world route uses.

Medians reported on the issue, on an RTX 3090 with an optimized build: 17 ms at 10k splats,
259 ms at 100k, **2124 ms at 1M**. Those figures are from #1008 and have not been
re-measured here; the code path is verified. It is not a per-frame cost — it is paid when
the payload changes.

**Treat the timings as an upper bound, not a measurement.** They were taken on 2026-09-17,
inside a window when a stray `godot.head.exe` from 2026-09-16 was holding this machine's
GPU continuously until it was killed on 2026-09-19. Another PR's figures taken in the same
window moved by roughly 10% when retaken on the quiet machine. The *shape* of the finding
does not depend on the numbers — a full rebuild with no dirty-field tracking and an
unconditional re-upload is a code fact, and the ~2 s order of magnitude at 1M is not
plausibly contention alone — but the three medians should be retaken before anyone quotes
them as a baseline.

**Workaround:** change world content at load boundaries, not during gameplay.

### An emptied world does not reach the renderer ([#1002](https://github.com/klausi3D/godotGS/issues/1002))

**Status: Active.**

`GaussianSplatWorld::clear()` is the one payload mutator that emits no `changed` signal.
Every other one does. The director holds its own copy of the payload rather than the
resource, so with no signal nothing re-registers and the previous content stays on screen.
`clear()` is script-bound and has no in-tree C++ callers, so this is reachable only from
user code.

A second, related path: `get_metadata()` returns the resource's `Dictionary` by reference,
so mutating it from script changes the resource without emitting `changed` either.

**Workaround:** assign an empty `GaussianData` instead of calling `clear()`.

### Bounds are never re-derived after they are once set ([#1003](https://github.com/klausi3D/godotGS/issues/1003))

**Status: Active.**

Bounds are derived from the payload only while they are empty
(`GaussianSplatWorld::set_gaussian_data` and `set_chunk_payload_source`). Once `bounds` has volume —
however it got there — a later payload assignment never re-derives it, and the stale value
becomes the culling AABB. Content relocated with its payload can be culled at the wrong
place.

**Workaround:** call `set_bounds()` explicitly after relocating a payload.

### `strict_identity_transform` is bypassed on every resubmit ([#1006](https://github.com/klausi3D/godotGS/issues/1006))

**Status: Active.**

The transform check runs on every *apply* path and on no *resubmit* path — the resubmit
helper, `_resubmit_world_submission_if_registered()`, calls `_register_shared_renderer()` directly, which contains no check. So a world
node moved after a successful apply is republished unchecked by any of the parameter
setters. The node also never tracks transforms, deliberately.

**Workaround:** do not move a `GaussianSplatWorld3D` after applying it while
`strict_identity_transform` is on; the setting will not catch you.

### Applying a world writes `world_path` into your resource ([#1007](https://github.com/klausi3D/godotGS/issues/1007))

**Status: Active.**

Every apply of a world that has a resource path injects a `world_path` key into the
resource's own metadata dictionary — and because metadata is returned by reference, the
director's record aliases the same dictionary. The write bypasses `set_metadata()`, so no
`changed` signal fires and the injection is invisible. Metadata is serialized, so a
`.gsplatworld` saved after an apply persists the key.

In-memory worlds with no resource path are not affected.

**Workaround:** none. Do not treat a world's metadata dictionary as exclusively yours.

### A resource replaced mid-load can stay connected ([#1005](https://github.com/klausi3D/godotGS/issues/1005))

**Status: Engine limitation.** The mechanism is in upstream Godot's `Resource`; the module
triggers it by connecting to a world resource that may still be loading.

`Resource::connect_changed` routes through the loader when called off the main thread
during a load, queueing the connection rather than making it; a matching
`disconnect_changed` on the main thread then finds nothing connected and is a silent no-op,
because it never reaches that queue. The queued connection is honoured afterwards anyway.
A world resource swapped inside that window can leave the old resource connected and
resubmitting.

**Evidence:** confirmed at code level; the reachable window is narrow and no end-to-end
reproduction was constructed.

**Workaround:** avoid replacing a world resource while it is still loading.

## Sorting

### A failed sorter grow can lose both sorters ([#983](https://github.com/klausi3D/godotGS/issues/983))

**Status: Active.**

A tile-sorter grow in `TileGlobalSortResources::ensure_resources()` retires the old sorter
before the enlarged buffers are allocated. If
that allocation fails, both are lost, and the reduced-capacity fallback then churns every
frame.

**Evidence:** a code-reading finding, **not reproduced on NVIDIA hardware** — unproven, not
absent. It is strictly narrower than what #982 fixed.

**Workaround:** reduce splat count rather than raising the overlap-record cap.

## Platforms and packaging

### Linux is smoke-tested, not editor-tested

**Status: Active.**

The Linux CI lane (the `cpu-tests` job in `baseline_qa.yml`, which does not run on pull
requests) runs the `ply`, `pipeline`, `runtime` and `module` categories headless
under xvfb on a GPU-less hosted runner. It runs neither `qa` nor `sorting`, so no QA-scene
and no GPU-sorting evidence exists for Linux at all; every GPU-backed lane in this project
is Windows. See the [compatibility matrix](../reference/compatibility-matrix.md).

**Workaround:** none. Treat Linux as an evaluation platform.

### macOS is build-supported and unvalidated

**Status: Active.**

The build accepts macOS and no lane exercises it. There is no macOS CI, no published macOS
binary, and no evidence of any kind.

**Workaround:** build from source and validate it yourself.

### Nightly Linux editors are unoptimized

**Status: Active.**

Nightly Linux builds are `dev_build=yes` (`-O0`), which inflates CPU-side frame cost by
roughly an order of magnitude; the `.dev` segment in the filename is that flag. This is a
**nightly** property — a tagged release builds Linux without it.

**Workaround:** use nightlies to see GodotGS work, not to judge how fast it is. Build with
`target=editor optimize=speed_trace` for representative numbers.

### No Linux export template is attached to releases ([#1015](https://github.com/klausi3D/godotGS/issues/1015))

**Status: Active.**

The Linux export template is built and uploaded as a CI artifact
(`build_linux_export_template` in `release_builds.yml`), but `publish_release` does not
attach it. The only template attached to a release is the Windows one, and only when the
Windows build of that run succeeded. Exporting a Gaussian-Splatting
game on Linux therefore requires building the template yourself. See
[export templates](export-templates.md).

**Workaround:** build the Linux template from source.

### Nothing is code-signed ([#1015](https://github.com/klausi3D/godotGS/issues/1015))

**Status: Active.**

No published binary is signed, on any platform — there is no signing step anywhere in the
release workflows. Windows SmartScreen will warn on the editor and on games exported with
the GodotGS template.

**Workaround:** verify downloads against the published `.sha256` sidecars.

---

## Proposed, not yet accepted

**Everything in this section is still a blocker.** These are defects someone has proposed
shipping with, where no named human has accepted them yet. They are drafted here so the
user-facing disclosure and the acceptance-bar disposition are written together and cannot
drift apart. **Nothing here may be cited as an `accepted_alpha_limitation`** — a candidate
bundle that does so is wrong twice over, because the acceptance does not exist and because
the gate additionally requires a `public_alpha_issue_ledger` entry that does not exist
either. The machinery behind both conditions — and the reason a classification means less
than it looks — is in the acceptance bar's
[§9.1](../governance/release-acceptance-bar.md); it is deliberately not restated here.

---

**No proposal is outstanding.** The section is empty, and it is kept
even when empty because the mechanism is the point: it is where a disclosure is drafted
while its disposition is still open, and an empty section is a statement that no such draft
is outstanding — not an invitation to skip the step.

A previous entry here was **#1025** (splats trailing under TAA in motion). A named human
accepted it on
[2026-09-20](https://github.com/klausi3D/godotGS/issues/1025#issuecomment-5752837553), which
met the acceptance bar's §10.1 condition 4, so it moved up into
[Rendering](#rendering) as a real limitation. An earlier revision of that entry said splats
ghost under FSR2 too and told you to avoid it; that was a prediction, it was measured, and it
was wrong in the direction that costs you a working feature — which is the reason this
section exists at all.

The most recent entry here was **#54** (the bottom of the frame going empty when a frame needs
more overlap records than are allocated). A named human accepted it on
[2026-09-25](https://github.com/klausi3D/godotGS/issues/54#issuecomment-5838456717), so it
moved up into [Rendering](#rendering) as a real limitation, with the condition under which
that acceptance lapses stated in the entry.

---

## Recently resolved

These defects were listed or named on this page and have since been fixed on master. If you
read an earlier version of this page, the claims below are no longer true. Each fix was
re-checked in code at `eff00db450c`.

| Issue | What this page used to say | Fixed by | Date | Now |
| --- | --- | --- | --- | --- |
| [#997](https://github.com/klausi3D/godotGS/issues/997) | The painterly material cannot be assigned from a scene, because `GaussianSplatNode3D` does not bind `painterly/material`. The demo scenes' assignment is discarded at load, and no in-repo test exercises the painterly GPU path. | [#1028](https://github.com/klausi3D/godotGS/pull/1028) | 2026-09-20 | `GaussianSplatNode3D` binds `painterly/material` and pushes it to the renderer. The `Painterly Material Render` runtime scenario fails when painterly does not run. What is still open is listed under Rendering as **Mitigated**. Issue closed 2026-10-01. |
| [#1018](https://github.com/klausi3D/godotGS/issues/1018) | Per-node wind freezes mid-sway under painterly, because `wind_time_seconds` is never advanced. | [#1033](https://github.com/klausi3D/godotGS/pull/1033) | 2026-09-20 | `PainterlyRenderer::populate_painterly_gbuffer` calls `apply_wind_to_render_params`, and `tests/ci/check_render_param_family_producers.py` fails if a producer stops doing so. Issue closed 2026-10-01. |
| [#833](https://github.com/klausi3D/godotGS/issues/833) | Named in the note above as a §11 alpha blocker: the starter template's performance overlay never updates. | [#1027](https://github.com/klausi3D/godotGS/pull/1027), [#1031](https://github.com/klausi3D/godotGS/pull/1031), [#1032](https://github.com/klausi3D/godotGS/pull/1032) | merged 2026-09-20; issue closed 2026-09-27 | Issue closed. Struck from the bar's §11 list (item 9) on 2026-09-30. |
| [#851](https://github.com/klausi3D/godotGS/issues/851) | Painterly ignores scene lighting: it assigns none of the lighting, shadow or light-cluster fields and leaves `direct_lighting_mode` at `0`, so lights, shadows and the `lighting/*` project settings have no effect on a painterly frame. Named as a §11 alpha blocker. | [#1078](https://github.com/klausi3D/godotGS/pull/1078) | 2026-10-01 | `PainterlyRenderer::populate_painterly_gbuffer` calls `apply_lighting_to_render_params`, the writer the baseline uses, which sets `direct_lighting_mode = 1`. Issue closed 2026-10-01. No PR-gating guard covers the lighting writer yet ([#1079](https://github.com/klausi3D/godotGS/issues/1079), item 3). |
| [#1092](https://github.com/klausi3D/godotGS/issues/1092) | Every `GaussianSplatNode3D` re-queries the viewport's render target and texture every frame, which under `thread_model=2` forces two render-thread syncs per frame: the starter template ran at 43 FPS with 768 splats and 24 FPS with a 252k-splat scan. The workaround was `thread_model=1`. | [#1094](https://github.com/klausi3D/godotGS/pull/1094) | 2026-10-01 | The node re-queries only when the viewport or its size changes. Measured on #1094 (optimized build, RTX 3090, 1280×720, vsync 60 Hz, `thread_model=2`): the template went from 44.1–46.8 to 59.1–59.8 FPS with no sync warnings. **The issue is still open**: the 252k-splat scan went from 24.3–25.4 to 29.3–31.9 FPS, still below vsync, and was not measured under `thread_model=1`, so whether `thread_model=2` still costs it anything is unknown. |
