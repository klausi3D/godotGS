#ifndef GAUSSIAN_SPLAT_WORLD_IO_H
#define GAUSSIAN_SPLAT_WORLD_IO_H

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"

class GaussianSplatWorld;
class GaussianData;

class ResourceFormatLoaderGaussianSplatWorld : public ResourceFormatLoader {
public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "",
			Error *r_error = nullptr, bool p_use_sub_threads = false,
			float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	Ref<GaussianSplatWorld> load_resident(const String &p_path, Error *r_error = nullptr) const;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool recognize_path(const String &p_path, const String &p_for_type = String()) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};

class ResourceFormatLoaderImportedGaussianSplatWorld : public ResourceFormatLoaderGaussianSplatWorld {
public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "",
			Error *r_error = nullptr, bool p_use_sub_threads = false,
			float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual bool recognize_path(const String &p_path, const String &p_for_type = String()) const override;
	virtual bool is_import_valid(const String &p_path) const override;
	virtual bool is_imported(const String &p_path) const override;
	virtual bool has_custom_uid_support() const override;
	virtual int get_import_order(const String &p_path) const override;
	virtual String get_import_group_file(const String &p_path) const override;
	virtual void get_classes_used(const String &p_path, HashSet<StringName> *r_classes) override;
};

class ResourceFormatSaverGaussianSplatWorld : public ResourceFormatSaver {
public:
#ifdef TESTS_ENABLED
	using HlodSnapshotTestHook = void (*)(void *, const Ref<GaussianData> &, bool);
	static HlodSnapshotTestHook hlod_snapshot_test_hook;
	static void *hlod_snapshot_test_userdata;
#endif
	enum PayloadSaveMode {
		SAVE_PAYLOAD_PRESERVE = 0,
		SAVE_PAYLOAD_STREAMABLE_UNCOMPRESSED,
		SAVE_PAYLOAD_RESIDENT_UNCOMPRESSED,
		SAVE_PAYLOAD_RESIDENT_COMPRESSED,
	};

	virtual Error save(const Ref<Resource> &p_resource, const String &p_path,
			uint32_t p_flags = 0) override;
	Error save_with_payload_mode(const Ref<Resource> &p_resource, const String &p_path,
			PayloadSaveMode p_mode, uint32_t p_flags = 0);
	Error save_resident_compressed(const Ref<Resource> &p_resource, const String &p_path,
			uint32_t p_flags = 0);
	Error save_resident_uncompressed(const Ref<Resource> &p_resource, const String &p_path,
			uint32_t p_flags = 0);
	virtual void get_recognized_extensions(const Ref<Resource> &p_resource,
			List<String> *p_extensions) const override;
	virtual bool recognize(const Ref<Resource> &p_resource) const override;
	virtual bool recognize_path(const Ref<Resource> &p_resource, const String &p_path) const override;
};

#endif // GAUSSIAN_SPLAT_WORLD_IO_H
