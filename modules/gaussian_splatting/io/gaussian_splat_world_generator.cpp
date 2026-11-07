#include "gaussian_splat_world_generator.h"

#include "gs_world_generator.h"

#include "core/object/class_db.h"

namespace {

Dictionary error_dict(const String &p_message) {
	Dictionary d;
	d["ok"] = false;
	d["message"] = p_message;
	return d;
}

} // namespace

void GaussianSplatWorldGenerator::_bind_methods() {
	ClassDB::bind_static_method("GaussianSplatWorldGenerator", D_METHOD("get_tier_names"), &GaussianSplatWorldGenerator::get_tier_names);
	ClassDB::bind_static_method("GaussianSplatWorldGenerator", D_METHOD("get_tier_params", "tier"), &GaussianSplatWorldGenerator::get_tier_params);
	ClassDB::bind_static_method("GaussianSplatWorldGenerator", D_METHOD("apply_overrides", "params", "overrides"), &GaussianSplatWorldGenerator::apply_overrides);
	ClassDB::bind_static_method("GaussianSplatWorldGenerator", D_METHOD("get_params_sha", "params"), &GaussianSplatWorldGenerator::get_params_sha);
	ClassDB::bind_static_method("GaussianSplatWorldGenerator", D_METHOD("plan", "params"), &GaussianSplatWorldGenerator::plan);
	ClassDB::bind_static_method("GaussianSplatWorldGenerator", D_METHOD("generate", "params", "world_path", "chunk_selection", "print_progress"),
			&GaussianSplatWorldGenerator::generate, DEFVAL(PackedInt64Array()), DEFVAL(false));
	ClassDB::bind_static_method("GaussianSplatWorldGenerator", D_METHOD("verify", "world_path"), &GaussianSplatWorldGenerator::verify);
	ClassDB::bind_static_method("GaussianSplatWorldGenerator", D_METHOD("get_manifest_path", "world_path"), &GaussianSplatWorldGenerator::get_manifest_path);
}

PackedStringArray GaussianSplatWorldGenerator::get_tier_names() {
	return gs_world_generator::get_tier_names();
}

Dictionary GaussianSplatWorldGenerator::get_tier_params(const String &p_tier) {
	gs_world_generator::Params params;
	if (gs_world_generator::params_for_tier(p_tier, params) != OK) {
		return Dictionary();
	}
	return gs_world_generator::params_to_dict(params);
}

Dictionary GaussianSplatWorldGenerator::apply_overrides(const Dictionary &p_params, const Dictionary &p_overrides) {
	gs_world_generator::Params base;
	String err;
	if (gs_world_generator::params_from_dict(p_params, base, err) != OK) {
		return error_dict(err);
	}
	gs_world_generator::Params out;
	if (gs_world_generator::apply_overrides(base, p_overrides, out, err) != OK) {
		return error_dict(err);
	}
	Dictionary d;
	d["ok"] = true;
	d["params"] = gs_world_generator::params_to_dict(out);
	return d;
}

String GaussianSplatWorldGenerator::get_params_sha(const Dictionary &p_params) {
	gs_world_generator::Params params;
	String err;
	if (gs_world_generator::params_from_dict(p_params, params, err) != OK) {
		return String();
	}
	return gs_world_generator::params_sha(params);
}

Dictionary GaussianSplatWorldGenerator::plan(const Dictionary &p_params) {
	gs_world_generator::Params params;
	String err;
	if (gs_world_generator::params_from_dict(p_params, params, err) != OK) {
		return error_dict(err);
	}
	gs_world_generator::Plan plan;
	if (gs_world_generator::build_plan(params, plan, err) != OK) {
		return error_dict(err);
	}
	Dictionary d;
	d["ok"] = true;
	d["generator_params_sha"] = plan.params_sha;
	d["splat_count"] = int64_t(plan.total_splats);
	d["chunk_count"] = int64_t(plan.chunks.size());
	d["max_chunk_splats"] = int64_t(plan.max_chunk_count);
	d["expected_file_bytes"] = int64_t(gs_world_generator::expected_file_bytes(plan, LocalVector<uint32_t>()));
	Dictionary table;
	for (int k = 0; k < gs_world_generator::CLASS_COUNT; k++) {
		table[gs_world_generator::class_name(gs_world_generator::ContentClass(k))] = int64_t(plan.class_totals[k]);
	}
	d["content_table"] = table;
	return d;
}

Dictionary GaussianSplatWorldGenerator::generate(const Dictionary &p_params, const String &p_world_path, const PackedInt64Array &p_chunk_selection, bool p_print_progress) {
	gs_world_generator::Params params;
	String err;
	if (gs_world_generator::params_from_dict(p_params, params, err) != OK) {
		return error_dict(err);
	}
	gs_world_generator::GenerateOptions options;
	options.print_progress = p_print_progress;
	for (int64_t i = 0; i < p_chunk_selection.size(); i++) {
		const int64_t c = p_chunk_selection[i];
		if (c < 0 || c > int64_t(UINT32_MAX)) {
			return error_dict("chunk_selection entries must be chunk indices");
		}
		options.selection.push_back(uint32_t(c));
	}
	gs_world_generator::Report report;
	gs_world_generator::generate_world(params, p_world_path, options, report);
	return report.to_dict();
}

Dictionary GaussianSplatWorldGenerator::verify(const String &p_world_path) {
	Dictionary report;
	gs_world_generator::verify_world(p_world_path, report);
	return report;
}

String GaussianSplatWorldGenerator::get_manifest_path(const String &p_world_path) {
	return gs_world_generator::manifest_path_for(p_world_path);
}
