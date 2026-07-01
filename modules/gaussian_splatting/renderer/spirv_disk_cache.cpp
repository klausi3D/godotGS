#include "spirv_disk_cache.h"

#include "../logger/gs_logger.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/string_builder.h"
#include "core/templates/hashfuncs.h"
#include "servers/rendering/rendering_device.h"

#include <algorithm>

#if defined(_WIN32)
#include <sys/types.h>
#include <sys/utime.h>
#include <time.h>
#else
#include <sys/types.h>
#include <time.h>
#include <utime.h>
#endif

// =====================================================================================
// Cache-key define audit (Phase 1 spirv disk cache).
//
// A subtly wrong cache key produces silent visual regressions on real-scan content:
// a stale blob compiled for a different runtime configuration is served and
// reinterpreted under the new bindings. Every define that influences the binary
// MUST be threaded into compute_key() via the `defines` vector passed to
// ShaderCompilationHelper. The following audit lists every define source the
// module currently feeds to a compile call.
//
//   modules/gaussian_splatting/renderer/tile_renderer.cpp:1855
//     TileRenderer::_build_common_shader_defines() — base set:
//       GS_TILE_SIZE, GS_TILE_LOCAL_SIZE_X/Y, GS_DISPATCH_LOCAL_SIZE_X,
//       GS_ENABLE_SUBGROUPS (subgroup support is per-device, but the device
//       fingerprint below also discriminates devices so this is doubly safe),
//       GS_DEBUG_COUNTERS_DISABLED, GS_PACKED_STAGE_DATA, GS_TIGHTER_BOUNDS,
//       GS_SH_AMORTIZATION, USE_QUANTIZED_GAUSSIANS,
//       MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS, GS_MAX_OMNI_LIGHTS, GS_MAX_SPOT_LIGHTS.
//
//   modules/gaussian_splatting/renderer/tile_renderer.cpp:1955
//     TileRenderer::_build_binning_shader_defines() — adds:
//       GS_TILE_SPLAT_CAPACITY, GS_SORT_KEY_BITS, GS_SORT_TILE_BITS,
//       GS_SORT_DEPTH_BITS, GS_SORT_TIE_BITS,
//       GS_TILE_GLOBAL_SORT, GS_TILE_GLOBAL_SORT_EMIT_PASS.
//
//   modules/gaussian_splatting/renderer/tile_renderer.cpp:1976
//     TileRenderer::_build_raster_shader_defines() — adds:
//       GS_TILE_SPLAT_CAPACITY, GS_MAX_RASTER_SPLATS_PER_TILE,
//       GS_TILE_GLOBAL_SORT, GS_COLLECT_RASTER_STATS.
//
//   modules/gaussian_splatting/renderer/shader_compilation_helper.cpp:517,535
//     ShaderCompilationManager::_build_prefix_defines() — adds:
//       GS_TILE_GLOBAL_SORT, GS_TILE_PREFIX_PASS_{1,2,3},
//       GS_PREFIX_LOCAL_SIZE,
//       GS_TILE_PREFIX_PASS2_OP_{INCLUSIVE_STEP,EXCLUSIVE_SHIFT,COPY},
//       GS_TILE_PREFIX_PASS2_SOURCE_{WG_SUMS,WG_OFFSETS}.
//     ShaderCompilationManager::_build_binning_count_defines() — adds:
//       GS_TILE_SPLAT_CAPACITY, GS_TILE_GLOBAL_SORT, GS_TILE_GLOBAL_SORT_COUNT_PASS.
//     compile_raster_shaders also appends GS_TILE_RASTER_COMPUTE for the
//     compute-rasterizer variant (shader_compilation_helper.cpp:496).
//
//   modules/gaussian_splatting/renderer/tile_render_resolve.cpp:397
//     adds TILE_RESOLVE_FORMAT on top of _build_common_shader_defines(false).
//
//   modules/gaussian_splatting/interfaces/output_compositor.cpp:606
//     viewport-blit compile — single define from _viewport_blit_define(format).
//
// gpu_sorter.cpp's create_compute_shader_from_spirv() does NOT route through
// ShaderCompilationHelper; its variants embed all configuration directly in the
// generated source string (radix_bits, radix_size, workgroup_size, the entire
// key/helper/subgroup_preamble blocks). Since compute_key() hashes the
// `source` argument, those variants are discriminated by the source text alone
// and need no separate define list — but this is exactly why we MUST include
// the full source in the hash and not just defines.
//
// Device fingerprint components (also folded into the key):
//   RenderingDevice::get_device_vendor_name()
//   RenderingDevice::get_device_name()
//   RenderingDevice::get_device_pipeline_cache_uuid()   — driver-version proxy
//   RenderingDevice::get_device_api_name()
//   RenderingDevice::get_device_api_version()
//   GSPLAT_SHADER_CACHE_VERSION                          — bump on cache format change
//
// If you add a new define source anywhere, also add it above and ensure it is
// in the `defines` Vector<String> at the compile callsite. Missing entries =
// silent miscompilation cached to disk and replayed on every warm start.
// =====================================================================================

namespace {

static constexpr uint32_t GSPLAT_SHADER_CACHE_VERSION = 1;
static constexpr const char *CACHE_ROOT_BASE = "user://gsplat_spirv_cache";
static constexpr const char *CACHE_FILE_EXT = ".spv";
static constexpr const char *CACHE_TMP_EXT = ".tmp";

static String _hex64(uint64_t p_value) {
    // 16-char lowercase hex. Used both for cache filenames and for the
    // per-device subdir suffix. A 64-bit key keeps birthday-collision
    // probability negligible across the entire module's shader matrix.
    char buf[17];
    static const char hex_digits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex_digits[p_value & 0xFu];
        p_value >>= 4u;
    }
    buf[16] = 0;
    return String(buf);
}

static uint32_t _hash_string(const String &p_value, uint32_t p_seed) {
    if (p_value.is_empty()) {
        return hash_murmur3_buffer(&p_seed, 0, p_seed);
    }
    const CharString utf8 = p_value.utf8();
    return hash_murmur3_buffer(utf8.get_data(), utf8.length(), p_seed);
}

static void _touch_file_mtime(const String &p_path) {
    // Best-effort LRU bump: set mtime to "now" so prune_above() (which evicts
    // by mtime) keeps hot entries even when the cache exceeds its cap. Failure
    // is silent — this is a perf hint, not a correctness path.
    String global_path = p_path;
    if (ProjectSettings *ps = ProjectSettings::get_singleton()) {
        global_path = ps->globalize_path(p_path);
    }
#if defined(_WIN32)
    struct __utimbuf64 times {};
    times.actime = _time64(nullptr);
    times.modtime = times.actime;
    const Char16String utf16 = global_path.utf16();
    _wutime64(reinterpret_cast<const wchar_t *>(utf16.get_data()), &times);
#else
    struct utimbuf times {};
    times.actime = time(nullptr);
    times.modtime = times.actime;
    const CharString utf8 = global_path.utf8();
    utime(utf8.get_data(), &times);
#endif
}

} // namespace

// Function-local static. C++11 guarantees thread-safe initialization on first
// call ("magic statics"), which is the simplest correct fix for the data race
// the previous if-null/new pattern had under concurrent shader compilation.
SPIRVDiskCache *SPIRVDiskCache::get() {
    static SPIRVDiskCache instance;
    return &instance;
}

void SPIRVDiskCache::shutdown() {
    // No-op: the singleton is a function-local static and is destroyed
    // automatically at program exit. Kept as a public API for callers that
    // expect a teardown hook.
}

bool SPIRVDiskCache::is_enabled() const {
    MutexLock lock(mutex);
    return enabled;
}

void SPIRVDiskCache::set_enabled(bool p_enabled) {
    MutexLock lock(mutex);
    enabled = p_enabled;
}

String SPIRVDiskCache::_device_fingerprint(RenderingDevice *p_device) {
    if (!p_device) {
        return String("no_device");
    }
    StringBuilder sb;
    sb.append(p_device->get_device_vendor_name());
    sb.append("|");
    sb.append(p_device->get_device_name());
    sb.append("|");
    sb.append(p_device->get_device_api_name());
    sb.append("|");
    sb.append(p_device->get_device_api_version());
    sb.append("|");
    // Pipeline cache UUID changes when the driver updates; using it as a driver
    // version proxy invalidates the cache on driver upgrades without parsing
    // vendor-specific version strings.
    sb.append(p_device->get_device_pipeline_cache_uuid());
    return sb.as_string();
}

String SPIRVDiskCache::_device_short_hash(RenderingDevice *p_device) {
    const String fp = _device_fingerprint(p_device);
    const CharString utf8 = fp.utf8();
    // Mix two seeds to widen the device subdir tag to 64 bits so swapping
    // GPUs (AMD vs NV vs Intel iGPU) cannot collide a shorter hash space.
    const uint32_t lo = hash_murmur3_buffer(utf8.get_data(), utf8.length(), GSPLAT_SHADER_CACHE_VERSION);
    const uint32_t hi = hash_murmur3_buffer(utf8.get_data(), utf8.length(), GSPLAT_SHADER_CACHE_VERSION ^ 0x9e3779b9u);
    return _hex64((uint64_t(hi) << 32) | uint64_t(lo));
}

String SPIRVDiskCache::_cache_dir_for_device(RenderingDevice *p_device) {
    // Cache last computed dir keyed by RD instance id so we don't re-stringify
    // for every compile in a hot session. Caller holds `mutex`.
    const uint64_t device_id = p_device ? p_device->get_device_instance_id() : 0;
    if (!cached_device_dir.is_empty() && device_id == cached_device_id) {
        return cached_device_dir;
    }
    const String dir = String(CACHE_ROOT_BASE) + "/" + _device_short_hash(p_device) + "/";
    cached_device_dir = dir;
    cached_device_id = device_id;
    return dir;
}

bool SPIRVDiskCache::_ensure_dir(const String &p_dir) {
    if (DirAccess::dir_exists_absolute(p_dir)) {
        return true;
    }
    Error err = DirAccess::make_dir_recursive_absolute(p_dir);
    return err == OK;
}

String SPIRVDiskCache::compute_key(const String &p_source, const Vector<String> &p_defines, RenderingDevice *p_device) {
    // No side effects on cached_device_dir here — try_load/store/invalidate
    // now resolve the per-device subdir themselves under their own lock so a
    // concurrent compile against a different device cannot route this thread's
    // I/O to the wrong subdirectory.

    // Sort defines so that callsite ordering can never produce two distinct
    // keys for what is actually the same compile input.
    Vector<String> sorted_defines = p_defines;
    sorted_defines.sort();

    // Two independent murmur passes with disjoint seeds yield a 64-bit
    // composite key. Each pass folds in source + sorted defines + device
    // fingerprint identically, so the same inputs always collapse to the
    // same key but separate inputs share only 32 bits of accidental
    // overlap with negligible probability.
    uint32_t h_lo = GSPLAT_SHADER_CACHE_VERSION;
    uint32_t h_hi = GSPLAT_SHADER_CACHE_VERSION ^ 0x9e3779b9u;
    h_lo = _hash_string(p_source, h_lo);
    h_hi = _hash_string(p_source, h_hi);
    for (int i = 0; i < sorted_defines.size(); i++) {
        h_lo = _hash_string(sorted_defines[i], h_lo);
        h_hi = _hash_string(sorted_defines[i], h_hi);
    }
    const String fp = _device_fingerprint(p_device);
    h_lo = _hash_string(fp, h_lo);
    h_hi = _hash_string(fp, h_hi);
    return _hex64((uint64_t(h_hi) << 32) | uint64_t(h_lo));
}

bool SPIRVDiskCache::try_load(const String &p_key, RenderingDevice *p_device, Vector<uint8_t> &r_spirv) {
    MutexLock lock(mutex);
    if (!enabled) {
        return false;
    }
    const String dir = _cache_dir_for_device(p_device);
    if (dir.is_empty()) {
        return false;
    }
    const String path = dir + p_key + CACHE_FILE_EXT;
    if (!FileAccess::exists(path)) {
        return false;
    }
    Error err = OK;
    Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ, &err);
    if (f.is_null() || err != OK) {
        return false;
    }
    const uint64_t len = f->get_length();
    if (len == 0 || len > (uint64_t)1024 * 1024 * 256) {
        // Sanity bound: any single SPIR-V blob >256 MB is corruption.
        return false;
    }
    r_spirv.resize((int)len);
    if (r_spirv.size() != (int)len) {
        return false;
    }
    const uint64_t got = f->get_buffer(r_spirv.ptrw(), len);
    if (got != len) {
        r_spirv.clear();
        return false;
    }
    // Release the file handle before touching mtime so the utime call on
    // Windows isn't fighting our own open READ handle.
    f.unref();
    // Touch mtime via platform utime() so prune_above() (which evicts by mtime)
    // treats this hit as recently used and won't evict it ahead of cold entries.
    _touch_file_mtime(path);
    return true;
}

void SPIRVDiskCache::store(const String &p_key, RenderingDevice *p_device, const Vector<uint8_t> &p_spirv) {
    if (p_spirv.is_empty()) {
        return;
    }
    MutexLock lock(mutex);
    if (!enabled) {
        return;
    }
    const String dir = _cache_dir_for_device(p_device);
    if (dir.is_empty()) {
        return;
    }
    if (!_ensure_dir(dir)) {
        return;
    }
    const String final_path = dir + p_key + CACHE_FILE_EXT;
    const String tmp_path = dir + p_key + CACHE_TMP_EXT;

    Error err = OK;
    bool write_ok = false;
    {
        Ref<FileAccess> f = FileAccess::open(tmp_path, FileAccess::WRITE, &err);
        if (f.is_null() || err != OK) {
            return;
        }
        f->store_buffer(p_spirv.ptr(), p_spirv.size());
        // FileAccess::get_error() reports the last I/O failure (e.g. disk full).
        // Probing before close lets us bail without ever swapping a truncated
        // blob over a previously-good entry.
        write_ok = (f->get_error() == OK);
        // Force-close before rename so the file handle is released on Windows.
    }
    if (!write_ok) {
        // Reap the partial tmp so prune_above() doesn't have to.
        DirAccess::remove_absolute(tmp_path);
        return;
    }

    // Atomic-ish replace: stash any existing blob in a .bak, install the new
    // blob, then drop the .bak. If the install rename fails, restore the .bak
    // so a transient I/O hiccup cannot drop a previously good cache entry.
    Ref<DirAccess> da = DirAccess::open(dir);
    if (da.is_null()) {
        DirAccess::remove_absolute(tmp_path);
        return;
    }
    const String backup_name = p_key + CACHE_FILE_EXT + ".bak";
    const String final_name = p_key + CACHE_FILE_EXT;
    const String tmp_name = p_key + CACHE_TMP_EXT;
    const String backup_path = dir + backup_name;

    // Clean any leftover backup from a prior crash so the new stash can land.
    if (FileAccess::exists(backup_path)) {
        da->remove(backup_name);
    }
    bool backup_made = false;
    if (FileAccess::exists(final_path)) {
        if (da->rename(final_name, backup_name) == OK) {
            backup_made = true;
        }
    }
    Error rename_err = da->rename(tmp_name, final_name);
    if (rename_err != OK) {
        // Roll the backup back into place before giving up so we keep the
        // last known-good blob.
        da->remove(tmp_name);
        if (backup_made) {
            da->rename(backup_name, final_name);
        }
        return;
    }
    if (backup_made) {
        da->remove(backup_name);
    }
}

void SPIRVDiskCache::invalidate(const String &p_key, RenderingDevice *p_device) {
    MutexLock lock(mutex);
    const String dir = _cache_dir_for_device(p_device);
    if (dir.is_empty()) {
        return;
    }
    const String path = dir + p_key + CACHE_FILE_EXT;
    if (FileAccess::exists(path)) {
        DirAccess::remove_absolute(path);
    }
}

void SPIRVDiskCache::prune_above(uint64_t p_max_bytes) {
    MutexLock lock(mutex);
    // Walk every per-device subdirectory under the cache root so a GPU swap
    // does not strand unbounded data from the previous device.
    const String root = String(CACHE_ROOT_BASE) + "/";
    if (!DirAccess::dir_exists_absolute(root)) {
        return;
    }

    struct Entry {
        String path;
        uint64_t size = 0;
        uint64_t mtime = 0;
    };
    Vector<Entry> entries;
    uint64_t total = 0;

    Ref<DirAccess> root_dir = DirAccess::open(root);
    if (root_dir.is_null()) {
        return;
    }
    root_dir->list_dir_begin();
    while (true) {
        const String subname = root_dir->get_next();
        if (subname.is_empty()) {
            break;
        }
        if (subname == "." || subname == "..") {
            continue;
        }
        if (!root_dir->current_is_dir()) {
            continue;
        }
        const String sub_path = root + subname + "/";
        Ref<DirAccess> sub_dir = DirAccess::open(sub_path);
        if (sub_dir.is_null()) {
            continue;
        }
        sub_dir->list_dir_begin();
        while (true) {
            const String fname = sub_dir->get_next();
            if (fname.is_empty()) {
                break;
            }
            if (sub_dir->current_is_dir()) {
                continue;
            }
            const String full = sub_path + fname;
            // Reap leftover .tmp blobs from interrupted stores regardless of cap.
            if (fname.ends_with(CACHE_TMP_EXT)) {
                DirAccess::remove_absolute(full);
                continue;
            }
            // Reap leftover .bak rollback files from interrupted stores. These
            // only exist when the process exited between final->.bak and
            // .tmp->final. Without this they would persist forever and never
            // count toward the cache cap.
            if (fname.ends_with(".bak")) {
                DirAccess::remove_absolute(full);
                continue;
            }
            if (!fname.ends_with(CACHE_FILE_EXT)) {
                continue;
            }
            Entry e;
            e.path = full;
            const int64_t sz = FileAccess::get_size(full);
            e.size = sz > 0 ? (uint64_t)sz : 0;
            e.mtime = FileAccess::get_modified_time(full);
            entries.push_back(e);
            total += e.size;
        }
        sub_dir->list_dir_end();
    }
    root_dir->list_dir_end();

    if (total <= p_max_bytes) {
        return;
    }
    // LRU: drop oldest mtime first until total <= cap.
    Entry *data = entries.ptrw();
    const int count = entries.size();
    std::sort(data, data + count, [](const Entry &a, const Entry &b) {
        return a.mtime < b.mtime;
    });
    for (int i = 0; i < count && total > p_max_bytes; i++) {
        if (DirAccess::remove_absolute(data[i].path) == OK) {
            total -= data[i].size;
        }
    }
}
