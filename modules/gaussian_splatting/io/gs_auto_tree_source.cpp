#include "gs_auto_tree_source.h"

Error GsqAutoTreeSource::open(const String &p_path, String *r_error) {
	return ERR_UNAVAILABLE;
}

gs_hlod::TiledBakeSourceInfo GsqAutoTreeSource::get_info() const {
	return {};
}

Error GsqAutoTreeSource::get_chunk_info(uint32_t p_chunk, gs_hlod::TiledBakeChunkInfo &r_info, String *r_error) const {
	return ERR_UNAVAILABLE;
}

Error GsqAutoTreeSource::read_chunk(uint32_t p_chunk, gs_hlod::TiledBakeChunk &r_chunk, String *r_error) const {
	return ERR_UNAVAILABLE;
}

GsqAutoTreeSource::Stats GsqAutoTreeSource::get_stats() const {
	return {};
}
