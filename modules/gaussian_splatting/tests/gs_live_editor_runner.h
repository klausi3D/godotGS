#pragma once
#if defined(TESTS_ENABLED) && defined(TOOLS_ENABLED)

#include "gs_live_editor_observation.h"
#include "core/object/ref_counted.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "scene/main/node.h"

class GSLiveEditorTestRunner : public RefCounted {
	GDCLASS(GSLiveEditorTestRunner, RefCounted);
	static GSLiveEditorTestRunner *current;
	static Ref<GSLiveEditorTestRunner> running_owner;
	GSLiveEditorObservation::Observer *observer = nullptr;
	ErrorHandlerList handler;
	Vector<ObjectID> borrowed;
	uintptr_t queue = 0, main_queue = 0;
	ObjectID gui, plain, advanced, dialog_owner, fixture;
	bool armed = false, draining = false, handler_installed = false, invalid = false;
	bool factory_armed = false, factory_advanced = false, fixture_pre_repair = true;
	bool fixture_parented = false, logger_initial = true, pending_reported = false;
	uint64_t armed_usec = 0, drain_usec = 0;
	uint32_t case_count = 0, created_total = 0, closed_total = 0, created_case = 0, closed_case = 0;
	uint32_t stable_frames = 0;
	int context_result = 1, assertions = 0, failed_assertions = 0;
	String token, case_name;
	bool _capture();
	bool _identities_valid() const;
	bool _queues_safe() const;
	void _process_frame();
	void _finish();
	void _case_start(const char *p_name);
	void _case_end(int p_assertions, int p_failed, bool p_success);
	void _run_end(unsigned p_cases, unsigned p_failed_cases, int p_assertions, int p_failed_assertions);
	bool _preview_closed(uint32_t p_case) const;
	bool _all_deliveries_closed() const;
	Array _borrowed_ids() const;
	void _restore_logger();
	void _restore_dialogs();
	bool _drain_on_time(uint64_t p_now) const;
	void _emit(Dictionary &p_data);
	static void _error(void *p_userdata, const char *p_function, const char *p_file, int p_line,
			const char *p_error, const char *p_message, bool p_notify, ErrorHandlerType p_type);
protected:
	static void _bind_methods();
public:
	bool arm();
	static GSLiveEditorTestRunner *get_current() { return current; }
	bool begin_fixture(bool p_advanced);
	Node *finish_fixture();
	void end_fixture();
	Object *preserve_publication(bool p_advanced, Object *p_constructing, bool p_destroying);
	~GSLiveEditorTestRunner();
};

class GSLiveEditorFixtureLease {
	GSLiveEditorTestRunner *runner = nullptr;
public:
	template <class T>
	T *create(bool p_advanced = false) {
		runner = GSLiveEditorTestRunner::get_current();
		if (!runner) { return memnew(T); }
		if (!runner->begin_fixture(p_advanced)) { return nullptr; }
		T *constructed = memnew(T);
		if (!constructed) { runner->end_fixture(); return nullptr; }
		return static_cast<T *>(runner->finish_fixture());
	}
	~GSLiveEditorFixtureLease() { if (runner) { runner->end_fixture(); } }
};
#endif
