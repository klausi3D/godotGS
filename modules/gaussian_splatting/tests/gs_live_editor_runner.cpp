#include "gs_live_editor_runner.h"
#if defined(TESTS_ENABLED) && defined(TOOLS_ENABLED)

#include "core/config/engine.h"
#include "core/core_globals.h"
#include "core/input/input.h"
#include "core/input/input_map.h"
#include "core/io/json.h"
#include "core/io/file_access.h"
#include "core/io/dir_access.h"
#include "core/crypto/hashing_context.h"
#include "synthetic_ply_writer.h"
#include "synthetic_spz_writer.h"
#include "modules/gaussian_splatting/io/ply_loader.h"
#include "modules/gaussian_splatting/io/spz_loader.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

#include <initializer_list>
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/file_system/editor_paths.h"
#include "editor/inspector/editor_resource_preview.h"
#include "editor/settings/editor_settings.h"
#include "modules/gaussian_splatting/editor/gaussian_editor_plugin.h"
#include "modules/gaussian_splatting/editor/gaussian_import_dialog.h"
#include "modules/gaussian_splatting/editor/gaussian_import_settings_dialog.h"
#include "scene/main/scene_tree.h"
#include "servers/display_server.h"
#include "servers/rendering_server.h"
#include "tests/test_main.h"

using namespace GSLiveEditorObservation;
GSLiveEditorTestRunner *GSLiveEditorTestRunner::current = nullptr;
Ref<GSLiveEditorTestRunner> GSLiveEditorTestRunner::running_owner;

// Only this frozen native process produces these two fresh-project inputs.
bool GSLiveEditorTestRunner::_fingerprint_input(const String &p_path, InputFingerprint &r_value) const {
	Ref<DirAccess> directory = DirAccess::open("res://");
	if (directory.is_null() || directory->is_link(p_path) || directory->dir_exists(p_path)) { return false; }
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) { return false; }
	const uint64_t size = file->get_length();
	if (!size || size > 65536) { return false; }
	PackedByteArray bytes;
	if (bytes.resize(size) != OK || file->get_buffer(bytes.ptrw(), size) != size || file->get_length() != size || file->get_error() != OK) { return false; }
	Ref<HashingContext> hash;
	hash.instantiate();
	if (hash.is_null() || hash->start(HashingContext::HASH_SHA256) != OK || hash->update(bytes) != OK) { return false; }
	const PackedByteArray digest = hash->finish();
	if (digest.size() != 32) { return false; }
	r_value.path = p_path; r_value.bytes = size;
	r_value.sha256 = String::hex_encode_buffer(digest.ptr(), digest.size());
	return true;
}
bool GSLiveEditorTestRunner::_loader_values_valid(const Ref<GaussianData> &p_data) const {
	if (p_data.is_null() || p_data->get_count() != 3) { return false; }
	const Vector3 positions[3] = { Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(1, 0, 1) };
	for (int i = 0; i < 3; ++i) {
		const Gaussian g = p_data->get_gaussian(i);
		if (g.position != positions[i]) { return false; }
		for (real_t value : { real_t(g.opacity), real_t(g.sh_dc.r), real_t(g.sh_dc.g), real_t(g.sh_dc.b) }) {
			if (!Math::is_finite(value) || Math::abs(value - real_t(128.0 / 255.0)) > 0.00001) { return false; }
		}
		for (real_t value : { g.scale.x, g.scale.y, g.scale.z, g.rotation.w }) {
			if (!Math::is_finite(value) || Math::abs(value - 1) > 0.00001) { return false; }
		}
		for (real_t value : { g.rotation.x, g.rotation.y, g.rotation.z }) {
			if (!Math::is_finite(value) || Math::abs(value) > 0.00001) { return false; }
		}
	}
	return true;
}
bool GSLiveEditorTestRunner::_write_input_fixtures() const {
	const Vector3 positions[3] = { Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(1, 0, 1) };
	LocalVector<Gaussian> ply;
	LocalVector<TestGaussianSplatting::SyntheticSpzSplat> spz;
	for (int i = 0; i < 3; ++i) {
		Gaussian g{};
		g.position = positions[i]; g.opacity = 128.0f / 255.0f;
		g.sh_dc = Color(128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, 1);
		g.scale = Vector3(1, 1, 1); g.rotation = Quaternion();
		ply.push_back(g);
		TestGaussianSplatting::SyntheticSpzSplat s;
		s.position = g.position; s.opacity = g.opacity; s.color = g.sh_dc; s.scale = g.scale; s.rotation = g.rotation;
		spz.push_back(s);
	}
	return TestGaussianSplatting::write_gaussian_ply("res://test_splats.ply", ply, false, false) &&
			TestGaussianSplatting::write_synthetic_spz("res://test_splats.spz", spz, 12);
}
bool GSLiveEditorTestRunner::_load_input_fixture(int p_index) const {
	if (p_index == 0) {
		Ref<PLYLoader> loader;
		loader.instantiate();
		return loader.is_valid() && loader->load_file(input_files[0].path) == OK && _loader_values_valid(loader->get_gaussian_data());
	}
	Ref<SPZLoader> loader;
	loader.instantiate();
	return loader.is_valid() && loader->load_file(input_files[1].path) == OK && _loader_values_valid(loader->get_gaussian_data());
}
bool GSLiveEditorTestRunner::_prepare_input_fixtures() {
	Ref<DirAccess> directory = DirAccess::open("res://");
	if (directory.is_null()) { return false; }
	for (const String &path : { String("res://test_splats.ply"), String("res://test_splats.spz") }) {
		if (directory->file_exists(path) || directory->dir_exists(path) || directory->is_link(path)) { return false; }
	}
	if (!_write_input_fixtures()) { return false; }
	for (int i = 0; i < 2; ++i) {
		const String path = i == 0 ? "res://test_splats.ply" : "res://test_splats.spz";
		if (!_fingerprint_input(path, input_files[i]) || !_load_input_fixture(i)) { return false; }
	}
	inputs_ready = true;
	return _inputs_unchanged();
}
bool GSLiveEditorTestRunner::_inputs_unchanged() const {
	if (!inputs_ready) { return false; }
	for (int i = 0; i < 2; ++i) {
		InputFingerprint now;
		if (!_fingerprint_input(input_files[i].path, now) || now.bytes != input_files[i].bytes || now.sha256 != input_files[i].sha256) { return false; }
	}
	return true;
}
Array GSLiveEditorTestRunner::_input_receipt_rows() const {
	Array rows;
	if (!inputs_ready) { return rows; }
	for (int i = 0; i < 2; ++i) {
		Dictionary row;
		row["path"] = i == 0 ? "test_splats.ply" : "test_splats.spz";
		row["format"] = i == 0 ? "ply" : "spz";
		row["producer"] = i == 0 ? "write_gaussian_ply" : "write_synthetic_spz";
		row["bytes"] = int64_t(input_files[i].bytes); row["sha256"] = input_files[i].sha256;
		row["count"] = 3; row["loader_ok"] = true; row["values_ok"] = true;
		rows.push_back(row);
	}
	return rows;
}

static Vector<Object *> live_editor_objects() {
	Vector<Object *> objects;
	objects.push_back(EditorNode::get_singleton());
	objects.push_back(SceneTree::get_singleton());
	objects.push_back(DisplayServer::get_singleton());
	objects.push_back(RenderingServer::get_singleton());
	objects.push_back(Input::get_singleton());
	objects.push_back(InputMap::get_singleton());
	objects.push_back(EditorPaths::get_singleton());
	objects.push_back(EditorSettings::get_singleton());
	objects.push_back(EditorFileSystem::get_singleton());
	objects.push_back(EditorResourcePreview::get_singleton());
	return objects;
}
bool GSLiveEditorTestRunner::_drain_on_time(uint64_t p_now) const {
	return p_now >= drain_usec && p_now - drain_usec <= 45000000;
}
void GSLiveEditorTestRunner::_emit(Dictionary &data) {
	data["token"] = token;
	data["pid"] = OS::get_singleton()->get_process_id();
	String payload = JSON::stringify(data);
	// Formatting can itself consume the last part of the drain budget. Fence the
	// actual PASS publication after that work, not only at frame entry.
	if (data["kind"] == "final" && (!draining || !_drain_on_time(OS::get_singleton()->get_ticks_usec()))) {
		invalid = true;
		data["valid"] = false;
		data["drained"] = false;
		payload = JSON::stringify(data);
	}
	print_line("[GS-LIVE-EDITOR] " + payload);
	// A blocked output sink cannot convert an overrun into authoritative PASS.
	// The parent requires the whole output and native exit; latch before quit.
	if (data["kind"] == "final" && (!draining || !_drain_on_time(OS::get_singleton()->get_ticks_usec()))) {
		invalid = true;
		data["valid"] = false;
		data["drained"] = false;
		print_line("[GS-LIVE-EDITOR INVALID] final publication exceeded drain budget");
	}
}
bool GSLiveEditorTestRunner::_queues_safe() const {
	CallQueue *q = MessageQueue::get_singleton();
	CallQueue *m = MessageQueue::get_main_singleton();
	return q && m && !q->is_flushing() && !m->is_flushing() &&
			(!queue || queue == reinterpret_cast<uintptr_t>(q)) && (!main_queue || main_queue == reinterpret_cast<uintptr_t>(m));
}
bool GSLiveEditorTestRunner::_capture_context_ready(EditorNode *editor, Control *base, GaussianImportDialog *a, GaussianImportSettingsDialog *b) const {
	if (!base || !base->is_inside_tree() || !editor->is_inside_tree() || !a || !b ||
			!a->is_inside_tree() || !b->is_inside_tree() || !a->get_parent() || a->get_parent() != b->get_parent() ||
			!Object::cast_to<GaussianEditorPlugin>(a->get_parent()) || EditorFileSystem::get_singleton()->is_scanning()) { return false; }
	return true;
}

bool GSLiveEditorTestRunner::_scheduled_delivery_closed(const Record &r, uint32_t count) const {
	uint32_t entries = 0, completions = 0;
	for (uint32_t j = 0; j < count; ++j) {
		const Record &c = observer->records[j];
		if (!c.ready.load(std::memory_order_acquire)) { return false; }
		if (c.case_id != r.case_id || c.generator != r.generator || c.token != r.token) { continue; }
		if (c.kind == PREVIEW_ENTRY) { ++entries; }
		if (c.kind == PREVIEW_COMPLETE) { ++completions; }
	}
	if (entries != 1 || completions != 1) { return false; }
	return true;
}

bool GSLiveEditorTestRunner::_parse_user_arguments(const List<String> &args, bool &mode, bool &include, bool &exclude) {
	for (const String &arg : args) {
		if (arg == "--gs-live-editor-tests") { mode = true; }
		else if (arg == "--test-case=*GaussianSplatting*][Editor]*") { include = true; }
		else if (arg == "--test-case-exclude=*][RequiresGPU]*") { exclude = true; }
		else if (arg.begins_with("--gs-live-editor-token=")) { token = arg.trim_prefix("--gs-live-editor-token="); }
		else { return false; }
	}
	return true;
}

void GSLiveEditorTestRunner::_encode_event(Dictionary &d, const Record &r) const {
	d["kind"] = "event"; d["sequence"] = r.sequence; d["event"] = r.kind;
	d["case"] = r.case_id; d["thread"] = String::num_uint64(r.thread); d["phase"] = r.phase;
	d["generator"] = String::num_uint64(r.generator); d["asset"] = String::num_uint64(r.asset);
	d["token_id"] = String::num_uint64(r.token); d["arm"] = String::num_uint64(r.arm);
	d["value"] = r.value; d["expected"] = r.expected; d["notify"] = r.notify;
	d["function"] = r.function; d["file"] = r.file; d["line"] = r.line; d["error"] = r.error; d["message"] = r.message;
}

void GSLiveEditorTestRunner::_encode_final(Dictionary &d, uint64_t final_now, bool final_on_time) const {
	d["kind"] = "final";
	d["inputs_valid"] = inputs_ready && !invalid; d["input_files"] = _input_receipt_rows();
	d["drain_elapsed_usec"] = String::num_uint64(final_now >= drain_usec ? final_now - drain_usec : UINT64_MAX);
	d["valid"] = observer && !invalid && !context_result && case_count == 23 && !failed_assertions;
	d["cases"] = case_count; d["assertions"] = assertions; d["failed_assertions"] = failed_assertions;
	d["created"] = created_total; d["closed"] = closed_total; d["logger_restored"] = CoreGlobals::print_error_enabled == logger_initial;
	d["borrowed_ids"] = _borrowed_ids();
	d["drained"] = final_on_time && stable_frames >= 2 && _all_deliveries_closed(); d["borrowed_valid"] = observer && _identities_valid();
}

bool GSLiveEditorTestRunner::_capture() {
	if (!Thread::is_main_thread() || !Engine::get_singleton()->is_editor_hint() || !_queues_safe() || !CoreGlobals::print_error_enabled) { return false; }
	Vector<Object *> objects = live_editor_objects();
	for (Object *object : objects) { if (!object) { return false; } }
	EditorNode *editor = EditorNode::get_singleton();
	Control *base = editor->get_gui_base();
	GaussianImportDialog *a = GaussianImportDialog::get_singleton();
	GaussianImportSettingsDialog *b = GaussianImportSettingsDialog::get_singleton();
	if (!_capture_context_ready(editor, base, a, b)) { return false; }
	borrowed.clear();
	for (Object *object : objects) { borrowed.push_back(object->get_instance_id()); }
	gui = base->get_instance_id(); plain = a->get_instance_id(); advanced = b->get_instance_id();
	dialog_owner = a->get_parent()->get_instance_id();
	queue = reinterpret_cast<uintptr_t>(MessageQueue::get_singleton());
	main_queue = reinterpret_cast<uintptr_t>(MessageQueue::get_main_singleton());
	logger_initial = CoreGlobals::print_error_enabled;
	return _identities_valid();
}
bool GSLiveEditorTestRunner::_identities_valid() const {
	if (!Thread::is_main_thread() || !Engine::get_singleton()->is_editor_hint()) { return false; }
	Vector<Object *> objects = live_editor_objects();
	if (objects.size() != borrowed.size()) { return false; }
	for (int i = 0; i < objects.size(); ++i) {
		if (!objects[i] || objects[i]->get_instance_id() != borrowed[i] || ObjectDB::get_instance(borrowed[i]) != objects[i]) { return false; }
	}
	Control *base = Object::cast_to<Control>(ObjectDB::get_instance(gui));
	GaussianImportDialog *a = Object::cast_to<GaussianImportDialog>(ObjectDB::get_instance(plain));
	GaussianImportSettingsDialog *b = Object::cast_to<GaussianImportSettingsDialog>(ObjectDB::get_instance(advanced));
	Node *owner = Object::cast_to<GaussianEditorPlugin>(ObjectDB::get_instance(dialog_owner));
	return base && base == EditorNode::get_singleton()->get_gui_base() && base->is_inside_tree() && owner &&
			a && b && a->get_parent() == owner && b->get_parent() == owner && a->is_inside_tree() && b->is_inside_tree() &&
			GaussianImportDialog::get_singleton() == a && GaussianImportSettingsDialog::get_singleton() == b;
}
void GSLiveEditorTestRunner::_restore_dialogs() {
	GaussianImportDialog *a = Object::cast_to<GaussianImportDialog>(ObjectDB::get_instance(plain));
	GaussianImportSettingsDialog *b = Object::cast_to<GaussianImportSettingsDialog>(ObjectDB::get_instance(advanced));
	Node *owner = Object::cast_to<GaussianEditorPlugin>(ObjectDB::get_instance(dialog_owner));
	if (!a || !b || !owner || a->get_parent() != owner || b->get_parent() != owner) { invalid = true; return; }
	GaussianImportDialog::_test_restore_live_singleton(a);
	GaussianImportSettingsDialog::_test_restore_live_singleton(b);
}
void GSLiveEditorTestRunner::_restore_logger() {
	if (CoreGlobals::print_error_enabled != logger_initial) { invalid = true; }
	CoreGlobals::print_error_enabled = logger_initial;
}
Object *GSLiveEditorTestRunner::preserve_publication(bool is_advanced, Object *constructing, bool destroying) {
	Object *genuine = ObjectDB::get_instance(is_advanced ? advanced : plain);
	Node *node = Object::cast_to<Node>(genuine);
	if (!Thread::is_main_thread() || !node || node->get_parent() != ObjectDB::get_instance(dialog_owner) ||
			(destroying ? fixture != constructing->get_instance_id() : (!factory_armed || factory_advanced != is_advanced || fixture.is_valid()))) {
		invalid = true;
	}
	if (!destroying && factory_armed && factory_advanced == is_advanced) {
		fixture = constructing->get_instance_id();
	}
	return node;
}
bool GSLiveEditorTestRunner::begin_fixture(bool is_advanced) {
	if (factory_armed || fixture.is_valid() || !_identities_valid() || !_queues_safe() || ++created_case != 1) { invalid = true; return false; }
	factory_armed = true; factory_advanced = is_advanced; ++created_total;
	return true;
}
Node *GSLiveEditorTestRunner::finish_fixture() {
	Node *node = Object::cast_to<Node>(ObjectDB::get_instance(fixture));
	Control *base = Object::cast_to<Control>(ObjectDB::get_instance(gui));
	if (!factory_armed || !node || !base || !_identities_valid() || node->get_parent()) { invalid = true; return nullptr; }
	base->add_child(node); // Full memnew/derived post-initialization already returned.
	node = Object::cast_to<Node>(ObjectDB::get_instance(fixture));
	if (!node || node->get_parent() != base || !node->is_inside_tree() || !_identities_valid()) { invalid = true; return nullptr; }
	fixture_parented = true;
	return node;
}
void GSLiveEditorTestRunner::end_fixture() {
	if (!factory_armed) { return; }
	fixture_pre_repair = fixture_pre_repair && _identities_valid();
	if (!fixture_pre_repair) { invalid = true; }
	_restore_logger();
	Node *node = Object::cast_to<Node>(ObjectDB::get_instance(fixture));
	if (node) { memdelete(node); }
	if (ObjectDB::get_instance(fixture)) { invalid = true; }
	else if (fixture.is_valid()) { ++closed_case; ++closed_total; }
	_restore_dialogs();
	fixture = ObjectID(); factory_armed = false;
}

// Executed under core _global_lock: no module calls, allocations, waits or second lock.
void GSLiveEditorTestRunner::_error(void *userdata, const char *function, const char *file, int line,
		const char *error, const char *message, bool notify, ErrorHandlerType type) {
	Observer *o = static_cast<Observer *>(userdata);
	uint32_t n = o->next.load(std::memory_order_relaxed);
	while (n < Observer::CAPACITY && !o->next.compare_exchange_weak(n, n + 1, std::memory_order_relaxed)) {}
	if (n >= Observer::CAPACITY) { o->invalid.store(true, std::memory_order_release); return; }
	Record &r = o->records[n];
	r.sequence = n + 1; r.kind = ERROR_EVENT; r.case_id = o->current_case.load(std::memory_order_acquire);
	r.thread = Thread::get_caller_id(); r.value = type; r.line = line; r.notify = notify;
	bool bounded = true;
#define GS_ERROR_COPY(m_field, m_source) \
	do { \
		const char *src = m_source; bool terminated = false; \
		if (src) { for (size_t j = 0; j < sizeof(r.m_field); ++j) { r.m_field[j] = src[j]; if (!src[j]) { terminated = true; break; } } } \
		if (!terminated) { r.m_field[sizeof(r.m_field) - 1] = '\0'; bounded = false; } \
	} while (false)
	GS_ERROR_COPY(function, function); GS_ERROR_COPY(file, file);
	GS_ERROR_COPY(error, error); GS_ERROR_COPY(message, message);
#undef GS_ERROR_COPY
	bool expected = bounded && emission.active && !emission.claimed && r.thread == o->owner && r.thread > 0 &&
			emission.case_id == r.case_id && emission.phase >= 1 && emission.phase <= 2 && emission.invocation > 0 &&
			emission.arm == emission.phase && emission.generator && emission.asset && type == ERR_HANDLER_ERROR && !notify &&
			emission.line == line && emission.function && emission.file && !strcmp(function, emission.function) && !strcmp(file, emission.file) &&
			!strcmp(error, "Method/function failed. Returning: false") && !strcmp(message, RATIONALE);
	r.expected = expected;
	if (expected) {
		emission.claimed = true; r.phase = emission.phase; r.arm = emission.arm;
		r.token = emission.invocation; r.generator = emission.generator; r.asset = emission.asset;
	}
	if (!bounded || (!expected && type != ERR_HANDLER_WARNING)) { o->invalid.store(true, std::memory_order_release); }
	r.ready.store(true, std::memory_order_release);
}

bool GSLiveEditorTestRunner::_preview_closed(uint32_t id) const {
	const Record *schedule = nullptr, *entry = nullptr, *complete = nullptr, *join = nullptr;
	const uint32_t count = observer->next.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < count && i < Observer::CAPACITY; ++i) {
		const Record &r = observer->records[i];
		if (!r.ready.load(std::memory_order_acquire)) { return false; }
		if (r.case_id != id) { continue; }
		const Record **slot = nullptr;
		if (r.kind == PREVIEW_SCHEDULE) { slot = &schedule; }
		if (r.kind == PREVIEW_ENTRY) { slot = &entry; }
		if (r.kind == PREVIEW_COMPLETE) { slot = &complete; }
		if (r.kind == PREVIEW_JOIN) { slot = &join; }
		if (slot) { if (*slot) { return false; } *slot = &r; }
	}
	return schedule && entry && complete && join && schedule->value == OK && schedule->thread > 0 &&
			schedule->thread != observer->owner && join->token == schedule->thread && join->thread == observer->owner &&
			schedule->token == entry->token && entry->token == complete->token && schedule->generator == entry->generator &&
			entry->generator == complete->generator && join->generator == complete->generator &&
			entry->thread == observer->owner && complete->thread == observer->owner &&
			schedule->sequence < join->sequence && join->sequence < entry->sequence && entry->sequence < complete->sequence;
}
bool GSLiveEditorTestRunner::_all_deliveries_closed() const {
	if (!observer) { return true; }
	const uint32_t count = observer->next.load(std::memory_order_acquire);
	if (count > Observer::CAPACITY) { return false; }
	for (uint32_t i = 0; i < count; ++i) {
		const Record &r = observer->records[i];
		if (!r.ready.load(std::memory_order_acquire)) { return false; }
		if (r.kind != PREVIEW_SCHEDULE || r.value != OK) { continue; }
		if (!_scheduled_delivery_closed(r, count)) { return false; }
	}
	return true;
}
Array GSLiveEditorTestRunner::_borrowed_ids() const {
	Array ids;
	for (Object *object : live_editor_objects()) { ids.push_back(object ? String::num_uint64(uint64_t(object->get_instance_id())) : String("0")); }
	for (ObjectID id : { gui, plain, advanced, dialog_owner }) {
		Object *object = ObjectDB::get_instance(id);
		ids.push_back(object ? String::num_uint64(uint64_t(object->get_instance_id())) : String("0"));
	}
	return ids;
}
void GSLiveEditorTestRunner::_case_start(const char *name) {
	++case_count; case_name = String::utf8(name);
	input_case_valid = _inputs_unchanged();
	if (!input_case_valid) { invalid = true; }
	if (!_identities_valid() || !_queues_safe() || case_count > 23) { invalid = true; }
	_restore_logger();
	created_case = 0; closed_case = 0; fixture_parented = false; fixture_pre_repair = true;
	observer->current_case.store(case_count, std::memory_order_release);
	observer->thumbnail_case.store(case_name.contains("A failed thumbnail canvas allocation is not cached"), std::memory_order_release);
	observer->preview_case.store(case_name.contains("(#592)"), std::memory_order_release);
	observer->preview_generator.store(0, std::memory_order_release);
}
void GSLiveEditorTestRunner::_case_end(int count, int failed, bool success) {
	const bool inputs_valid = _inputs_unchanged() && input_case_valid;
	if (!inputs_valid) { invalid = true; }
	const bool logger_before = CoreGlobals::print_error_enabled == logger_initial;
	const bool identities_before = _identities_valid() && fixture_pre_repair;
	const bool callback_closed = !observer->preview_case.load() || _preview_closed(case_count);
	if (!count || failed || !success || !logger_before || !identities_before || !callback_closed || factory_armed ||
			created_case != closed_case || (created_case && !fixture_parented)) { invalid = true; }
	_restore_logger(); end_fixture(); _restore_dialogs();
	Dictionary d;
	d["inputs_valid"] = inputs_valid; d["input_files"] = _input_receipt_rows();
	d["kind"] = "case"; d["ordinal"] = case_count; d["name"] = case_name;
	d["assertions"] = count; d["failed"] = failed; d["borrowed_valid"] = identities_before;
	d["logger_before_restore"] = logger_before; d["callback_closed"] = callback_closed;
	d["fixture_created"] = created_case; d["fixture_closed"] = closed_case; d["fixture_parented"] = fixture_parented;
	_emit(d);
}
void GSLiveEditorTestRunner::_run_end(unsigned cases, unsigned failed_cases, int count, int failed) {
	assertions = count; failed_assertions = failed;
	if (cases != 23 || case_count != cases || failed_cases || !count || failed) { invalid = true; }
}
void GSLiveEditorTestRunner::_bind_methods() { ClassDB::bind_method(D_METHOD("arm"), &GSLiveEditorTestRunner::arm); }
bool GSLiveEditorTestRunner::arm() {
	if (!Thread::is_main_thread() || armed || running_owner.is_valid() || current || active.load()) { return false; }
	const List<String> &args = OS::get_singleton()->get_cmdline_user_args();
	bool mode = false, include = false, exclude = false;
	if (!_parse_user_arguments(args, mode, include, exclude)) { return false; }
	if (!mode || !include || !exclude || token.length() != 32 || !token.is_valid_hex_number(false) || !SceneTree::get_singleton()) { return false; }
	if (SceneTree::get_singleton()->connect("process_frame", callable_mp(this, &GSLiveEditorTestRunner::_process_frame)) != OK) { return false; }
	armed = true; armed_usec = OS::get_singleton()->get_ticks_usec();
	running_owner = Ref<GSLiveEditorTestRunner>(this);
	return true;
}
void GSLiveEditorTestRunner::_process_frame() {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	if (!Thread::is_main_thread() || !_queues_safe()) { invalid = true; _finish(); return; }
	if (!draining) {
		_start_test_context(now);
		return;
	}
	_drain_test_context(now);
}
void GSLiveEditorTestRunner::_start_test_context(uint64_t now) {
	if (!CoreGlobals::print_error_enabled) { invalid = true; _finish(); return; }
	if (now - armed_usec > 120000000) { invalid = true; _finish(); return; }
	if (!_capture()) { return; }
	observer = memnew(Observer);
	if (!observer) { invalid = true; _finish(); return; }
	observer->owner = Thread::get_caller_id();
	Observer *zero = nullptr;
	if (!active.compare_exchange_strong(zero, observer)) { invalid = true; _finish(); return; }
	current = this;
	handler.userdata = observer; handler.errfunc = _error;
	add_error_handler(&handler); handler_installed = true;
	Dictionary begin; begin["kind"] = "begin"; begin["owner"] = String::num_uint64(observer->owner);

	begin["borrowed_ids"] = _borrowed_ids(); begin["logger_captured"] = logger_initial; begin["queues_not_flushing"] = true;
	_emit(begin);
	if (!_prepare_input_fixtures()) { invalid = true; _finish(); return; }
	Dictionary inputs; inputs["kind"] = "input-fixtures"; inputs["recipe"] = "gs-live-editor-inputs-v1";
	inputs["files"] = _input_receipt_rows(); _emit(inputs);
	const uint64_t prepared_now = OS::get_singleton()->get_ticks_usec();
	if (prepared_now < armed_usec || prepared_now - armed_usec > 120000000 || !_identities_valid() || !_queues_safe() || !CoreGlobals::print_error_enabled || observer->invalid.load(std::memory_order_acquire)) { invalid = true; _finish(); return; }
	BorrowedEditorTestHooks hooks;
	hooks.userdata = this;
	hooks.case_start = [](void *u, const char *n) { static_cast<GSLiveEditorTestRunner *>(u)->_case_start(n); };
	hooks.case_end = [](void *u, int a, int f, bool s) { static_cast<GSLiveEditorTestRunner *>(u)->_case_end(a, f, s); };
	hooks.run_end = [](void *u, unsigned c, unsigned f, int a, int e) { static_cast<GSLiveEditorTestRunner *>(u)->_run_end(c, f, a, e); };
	Vector<String> test_args;
	test_args.push_back("live-editor");
	test_args.push_back("--test-case=*GaussianSplatting*][Editor]*");
	test_args.push_back("--test-case-exclude=*][RequiresGPU]*");
	context_result = run_borrowed_editor_tests(hooks, test_args);
	_restore_logger(); end_fixture(); _restore_dialogs();
	draining = true; drain_usec = OS::get_singleton()->get_ticks_usec();
	return;
}

void GSLiveEditorTestRunner::_drain_test_context(uint64_t now) {
	if (!_drain_on_time(now)) { invalid = true; _finish(); return; }
	_restore_logger();
	if (!_identities_valid() || factory_armed || fixture.is_valid()) { invalid = true; }
	MessageQueue::get_main_singleton()->flush();
	if (MessageQueue::get_singleton() != MessageQueue::get_main_singleton()) { MessageQueue::get_singleton()->flush(); }
	if (!_identities_valid()) { invalid = true; }
	if (!_drain_on_time(OS::get_singleton()->get_ticks_usec())) { invalid = true; _finish(); return; }
	if (!_all_deliveries_closed()) { stable_frames = 0; return; }
	if (stable_frames++ >= 1) { _finish(); }
}

void GSLiveEditorTestRunner::_finish() {
	if (!armed) { return; }
	if (!_all_deliveries_closed()) {
		invalid = true;
		// The parent watchdog owns this isolated process. Do not remove userdata,
		// quit normally, cancel a callable or claim a terminal lifetime receipt.
		if (!pending_reported) { Dictionary d; d["kind"] = "invalid_pending_delivery"; _emit(d); pending_reported = true; }
		return;
	}
	if (!_inputs_unchanged()) { invalid = true; }
	if (observer) {
		Observer *expected_active = observer;
		active.compare_exchange_strong(expected_active, nullptr, std::memory_order_seq_cst);
		if (helper_readers.load(std::memory_order_seq_cst) != 0) {
			invalid = true;
			if (!pending_reported) { Dictionary d; d["kind"] = "invalid_pending_helper"; _emit(d); pending_reported = true; }
			return;
		}
		_restore_logger(); end_fixture(); _restore_dialogs();
		if (_observer_final_state_invalid()) { invalid = true; }
		if (handler_installed) { remove_error_handler(&handler); handler_installed = false; }
		Observer *expected = observer; active.compare_exchange_strong(expected, nullptr);
		invalid = invalid || observer->invalid.load(std::memory_order_acquire) || helper_overflow.load(std::memory_order_seq_cst);
		const uint32_t count = observer->next.load(std::memory_order_acquire);
		_emit_observer_records(count);
	}
	const uint64_t final_now = OS::get_singleton()->get_ticks_usec();
	const bool final_on_time = draining && _drain_on_time(final_now);
	if (!final_on_time) { invalid = true; }
	Dictionary d; _encode_final(d, final_now, final_on_time);
	_emit(d);
	if (current == this) { current = nullptr; }
	if (observer) { memdelete(observer); observer = nullptr; }
	SceneTree *tree = SceneTree::get_singleton();
	if (tree) {
		tree->disconnect("process_frame", callable_mp(this, &GSLiveEditorTestRunner::_process_frame));
		tree->quit(bool(d["valid"]) && bool(d["drained"]) ? 0 : 1);
	}
	armed = false;
	Ref<GSLiveEditorTestRunner> keepalive = running_owner;
	running_owner.unref();
}
bool GSLiveEditorTestRunner::_observer_final_state_invalid() const {
	return !_identities_valid() || !_queues_safe() || created_total != 7 || closed_total != 7 || observer->arms.load() != 2 || observer->invocations.load() != 2;
}
void GSLiveEditorTestRunner::_emit_observer_records(uint32_t count) {
	for (uint32_t i = 0; i < count && i < Observer::CAPACITY; ++i) {
		const Record &r = observer->records[i];
		if (!r.ready.load(std::memory_order_acquire)) { invalid = true; continue; }
		Dictionary d; _encode_event(d, r);
		_emit(d);
	}
}
GSLiveEditorTestRunner::~GSLiveEditorTestRunner() {
	if (armed) { invalid = true; _finish(); }
}
#endif
