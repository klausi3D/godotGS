#pragma once

#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"

// D2-3 driver: GsqAutoTreeSource -> TiledAutoTree -> WriterV4 (codec NONE)
// -> reopen every node, recompute metrics and compare spill-time digests ->
// stage output and <stem>.manifest.json, then publish with per-file atomic
// replacements and rollback on ordinary I/O errors. Native filesystems cannot
// atomically replace TWO fixed names together: a crash/concurrent reader can see
// a mixed pair. Hard-link backups preserve previous bytes until publication;
// unsupported filesystems fail closed. Rollback filesystem failures preserve
// recovery files and name their directory in the error, rather than deleting old
// data. Callers must exclude external writers and validate the sidecar file SHA.
// Only owned private staging/scratch directories are cleaned, never caller files.
// No GSQ node encoding/container until BV-C4: codec="gsq" must return
// ERR_UNAVAILABLE / v4_gsq_output_requires_bv_c4.
//
// Manifest schema godotgs-auto-tree-manifest/1:
// tree_sha, source_fingerprint, input_file_sha256, input_manifest_sha256,
// input_gen_sha, input_generator_params_sha (when available), build_params,
// codec={"codec":"none"}, nodes=[{stored_sha256, decoded_sha256}], totals,
// levels=[{depth,node_count,eps_min,eps_median,eps_max}], content_generation,
// output_file_sha256 and report. content_generation is an unsigned decimal
// string (JSON numbers cannot retain all 64 bits). decoded_sha256 hashes runtime-form Gaussian
// bytes followed by high-order SH bytes; stored_sha256 hashes the V4 record.
//
// tree_sha hashes UTF-8 JSON::stringify(key, "", true, true), without newline,
// where key={source_fingerprint,build_params,codec,input_file_sha256} and
// build_params excludes worker_count. Record worker_count in manifest build_params
// and report, but exclude it from the key (schedule independence, design section 6).
// Frozen interior fields: merge_grid, reduction, node_max_splats, eps_growth,
// max_eps_steps. Canonical numeric types are INT for leaf_max, max_tile_splats,
// merge_grid, node_max_splats, max_eps_steps; FLOAT for reduction and eps_growth.
// Restore these schema types after JSON parsing before recomputing tree_sha.
// Limits, paths, timings and memory are execution metadata only.
// wall_usec measures through verified staging, excluding final manifest write
// and publication so the returned and persisted report are identical. The whole
// BakeResult is held in memory at T0 scale, then freed after spilling its nodes.
class GaussianSplatHlodAutoTree : public RefCounted {
	GDCLASS(GaussianSplatHlodAutoTree, RefCounted);

protected:
	static void _bind_methods();

public:
	static Dictionary build(const String &p_input_path, const String &p_output_path, const Dictionary &p_options);
};
