#pragma once

#include "gaussian_splat_hlod_bake.h"
#include "gaussian_splat_hlod_tiled_bake.h"
#include "core/variant/dictionary.h"

namespace gs_hlod {
// ADR dynamics 6.1: independent of the partition and merge grids.
constexpr double kIslandHoleCellSizeM = 0.25;
// Re-iterable filtered view: island removal precedes the tiled histogram/leaf
// partition. Its global indices are dense in original source order. All mutable
// preparation happens in open(); subsequent reads are immutable and concurrent.
class SemanticBakeSource : public TiledBakeSource {
    struct State;
    State *state = nullptr;
public:
    SemanticBakeSource();
    ~SemanticBakeSource();
    SemanticBakeSource(const SemanticBakeSource &) = delete;
    SemanticBakeSource &operator=(const SemanticBakeSource &) = delete;
    Error open(const TiledBakeSource &p_source, const Array &p_volumes, const Dictionary &p_metadata,
            const String &p_island_directory, uint64_t p_max_ram = UINT64_MAX,
            uint64_t p_max_output = UINT64_MAX, uint64_t p_deadline = UINT64_MAX,
            uint32_t p_sh_degree = 0, bool p_is_2d = false);
    TiledBakeSourceInfo get_info() const override;
    Error get_chunk_info(uint32_t, TiledBakeChunkInfo &, String *) const override;
    Error read_chunk(uint32_t, TiledBakeChunk &, String *) const override;
    Dictionary get_metadata() const;
    Dictionary get_report() const;
    String get_reason() const;
    uint64_t get_reserved_bytes() const;
    uint64_t get_read_workspace_bytes() const;
    uint64_t get_island_output_bytes() const;
    void commit_islands(); // after the enclosing world/manifest publication
};
// volumes are serializable descriptors returned by the authoring nodes.
// source contains PLY property-presence and island side data (not GPU lanes).
Error prepare_semantic_bake(const BakeInput &p_input, const Array &p_volumes,
        const Dictionary &p_source, const String &p_island_directory,
        SplatList &r_world, Dictionary &r_metadata, Dictionary &r_report, String &r_reason, bool p_is_2d = false);
}
