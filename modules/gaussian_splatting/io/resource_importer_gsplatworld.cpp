#include "resource_importer_gsplatworld.h"

#ifdef TOOLS_ENABLED

#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "../core/gaussian_data.h"
#include "../core/streaming_chunk_payload_source.h"
#include "../logger/gs_logger.h"
#include "gs_atomic_file_writer.h"
#include <cstdint>

namespace {

static bool _range_in_file(uint64_t p_offset, uint64_t p_size, uint64_t p_file_size) {
	if (p_offset > p_file_size) {
		return false;
	}
	return p_size <= (p_file_size - p_offset);
}

static bool _checked_mul_u64(uint64_t p_a, uint64_t p_b, uint64_t &r_result) {
	if (p_a != 0u && p_b > (UINT64_MAX / p_a)) {
		return false;
	}
	r_result = p_a * p_b;
	return true;
}

struct GSplatWorldHeaderInfo {
	bool compressed = false;
	bool resident_payload = false;
	uint32_t splat_count = 0;
	uint32_t chunk_count = 0;
	uint32_t sh_degree = 0;
	uint32_t sh_first_order = 0;
	uint32_t sh_high_order = 0;
};

static Error _validate_gsplatworld_header(const String &p_source_file, GSplatWorldHeaderInfo *r_info = nullptr) {
	constexpr uint32_t world_magic = 0x57505347u; // 'GSPW' little-endian.
	constexpr uint32_t world_version = 1u;
	constexpr uint32_t max_sh_degree = 3u;
	constexpr uint32_t max_sh_first_order = 3u;
	constexpr uint32_t flag_has_metadata = 1u << 0u;
	constexpr uint32_t flag_has_chunks = 1u << 2u;
	constexpr uint32_t flag_has_high_sh = 1u << 3u;
	constexpr uint32_t flag_compressed = 1u << 4u;
	constexpr uint32_t flag_resident_payload = 1u << 5u;
	constexpr uint64_t header_size_bytes = 104u;
	constexpr uint64_t chunk_record_size_bytes = 56u;
	// Compressed-payload cap = the gzip decompression limit (INT32_MAX). KEEP IN
	// SYNC with the loader's kMaxCompressedGaussianBytes in
	// io/gaussian_splat_world_io.cpp. A compressed world larger than this can never
	// decompress, so rejecting it lets the editor import fail cleanly instead of the
	// loader aborting on a doomed multi-GiB resize. Rationale is documented there.
	constexpr uint64_t max_compressed_gaussian_bytes = INT32_MAX;

	Error open_err = OK;
	Ref<FileAccess> file = FileAccess::open(p_source_file, FileAccess::READ, &open_err);
	if (file.is_null()) {
		return open_err != OK ? open_err : ERR_CANT_OPEN;
	}

	const uint64_t file_size = file->get_length();
	if (file_size < header_size_bytes) {
		return ERR_FILE_CORRUPT;
	}

	const uint32_t magic = file->get_32();
	if (magic != world_magic) {
		return ERR_FILE_UNRECOGNIZED;
	}

	const uint32_t version = file->get_32();
	if (version != world_version) {
		return ERR_FILE_CORRUPT;
	}

	const uint32_t flags = file->get_32();
	const uint32_t splat_count = file->get_32();

	const uint32_t sh_degree = file->get_32();
	if (sh_degree > max_sh_degree) {
		return ERR_FILE_CORRUPT;
	}

	const uint32_t sh_first_order = file->get_32();
	const uint32_t sh_high_order = file->get_32();
	// Mirror the loader: cap sh_first_order at the on-disk struct slot count
	// (the saver clamps to 3 too). Do not cap sh_high_order with a magnitude
	// limit — set_gaussian_payload accepts arbitrary counts and the saver
	// writes them verbatim, so a hard cap rejects valid round-trip files.
	// Structural safety is provided by the SH payload range/multiply checks
	// further down.
	if (sh_first_order > max_sh_first_order) {
		return ERR_FILE_CORRUPT;
	}

	file->seek(file->get_position() + 12u); // bounds_pos
	file->seek(file->get_position() + 12u); // bounds_size

	const uint32_t chunk_count = file->get_32();
	const uint64_t gaussian_offset = file->get_64();
	const uint64_t sh_offset = file->get_64();
	const uint64_t chunk_table_offset = file->get_64();
	const uint64_t indices_offset = file->get_64();
	const uint64_t metadata_offset = file->get_64();
	const uint64_t metadata_size = file->get_64();

	if (gaussian_offset < header_size_bytes || gaussian_offset >= file_size) {
		return ERR_FILE_CORRUPT;
	}

	uint64_t gaussian_bytes = 0u;
	if (!_checked_mul_u64(uint64_t(splat_count), sizeof(Gaussian), gaussian_bytes)) {
		return ERR_FILE_CORRUPT;
	}
	if ((flags & flag_compressed) != 0u) {
		if (!_range_in_file(gaussian_offset, sizeof(uint64_t), file_size)) {
			return ERR_FILE_CORRUPT;
		}
		file->seek(gaussian_offset);
		const uint64_t compressed_size = file->get_64();
		if (!_range_in_file(gaussian_offset + sizeof(uint64_t), compressed_size, file_size)) {
			return ERR_FILE_CORRUPT;
		}
		// Bound the decompressed payload the loader would materialize (mirrors the
		// loader's compressed-path guard): reject a payload too large to ever
		// decompress rather than letting the loader abort on resize.
		if (gaussian_bytes > max_compressed_gaussian_bytes) {
			return ERR_FILE_CORRUPT;
		}
	} else {
		if (!_range_in_file(gaussian_offset, gaussian_bytes, file_size)) {
			return ERR_FILE_CORRUPT;
		}
	}

	if ((flags & flag_has_high_sh) != 0u && sh_high_order > 0u) {
		uint64_t sh_count = 0u;
		if (!_checked_mul_u64(uint64_t(splat_count), uint64_t(sh_high_order), sh_count)) {
			return ERR_FILE_CORRUPT;
		}
		uint64_t sh_bytes = 0u;
		if (!_checked_mul_u64(sh_count, sizeof(Vector3), sh_bytes)) {
			return ERR_FILE_CORRUPT;
		}
		if (!_range_in_file(sh_offset, sh_bytes, file_size)) {
			return ERR_FILE_CORRUPT;
		}
	}

	if ((flags & flag_has_chunks) != 0u && chunk_count > 0u) {
		if (chunk_count > (UINT64_MAX / chunk_record_size_bytes)) {
			return ERR_FILE_CORRUPT;
		}
		const uint64_t chunk_table_size = uint64_t(chunk_count) * chunk_record_size_bytes;
		if (!_range_in_file(chunk_table_offset, chunk_table_size, file_size)) {
			return ERR_FILE_CORRUPT;
		}
		if (indices_offset >= file_size) {
			return ERR_FILE_CORRUPT;
		}
	}

	if ((flags & flag_has_metadata) != 0u && metadata_size > 0u) {
		if (!_range_in_file(metadata_offset, metadata_size, file_size)) {
			return ERR_FILE_CORRUPT;
		}
	}

	if (r_info) {
		r_info->compressed = (flags & flag_compressed) != 0u;
		r_info->resident_payload = (flags & flag_resident_payload) != 0u;
		r_info->splat_count = splat_count;
		r_info->chunk_count = chunk_count;
		r_info->sh_degree = sh_degree;
		r_info->sh_first_order = sh_first_order;
		r_info->sh_high_order = sh_high_order;
	}

	return OK;
}

static Error _copy_binary_file(const String &p_source_file, const String &p_dest_file) {
	Error read_err = OK;
	Ref<FileAccess> src = FileAccess::open(p_source_file, FileAccess::READ, &read_err);
	if (src.is_null()) {
		return read_err != OK ? read_err : ERR_CANT_OPEN;
	}

	// #714: an atomic replace needs delete access to the destination, which Windows
	// does not grant while a FileAccess handle is open on it (file_access_windows.cpp
	// opens with _SH_DENYNO / _SH_DENYWR / _SH_DENYRW -- none share delete). A world
	// that is currently streaming caches read handles to exactly this file, so both
	// MoveFileExW and the backup-swap fallback would fail with a sharing violation
	// where the old truncating write used to succeed.
	//
	// The guard is created by the helper immediately before the RENAME, not here:
	// quiescing readers across the whole copy would stall streaming for the size of
	// the file, while the rename is a metadata operation. For its lifetime the guard
	// sets each matching source's lock-free `suspend_opens` flag (so no reader can
	// reopen the destination in the gap -- which is what made a release-then-copy
	// approach intermittent under continuous streaming) and waits for reads already
	// in flight to release their handles. It does NOT hold file_mutex across that
	// wait: an in-flight read needs that mutex to finish, so holding it would block
	// the very reads being drained.
	//
	// If that wait expires with a read still holding the file open, the helper
	// refuses the replace and returns ERR_BUSY rather than attempting a rename that
	// cannot succeed; see the ERR_BUSY branch at the call site.
	//
	// Route the destination write through the crash-atomic helper (temp sibling ->
	// atomic replace-over-existing), exactly like every other final-output saver
	// (gaussian_splat_world_io / gaussian_scene_serializer / incremental_saver).
	// A plain FileAccess::WRITE truncates the destination the instant it is opened,
	// so a crash or write error mid-copy would destroy any pre-existing generated
	// output. gs_atomic_file_write streams the copy into a temp file and only moves
	// it into place once the whole copy has succeeded and closed, so an interrupted
	// copy leaves the prior good file byte-intact (see #714). The source stream is
	// captured by reference; the writer never retains the destination Ref.
	return gs_atomic_file_write(p_dest_file, [&src](const Ref<FileAccess> &dst) -> Error {
		const uint64_t file_size = src->get_length();
		constexpr uint64_t chunk_size = 1024 * 1024; // 1 MiB
		PackedByteArray buffer;
		// #798 exclusion rule 2: chunk_size is a compile-time constant that does not scale
		// with the file, so this is not a data-driven allocation. It also fails safely: a
		// failed resize leaves buffer_ptr null, and get_buffer() null-guards
		// (ERR_FAIL_COND_V(!p_dst && p_length > 0, -1)) -- and unlike most call sites this
		// one CHECKS the return via `read != to_read`, so the copy aborts before
		// store_buffer() ever sees the null (it would be reported as ERR_FILE_CORRUPT).
		buffer.resize(chunk_size);
		uint8_t *buffer_ptr = buffer.ptrw();

		while (src->get_position() < file_size) {
			const uint64_t remaining = file_size - src->get_position();
			const uint64_t to_read = remaining < chunk_size ? remaining : chunk_size;
			const uint64_t read = src->get_buffer(buffer_ptr, to_read);
			if (read != to_read) {
				return ERR_FILE_CORRUPT;
			}
			if (!dst->store_buffer(buffer_ptr, to_read)) {
				const Error dst_err = dst->get_error();
				return dst_err != OK ? dst_err : ERR_FILE_CANT_WRITE;
			}
		}

		return OK;
	},
			[&p_dest_file]() {
				return StagedFileChunkPayloadSource::ScopedReaderSuspend(p_dest_file);
			});
}

} // namespace

String ResourceImporterGSplatWorld::get_importer_name() const {
	return "gaussian_splat_world";
}

String ResourceImporterGSplatWorld::get_visible_name() const {
	return "Gaussian Splat World";
}

void ResourceImporterGSplatWorld::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("gsplatworld");
}

String ResourceImporterGSplatWorld::get_save_extension() const {
	// Preserve binary world payloads during import. Saving as .tres drops
	// gaussian/chunk data because Generic text serialization cannot encode
	// GaussianSplatWorld payload arrays.
	return "gsplatworld";
}

String ResourceImporterGSplatWorld::get_resource_type() const {
	return "GaussianSplatWorld";
}

int ResourceImporterGSplatWorld::get_preset_count() const {
	return 0;
}

String ResourceImporterGSplatWorld::get_preset_name(int p_idx) const {
	(void)p_idx;
	return String();
}

void ResourceImporterGSplatWorld::get_import_options(const String &p_path, List<ImportOption> *r_options,
		int p_preset) const {
	(void)p_path;
	(void)r_options;
	(void)p_preset;
}

bool ResourceImporterGSplatWorld::get_option_visibility(const String &p_path, const String &p_option,
		const HashMap<StringName, Variant> &p_options) const {
	(void)p_path;
	(void)p_option;
	(void)p_options;
	return true;
}

Error ResourceImporterGSplatWorld::import(ResourceUID::ID p_source_id, const String &p_source_file,
		const String &p_save_path, const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants,
		List<String> *r_gen_files, Variant *r_metadata) {
	(void)p_source_id;
	(void)p_options;
	(void)r_gen_files;
	if (r_platform_variants) {
		r_platform_variants->clear();
	}

	GSplatWorldHeaderInfo header_info;
	Error validation_err = _validate_gsplatworld_header(p_source_file, &header_info);
	if (validation_err != OK) {
		GS_LOG_ERROR_DEFAULT(vformat("GaussianSplatWorld importer rejected invalid payload %s (error %d)",
				p_source_file, validation_err));
		return validation_err;
	}

	String save_path = p_save_path + "." + get_save_extension();
	// Keep imports cheap for large worlds: source/destination formats are identical.
	Error err = _copy_binary_file(p_source_file, save_path);
	if (err == ERR_BUSY) {
		// #714: the copy itself succeeded; the atomic replace was refused because a
		// streaming read still held the destination open when the drain budget
		// expired. Windows grants no delete access to such a handle, so no rename
		// could have worked. Name the cause and the remedy -- the generic copy
		// message below would send the reader looking for a disk or format problem.
		GS_LOG_ERROR_DEFAULT(vformat("GaussianSplatWorld importer could not replace %s: it is still being read "
									 "by an active stream. The previous file is unchanged. Close or stop any "
									 "scene streaming this world and reimport.",
				save_path));
		return err;
	}
	if (err != OK) {
		GS_LOG_ERROR_DEFAULT(vformat("GaussianSplatWorld importer failed to copy %s -> %s (error %d)",
				p_source_file, save_path, err));
		return err;
	}
	Error imported_decode_err = OK;
	Ref<Resource> imported_world = ResourceLoader::load(
			save_path, "GaussianSplatWorld", ResourceFormatLoader::CACHE_MODE_IGNORE, &imported_decode_err);
	if (imported_world.is_null()) {
		const Error final_err = imported_decode_err != OK ? imported_decode_err : ERR_FILE_CORRUPT;
		GS_LOG_ERROR_DEFAULT(vformat("GaussianSplatWorld importer produced unreadable output %s (error %d)",
				save_path, final_err));
		if (FileAccess::exists(save_path)) {
			DirAccess::remove_absolute(save_path);
		}
		return final_err;
	}

	if (r_metadata) {
		const bool resident_only = header_info.compressed || header_info.resident_payload;
		const String resident_only_reason = header_info.compressed
				? String("compressed_world_has_no_random_access_chunks")
				: (header_info.resident_payload ? String("explicit_resident_payload") : String());
		Dictionary import_metadata;
		import_metadata[StringName("source_path")] = p_source_file;
		import_metadata[StringName("resource_path")] = save_path;
		import_metadata[StringName("compressed")] = header_info.compressed;
		import_metadata[StringName("payload_mode")] = resident_only
				? String("resident_only")
				: String("streamable_uncompressed");
		import_metadata[StringName("streamable")] = !resident_only;
		import_metadata[StringName("resident_only_reason")] = resident_only_reason;
		import_metadata[StringName("splat_count")] = static_cast<int64_t>(header_info.splat_count);
		import_metadata[StringName("chunk_count")] = static_cast<int64_t>(header_info.chunk_count);
		import_metadata[StringName("sh_degree")] = static_cast<int64_t>(header_info.sh_degree);
		import_metadata[StringName("sh_first_order")] = static_cast<int64_t>(header_info.sh_first_order);
		import_metadata[StringName("sh_high_order")] = static_cast<int64_t>(header_info.sh_high_order);
		*r_metadata = import_metadata;
	}

	return OK;
}

#endif // TOOLS_ENABLED
