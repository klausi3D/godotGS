#ifndef GAUSSIAN_INCREMENTAL_SAVER_H
#define GAUSSIAN_INCREMENTAL_SAVER_H

#include "core/io/file_access.h"
#include "core/io/resource.h"
#include "core/variant/variant.h"
#include "core/variant/dictionary.h"
#include "core/math/vector3.h"
#include "core/math/color.h"
#include "core/math/quaternion.h"
#include "core/templates/local_vector.h"
#include "core/templates/hash_map.h"
#include "core/templates/safe_refcount.h"
#include "core/string/ustring.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"

struct Gaussian;
class GaussianData;

namespace GaussianSplatting {

class GaussianAnimationStateMachine;
enum AnimationProperty : int;

// Change tracking structures
struct SplatChange {
    uint32_t index;
    uint8_t changed_properties; // Bitfield: position|color|opacity|scale|rotation
    Vector3 position;
    Color color;
    float opacity;
    Vector3 scale;
    Quaternion rotation;
};

struct AnimationChange {
    int clip_index;
    AnimationProperty property;
    uint8_t change_type; // 0=modified, 1=added, 2=removed
    Dictionary data; // Flexible storage for keyframes, tracks, etc.
};

// Incremental save file format
static const uint32_t INCREMENTAL_MAGIC = 0x47534946; // "GSIF" - Gaussian Scene Incremental File
static const uint16_t INCREMENTAL_VERSION = 1;

// Layout version tracks the on-disk struct layout of SplatChange / ChangeEntry.
// Bump this whenever the binary layout of serialised change entries changes
// (e.g. adding fields, reordering members, changing sizes).  The reader must
// reject files whose layout version differs from the compiled-in constant to
// prevent silent data corruption from mismatched struct packing.
static const uint16_t INCREMENTAL_SAVER_LAYOUT_VERSION = 1;

enum class ChangeType : uint8_t {
    SPLAT_MODIFIED = 0,
    SPLAT_ADDED = 1,
    SPLAT_REMOVED = 2,
    ANIMATION_MODIFIED = 3,
    METADATA_MODIFIED = 4
};

struct ChangeEntry {
    ChangeType type;
    uint32_t data_offset;
    uint32_t data_size;
    uint64_t timestamp;
};

// THREADING CONTRACT
// ------------------
// The saver's internal state (change tables, dedup map, baseline fields, requires_full_save,
// is_tracking, counters, config) is serialised by an internal recursive change_mutex, so
// RECORDING (via GaussianData setters, under data_rwlock) and SAVING (save_changes) may run
// concurrently: the common case -- a worker thread editing while the main thread saves -- is
// thread-safe.
//
// However, load_and_apply_changes() and create_baseline() deserialise a delta INTO / serialise
// a baseline FROM the WHOLE attached GaussianData. Those whole-object operations are NOT atomic
// w.r.t. concurrent edits of the SAME GaussianData, and the saver cannot make them so (it does
// not own data_rwlock, and holding it across a full apply would deadlock the record path). The
// CALLER must therefore ensure EXCLUSIVE access to the GaussianData for the duration of
// load_and_apply_changes()/create_baseline() -- i.e. quiesce other edits to that data -- exactly
// as one would not deserialise into an object being mutated elsewhere. Only ONE
// load_and_apply_changes() may run per saver at a time. create_baseline() additionally fails
// closed (via content_revision) if it detects the data changed under it, so a stray edit is not
// silently dropped; a structural replace racing the serializer's raw-storage read is a separate,
// pre-existing serializer concern tracked in its own issue.
class GaussianIncrementalSaver : public Resource {
    GDCLASS(GaussianIncrementalSaver, Resource);

private:
    // Change tracking
    LocalVector<SplatChange> splat_changes;
    LocalVector<AnimationChange> animation_changes;
    HashMap<uint32_t, int> splat_index_to_change;
    struct MetadataDelta {
        Variant old_value;
        Variant new_value;
    };
    HashMap<String, MetadataDelta> metadata_changes;

    // Baseline tracking
    String baseline_file_path;
    uint64_t baseline_timestamp = 0;
    uint32_t baseline_splat_count = 0;

    // Save settings
    float auto_save_interval = 30.0f; // seconds
    uint32_t max_changes_before_full_save = 10000;
    bool enable_change_compression = true;

    // Internal state
    bool is_tracking = false;
    uint64_t last_save_time = 0;
    uint32_t accumulated_changes = 0;
    // PERSIST-001: set when a STRUCTURAL batch setter (set_gaussians /
    // set_gaussian_payload) replaces/reshapes the whole splat array in a way that
    // cannot be expressed as per-index deltas. While set, save_changes() fails
    // closed (ERR_UNAVAILABLE) instead of writing an empty/partial delta that would
    // silently drop the edit -> data loss. Cleared only by the paths that
    // (re)establish a baseline: save_changes() success, create_baseline(),
    // start_tracking(), update_baseline().
    bool requires_full_save = false;
    // PERSIST-002 re-entrancy guard: holds the Thread::ID of the thread currently
    // replaying a delta in load_and_apply_changes() (Thread::UNASSIGNED_ID otherwise).
    // GaussianData::set_gaussian() consults is_applying() so the replaying thread does
    // NOT re-record the changes it is applying -- while edits from OTHER threads are
    // still recorded normally (Codex: replay suppression is scoped to the replay owner).
    // Atomic so is_applying() can be read WITHOUT change_mutex; it is called from
    // set_gaussian() under data_rwlock, and taking change_mutex there would invert the
    // record path's data_rwlock -> change_mutex lock order and risk deadlock.
    SafeNumeric<uint64_t> applying_thread_id;
    // Serialises all mutable saver state (the change tables, the dedup map, the baseline
    // fields, requires_full_save, is_tracking, accumulated_changes). Recursive so the
    // record -> _track_splat_change -> mark_requires_full_save re-entry is safe. NEVER
    // held across a call into GaussianData (set_gaussian / save_scene take data_rwlock;
    // holding change_mutex across them would deadlock against the record path's
    // data_rwlock -> change_mutex order).
    mutable Mutex change_mutex;

    // Internal methods
    void _track_splat_change(uint32_t index, const Gaussian& old_splat, const Gaussian& new_splat);
    void _track_animation_change(int clip_index, AnimationProperty property, uint8_t change_type, const Dictionary& data);
    Error _write_change_entry(Ref<FileAccess> file, const ChangeEntry& entry) const;
    Error _read_change_entry(Ref<FileAccess> file, ChangeEntry& entry) const;
    // Non-mutating structural read shared by load_and_apply_changes() (which then
    // commits/applies) and validate_incremental_file() (which discards). Reads +
    // validates the header, the change-entry table and the payload bounds, and
    // returns the change entries plus the payload blob. Touches no saver state.
    Error _read_incremental_layout(const Ref<FileAccess>& file, const String& path,
            uint64_t& r_change_timestamp, uint64_t& r_baseline_timestamp, uint32_t& r_baseline_splat_count,
            LocalVector<ChangeEntry>& r_entries, PackedByteArray& r_data_blob) const;
    // Extracts and strict-decodes one change entry's payload into a Dictionary.
    // A decode failure or non-Dictionary payload is reported as ERR_FILE_CORRUPT
    // rather than collapsing to defaults (#603b).
    static Error _decode_change_payload(const PackedByteArray& data_blob, const ChangeEntry& entry, Dictionary& r_dict);
    Error _apply_splat_changes(::GaussianData* gaussian_data, const LocalVector<SplatChange>& changes) const;
    Error _apply_metadata_changes(GaussianAnimationStateMachine* animation, const HashMap<String, MetadataDelta>& changes) const;
    Error _apply_animation_changes(GaussianAnimationStateMachine* animation, const LocalVector<AnimationChange>& changes, const HashMap<String, MetadataDelta>& metadata_snapshot) const;

protected:
    static void _bind_methods();

public:
    GaussianIncrementalSaver();
    ~GaussianIncrementalSaver();

    // Change tracking control
    void start_tracking(const String& baseline_file);
    void stop_tracking();
    bool is_tracking_enabled() const {
        MutexLock lock(change_mutex);
        return is_tracking;
    }

    // Manual change recording
    void record_splat_change(uint32_t index, const Gaussian& old_splat, const Gaussian& new_splat);
    void record_animation_change(int clip_index, AnimationProperty property, const Dictionary& old_data, const Dictionary& new_data);
    void record_metadata_change(const String& key, const Variant& old_value, const Variant& new_value);

    // PERSIST-001/002 hooks used by GaussianData's public setters.
    // mark_requires_full_save(): a structural batch edit invalidated the delta;
    // save_changes() must fail closed until a full re-baseline is taken.
    void mark_requires_full_save() {
        MutexLock lock(change_mutex);
        requires_full_save = true;
    }
    bool get_requires_full_save() const {
        MutexLock lock(change_mutex);
        return requires_full_save;
    }
    // is_applying(): true only on the thread currently replaying a delta in
    // load_and_apply_changes(), so that thread's set_gaussian() suppresses re-recording
    // the changes it is applying while other threads keep recording. Reads the atomic
    // thread-id WITHOUT change_mutex (called from set_gaussian() under data_rwlock).
    bool is_applying() const { return applying_thread_id.get() == Thread::get_caller_id(); }

    // Incremental save operations
    Error save_changes(const String& incremental_file_path);
    Error load_and_apply_changes(const String& incremental_file_path, ::GaussianData* gaussian_data, GaussianAnimationStateMachine* animation = nullptr);
    Error load_and_apply_changes_bind(const String& incremental_file_path, const Ref<::GaussianData>& gaussian_data, const Ref<GaussianAnimationStateMachine>& animation = Ref<GaussianAnimationStateMachine>());
    Error merge_incremental_files(const Array& incremental_files, const String& output_file);

    // Baseline management
    Error create_baseline(const String& baseline_file_path, const ::GaussianData* gaussian_data, const GaussianAnimationStateMachine* animation = nullptr);
    Error create_baseline_bind(const String& baseline_file_path, const Ref<::GaussianData>& gaussian_data, const Ref<GaussianAnimationStateMachine>& animation = Ref<GaussianAnimationStateMachine>());
    Error update_baseline(const String& new_baseline_file_path);
    String get_baseline_file() const {
        MutexLock lock(change_mutex);
        return baseline_file_path;
    }

    // Change analysis
    uint32_t get_change_count() const {
        MutexLock lock(change_mutex);
        return splat_changes.size() + animation_changes.size();
    }
    uint32_t get_splat_change_count() const {
        MutexLock lock(change_mutex);
        return splat_changes.size();
    }
    uint32_t get_animation_change_count() const {
        MutexLock lock(change_mutex);
        return animation_changes.size();
    }
    Array get_changed_splat_indices() const;
    Dictionary get_change_statistics() const;

    // Auto-save functionality
    void set_auto_save_interval(float seconds) {
        MutexLock lock(change_mutex);
        auto_save_interval = MAX(1.0f, seconds);
    }
    float get_auto_save_interval() const {
        MutexLock lock(change_mutex);
        return auto_save_interval;
    }

    void set_max_changes_before_full_save(uint32_t count) {
        MutexLock lock(change_mutex);
        max_changes_before_full_save = count;
    }
    uint32_t get_max_changes_before_full_save() const {
        MutexLock lock(change_mutex);
        return max_changes_before_full_save;
    }

    bool should_auto_save() const;
    bool should_create_full_save() const;

    // Change compression
    void set_enable_change_compression(bool enable) {
        MutexLock lock(change_mutex);
        enable_change_compression = enable;
    }
    bool get_enable_change_compression() const {
        MutexLock lock(change_mutex);
        return enable_change_compression;
    }

    // Utility methods
    void clear_changes();
    Error validate_incremental_file(const String& file_path) const;
    Dictionary get_incremental_file_info(const String& file_path) const;
    uint64_t estimate_save_size() const;

    // Static helpers
    static bool is_incremental_file(const String& file_path);
    static String get_incremental_extension() { return "gsif"; }
    static Array find_incremental_files(const String& directory, const String& basename);
};

} // namespace GaussianSplatting

#endif // GAUSSIAN_INCREMENTAL_SAVER_H
