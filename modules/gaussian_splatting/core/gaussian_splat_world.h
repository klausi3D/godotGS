#ifndef GAUSSIAN_SPLAT_WORLD_H
#define GAUSSIAN_SPLAT_WORLD_H

#include "core/io/resource.h"
#include "core/math/aabb.h"
#include "core/variant/typed_array.h"
#include "core/variant/variant.h"

#include "gaussian_data.h"
#include "gaussian_splat_hlod_tree.h"
#include "streaming_chunk_payload_source.h"
#include "../renderer/gaussian_splat_renderer.h"

class GaussianSplatWorld : public Resource {
    GDCLASS(GaussianSplatWorld, Resource);
    RES_BASE_EXTENSION("gsplatworld");

private:
    Ref<GaussianData> gaussian_data;
    Ref<ChunkPayloadSource> chunk_payload_source;
    Vector<GaussianSplatRenderer::StaticChunk> static_chunks;
    AABB bounds;
    Dictionary metadata;
    uint32_t splat_count_metadata = 0;
    uint32_t sh_degree_metadata = 0;
    uint32_t sh_first_order_count_metadata = 0;
    uint32_t sh_high_order_count_metadata = 0;
    bool is_2d_metadata = false;
    // HLOD tree (ADR adr-hlod-streaming.md, slice S1a). Empty unless baked or loaded from a v2
    // file. Its leaf payload IS the gaussian payload above (same order); a new payload drops it.
    GaussianSplatHlodTree hlod_tree;

    void _assign_gaussian_data(const Ref<GaussianData> &p_data, bool p_keep_hlod);

protected:
    static void _bind_methods();
    bool _get(const StringName &p_name, Variant &r_ret) const;
    void _get_property_list(List<PropertyInfo> *p_list) const;

public:
    void set_gaussian_data(const Ref<GaussianData> &p_data);
    Ref<GaussianData> get_gaussian_data() const { return gaussian_data; }
    bool has_resident_gaussian_data() const;

    void set_bounds(const AABB &p_bounds);
    AABB get_bounds() const { return bounds; }

    void set_metadata(const Dictionary &p_metadata);
    Dictionary get_metadata() const { return metadata; }

    void set_static_chunks(const Vector<GaussianSplatRenderer::StaticChunk> &p_chunks);
    const Vector<GaussianSplatRenderer::StaticChunk> &get_static_chunks() const { return static_chunks; }

    void set_chunk_payload_source(const Ref<ChunkPayloadSource> &p_source);
    Ref<ChunkPayloadSource> get_chunk_payload_source() const { return chunk_payload_source; }
    bool has_chunk_payload_source() const;
    bool is_payload_source_backed() const;
    bool has_renderable_payload() const;
    String get_payload_mode() const;
    bool is_streamable_payload() const;
    String get_resident_only_reason() const;

    void set_payload_metadata(uint32_t p_splat_count, uint32_t p_sh_degree,
            uint32_t p_sh_first_order_count, uint32_t p_sh_high_order_count, bool p_is_2d);
    uint32_t get_splat_count() const;
    uint32_t get_sh_degree() const;
    uint32_t get_sh_first_order_count() const;
    uint32_t get_sh_high_order_count() const;
    bool get_2d_mode() const;
    Error materialize_resident_gaussian_data();

    int get_chunk_count() const;
    PackedInt32Array get_chunk_sizes() const;
    Array get_chunk_aabbs() const;

    void clear();

    // Editor reimport (Resource::reload_from_file()) lands here. The base copies only storage
    // properties, and its gaussian_data setter drops the tree, so this copies the whole world.
    Error copy_from(const Ref<Resource> &p_resource) override;

    Error save_to_file(const String &p_path) const;

    // HLOD (slice S1a). bake_hlod() bakes the tree from a resident or file-backed payload,
    // reorders the payload into leaf order and replaces the chunks with
    // the leaves. Opt-in: nothing bakes implicitly at export (ADR §7).
    Error bake_hlod();
#ifdef TESTS_ENABLED
    using HlodBakeTestHook = void (*)(void *, GaussianSplatWorld *);
    static HlodBakeTestHook hlod_bake_test_hook;
    static void *hlod_bake_test_userdata;
#endif
    bool has_hlod_tree() const { return !hlod_tree.is_empty(); }
    // False when the resident payload was edited in place (GaussianData setters) after the tree
    // was baked or loaded: the interior payload no longer summarizes the leaves.
    bool is_hlod_payload_current() const;
    Dictionary get_hlod_info() const;
    const GaussianSplatHlodTree &get_hlod_tree() const { return hlod_tree; }
    // Installs a tree whose leaf payload is the current gaussian payload (loader use).
    void set_hlod_tree(GaussianSplatHlodTree &&p_tree);
    // One StaticChunk per leaf, in payload order: world bounds, a sphere enclosing at least
    // its AABB, and its contiguous index range. Until S2's cut, today's runtime streams leaves as chunks.
    // Returns false (r_chunks empty) when an allocation fails.
    static bool build_hlod_leaf_chunks(const GaussianSplatHlodTree &p_tree, Vector<GaussianSplatRenderer::StaticChunk> &r_chunks);
};

#endif // GAUSSIAN_SPLAT_WORLD_H
