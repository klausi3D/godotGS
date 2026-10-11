/**************************************************************************/
/*  test_main.cpp                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "test_main.h"

#include "core/error/error_macros.h"
#include "core/io/dir_access.h"
#if defined(TESTS_ENABLED) && defined(TOOLS_ENABLED)
#include "core/os/thread.h"
#endif
#include "modules/modules_enabled.gen.h"

#ifdef TOOLS_ENABLED
#include "editor/file_system/editor_paths.h"
#include "editor/settings/editor_settings.h"
#endif // TOOLS_ENABLED

#include "tests/core/config/test_project_settings.h"
#include "tests/core/input/test_input_event.h"
#include "tests/core/input/test_input_event_key.h"
#include "tests/core/input/test_input_event_mouse.h"
#include "tests/core/input/test_shortcut.h"
#include "tests/core/io/test_config_file.h"
#include "tests/core/io/test_file_access.h"
#include "tests/core/io/test_http_client.h"
#include "tests/core/io/test_image.h"
#include "tests/core/io/test_ip.h"
#include "tests/core/io/test_json.h"
#include "tests/core/io/test_json_native.h"
#include "tests/core/io/test_logger.h"
#include "tests/core/io/test_marshalls.h"
#include "tests/core/io/test_packet_peer.h"
#include "tests/core/io/test_pck_packer.h"
#include "tests/core/io/test_resource.h"
#include "tests/core/io/test_resource_uid.h"
#include "tests/core/io/test_stream_peer.h"
#include "tests/core/io/test_stream_peer_buffer.h"
#include "tests/core/io/test_stream_peer_gzip.h"
#include "tests/core/io/test_tcp_server.h"
#include "tests/core/io/test_udp_server.h"
#include "tests/core/io/test_xml_parser.h"
#include "tests/core/math/test_aabb.h"
#include "tests/core/math/test_astar.h"
#include "tests/core/math/test_basis.h"
#include "tests/core/math/test_color.h"
#include "tests/core/math/test_expression.h"
#include "tests/core/math/test_geometry_2d.h"
#include "tests/core/math/test_geometry_3d.h"
#include "tests/core/math/test_math_funcs.h"
#include "tests/core/math/test_plane.h"
#include "tests/core/math/test_projection.h"
#include "tests/core/math/test_quaternion.h"
#include "tests/core/math/test_random_number_generator.h"
#include "tests/core/math/test_rect2.h"
#include "tests/core/math/test_rect2i.h"
#include "tests/core/math/test_transform_2d.h"
#include "tests/core/math/test_transform_3d.h"
#include "tests/core/math/test_vector2.h"
#include "tests/core/math/test_vector2i.h"
#include "tests/core/math/test_vector3.h"
#include "tests/core/math/test_vector3i.h"
#include "tests/core/math/test_vector4.h"
#include "tests/core/math/test_vector4i.h"
#include "tests/core/object/test_class_db.h"
#include "tests/core/object/test_method_bind.h"
#include "tests/core/object/test_object.h"
#include "tests/core/object/test_undo_redo.h"
#include "tests/core/os/test_os.h"
#include "tests/core/string/test_fuzzy_search.h"
#include "tests/core/string/test_node_path.h"
#include "tests/core/string/test_string.h"
#include "tests/core/string/test_translation.h"
#include "tests/core/string/test_translation_server.h"
#include "tests/core/templates/test_a_hash_map.h"
#include "tests/core/templates/test_command_queue.h"
#include "tests/core/templates/test_fixed_vector.h"
#include "tests/core/templates/test_hash_map.h"
#include "tests/core/templates/test_hash_set.h"
#include "tests/core/templates/test_list.h"
#include "tests/core/templates/test_local_vector.h"
#include "tests/core/templates/test_lru.h"
#include "tests/core/templates/test_paged_array.h"
#include "tests/core/templates/test_rid.h"
#include "tests/core/templates/test_self_list.h"
#include "tests/core/templates/test_span.h"
#include "tests/core/templates/test_vector.h"
#include "tests/core/templates/test_vset.h"
#include "tests/core/test_crypto.h"
#include "tests/core/test_hashing_context.h"
#include "tests/core/test_time.h"
#include "tests/core/threads/test_worker_thread_pool.h"
#include "tests/core/variant/test_array.h"
#include "tests/core/variant/test_callable.h"
#include "tests/core/variant/test_dictionary.h"
#include "tests/core/variant/test_variant.h"
#include "tests/core/variant/test_variant_utility.h"
#include "tests/scene/test_animation.h"
#include "tests/scene/test_animation_blend_tree.h"
#include "tests/scene/test_audio_stream_wav.h"
#include "tests/scene/test_bit_map.h"
#include "tests/scene/test_button.h"
#include "tests/scene/test_camera_2d.h"
#include "tests/scene/test_control.h"
#include "tests/scene/test_curve.h"
#include "tests/scene/test_curve_2d.h"
#include "tests/scene/test_curve_3d.h"
#include "tests/scene/test_fontfile.h"
#include "tests/scene/test_gradient.h"
#include "tests/scene/test_gradient_texture.h"
#include "tests/scene/test_image_texture.h"
#include "tests/scene/test_image_texture_3d.h"
#include "tests/scene/test_instance_placeholder.h"
#include "tests/scene/test_node.h"
#include "tests/scene/test_node_2d.h"
#include "tests/scene/test_packed_scene.h"
#include "tests/scene/test_parallax_2d.h"
#include "tests/scene/test_path_2d.h"
#include "tests/scene/test_path_follow_2d.h"
#include "tests/scene/test_sprite_2d.h"
#include "tests/scene/test_sprite_frames.h"
#include "tests/scene/test_style_box_texture.h"
#include "tests/scene/test_texture_progress_bar.h"
#include "tests/scene/test_theme.h"
#include "tests/scene/test_timer.h"
#include "tests/scene/test_viewport.h"
#include "tests/scene/test_visual_shader.h"
#include "tests/scene/test_window.h"
#ifndef _3D_DISABLED
#include "tests/servers/rendering/test_renderer_scene_cull.h"
#endif // _3D_DISABLED
#include "tests/servers/rendering/test_shader_preprocessor.h"
#include "tests/servers/test_nav_heap.h"
#include "tests/servers/test_text_server.h"
#include "tests/test_validate_testing.h"

#ifndef ADVANCED_GUI_DISABLED
#include "tests/scene/test_code_edit.h"
#include "tests/scene/test_color_picker.h"
#include "tests/scene/test_graph_node.h"
#include "tests/scene/test_option_button.h"
#include "tests/scene/test_split_container.h"
#include "tests/scene/test_tab_bar.h"
#include "tests/scene/test_tab_container.h"
#include "tests/scene/test_text_edit.h"
#include "tests/scene/test_tree.h"
#endif // ADVANCED_GUI_DISABLED

#ifndef _3D_DISABLED
#include "tests/core/math/test_triangle_mesh.h"
#include "tests/scene/test_arraymesh.h"
#include "tests/scene/test_camera_3d.h"
#include "tests/scene/test_convert_transform_modifier_3d.h"
#include "tests/scene/test_copy_transform_modifier_3d.h"
#include "tests/scene/test_gltf_document.h"
#include "tests/scene/test_path_3d.h"
#include "tests/scene/test_path_follow_3d.h"
#include "tests/scene/test_primitives.h"
#include "tests/scene/test_skeleton_3d.h"
#include "tests/scene/test_sky.h"
#endif // _3D_DISABLED

#ifndef PHYSICS_3D_DISABLED
#include "tests/scene/test_height_map_shape_3d.h"
#include "tests/scene/test_physics_material.h"
#endif // PHYSICS_3D_DISABLED

#ifdef MODULE_NAVIGATION_2D_ENABLED
#include "tests/scene/test_navigation_agent_2d.h"
#include "tests/scene/test_navigation_obstacle_2d.h"
#include "tests/scene/test_navigation_region_2d.h"
#include "tests/servers/test_navigation_server_2d.h"
#endif // MODULE_NAVIGATION_2D_ENABLED

#ifdef MODULE_NAVIGATION_3D_ENABLED
#include "tests/scene/test_navigation_agent_3d.h"
#include "tests/scene/test_navigation_obstacle_3d.h"
#include "tests/scene/test_navigation_region_3d.h"
#include "tests/servers/test_navigation_server_3d.h"
#endif // MODULE_NAVIGATION_3D_ENABLED

#include "modules/modules_tests.gen.h"

#include "tests/display_server_mock.h"
#include "tests/test_macros.h"

#include "scene/theme/theme_db.h"

#ifndef NAVIGATION_2D_DISABLED
#include "servers/navigation_server_2d.h"
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
#include "servers/navigation_server_3d.h"
#endif // NAVIGATION_3D_DISABLED

#ifndef PHYSICS_2D_DISABLED
#include "servers/physics_server_2d.h"
#include "servers/physics_server_2d_dummy.h"
#endif // PHYSICS_2D_DISABLED
#ifndef PHYSICS_3D_DISABLED
#include "servers/physics_server_3d.h"
#include "servers/physics_server_3d_dummy.h"
#endif // PHYSICS_3D_DISABLED

#include "servers/rendering/rendering_server_default.h"

int test_main(int argc, char *argv[]) {
	bool run_tests = true;

	// Convert arguments to Godot's command-line.
	List<String> args;

	for (int i = 0; i < argc; i++) {
		args.push_back(String::utf8(argv[i]));
	}
	OS::get_singleton()->set_cmdline("", args, List<String>());
	DisplayServerMock::register_mock_driver();

	WorkerThreadPool::get_singleton()->init();

	{
		const String test_path = TestUtils::get_temp_path("");
		Ref<DirAccess> da = DirAccess::open(test_path); // get_temp_path() automatically creates the folder.
		ERR_FAIL_COND_V(da.is_null(), 0);
		ERR_FAIL_COND_V_MSG(da->erase_contents_recursive() != OK, 0, "Failed to delete files");
	}

	// Run custom test tools.
	if (test_commands) {
		for (const KeyValue<String, TestFunc> &E : (*test_commands)) {
			if (args.find(E.key)) {
				const TestFunc &test_func = E.value;
				test_func();
				run_tests = false;
				break;
			}
		}
		if (!run_tests) {
			delete test_commands;
			return 0;
		}
	}
	// Doctest runner.
	doctest::Context test_context;
	LocalVector<String> test_args;

	// Clean arguments of "--test" from the args.
	for (int x = 0; x < argc; x++) {
		String arg = String(argv[x]);
		if (arg != "--test") {
			test_args.push_back(arg);
		}
	}

	if (test_args.size() > 0) {
		// RAII-owned argv storage. `CharString` owns its UTF-8 buffer (freed
		// in its destructor on scope exit / unwind), so we don't need any
		// explicit cleanup and a throwing applyCommandLine can't leak.
		//
		// Reserve up-front so push_back never reallocates after we start
		// capturing pointers below — LocalVector::push_back reallocates on
		// capacity growth, which would invalidate any prior `ptrw()` pointers.
		LocalVector<CharString> argv_storage;
		argv_storage.reserve(test_args.size());
		for (uint32_t x = 0; x < test_args.size(); x++) {
			argv_storage.push_back(test_args[x].utf8());
		}

		// Capture pointers AFTER all storage push_backs are done so the
		// argv_storage layout is final and stable.
		LocalVector<char *> doctest_argv;
		doctest_argv.reserve(argv_storage.size());
		for (uint32_t x = 0; x < argv_storage.size(); x++) {
			doctest_argv.push_back(argv_storage[x].ptrw());
		}

		test_context.applyCommandLine(int(doctest_argv.size()), doctest_argv.ptr());
	}

	return test_context.run();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if defined(TESTS_ENABLED) && defined(TOOLS_ENABLED)
static const BorrowedEditorTestHooks *borrowed_editor_test_hooks = nullptr;

int run_borrowed_editor_tests(const BorrowedEditorTestHooks &p_hooks, const Vector<String> &p_args) {
	if (!Thread::is_main_thread() || borrowed_editor_test_hooks || SignalWatcher::get_singleton() ||
			!p_hooks.case_start || !p_hooks.case_end || !p_hooks.run_end) {
		return 1;
	}
	Vector<CharString> storage;
	storage.resize(p_args.size());
	if (storage.size() != p_args.size()) { return 1; }
	Vector<const char *> argv;
	argv.resize(p_args.size());
	if (argv.size() != p_args.size()) { return 1; }
	for (int i = 0; i < p_args.size(); ++i) {
		storage.write[i] = p_args[i].utf8();
		argv.write[i] = storage[i].get_data();
	}
	doctest::Context context;
	context.applyCommandLine(argv.size(), argv.ptr());
	borrowed_editor_test_hooks = &p_hooks;
	const int result = context.run();
	borrowed_editor_test_hooks = nullptr;
	return result;
}
#endif

struct GodotTestCaseListener : public doctest::IReporter {
	GodotTestCaseListener(const doctest::ContextOptions &p_in) {}

	SignalWatcher *signal_watcher = nullptr;

#ifndef PHYSICS_2D_DISABLED
	PhysicsServer2D *physics_server_2d = nullptr;
#endif // PHYSICS_2D_DISABLED
#ifndef PHYSICS_3D_DISABLED
	PhysicsServer3D *physics_server_3d = nullptr;
#endif // PHYSICS_3D_DISABLED

#ifndef NAVIGATION_2D_DISABLED
	NavigationServer2D *navigation_server_2d = nullptr;
#endif // NAVIGATION_2D_DISABLED
#ifndef NAVIGATION_3D_DISABLED
	NavigationServer3D *navigation_server_3d = nullptr;
#endif // NAVIGATION_3D_DISABLED

	void test_case_start(const doctest::TestCaseData &p_in) override {
		reinitialize();
#if defined(TESTS_ENABLED) && defined(TOOLS_ENABLED)
		if (borrowed_editor_test_hooks) {
			borrowed_editor_test_hooks->case_start(borrowed_editor_test_hooks->userdata, p_in.m_name);
			return;
		}
#endif

		String name = String(p_in.m_name);
		String suite_name = String(p_in.m_test_suite);

		if (name.contains("[SceneTree]") || name.contains("[Editor]")) {
			memnew(MessageQueue);

			memnew(Input);
			Input::get_singleton()->set_use_accumulated_input(false);

			Error err = OK;
			OS::get_singleton()->set_has_server_feature_callback(nullptr);
			for (int i = 0; i < DisplayServer::get_create_function_count(); i++) {
				if (String("mock") == DisplayServer::get_create_function_name(i)) {
					DisplayServer::create(i, "", DisplayServer::WindowMode::WINDOW_MODE_MINIMIZED, DisplayServer::VSyncMode::VSYNC_ENABLED, 0, nullptr, Vector2i(0, 0), DisplayServer::SCREEN_PRIMARY, DisplayServer::CONTEXT_EDITOR, 0, err);
					break;
				}
			}
			memnew(RenderingServerDefault());
			RenderingServerDefault::get_singleton()->init();
			RenderingServerDefault::get_singleton()->set_render_loop_enabled(false);

			// ThemeDB requires RenderingServer to initialize the default theme.
			// So we have to do this for each test case. Also make sure there is
			// no residual theme from something else.
			ThemeDB::get_singleton()->finalize_theme();
			ThemeDB::get_singleton()->initialize_theme();

#ifndef PHYSICS_3D_DISABLED
			physics_server_3d = PhysicsServer3DManager::get_singleton()->new_default_server();
			if (!physics_server_3d) {
				physics_server_3d = memnew(PhysicsServer3DDummy);
			}
			physics_server_3d->init();
#endif // PHYSICS_3D_DISABLED

#ifndef PHYSICS_2D_DISABLED
			physics_server_2d = PhysicsServer2DManager::get_singleton()->new_default_server();
			if (!physics_server_2d) {
				physics_server_2d = memnew(PhysicsServer2DDummy);
			}
			physics_server_2d->init();
#endif // PHYSICS_2D_DISABLED

			ERR_PRINT_OFF;
#ifndef NAVIGATION_3D_DISABLED
			navigation_server_3d = NavigationServer3DManager::new_default_server();
#endif // NAVIGATION_3D_DISABLED
#ifndef NAVIGATION_2D_DISABLED
			navigation_server_2d = NavigationServer2DManager::new_default_server();
#endif // NAVIGATION_2D_DISABLED
			ERR_PRINT_ON;

			memnew(InputMap);
			InputMap::get_singleton()->load_default();

			memnew(SceneTree);
			SceneTree::get_singleton()->initialize();
			if (!DisplayServer::get_singleton()->has_feature(DisplayServer::Feature::FEATURE_SUBWINDOWS)) {
				SceneTree::get_singleton()->get_root()->set_embedding_subwindows(true);
			}

#ifdef TOOLS_ENABLED
			if (name.contains("[Editor]")) {
				Engine::get_singleton()->set_editor_hint(true);
				EditorPaths::create();
				EditorSettings::create();
			}
#endif // TOOLS_ENABLED

			return;
		}

		if (name.contains("[Audio]")) {
			// The last driver index should always be the dummy driver.
			int dummy_idx = AudioDriverManager::get_driver_count() - 1;
			AudioDriverManager::initialize(dummy_idx);
			AudioServer *audio_server = memnew(AudioServer);
			audio_server->init();
			return;
		}

#ifndef NAVIGATION_3D_DISABLED
		if (suite_name.contains("[Navigation3D]") && navigation_server_3d == nullptr) {
			ERR_PRINT_OFF;
			navigation_server_3d = NavigationServer3DManager::new_default_server();
			ERR_PRINT_ON;
			return;
		}
#endif // NAVIGATION_3D_DISABLED

#ifndef NAVIGATION_2D_DISABLED
		if (suite_name.contains("[Navigation2D]") && navigation_server_2d == nullptr) {
			ERR_PRINT_OFF;
			navigation_server_2d = NavigationServer2DManager::new_default_server();
			ERR_PRINT_ON;
			return;
		}
#endif // NAVIGATION_2D_DISABLED
	}

#if defined(TESTS_ENABLED) && defined(TOOLS_ENABLED)
	void test_case_end(const doctest::CurrentTestCaseStats &p_stats) override {
		if (borrowed_editor_test_hooks) {
			borrowed_editor_test_hooks->case_end(borrowed_editor_test_hooks->userdata, p_stats.numAssertsCurrentTest, p_stats.numAssertsFailedCurrentTest, p_stats.testCaseSuccess);
			return;
		}
#else
	void test_case_end(const doctest::CurrentTestCaseStats &) override {
#endif
#ifdef TOOLS_ENABLED
		if (EditorSettings::get_singleton()) {
			EditorSettings::destroy();

			// Instantiating the EditorSettings singleton sets the locale to the editor's language.
			TranslationServer::get_singleton()->set_locale("en");
		}
		if (EditorPaths::get_singleton()) {
			EditorPaths::free();
		}
#endif // TOOLS_ENABLED

		Engine::get_singleton()->set_editor_hint(false);

		if (SceneTree::get_singleton()) {
			SceneTree::get_singleton()->finalize();
		}

		if (MessageQueue::get_singleton()) {
			MessageQueue::get_singleton()->flush();
		}

		if (SceneTree::get_singleton()) {
			memdelete(SceneTree::get_singleton());
		}

#ifndef NAVIGATION_3D_DISABLED
		if (navigation_server_3d) {
			memdelete(navigation_server_3d);
			navigation_server_3d = nullptr;
		}
#endif // NAVIGATION_3D_DISABLED

#ifndef NAVIGATION_2D_DISABLED
		if (navigation_server_2d) {
			memdelete(navigation_server_2d);
			navigation_server_2d = nullptr;
		}
#endif // NAVIGATION_2D_DISABLED

#ifndef PHYSICS_3D_DISABLED
		if (physics_server_3d) {
			physics_server_3d->finish();
			memdelete(physics_server_3d);
			physics_server_3d = nullptr;
		}
#endif // PHYSICS_3D_DISABLED

#ifndef PHYSICS_2D_DISABLED
		if (physics_server_2d) {
			physics_server_2d->finish();
			memdelete(physics_server_2d);
			physics_server_2d = nullptr;
		}
#endif // PHYSICS_2D_DISABLED

		if (Input::get_singleton()) {
			memdelete(Input::get_singleton());
		}

		if (RenderingServer::get_singleton()) {
			// ThemeDB requires RenderingServer to finalize the default theme.
			// So we have to do this for each test case.
			ThemeDB::get_singleton()->finalize_theme();

			RenderingServer::get_singleton()->sync();
			RenderingServer::get_singleton()->global_shader_parameters_clear();
			RenderingServer::get_singleton()->finish();
			memdelete(RenderingServer::get_singleton());
		}

		if (DisplayServer::get_singleton()) {
			memdelete(DisplayServer::get_singleton());
		}

		if (InputMap::get_singleton()) {
			memdelete(InputMap::get_singleton());
		}

		if (MessageQueue::get_singleton()) {
			MessageQueue::get_singleton()->flush();
			memdelete(MessageQueue::get_singleton());
		}

		if (AudioServer::get_singleton()) {
			AudioServer::get_singleton()->finish();
			memdelete(AudioServer::get_singleton());
		}
	}

	void test_run_start() override {
		signal_watcher = memnew(SignalWatcher);
	}

#if defined(TESTS_ENABLED) && defined(TOOLS_ENABLED)
	void test_run_end(const doctest::TestRunStats &p_stats) override {
		if (borrowed_editor_test_hooks) {
			borrowed_editor_test_hooks->run_end(borrowed_editor_test_hooks->userdata, p_stats.numTestCasesPassingFilters, p_stats.numTestCasesFailed, p_stats.numAsserts, p_stats.numAssertsFailed);
		}
#else
	void test_run_end(const doctest::TestRunStats &) override {
#endif
		memdelete(signal_watcher);
	}

	void test_case_reenter(const doctest::TestCaseData &) override {
		reinitialize();
	}

	void subcase_start(const doctest::SubcaseSignature &) override {
		reinitialize();
	}

	void report_query(const doctest::QueryData &) override {}
	void test_case_exception(const doctest::TestCaseException &) override {}
	void subcase_end() override {}

	void log_assert(const doctest::AssertData &in) override {}
	void log_message(const doctest::MessageData &) override {}
	void test_case_skipped(const doctest::TestCaseData &) override {}

private:
	void reinitialize() {
		Math::seed(0x60d07);
		SignalWatcher::get_singleton()->_clear_signals();
	}
};

REGISTER_LISTENER("GodotTestCaseListener", 1, GodotTestCaseListener);
