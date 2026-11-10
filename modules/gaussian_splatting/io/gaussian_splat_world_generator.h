#pragma once

#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"

// Script entry point for the BV-05 synthetic world generator (io/gs_world_generator.h).
// The beyond-VRAM prime job drives it from tests/runtime/flythrough/generate_world.gd;
// every method is static and stateless.
class GaussianSplatWorldGenerator : public RefCounted {
	GDCLASS(GaussianSplatWorldGenerator, RefCounted);

protected:
	static void _bind_methods();

public:
	static PackedStringArray get_tier_names();
	static Dictionary get_tier_params(const String &p_tier);
	static Dictionary apply_overrides(const Dictionary &p_params, const Dictionary &p_overrides);
	static String get_params_sha(const Dictionary &p_params);
	static Dictionary plan(const Dictionary &p_params);
	static Dictionary generate(const Dictionary &p_params, const String &p_world_path, const PackedInt64Array &p_chunk_selection, bool p_print_progress, const String &p_codec);
	static Dictionary verify(const String &p_world_path);
	static String get_manifest_path(const String &p_world_path);
};
