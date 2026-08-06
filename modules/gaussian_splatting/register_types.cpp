#include "register_types.h"

#include "core/config/project_settings.h"
#include "logger/startup_trace.h"

#include "core/gaussian_data.h"
#include "core/gaussian_splat_asset.h"
#include "core/gaussian_splat_world.h"
#include "core/gaussian_splat_scene_director.h"
#include "core/gaussian_splat_config_registry.h"
#include "core/gaussian_streaming.h"
#include "core/module_string_names.h"
#include "core/streaming_chunk_payload_source.h"
#include "renderer/gaussian_splat_renderer.h"
#include "renderer/gpu_buffer_manager.h"
#include "renderer/pipeline_feature_set.h"
#include "renderer/spirv_disk_cache.h"
#include "core/gaussian_splat_manager.h"
#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "io/ply_loader.h"
#include "io/spz_loader.h"
#include "io/i_gaussian_loader.h"
#include "io/gaussian_splat_world_io.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "resources/color_grading_resource.h"
#include "renderer/gpu_sorter.h"
#include "renderer/gpu_memory_stream.h"
#include "renderer/rendering_diagnostics.h"
#include "painterly/painterly_material.h"
#include "nodes/gaussian_splat_node_3d.h"
#include "nodes/gaussian_splat_debug_hud.h"
#include "nodes/gaussian_splat_container.h"
#include "nodes/gaussian_splat_dynamic_instance_3d.h"
#include "nodes/gaussian_splat_world_3d.h"
#include "nodes/sphere_effector_3d.h"

// Animation and Persistence (v0.6.0)
#include "animation/animation_state_machine.h"
#include "animation/keyframe_interpolator.h"
#include "persistence/gaussian_scene_serializer.h"
#include "persistence/incremental_saver.h"

// Asset Management System (v0.7.0)
#include "asset_management/asset_dependency_manager.h"
#include "core/performance_monitors.h"

#ifdef TOOLS_ENABLED
#include "editor/gaussian_editor_plugin.h"
#include "io/resource_importer_ply.h"
#include "io/resource_importer_spz.h"
#include "io/resource_importer_gsplatworld.h"
#endif


// Global resource loader instance
static Ref<ResourceFormatLoaderGaussianSplat> gaussian_format_loader;
static Ref<ResourceFormatLoaderGaussianSplatWorld> gaussian_world_format_loader;
static Ref<ResourceFormatSaverGaussianSplatWorld> gaussian_world_format_saver;

static GaussianSplatManager *gaussian_splat_manager_singleton = nullptr;
static GaussianSplatSceneDirector *gaussian_splat_scene_director_singleton = nullptr;

#ifdef TOOLS_ENABLED
// PLY and SPZ importer instances
static Ref<ResourceImporterPLY> ply_importer;
static Ref<ResourceImporterSPZ> spz_importer;
static Ref<ResourceImporterGSplatWorld> gsplatworld_importer;
#endif

void initialize_gaussian_splatting_module(ModuleInitializationLevel p_level) {
    // Register diagnostics setting before any GS code reads it; the trace
    // singleton refreshes its cached enable flag after registration.
    if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        // Module-owned StringName cache must be live before any code under
        // initialize_gaussian_splatting_module reads a path through the
        // module_string_names accessors. Construct it first so paths like
        // sorting_target_sort_time_path resolve into the releasable
        // storage instead of the on-demand fallback that would leak.
        gs::initialize_module_string_names();

        GLOBAL_DEF("rendering/gaussian_splatting/diagnostics/startup_trace", true);
        GSStartupTrace::get_singleton()->refresh_enabled();
        GSStartupTrace::get_singleton()->reset();
    }
    GS_STARTUP_SCOPE("module_register");
    switch (p_level) {
        case MODULE_INITIALIZATION_LEVEL_SCENE: {
            // Initialize configuration systems ahead of renderer setup.
            GaussianSplatConfigRegistry::initialize_all();
            // Core data structures
            GDREGISTER_CLASS(GaussianData);
            GDREGISTER_CLASS(GaussianSplatAsset);
            GDREGISTER_CLASS(GaussianSplatWorld);
            GDREGISTER_CLASS(GaussianStreamingSystem);
            GDREGISTER_ABSTRACT_CLASS(ChunkPayloadSource);
            GDREGISTER_CLASS(InMemoryChunkPayloadSource);
            GDREGISTER_CLASS(StagedFileChunkPayloadSource);
            GDREGISTER_CLASS(VRAMBudgetRegulator);

            // Node classes
            GDREGISTER_CLASS(GaussianSplatNode3D);
            GDREGISTER_CLASS(GaussianSplatDebugHUD);
            GDREGISTER_CLASS(GaussianSplatContainer);
#ifndef DISABLE_DEPRECATED
            // One-release compatibility shim — keeps serialized scenes that still
            // reference GaussianSplatDynamicInstance3D loadable. Inherits from
            // GaussianSplatNode3D verbatim and emits WARN_DEPRECATED on construction.
            GDREGISTER_CLASS(GaussianSplatDynamicInstance3D);
#endif
            GDREGISTER_CLASS(GaussianSplatWorld3D);
            GDREGISTER_CLASS(SphereEffector3D);

            // Rendering components
            GDREGISTER_CLASS(GaussianSplatRenderer);
            GDREGISTER_CLASS(GaussianMemoryStream);
            GDREGISTER_CLASS(StreamingPipeline);
            GDREGISTER_CLASS(PainterlyMaterial);
            GDREGISTER_CLASS(GPUBufferManager);
            GDREGISTER_CLASS(GaussianSplatManager);
            GDREGISTER_CLASS(GaussianSplatSceneDirector);
            GDREGISTER_CLASS(ColorGradingResource);

            // #839 round 3, thread C: let the renderer layer reach the node layer
            // for "a debug HUD source flag on this renderer changed". renderer/
            // deliberately does not include nodes/, and the write that triggers
            // this (GaussianSplatRenderer::set_debug_show_device_boundaries and
            // siblings, all bound to script) has no node to route through, so the
            // director carries the callback. Cleared in the uninitialize path.
            GaussianSplatSceneDirector::set_renderer_debug_hud_sources_changed_callback(
                    &GaussianSplatNode3D::_notify_debug_hud_dirty_for_renderer);

            if (!gaussian_splat_manager_singleton) {
                gaussian_splat_manager_singleton = memnew(GaussianSplatManager);
                gaussian_splat_manager_singleton->initialize_module();

                Engine::Singleton singleton_info;
                singleton_info.name = "GaussianSplatManager";
                singleton_info.ptr = gaussian_splat_manager_singleton;
                Engine::get_singleton()->add_singleton(singleton_info);
            }

            // SPIR-V disk cache settings. Registered here (not in the manager)
            // so they exist before any shader compile on first launch.
            GLOBAL_DEF("rendering/gaussian_splatting/cache/spirv_cache_enabled", true);
            GLOBAL_DEF(PropertyInfo(Variant::INT,
                              "rendering/gaussian_splatting/cache/spirv_cache_max_mb",
                              PROPERTY_HINT_RANGE, "4,1024,1"),
                    64);
            {
                SPIRVDiskCache *spirv_cache = SPIRVDiskCache::get();
                const bool cache_enabled = bool(GLOBAL_GET("rendering/gaussian_splatting/cache/spirv_cache_enabled"));
                spirv_cache->set_enabled(cache_enabled);
                int max_mb = int(GLOBAL_GET("rendering/gaussian_splatting/cache/spirv_cache_max_mb"));
                if (max_mb < 4) {
                    max_mb = 4;
                } else if (max_mb > 1024) {
                    max_mb = 1024;
                }
                spirv_cache->prune_above(uint64_t(max_mb) * 1024ull * 1024ull);
            }

            if (!gaussian_splat_scene_director_singleton) {
                gaussian_splat_scene_director_singleton = memnew(GaussianSplatSceneDirector);

                Engine::Singleton director_info;
                director_info.name = "GaussianSplatSceneDirector";
                director_info.ptr = gaussian_splat_scene_director_singleton;
                Engine::get_singleton()->add_singleton(director_info);
            }

            GaussianRenderingDiagnostics::ensure_singleton();
            if (GaussianRenderingDiagnostics::get_singleton()) {
                GaussianRenderingDiagnostics::get_singleton()->process_command_line_requests();
            }

            // Initialize Custom Performance Monitors for editor debugger
            GaussianSplattingPerformanceMonitors::create_singleton();

            // IO components
            GDREGISTER_CLASS(PLYLoader);
            GDREGISTER_CLASS(SPZLoader);
            GDREGISTER_ABSTRACT_CLASS(IGaussianLoader);

            // Modular GPU sorting implementation
            GDREGISTER_ABSTRACT_CLASS(IGPUSorter);
            GDREGISTER_CLASS(BitonicSort);
            GDREGISTER_CLASS(RadixSort);
            GDREGISTER_CLASS(OneSweepSort);

            // Animation and Persistence (v0.6.0)
            GDREGISTER_CLASS(GaussianSplatting::GaussianAnimationStateMachine);
            GDREGISTER_CLASS(GaussianSplatting::GaussianSceneSerializer);
            GDREGISTER_CLASS(GaussianSplatting::GaussianIncrementalSaver);

            // Asset Management System (v0.7.0)
            GDREGISTER_CLASS(AssetDependencyManager);
            // AssetDependencyManager is currently the only compiled asset management
            // type. Keep this list aligned with asset_management/*.cpp sources to
            // prevent unresolved symbol errors when new implementations land.

            if (!gaussian_format_loader.is_valid()) {
                gaussian_format_loader.instantiate();
                ResourceLoader::add_resource_format_loader(gaussian_format_loader);
            }
            if (!gaussian_world_format_loader.is_valid()) {
                gaussian_world_format_loader.instantiate();
                ResourceLoader::add_resource_format_loader(gaussian_world_format_loader, true);
            }
            if (!gaussian_world_format_saver.is_valid()) {
                gaussian_world_format_saver.instantiate();
                ResourceSaver::add_resource_format_saver(gaussian_world_format_saver, true);
            }
        } break;

#ifdef TOOLS_ENABLED
        case MODULE_INITIALIZATION_LEVEL_EDITOR: {
            EditorPlugins::add_by_type<GaussianEditorPlugin>();

            // Register PLY importer
            ply_importer.instantiate();
            ResourceFormatImporter::get_singleton()->add_importer(ply_importer);

            // Register SPZ importer
            spz_importer.instantiate();
            ResourceFormatImporter::get_singleton()->add_importer(spz_importer);

            // Register gsplatworld importer
            gsplatworld_importer.instantiate();
            ResourceFormatImporter::get_singleton()->add_importer(gsplatworld_importer);
        } break;
#endif

        default:
            break;
    }
}

void uninitialize_gaussian_splatting_module(ModuleInitializationLevel p_level) {
    // Cleanup when module is unloaded
    switch (p_level) {
        case MODULE_INITIALIZATION_LEVEL_SCENE:
            // #839 round 3, thread C: drop the renderer -> node HUD callback before
            // anything else is torn down. It points at a static member function, so
            // there is no object to outlive, but a renderer destroyed during the
            // teardown below must not fan a notification out into nodes that are
            // themselves being freed.
            GaussianSplatSceneDirector::set_renderer_debug_hud_sources_changed_callback(nullptr);
            if (gaussian_format_loader.is_valid()) {
                ResourceLoader::remove_resource_format_loader(gaussian_format_loader);
                gaussian_format_loader.unref();
            }
            if (gaussian_world_format_loader.is_valid()) {
                ResourceLoader::remove_resource_format_loader(gaussian_world_format_loader);
                gaussian_world_format_loader.unref();
            }
            if (gaussian_world_format_saver.is_valid()) {
                ResourceSaver::remove_resource_format_saver(gaussian_world_format_saver);
                gaussian_world_format_saver.unref();
            }
            // #589: tear down in REVERSE of init order — the director (and the
            // director-owned Ref<GaussianSplatRenderer>s its ~dtor drops via
            // worlds.clear()) must be destroyed BEFORE the manager. Init order is
            // manager -> director (see :125,:155); teardown must therefore be
            // director -> manager so that each renderer's teardown still observes a
            // live GaussianSplatManager singleton (correct owning device, buffer
            // hand-back) instead of the null-singleton / main-device fallback. The
            // StringName-cache release + singleton removals below are unchanged and
            // still run after BOTH singletons are gone.
            if (gaussian_splat_scene_director_singleton) {
                if (Engine::get_singleton()->has_singleton("GaussianSplatSceneDirector")) {
                    Engine::get_singleton()->remove_singleton("GaussianSplatSceneDirector");
                }
                memdelete(gaussian_splat_scene_director_singleton);
                gaussian_splat_scene_director_singleton = nullptr;
            }
            if (gaussian_splat_manager_singleton) {
                if (Engine::get_singleton()->has_singleton("GaussianSplatManager")) {
                    Engine::get_singleton()->remove_singleton("GaussianSplatManager");
                }
                gaussian_splat_manager_singleton->finalize_module();
                memdelete(gaussian_splat_manager_singleton);
                gaussian_splat_manager_singleton = nullptr;
            }
            // Cleanup Custom Performance Monitors
            GaussianSplattingPerformanceMonitors::destroy_singleton();
            GaussianRenderingDiagnostics::destroy_singleton();
            SPIRVDiskCache::shutdown();

            // Release module-owned StringName caches before the module
            // unregisters so the engine's exit-time orphan StringName
            // report (StringName::cleanup) does not surface them. Order
            // matches the bug report's grouping:
            //   1. PipelineFeatureSet snapshots — drops the pipeline_*
            //      keys and the generic value/source/source_label/
            //      display_value/note/fidelity_limited entry-field keys.
            //   2. GSStartupTrace phase storage — drops manager_construct,
            //      module_register, device_request_primary, and
            //      device_request_shared.
            //   3. ModuleStringNames — drops the function-local-static
            //      replacements for the renderdoc_compatibility and
            //      sorting target-sort-time paths.
            release_pipeline_feature_set_module_strings();
            GSStartupTrace::get_singleton()->release_module_strings();
            gs::release_module_string_names();
            break;

#ifdef TOOLS_ENABLED
        case MODULE_INITIALIZATION_LEVEL_EDITOR:
            // Cleanup PLY importer
            if (ply_importer.is_valid()) {
                ResourceFormatImporter::get_singleton()->remove_importer(ply_importer);
                ply_importer.unref();
            }
            // Cleanup SPZ importer
            if (spz_importer.is_valid()) {
                ResourceFormatImporter::get_singleton()->remove_importer(spz_importer);
                spz_importer.unref();
            }
            if (gsplatworld_importer.is_valid()) {
                ResourceFormatImporter::get_singleton()->remove_importer(gsplatworld_importer);
                gsplatworld_importer.unref();
            }
            break;
#endif

        default:
            break;
    }
}
