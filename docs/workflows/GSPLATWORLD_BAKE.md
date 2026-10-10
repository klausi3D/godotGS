# Gaussian Splat World Bake Workflow

Use this workflow when you want to merge multiple Gaussian splat assets into one `GaussianSplatWorld` resource for faster runtime loading.

<figure markdown="1">
![Diagram of the bake workflow joining scene mode and input-list mode into one GaussianSplatWorld output](../assets/images/bake-world-workflow.svg){ .gs-diagram }
<figcaption>Scene mode preserves node transforms, while input-list mode does not. Both end in the same baked world resource.</figcaption>
</figure>

## Choose a Bake Mode

| Mode | Required input | Transform behavior | Output |
| --- | --- | --- | --- |
| Scene mode | `--scene=res://...` | Preserves each child node transform during merge | `.gsplatworld` file |
| Input-list mode | `--inputs=res://a.ply,res://b.spz` or `--input=...` | Uses identity transforms for each input | `.gsplatworld` file |

## Run the Bake

The bake is a headless GDScript tool, [`scripts/bake_gsplatworld.gd`](../../scripts/bake_gsplatworld.gd) (it `extends SceneTree`).
It is not an editor menu entry. Copy it into your project, then run it with a GodotGS editor binary:

```bash
# Input-list mode: merge assets with identity transforms
godot --headless --path <your_project> --script res://scripts/bake_gsplatworld.gd -- \
  --inputs=res://splats/a.ply,res://splats/b.spz --output=res://worlds/world.gsplatworld

# Scene mode: merge the GaussianSplatNode3D children of one GaussianSplatContainer
godot --headless --path <your_project> --script res://scripts/bake_gsplatworld.gd -- \
  --scene=res://scenes/world.tscn --output=res://worlds/world.gsplatworld --chunk_size=25.0
```

Everything after the standalone `--` goes to the script (`OS.get_cmdline_user_args()`); options use the `--name=value` form.
On success the script prints `Saved gsplatworld: <path> (splats=N, chunks=M)` and exits with code `0`.
Other exit codes: `1` missing `--output` or mode, `2` bake produced no world, `3` output does not end in `.gsplatworld` (checked after the bake), `4` `ResourceSaver.save()` failed.

`--inputs` accepts `.ply`, `.spz`, and `GaussianSplatAsset` resources (`.tres`/`.res`). Raw `.ply`/`.spz` inputs are loaded with `GaussianSplatAsset.load_from_file()`, not through the import pipeline, so import options such as `quality/max_splats` do not apply to them.

In scene mode the instantiated scene is **not** added to the scene tree, so `GaussianSplatContainer::_merge_children_internal` uses each node's local transform. Child transforms relative to the container are kept; a transform on the container or its ancestors is not applied.

## Required Options

| Option | When you need it | Notes |
| --- | --- | --- |
| `--output` | Always | Must end with `.gsplatworld` |
| `--scene` | Scene mode | Use this when the scene already contains the splat nodes |
| `--container` | Scene mode with multiple containers | Selects the container to bake |
| `--inputs` or `--input` | Input-list mode | Use this when you want to merge assets directly |
| `--chunk_size` | Optional | Adjusts chunking in the baked world |

## Practical Order

1. Choose scene mode if you need the existing scene transforms.
2. Choose input-list mode if you want a direct asset merge.
3. Set the output path first.
4. Run the bake once, then validate the baked world in a clean scene.

To check a baked world, add a `GaussianSplatWorld3D` node to a clean scene and assign the `.gsplatworld` file to Inspector › World. With Inspector › Auto Apply on Ready on (the default), the node applies the world when it becomes ready, in the editor as well as in the running game.

<figure markdown="1">
![The godotGS editor with a GaussianSplatWorld3D node selected in a scene named world_view. The 3D viewport shows a captured red-brick community hall with a white bell tower, seen from above, on a lawn with trees. The Inspector shows World set to knock.gsplatworld, Auto Apply on Ready on, and the Quality section with LOD Enabled on, LOD Bias 1.0 and Max Render Distance 1000 m.](../assets/images/screenshots/editor-world-knock.webp){ .gs-shot width="1280" height="720" loading="lazy" }
<figcaption markdown="span">A world baked in scene mode from one scan (1,935,275 splats, 43 chunks) and shown by a `GaussianSplatWorld3D` node. The node's own settings are the defaults except Max Splat Count, raised so that no splat is dropped. The faint shapes in the dark sky are large translucent splats, a known difference from reference renderers ([#1222](https://github.com/klausi3D/godotGS/issues/1222)). Scan: Knock Community Hall by scbenoit ([CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)), converted from SOG to PLY. Editor: godotGS built from [PR #1226](https://github.com/klausi3D/godotGS/pull/1226), RTX 3090. Not the default settings: black background, scene light off, tile cap 65,536. See [Image credits](../reference/index.md#image-credits).</figcaption>
</figure>

## Payload Mode

For runtime worlds, keep baked `.gsplatworld` files uncompressed when you want out-of-core file-backed streaming. Normal uncompressed loads attach a staged chunk payload source and do not allocate resident `GaussianData` by default.

Compressed `.gsplatworld` output is resident-only. It can still use the streaming system for GPU residency, LOD, and culling, but the full gaussian payload must be decoded into CPU memory before rendering because the current compressed layout is not random-access by chunk.

Generic save/load/save round trips preserve streamable uncompressed worlds. Use an explicit resident compressed export path only when the file is intended to be resident-only.

## Common Failure Modes

| Symptom | Likely cause | What to do |
| --- | --- | --- |
| `Missing --scene or --inputs.` | No bake mode was selected | Pass exactly one mode selector and keep `--output` |
| `No GaussianSplatContainer found in scene.` | The scene does not contain a matching container | Add one or pass a valid `--container` path |
| `Multiple GaussianSplatContainer nodes found...` | Auto-discovery is ambiguous | Pass `--container=<nodepath>` |
| `Output must end with .gsplatworld` | Output extension is unsupported | Rename the output file |
| `Bake aborted: one or more GaussianSplatNode3D children are missing splat_asset.` | A splat node in the container has no `splat_asset` | Assign an asset to every `GaussianSplatNode3D` child, or remove the empty node |
| `Unsupported input format: ...` | An `--inputs` entry is not `.ply`, `.spz`, `.tres`, or `.res` | Convert the input or import it first |
| Misaligned runtime world | The baked world is applied to a transformed runtime node | Reset `GaussianSplatWorld3D` to identity before applying the world data |
| Streaming route policy still reports resident-only payload | The world file is compressed or was loaded resident explicitly | Re-export the world as uncompressed and load normally for out-of-core source streaming |

## Related Pages

- [Import workflow](importing.md)
- [Recurring issues](../troubleshooting/recurring-issues.md)
