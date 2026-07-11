#ifndef RESOURCE_IMPORTER_SPZ_H
#define RESOURCE_IMPORTER_SPZ_H

#ifdef TOOLS_ENABLED

#include "core/io/resource_importer.h"
#include "core/object/ref_counted.h"
#include "../core/gaussian_splat_asset.h"

/**
 * @class ResourceImporterSPZ
 * @brief Godot resource importer for SPZ compressed Gaussian Splatting files.
 *
 * This importer handles Niantic's SPZ format, which provides approximately
 * 10x compression over PLY files with minimal visual degradation.
 */
class ResourceImporterSPZ : public ResourceImporter {
    GDCLASS(ResourceImporterSPZ, ResourceImporter);

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

    virtual Error import(ResourceUID::ID p_source_id, const String &p_source_file, const String &p_save_path,
            const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants,
            List<String> *r_gen_files = nullptr, Variant *r_metadata = nullptr) override;

    virtual bool can_import_threaded() const override { return true; }
    virtual bool has_advanced_options() const override;
    virtual void show_advanced_options(const String &p_path) override;

    // See ResourceImporterPLY::get_format_version() for the bump rationale.
    // SPZ assets share the GaussianSplatAsset deserialization path.
    //   v4: SH bands 1-3 propagated into the asset (same fix applied to PLY at v4).
    //   v5: imported GaussianSplatAsset payloads are saved as binary .res
    //       instead of text .tres to avoid multi-hundred-MB text parsing during
    //       scene loads.
    //   v6: per-chunk streaming bake baked at import; see ResourceImporterPLY
    //       v7 comment for the runtime fast path it unlocks.
    //   v7: GS-PERF-PRUNE slice 2b (issue #456) wires opt-in importance pruning
    //       (processing/prune_ratio + processing/prune_importance_threshold) into
    //       the importer. Pruning changes the saved array lengths and the baked
    //       streaming-chunk records, so an asset imported by v4-v6 whose .import
    //       enables a prune option holds the un-pruned arrays in its .res and must
    //       re-import; bumping the format version makes Godot's scanner re-run
    //       import() automatically. Because pruning is opt-in and Ultra stays
    //       byte-identical, no shipped asset silently changes on the bump. (SPZ
    //       has no raw-decode cache to worry about; it decompresses on each load.)
    virtual int get_format_version() const override { return 7; }

    ResourceImporterSPZ();
};

#endif // TOOLS_ENABLED

#endif // RESOURCE_IMPORTER_SPZ_H
