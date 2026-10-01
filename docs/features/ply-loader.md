# PLY Loader

!!! info "Scope"
    For programmers and technical users who need loader behavior, property validation, or runtime save/load details.
    This page covers the technical loader layer, not the end-user import flow.
    It complements the canonical [Gaussian Splat Asset Import Workflow](../workflows/importing.md).

## Purpose

Load and inspect Gaussian PLY data with `PLYLoader`, or load and save PLY through `GaussianData`.

## Usage

| Step | Action | Implementation reference |
| --- | --- | --- |
| 1 | Use `GaussianData.load_from_file(path)` for runtime loading. | `GaussianData::load_from_file`, `load_gaussian_data_from_file()` in `modules/gaussian_splatting/io/gaussian_data_loader.cpp` |
| 2 | Use `PLYLoader.load_file(path)` when you need property inspection or load statistics. | `PLYLoader::load_file` |
| 3 | Use `GaussianData.save_to_file(path)` to export binary little-endian PLY. | `GaussianData::save_to_file` |
| 4 | In the editor, the importer `gaussian_splat_ply` turns a `.ply` into a `GaussianSplatAsset` saved as `.res` under `.godot/imported/`. | `ResourceImporterPLY::get_importer_name`, `ResourceImporterPLY::get_resource_type`, `ResourceImporterPLY::get_save_extension` |

## API

Only the methods below are callable from GDScript. `PLYLoader::get_property_deficiencies()` and `PLYLoader::get_property_summary()` exist in C++ but are **not** bound; from a script, use `has_property()` and `get_property_names()` instead (see the example below).

| API | Type | Behavior | Implementation reference |
| --- | --- | --- | --- |
| `GaussianData.load_from_file(path)` | Bound method | Routes by extension: `.spz` goes through `SPZLoader`, `.ply` through `PLYLoader`, any other extension returns `ERR_FILE_UNRECOGNIZED`. A PLY missing any required property, or with a non-finite position/scale/rotation/opacity, returns `ERR_FILE_CORRUPT`. | `GaussianData::load_from_file`, `load_gaussian_data_from_file()` in `modules/gaussian_splatting/io/gaussian_data_loader.cpp` |
| `GaussianData.save_to_file(path)` | Bound method | Writes binary little-endian PLY with position, SH DC, scale, rotation, opacity, and painterly fields, plus `nx,ny,nz` when the data is in 2D mode. Higher-order SH (`f_rest_*`) is **not** written. | `GaussianData::save_to_file` |
| `PLYLoader.load_file(path)` | Bound method | Parses the header, reuses the `.gsplatcache` sidecar when the cache is enabled and valid, otherwise parses binary or ASCII vertex data. It does not fail on missing required properties; missing fields fall back to defaults. | `PLYLoader::load_file`, `PLYLoader::try_load_cache` |
| `PLYLoader.get_load_statistics()` | Bound method | Returns `splat_count`, `format` (`"binary"` or `"ascii"`), `properties` (count), and, after a load, `load_time_ms`, `header_time_ms`, `parse_time_ms`, `cache_time_ms`, `cache_hit`, `bounds_min`, `bounds_max`. | `PLYLoader::get_load_statistics` |
| `PLYLoader.has_property(name)` | Bound method | `true` when the header declares that vertex property. | `PLYLoader::has_property` |
| `PLYLoader.get_property_names()` | Bound method | All vertex property names from the header, in file order. | `PLYLoader::get_property_names` |
| `PLYLoader.get_splat_count()` / `get_gaussian_data()` | Bound methods | Loaded splat count and the loaded `GaussianData`. | `PLYLoader::get_splat_count`, `PLYLoader::get_gaussian_data` |

| PLY property set | Required | Notes | Implementation reference |
| --- | --- | --- | --- |
| `x,y,z` | Yes | Position. Required by `GaussianData.load_from_file()` and by importer validation. | `PLYLoader::get_property_deficiencies`, `ResourceImporterPLY::validate_ply_properties` |
| `f_dc_0,f_dc_1,f_dc_2` | Yes | Color is read as SH DC and converted using `SH_C0`. | `PLYLoader::assemble_sh_coefficients` |
| `scale_0,scale_1,scale_2` | Yes | Scale is decoded with `exp`. | `PLYLoader::parse_binary_data`, `PLYLoader::parse_ascii_data` |
| `rot_0..rot_3` | Yes | Rotation is read as quaternion (`rot_0` = w) and normalized. | `PLYLoader::parse_binary_data`, `PLYLoader::parse_ascii_data` |
| `opacity` | Yes | Opacity is decoded as sigmoid from logit. | `PLYLoader::parse_binary_data`, `PLYLoader::parse_ascii_data` |
| `nx,ny,nz` | No | If all three are present, the loader enables 2D mode. | `PLYLoader::parse_binary_data` |
| `palette_id,brush_override_id,brush_axis_u,brush_axis_v,stroke_age` | No | Painterly metadata is loaded when present and written on save. | `PLYLoader::parse_binary_data`, `GaussianData::save_to_file` |
| `f_rest_*` | No | Higher-order SH is repacked from channel-major to coefficient-major RGB on load. It is not written by `GaussianData.save_to_file()`. | `PLYLoader::assemble_sh_coefficients` |

| Format path | Read support | Write support | Implementation reference |
| --- | --- | --- | --- |
| `ascii` | Yes | No | `PLYLoader::parse_header`, `PLYLoader::parse_ascii_data` |
| `binary_little_endian` | Yes | Yes | `PLYLoader::parse_header`, `GaussianData::save_to_file` |
| `binary_big_endian` | Yes | No | `PLYLoader::parse_header`, `PLYLoader::read_float_property` |

| Import option | Default | Effect | Implementation reference |
| --- | --- | --- | --- |
| `quality/preset` | preset-specific | Chooses the preset baseline: `mobile`, `desktop`, `high`, `ultra`, `development`, or `custom`. | `ResourceImporterPLY::get_import_options` |
| `quality/max_splats` | preset-specific | Caps the final splat count after import processing (`0` = no cap). | `ResourceImporterPLY::get_import_options`, `ResourceImporterPLY::import` |
| `quality/density_multiplier` | preset-specific | Reduces density (clamped to `0.1..1.0`) and can merge source ranges. | `ResourceImporterPLY::get_import_options`, `ResourceImporterPLY::import` |
| `validation/validate_required_properties` | `true` | Fails the import if required properties are missing or the first splats are invalid. Non-finite data is rejected even when this is off. | `ResourceImporterPLY::validate_ply_properties`, `ResourceImporterPLY::import` |
| `validation/warn_missing_optional` | `true` | Logs optional property presence and omissions. | `ResourceImporterPLY::log_missing_properties` |
| `preview/generate_thumbnail` | `true` | Generates a thumbnail and stores it as the asset's preview image. | `ResourceImporterPLY::import` |

## Examples

Both examples are complete scripts. Attach one to a `Node` and point the path at a PLY in your project.

```gdscript
extends Node

func _ready() -> void:
	load_and_save_ply()

func load_and_save_ply() -> void:
	var data := GaussianData.new()
	var err := data.load_from_file("res://models/scan.ply")
	if err != OK:
		push_error("load_from_file failed: %s" % error_string(err))
		return

	print("Loaded splats: ", data.get_count())
	print("Bounds: ", data.get_aabb())

	# Writes binary little-endian PLY. Higher-order SH (f_rest_*) is not written.
	err = data.save_to_file("user://scan_out.ply")
	if err != OK:
		push_error("save_to_file failed: %s" % error_string(err))
```

```gdscript
extends Node

const REQUIRED_PROPERTIES := [
	"x", "y", "z",
	"f_dc_0", "f_dc_1", "f_dc_2",
	"scale_0", "scale_1", "scale_2",
	"rot_0", "rot_1", "rot_2", "rot_3",
	"opacity",
]

func _ready() -> void:
	inspect_ply_header()

func inspect_ply_header() -> void:
	var loader := PLYLoader.new()
	var err := loader.load_file("res://models/scan.ply")
	if err != OK:
		push_error("PLYLoader.load_file failed: %s" % error_string(err))
		return

	print("Stats: ", loader.get_load_statistics())
	print("Properties: ", loader.get_property_names())

	var missing_required := PackedStringArray()
	for property_name in REQUIRED_PROPERTIES:
		if not loader.has_property(property_name):
			missing_required.append(property_name)
	print("Missing required: ", missing_required)
	print("Has normals (2D mode): ", loader.has_property("nx") and loader.has_property("ny") and loader.has_property("nz"))
```

## Troubleshooting

| Symptom | Cause | Fix | Implementation reference |
| --- | --- | --- | --- |
| `ERR_FILE_CORRUPT` from `GaussianData.load_from_file()` | Required properties are missing, or a splat has a NaN/Inf position, scale, rotation, or opacity. | Ensure `x,y,z`, `f_dc_0..2`, `scale_0..2`, `rot_0..3`, and `opacity` exist and are finite. | `GaussianData::load_from_file` |
| `ERR_FILE_UNRECOGNIZED` from `GaussianData.load_from_file()` | The file extension is neither `.ply` nor `.spz`, or the file does not start with a `ply` header. | Use a `.ply` or `.spz` path. | `load_gaussian_data_from_file()` in `modules/gaussian_splatting/io/gaussian_data_loader.cpp`, `PLYLoader::parse_header` |
| Import fails during validation | Validation found missing fields or invalid values. | Re-export with required fields and finite position/scale/opacity values. | `ResourceImporterPLY::validate_ply_properties` |
| 2D surfel mode is not enabled | Source PLY does not include the full normal triplet. | Export `nx,ny,nz` for each vertex. | `PLYLoader::parse_binary_data` |
| Repeated loads are slower than expected | Cache path is disabled or cache metadata mismatch prevents reuse. | Enable `rendering/gaussian_splatting/import/use_gsplatworld_cache` and keep source timestamp and size stable. | `_is_ply_cache_enabled()` in `modules/gaussian_splatting/io/ply_loader.cpp`, `PLYLoader::try_load_cache` |
