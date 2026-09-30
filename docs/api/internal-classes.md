# Internal Classes Index

Classes listed below are registered with Godot's ClassDB but serve as internal
infrastructure: they are either used exclusively by higher-level components, expose no
scripting-facing methods, or are abstract base types. Each entry names the registration
macro in `register_types.cpp` and gives a one-line purpose summary.

> **Correction.** This page used to say these classes "do not have dedicated API reference
> pages". That is no longer true — `modules/gaussian_splatting/doc_classes/` now holds 33
> XML files, one per registered class, so every class here ships in-editor documentation.
> "Internal" on this page means *not intended for use from your project*, not *undocumented*.
>
> The Registration column names the macro call in `initialize_gaussian_splatting_module()` (`modules/gaussian_splatting/register_types.cpp`); search that file for it. Line numbers are deliberately not given because they drift.

---

## Core Infrastructure

| Class | Description | Registration |
|-------|-------------|-------------|
| **GaussianSplatAsset** | Persistent resource wrapping a Gaussian splat dataset with asset-type metadata, compression flags, and import provenance. | `GDREGISTER_CLASS(GaussianSplatAsset)` |
| **GaussianSplatSceneDirector** | Singleton coordinator that manages multi-instance rendering, wind mode, and per-world scene orchestration across all active splat nodes. | `GDREGISTER_CLASS(GaussianSplatSceneDirector)` |
| **VRAMBudgetRegulator** | Monitors GPU memory pressure and throttles chunk loading to stay within a configurable VRAM budget. Used internally by `GaussianStreamingSystem`. | `GDREGISTER_CLASS(VRAMBudgetRegulator)` |

## Rendering Internals

| Class | Description | Registration |
|-------|-------------|-------------|
| **GPUBufferManager** | Manages allocation, resizing, and lifetime of RenderingDevice GPU buffers consumed by the splat renderer. | `GDREGISTER_CLASS(GPUBufferManager)` |
| **ColorGradingResource** | Resource holding exposure, contrast, saturation, temperature, tint, and hue-shift parameters for splat color grading, applied during tile binning (see [Color Grading Reference](../reference/color-grading.md)). | `GDREGISTER_CLASS(ColorGradingResource)` |

## GPU Sorting Implementations

| Class | Description | Registration |
|-------|-------------|-------------|
| **IGPUSorter** | Abstract base class defining the GPU sorting interface for depth-order sorting of visible Gaussians. | `GDREGISTER_ABSTRACT_CLASS(IGPUSorter)` |
| **BitonicSort** | Bitonic merge sort implementation of `IGPUSorter`, suited for small-to-medium splat counts with predictable dispatch cost. | `GDREGISTER_CLASS(BitonicSort)` |
| **RadixSort** | Radix sort implementation of `IGPUSorter`, offering linear-time performance for large splat counts. | `GDREGISTER_CLASS(RadixSort)` |
| **OneSweepSort** | Single-pass radix sort (OneSweep algorithm) implementation of `IGPUSorter`, providing high throughput with reduced synchronization barriers. | `GDREGISTER_CLASS(OneSweepSort)` |

## IO Abstractions

| Class | Description | Registration |
|-------|-------------|-------------|
| **IGaussianLoader** | Abstract base class for Gaussian data loaders. Concrete implementations include `PLYLoader` and `SPZLoader`. | `GDREGISTER_ABSTRACT_CLASS(IGaussianLoader)` |

## Animation and Persistence (v0.6.0)

| Class | Description | Registration |
|-------|-------------|-------------|
| **GaussianSplatting::GaussianAnimationStateMachine** | State machine resource for per-splat keyframe animation of position, color, opacity, scale, and rotation. CPU-sampled only; the renderer does not read it (see [Animation](../features/animation.md)). | `GDREGISTER_CLASS(GaussianSplatting::GaussianAnimationStateMachine)` |
| **GaussianSplatting::GaussianSceneSerializer** | Binary scene serializer using a chunked file format (magic `GSCF`) for saving and loading full Gaussian scenes with animation data. | `GDREGISTER_CLASS(GaussianSplatting::GaussianSceneSerializer)` |
| **GaussianSplatting::GaussianIncrementalSaver** | Tracks per-splat changes and writes incremental delta files to avoid full scene re-serialization during editing sessions. | `GDREGISTER_CLASS(GaussianSplatting::GaussianIncrementalSaver)` |

## Asset Management (v0.7.0)

| Class | Description | Registration |
|-------|-------------|-------------|
| **AssetDependencyManager** | Tracks inter-asset dependencies using collision-resistant hashing, enabling dependency-aware loading, cache invalidation, and hot-reload workflows. | `GDREGISTER_CLASS(AssetDependencyManager)` |

---

## Cross-Reference

The following classes **do** have full API reference pages (XML doc_classes or
dedicated documentation):

- `GaussianData`
- `GaussianMemoryStream`
- `GaussianSplatContainer`
- `GaussianSplatDebugHUD`
- `GaussianSplatManager`
- `GaussianSplatNode3D`
- `GaussianSplatRenderer`
- `GaussianSplatWorld`
- `GaussianSplatWorld3D`
- `GaussianStreamingSystem`
- `PainterlyMaterial`
- `PLYLoader`
- `SPZLoader`
