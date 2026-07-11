#ifndef RESOURCE_IMPORTER_PLY_H
#define RESOURCE_IMPORTER_PLY_H

#ifdef TOOLS_ENABLED

#include "core/io/resource_importer.h"
#include "core/object/ref_counted.h"
#include "../core/gaussian_splat_asset.h"

class ResourceImporterPLY : public ResourceImporter {
    GDCLASS(ResourceImporterPLY, ResourceImporter);

public:
    virtual String get_importer_name() const override;
    virtual String get_visible_name() const override;
    virtual void get_recognized_extensions(List<String> *p_extensions) const override;
    virtual String get_save_extension() const override;
    virtual String get_resource_type() const override;
    virtual int get_preset_count() const override;
    virtual String get_preset_name(int p_idx) const override;
    virtual void get_import_options(const String &p_path, List<ImportOption> *r_options, int p_preset = 0) const override;
    virtual bool get_option_visibility(const String &p_path, const String &p_option, const HashMap<StringName, Variant> &p_options) const override;

    virtual Error import(ResourceUID::ID p_source_id, const String &p_source_file, const String &p_save_path, const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants, List<String> *r_gen_files = nullptr, Variant *r_metadata = nullptr) override;

    virtual bool can_import_threaded() const override { return true; }
    virtual bool has_advanced_options() const override;
    virtual void show_advanced_options(const String &p_path) override;

    // Bump whenever importer behavior changes in a way that requires existing
    // imported caches to be re-imported. Godot's resource scanner compares this
    // against the value stored in each .ply.import file and re-runs import()
    // when they differ, so users do NOT need to manually wipe .godot/imported/
    // after a fix lands in the importer.
    //   v0 (implicit): pre-versioning baseline.
    //   v1: switch to versioned importer.
    //   v2: optional Packed*Array fields are now zero-initialized at import
    //       time (see gaussian_splat_asset.cpp::_ensure_buffer_sizes —
    //       resize_initialized() instead of resize() for POD vectors).
    //       Caches written by v0/v1 may contain 0xC0C0C0C0 poison and must
    //       be re-imported.
    //   v3: preview thumbnails now serialize as Image resources instead of
    //       ImageTexture to avoid threaded importer deadlocks under
    //       `--headless --import`.
    //   v4: PLY DC-encoding default flipped from `legacy_bias` (sigmoid
    //       compression `1.5*sigmoid(x) - 0.25`) to `linear_rgb` (canonical
    //       Inria `sh*C0 + 0.5`). Caches written by v1-v3 carry the wrong
    //       `dc_encoding` tag and must be reimported to render with full
    //       contrast / saturation.
    //   v5: SH bands 1-3 are now propagated into the asset. Caches written
    //       by v1-v4 carry only DC color; the renderer fell back to flat
    //       per-splat RGB and lost view-dependent specular / fresnel detail.
    //       v5 caches carry full per-splat sh_first_order / sh_high_order
    //       arrays sized splat_count * sh_terms * 3.
    //   v6: imported GaussianSplatAsset payloads are saved as binary .res
    //       instead of text .tres to avoid multi-hundred-MB text parsing during
    //       scene loads.
    //   v7: per-chunk streaming bake (start_idx/count/center/max_radius/bounds)
    //       now stored on the asset so GaussianStreamingSystem::register_asset
    //       can skip the per-splat center/bounds pass on scene load. Old
    //       caches without the bake fall through to the runtime compute path
    //       (no breakage), but reimporting recovers the ~7-9s startup win on
    //       multi-million-splat scenes.
    //   v8: PLY loader now decodes integer-typed vertex properties
    //       (char/uchar/short/ushort/int/uint and int8..uint32 aliases) to
    //       their real value instead of silently returning 0.0 (issue #465).
    //       An asset imported by v1-v7 from a PLY with integer-typed fields
    //       holds zeroed positions/opacity/scales in its .res and must be
    //       re-imported to recover correct data; bumping the format version
    //       makes Godot's scanner re-run import() automatically.
    //       COORDINATION: the accepted GS-PERF-PRUNE ADR also plans a 7->8
    //       bump. This safety fix lands first and takes v8, so the pruning
    //       slice must take v9 to avoid a format-version collision.
    //   v9: GS-PERF-PRUNE slice 2b (issue #456) wires opt-in importance pruning
    //       (processing/prune_ratio + processing/prune_importance_threshold) into
    //       the importer. Pruning changes the saved array LENGTHS and the baked
    //       streaming-chunk records, so an asset imported by v1-v8 whose .import
    //       enables a prune option holds the un-pruned arrays in its .res and must
    //       re-import; bumping the format version makes Godot's scanner re-run
    //       import() automatically. NOTE: the raw .gsplatcache (PLY_CACHE_VERSION)
    //       is deliberately NOT bumped -- pruning happens AFTER the raw decode, so
    //       the cached decode is unaffected (ADR "Two distinct caches"). Because
    //       pruning is opt-in and Ultra stays byte-identical, no shipped asset
    //       silently changes on the bump.
    virtual int get_format_version() const override { return 9; }

    // Validation helpers
    Error validate_ply_properties(const Ref<class PLYLoader> &p_loader) const;
    void log_missing_properties(const Ref<class PLYLoader> &p_loader) const;

    ResourceImporterPLY();
};

#endif // TOOLS_ENABLED

#endif // RESOURCE_IMPORTER_PLY_H
