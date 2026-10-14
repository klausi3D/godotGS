/**************************************************************************/
/*  test_gaussian_storage_render_thread_routing.h                         */
/**************************************************************************/
// #1161: GaussianSplatNode3D / GaussianSplatWorld3D must release their
// RendererRD::GaussianSplatStorage slot in the order
//     instance_set_base(render_instance, RID()) -> clear renderer -> free,
// with the free issued through RenderingServer::free(), never as a direct
// GaussianSplatStorage::gaussian_free() from the scene thread.
//
// WHY THE SERVER PATH IS THE PROPERTY UNDER TEST
//
// Under rendering/driver/threads/thread_model=2 (the starter template),
// instance_set_base() is a queued RenderingServer command while a direct
// gaussian_free() runs immediately, so the slot was freed while the render
// thread's cull list could still name it as a base. RenderingServer::free() is
// queued behind the unset on the same command queue, so it cannot overtake it.
//
// WHAT THESE TESTS CAN AND CANNOT SEE
//
// Every doctest process runs `--headless --test`: RenderingServerDefault is
// created without a render thread and with the dummy rasterizer, whose
// utilities know nothing about Gaussian slots, and no GaussianSplatStorage
// exists. The fixture below therefore (1) creates the storage and (2) wraps
// RSG::utilities so that, as RendererRD::Utilities does in a real build, a
// storage-owned RID reports INSTANCE_GAUSSIAN_SPLAT (so RendererSceneCull
// really binds it as the instance base) and RenderingServer::free() on it frees
// the slot. The wrapper records, at the instant the free arrives, whether any
// scene instance still names the slot and whether the slot still holds a
// renderer.
//
// It cannot reproduce the thread_model=2 race itself: with no render thread
// both a queued and a direct call run synchronously. What it pins is the
// routing (the free reaches the server) and the order (nothing names the slot
// when it is freed). On the base tree both node types call gaussian_free()
// directly, so frees_through_server stays 0 and the first assertion after
// remove_child() fails; GaussianSplatWorld3D additionally freed the slot while
// its instance still named it. The windowed thread_model=2 lane (#1148) is the
// runtime proof and is not part of this file.

#ifndef TEST_GAUSSIAN_STORAGE_RENDER_THREAD_ROUTING_H
#define TEST_GAUSSIAN_STORAGE_RENDER_THREAD_ROUTING_H

#include "tests/test_macros.h"

#include "core/os/os.h"
#include "core/os/thread.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/rendering/renderer_rd/storage_rd/gaussian_splat_storage.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

#include "../nodes/gaussian_splat_node_3d.h"
#include "../nodes/gaussian_splat_world_3d.h"
#include "../renderer/gaussian_splat_renderer.h"

#include <atomic>

namespace TestGaussianStorageRenderThreadRouting {

// Counts scene instances whose base is a Gaussian slot (any slot when p_base is
// null). r_base receives the last one found.
static uint32_t count_instances_with_gaussian_base(RID p_base, RID *r_base = nullptr) {
	RendererSceneCull *scene = static_cast<RendererSceneCull *>(RSG::scene);
	uint32_t count = 0;
	for (const RID &instance_rid : scene->instance_owner.get_owned_list()) {
		RendererSceneCull::Instance *instance = scene->instance_owner.get_or_null(instance_rid);
		if (!instance || instance->base_type != RS::INSTANCE_GAUSSIAN_SPLAT) {
			continue;
		}
		if (p_base.is_valid() && instance->base != p_base) {
			continue;
		}
		count++;
		if (r_base) {
			*r_base = instance->base;
		}
	}
	return count;
}

// Forwards to the dummy rasterizer's utilities, except for storage-owned RIDs.
class GaussianFreeWitness : public RendererUtilities {
public:
	RendererUtilities *inner = nullptr;
	RendererRD::GaussianSplatStorage *storage = nullptr;

	uint32_t frees_through_server = 0;
	uint32_t instances_naming_slot_at_free = 0;
	uint32_t renderers_held_at_free = 0;
	RID last_freed;

	RS::InstanceType get_base_type(RID p_rid) const override {
		if (storage && storage->owns_gaussian(p_rid)) {
			return RS::INSTANCE_GAUSSIAN_SPLAT;
		}
		return inner->get_base_type(p_rid);
	}
	bool free(RID p_rid) override {
		if (storage && storage->owns_gaussian(p_rid)) {
			frees_through_server++;
			instances_naming_slot_at_free += count_instances_with_gaussian_base(p_rid);
			if (storage->gaussian_get_renderer(p_rid).is_valid()) {
				renderers_held_at_free++;
			}
			last_freed = p_rid;
			storage->gaussian_free(p_rid);
			return true;
		}
		return inner->free(p_rid);
	}

	void base_update_dependency(RID p_base, DependencyTracker *p_instance) override { inner->base_update_dependency(p_base, p_instance); }
	RID visibility_notifier_allocate() override { return inner->visibility_notifier_allocate(); }
	void visibility_notifier_initialize(RID p_notifier) override { inner->visibility_notifier_initialize(p_notifier); }
	void visibility_notifier_free(RID p_notifier) override { inner->visibility_notifier_free(p_notifier); }
	void visibility_notifier_set_aabb(RID p_notifier, const AABB &p_aabb) override { inner->visibility_notifier_set_aabb(p_notifier, p_aabb); }
	void visibility_notifier_set_callbacks(RID p_notifier, const Callable &p_enter, const Callable &p_exit) override { inner->visibility_notifier_set_callbacks(p_notifier, p_enter, p_exit); }
	AABB visibility_notifier_get_aabb(RID p_notifier) const override { return inner->visibility_notifier_get_aabb(p_notifier); }
	void visibility_notifier_call(RID p_notifier, bool p_enter, bool p_deferred) override { inner->visibility_notifier_call(p_notifier, p_enter, p_deferred); }
	void capture_timestamps_begin() override { inner->capture_timestamps_begin(); }
	void capture_timestamp(const String &p_name) override { inner->capture_timestamp(p_name); }
	uint32_t get_captured_timestamps_count() const override { return inner->get_captured_timestamps_count(); }
	uint64_t get_captured_timestamps_frame() const override { return inner->get_captured_timestamps_frame(); }
	uint64_t get_captured_timestamp_gpu_time(uint32_t p_index) const override { return inner->get_captured_timestamp_gpu_time(p_index); }
	uint64_t get_captured_timestamp_cpu_time(uint32_t p_index) const override { return inner->get_captured_timestamp_cpu_time(p_index); }
	String get_captured_timestamp_name(uint32_t p_index) const override { return inner->get_captured_timestamp_name(p_index); }
	void update_dirty_resources() override { inner->update_dirty_resources(); }
	void set_debug_generate_wireframes(bool p_generate) override { inner->set_debug_generate_wireframes(p_generate); }
	bool has_os_feature(const String &p_feature) const override { return inner->has_os_feature(p_feature); }
	void update_memory_info() override { inner->update_memory_info(); }
	uint64_t get_rendering_info(RS::RenderingInfo p_info) override { return inner->get_rendering_info(p_info); }
	String get_video_adapter_name() const override { return inner->get_video_adapter_name(); }
	String get_video_adapter_vendor() const override { return inner->get_video_adapter_vendor(); }
	RenderingDevice::DeviceType get_video_adapter_type() const override { return inner->get_video_adapter_type(); }
	String get_video_adapter_api_version() const override { return inner->get_video_adapter_api_version(); }
	Size2i get_maximum_viewport_size() const override { return inner->get_maximum_viewport_size(); }
	uint32_t get_maximum_shader_varyings() const override { return inner->get_maximum_shader_varyings(); }
	uint64_t get_maximum_uniform_buffer_size() const override { return inner->get_maximum_uniform_buffer_size(); }
};

// Creates the storage and installs the witness; undoes both on destruction.
// Declare it before any node so the nodes are deleted while it is installed.
struct StorageRoutingFixture {
	RendererRD::GaussianSplatStorage *storage = nullptr;
	GaussianFreeWitness witness;
	RendererUtilities *previous_utilities = nullptr;

	// Returns an empty string on success, otherwise why it could not install.
	String install() {
		if (!RSG::scene || !RSG::utilities) {
			return "RenderingServer globals missing (the [SceneTree] harness must create RenderingServerDefault)";
		}
		if (RendererRD::GaussianSplatStorage::get_singleton() != nullptr) {
			return "a GaussianSplatStorage already exists; this test expects the headless dummy rasterizer";
		}
		storage = memnew(RendererRD::GaussianSplatStorage);
		witness.inner = RSG::utilities;
		witness.storage = storage;
		previous_utilities = RSG::utilities;
		RSG::utilities = &witness;
		return String();
	}

	~StorageRoutingFixture() {
		if (previous_utilities) {
			RSG::utilities = previous_utilities;
		}
		if (storage) {
			memdelete(storage);
		}
	}
};

TEST_CASE("[GaussianSplatting][Lifetime][SceneTree] GaussianSplatNode3D unsets its instance base before freeing its storage slot through the RenderingServer") {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		FAIL("SceneTree with a root window required (provided by the [SceneTree] tag)");
		return;
	}
	StorageRoutingFixture fixture;
	const String install_error = fixture.install();
	if (!install_error.is_empty()) {
		FAIL(install_error.utf8().get_data());
		return;
	}

	Window *root = tree->get_root();
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	// Two cycles: re-entering the tree allocates a fresh slot, which must be
	// released the same way.
	for (int cycle = 0; cycle < 2; cycle++) {
		CAPTURE(cycle);
		root->add_child(node);
		tree->process(0.0);

		// Positive signals. Without them every assertion after remove_child()
		// would also hold for a node that never bound a slot. (The build has no
		// exceptions, so a failed REQUIRE would not stop the case; fail and
		// return instead.)
		RID base;
		const uint32_t bound = count_instances_with_gaussian_base(RID(), &base);
		if (bound != 1) {
			FAIL("the node's render instance must name exactly one Gaussian slot while in the tree");
			root->remove_child(node);
			memdelete(node);
			return;
		}
		CHECK(fixture.storage->owns_gaussian(base));
		// Headless lanes have no RenderingDevice, so the shared renderer may be
		// null; the release ORDER under test does not depend on it. When it is
		// present the slot must hold it (and the renderers_held_at_free check
		// below is then a real signal rather than a vacuous one).
		const Ref<GaussianSplatRenderer> renderer = node->get_existing_renderer();
		if (renderer.is_valid()) {
			CHECK(fixture.storage->gaussian_get_renderer(base) == renderer);
		} else {
			MESSAGE("shared renderer unavailable (headless): the renderer-cleared check is vacuous in this lane");
		}

		const uint32_t frees_before = fixture.witness.frees_through_server;
		root->remove_child(node);

		CHECK_MESSAGE(fixture.witness.frees_through_server == frees_before + 1,
				"EXIT_TREE must free the slot through RenderingServer::free(), not GaussianSplatStorage::gaussian_free() directly");
		CHECK(fixture.witness.last_freed == base);
		CHECK_MESSAGE(fixture.witness.instances_naming_slot_at_free == 0,
				"no render instance may still name the slot when it is freed");
		CHECK_MESSAGE(fixture.witness.renderers_held_at_free == 0,
				"the renderer Ref must be cleared before the slot is freed");
		CHECK_FALSE(fixture.storage->owns_gaussian(base));
		CHECK(count_instances_with_gaussian_base(RID()) == 0);
	}
	memdelete(node);
}

TEST_CASE("[GaussianSplatting][Lifetime][SceneTree] GaussianSplatWorld3D unsets its instance base before freeing its storage slot through the RenderingServer") {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		FAIL("SceneTree with a root window required (provided by the [SceneTree] tag)");
		return;
	}
	StorageRoutingFixture fixture;
	const String install_error = fixture.install();
	if (!install_error.is_empty()) {
		FAIL(install_error.utf8().get_data());
		return;
	}

	Window *root = tree->get_root();
	GaussianSplatWorld3D *world_node = memnew(GaussianSplatWorld3D);
	// No GaussianSplatWorld resource is assigned; READY still binds the slot.
	world_node->set_auto_apply_on_ready(false);
	root->add_child(world_node);
	tree->process(0.0);

	RID base;
	const uint32_t bound = count_instances_with_gaussian_base(RID(), &base);
	if (bound != 1) {
		FAIL("the world node must bind exactly one Gaussian slot while in the tree");
		root->remove_child(world_node);
		memdelete(world_node);
		return;
	}
	CHECK(fixture.storage->owns_gaussian(base));
	// See the Node3D case: the renderer may be null headless; the release order
	// under test does not depend on it.
	const Ref<GaussianSplatRenderer> renderer = world_node->get_renderer();
	if (renderer.is_valid()) {
		CHECK(fixture.storage->gaussian_get_renderer(base) == renderer);
	} else {
		MESSAGE("shared renderer unavailable (headless): the renderer-cleared check is vacuous in this lane");
	}

	root->remove_child(world_node);

	CHECK_MESSAGE(fixture.witness.frees_through_server == 1,
			"EXIT_TREE must free the slot through RenderingServer::free(), not GaussianSplatStorage::gaussian_free() directly");
	CHECK(fixture.witness.last_freed == base);
	CHECK_MESSAGE(fixture.witness.instances_naming_slot_at_free == 0,
			"no render instance may still name the slot when it is freed");
	CHECK_MESSAGE(fixture.witness.renderers_held_at_free == 0,
			"the renderer Ref must be cleared before the slot is freed");
	CHECK_FALSE(fixture.storage->owns_gaussian(base));

	memdelete(world_node);
}

// Storage hardening. The reference-count checks are deterministic. The
// concurrent part is a smoke test: without TSAN it passes on the base tree
// too unless the race happens to crash, so it is evidence only in a TSAN build.
struct ConcurrentSlotReader {
	RendererRD::GaussianSplatStorage *storage = nullptr;
	const GaussianSplatRenderer *renderer_a = nullptr;
	const GaussianSplatRenderer *renderer_b = nullptr;
	std::atomic<uint64_t> published_id{ 0 };
	std::atomic<uint64_t> observed_id{ 0 };
	std::atomic<bool> stop{ false };
	std::atomic<uint32_t> foreign_pointers{ 0 };

	static void run(void *p_self) {
		ConcurrentSlotReader *self = static_cast<ConcurrentSlotReader *>(p_self);
		while (!self->stop.load(std::memory_order_acquire)) {
			const uint64_t id = self->published_id.load(std::memory_order_acquire);
			if (id == 0) {
				continue;
			}
			const RID rid = RID::from_uint64(id);
			const Ref<GaussianSplatRenderer> seen = self->storage->gaussian_get_renderer(rid);
			(void)self->storage->gaussian_get_aabb(rid);
			(void)self->storage->gaussian_get_casts_shadow(rid);
			if (seen.is_valid()) {
				if (seen.ptr() != self->renderer_a && seen.ptr() != self->renderer_b) {
					self->foreign_pointers.fetch_add(1, std::memory_order_relaxed);
				}
				self->observed_id.store(id, std::memory_order_release);
			}
		}
	}
};

TEST_CASE("[GaussianSplatting][Lifetime] GaussianSplatStorage releases swapped and freed renderer references and survives a concurrent reader") {
	if (RendererRD::GaussianSplatStorage::get_singleton() != nullptr) {
		FAIL("a GaussianSplatStorage already exists; this test expects the headless dummy rasterizer");
		return;
	}
	RendererRD::GaussianSplatStorage *storage = memnew(RendererRD::GaussianSplatStorage);
	Ref<GaussianSplatRenderer> renderer_a;
	renderer_a.instantiate();
	Ref<GaussianSplatRenderer> renderer_b;
	renderer_b.instantiate();
	const int refs_a = renderer_a->get_reference_count();
	const int refs_b = renderer_b->get_reference_count();

	RID rid = storage->gaussian_allocate();
	storage->gaussian_initialize(rid);
	storage->gaussian_set_renderer(rid, renderer_a);
	CHECK(renderer_a->get_reference_count() == refs_a + 1);
	storage->gaussian_set_renderer(rid, renderer_b);
	CHECK_MESSAGE(renderer_a->get_reference_count() == refs_a, "a swap must release the previous renderer");
	CHECK(renderer_b->get_reference_count() == refs_b + 1);
	storage->gaussian_free(rid);
	CHECK_MESSAGE(renderer_b->get_reference_count() == refs_b, "a free must release the held renderer");
	CHECK_FALSE(storage->owns_gaussian(rid));

	ConcurrentSlotReader reader;
	reader.storage = storage;
	reader.renderer_a = renderer_a.ptr();
	reader.renderer_b = renderer_b.ptr();
	// Reads that land after a free return defaults and print `!splat`; that is
	// expected here and would flood the log.
	ERR_PRINT_OFF;
	Thread thread;
	thread.start(&ConcurrentSlotReader::run, &reader);
	uint32_t overlapped = 0;
	OS *os = OS::get_singleton();
	for (int i = 0; i < 200; i++) {
		rid = storage->gaussian_allocate();
		storage->gaussian_initialize(rid);
		storage->gaussian_set_renderer(rid, (i & 1) ? renderer_a : renderer_b);
		reader.published_id.store(rid.get_id(), std::memory_order_release);
		// Bounded wait until the reader has seen this slot live, so the swap and
		// the free below race a reader that is known to be reading it.
		const uint64_t deadline = os ? os->get_ticks_msec() + 1000 : 0;
		while (reader.observed_id.load(std::memory_order_acquire) != rid.get_id() && os && os->get_ticks_msec() < deadline) {
			os->delay_usec(10);
		}
		if (reader.observed_id.load(std::memory_order_acquire) == rid.get_id()) {
			overlapped++;
		}
		storage->gaussian_set_renderer(rid, (i & 1) ? renderer_b : renderer_a);
		storage->gaussian_set_aabb(rid, AABB(Vector3(i, 0, 0), Vector3(1, 1, 1)));
		storage->gaussian_set_casts_shadow(rid, (i & 1) != 0);
		storage->gaussian_set_renderer(rid, Ref<GaussianSplatRenderer>());
		storage->gaussian_free(rid);
	}
	reader.stop.store(true, std::memory_order_release);
	thread.wait_to_finish();
	ERR_PRINT_ON;

	CHECK_MESSAGE(overlapped > 0, "the reader never observed a live slot; the concurrent part did not run");
	CHECK(reader.foreign_pointers.load() == 0);
	CHECK(renderer_a->get_reference_count() == refs_a);
	CHECK(renderer_b->get_reference_count() == refs_b);

	memdelete(storage);
}

} // namespace TestGaussianStorageRenderThreadRouting

#endif // TEST_GAUSSIAN_STORAGE_RENDER_THREAD_ROUTING_H
