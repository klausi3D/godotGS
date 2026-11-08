extern "C" int test_hlod_auto_route_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gs_project_settings.h"
#include "../io/gaussian_splat_world_io.h"
#include "../lod/hlod_auto_route.h"
#include "../lod/lod_config.h"
#include "../nodes/gaussian_splat_world_3d.h"
#include "../renderer/quantization_config.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/resource_saver.h"
#include "core/object/message_queue.h"
#include "core/object/ref_counted.h"
#include "core/os/os.h"
#include "scene/3d/node_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/world_3d.h"

// The automatic HLOD route (docs/architecture/adr-one-route-many-producers.md §4.7, decision 5): a world with an HLOD
// tree takes the HLOD route with no project setting changed; lod/hlod_runtime_cut_enabled = false is the opt-out; a world
// without a tree is unchanged. CPU only: the pure plan (lod/hlod_auto_route.h) and the real GaussianSplatWorld3D
// submission it drives, read back from the director (no RenderingDevice, so no renderer is created).
namespace TestHlodAutoRoute {
using gs_hlod_auto_route::WorldInputs;
using gs_hlod_auto_route::WorldPlan;
using gs_hlod_auto_route::plan_world;
using gs_hlod_auto_route::residency_hint_resident;

#define GS_NEED(cond) \
	do { \
		if (!(cond)) { \
			FAIL("requirement failed: ", #cond); \
			return; \
		} \
	} while (0)

static const char *CUT_KEY = LOD_CONFIG_HLOD_RUNTIME_ENABLED_PATH;
static const char *ROUTE_KEY = "rendering/gaussian_splatting/streaming/route_policy";

TEST_CASE("[GaussianSplatting][HLOD] automatic route: the plan over tree x payload x opt-out x paged x route policy x transform x domain") {
	const char *domains[] = { nullptr, "lod_disabled", "deformation_or_instance_domain", "painterly_enabled", "shadow_instance_filter" };
	for (int kind = 0; kind < 4; ++kind) { // 0 no tree, 1 tree over a resident payload, 2 paged package, 3 tree over a file-backed payload
		for (bool cut : { true, false }) {
			for (bool streaming : { true, false }) {
				for (bool identity : { true, false }) {
					for (const char *domain : domains) {
						WorldInputs in;
						in.cut_enabled = cut;
						in.has_hlod_tree = kind == 1 || kind == 3;
						in.resident_payload = kind != 3;
						in.paged = kind == 2;
						in.identity_transform = identity;
						in.route_policy_streaming = streaming;
						in.domain_refusal = domain;
						const WorldPlan plan = plan_world(in);
						const String what = vformat("kind=%d cut=%d streaming=%d identity=%d domain=%s", kind, cut, streaming, identity, domain ? domain : "none");
						if (kind == 0 || kind == 3 || !cut) {
							// No snapshot-capable tree, or the opt-out: exactly the routing from before (no snapshot, no reason,
							// route_policy decides). A file-backed tree cannot be snapshotted, so it is not an HLOD-route world.
							CHECK_MESSAGE(!plan.capture_snapshot, what);
							CHECK_MESSAGE(plan.reason.is_empty(), what);
							CHECK_MESSAGE(residency_hint_resident(streaming, plan.capture_snapshot) == !streaming, what);
						} else if (!identity) {
							CHECK_MESSAGE(!plan.capture_snapshot, what);
							CHECK_MESSAGE(plan.reason == String("hlod_world_transform_unsupported"), what);
							CHECK_MESSAGE(residency_hint_resident(streaming, false) == !streaming, what);
						} else if (domain && streaming && kind == 1) {
							// A setup the route never serves keeps the chunk route, with a distinct visible reason.
							CHECK_MESSAGE(!plan.capture_snapshot, what);
							CHECK_MESSAGE(plan.reason == String("hlod_auto_route_unavailable:") + domain, what);
							CHECK_MESSAGE(!residency_hint_resident(streaming, false), what);
						} else {
							// The automatic route: a snapshot, and the resident backend the HLOD route runs on, whatever route_policy says.
							CHECK_MESSAGE(plan.capture_snapshot, what);
							CHECK_MESSAGE(plan.reason.is_empty(), what);
							CHECK_MESSAGE(residency_hint_resident(streaming, true), what);
						}
					}
				}
			}
		}
	}
	// A snapshot the world could not capture (admission, deformation, ...) never pins the resident backend.
	CHECK_FALSE(residency_hint_resident(true, false));
	CHECK(residency_hint_resident(false, false));
}

// [Config]: the strict Config lane. In the advisory [untagged] lane this case failed from #1309 on and CI stayed green (#1371).
TEST_CASE("[GaussianSplatting][HLOD][Config] automatic route: the runtime cut is on by default and the getter reads it so") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) { FAIL("ProjectSettings singleton required"); return; }
	if (!ps->has_setting(CUT_KEY)) { FAIL("lod/hlod_runtime_cut_enabled is not registered"); return; }
	// The registered default (GLOBAL_DEF), which is what a project that never set the key reads.
	CHECK(ps->property_get_revert(CUT_KEY).get_type() == Variant::BOOL);
	CHECK(bool(ps->property_get_revert(CUT_KEY)));
	{
		// At its default the getter is on; the opt-out switches it off.
		const Variant previous = ps->get_setting(CUT_KEY);
		ps->set_setting(CUT_KEY, ps->property_get_revert(CUT_KEY));
		CHECK(gs::settings::get_hlod_runtime_cut_enabled(ps));
		ps->set_setting(CUT_KEY, false);
		CHECK_FALSE(gs::settings::get_hlod_runtime_cut_enabled(ps));
		ps->set_setting(CUT_KEY, previous);
	}
	// route_policy keeps its default: streaming. It decides only worlds that do not take the HLOD route.
	if (!ps->has_setting(ROUTE_KEY)) { FAIL("streaming/route_policy is not registered"); return; }
	CHECK(int(ps->property_get_revert(ROUTE_KEY)) == int(gs::settings::GS_ROUTE_STREAMING));
}

static Ref<GaussianSplatAsset> node_asset() {
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	asset->set_splat_count(4);
	PackedFloat32Array positions, scales, rotations, dc, opacity;
	for (uint32_t i = 0; i < 4; ++i) {
		positions.push_back(0.01f * float(i));
		positions.push_back(0.0f);
		positions.push_back(0.0f);
		for (int a = 0; a < 3; ++a) { scales.push_back(0.05f); }
		rotations.push_back(1.0f);
		rotations.push_back(0.0f);
		rotations.push_back(0.0f);
		rotations.push_back(0.0f);
		for (int a = 0; a < 3; ++a) { dc.push_back(1.0f); }
		opacity.push_back(10.0f);
	}
	asset->set_positions(positions);
	asset->set_scales(scales);
	asset->set_rotations(rotations);
	asset->set_sh_dc_coefficients(dc);
	asset->set_opacity_logits(opacity);
	return asset;
}

static Ref<GaussianData> wall_data(uint32_t p_count, bool p_mixed_dc = false) {
	Ref<GaussianData> data;
	data.instantiate();
	LocalVector<Gaussian> splats;
	splats.resize(p_count);
	for (uint32_t i = 0; i < p_count; ++i) {
		splats[i].position = Vector3(0.125f + float(i) * 0.25f, 0.125f, -4.0f);
		splats[i].scale = Vector3(0.125f, 0.125f, 0.125f);
		splats[i].rotation = Quaternion();
		splats[i].opacity = 0.75f;
		if (p_mixed_dc) {
			// Alternating DC encodings in one node: the quantized route refuses the tree (hlod_quantized_mixed_dc_node).
			splats[i].render_meta = gaussian_set_dc_encoding(splats[i].render_meta,
					(i % 2u) == 0u ? GAUSSIAN_DC_ENCODING_LEGACY_BIAS : GAUSSIAN_DC_ENCODING_LINEAR_RGB);
		}
	}
	LocalVector<Vector3> sh;
	data->set_gaussian_payload(splats, sh, 0u, 0u, false, false);
	return data;
}

enum class Tree { NONE, RESIDENT, PAGED, FILE_BACKED };

// Every world is loaded from a file, as an imported world is. NONE and FILE_BACKED: the streamable v3 file a generic
// ResourceSaver::save() writes (without / with the baked tree), loaded as ResourceLoader does (file-backed payload).
// RESIDENT: the same file with its baked tree loaded through load_resident(), the world importer's own path. PAGED: a
// V4 package (save_hlod_paged). r_path is the file, removed by the caller.
static Ref<GaussianSplatWorld> load_world(Tree p_tree, String &r_path, bool p_mixed_dc = false) {
	Ref<GaussianSplatWorld> source;
	source.instantiate();
	source->set_gaussian_data(wall_data(4, p_mixed_dc));
	const bool tree = p_tree != Tree::NONE;
	if (tree && source->bake_hlod() != OK) { return Ref<GaussianSplatWorld>(); }
	r_path = OS::get_singleton()->get_temp_path().path_join(vformat("godotgs_auto_route_%d_%d.gsplatworld", int(p_tree), int64_t(OS::get_singleton()->get_ticks_usec())));
	const Error saved = p_tree == Tree::PAGED ? source->save_hlod_paged(r_path) : ResourceSaver::save(source, r_path);
	if (saved != OK) { return Ref<GaussianSplatWorld>(); }
	Error err = ERR_BUG;
	ResourceFormatLoaderGaussianSplatWorld loader;
	Ref<GaussianSplatWorld> loaded = p_tree == Tree::RESIDENT ? loader.load_resident(r_path, &err) : Ref<GaussianSplatWorld>(loader.load(r_path, "", &err));
	const bool paged = p_tree == Tree::PAGED;
	if (err != OK || loaded.is_null() || loaded->has_hlod_paged_source() != paged || (!paged && loaded->has_hlod_tree() != tree) ||
			loaded->has_resident_gaussian_data() != (p_tree == Tree::RESIDENT)) {
		return Ref<GaussianSplatWorld>();
	}
	return loaded;
}

struct RouteCase {
	Tree tree = Tree::NONE;
	bool opt_out = false; // lod/hlod_runtime_cut_enabled = false
	uint32_t nodes = 0;
	bool route_policy_resident = false; // false: route_policy left at its default (streaming)
	bool lod_disabled = false; // a domain the HLOD route never serves
	bool moved = false; // a non-identity world transform
	bool mixed_dc = false; // the tree's node mixes DC encodings
	bool quantized = false; // per-chunk quantization on (g_quantization_config) while the world submits
	String demote_cause; // non-empty: then call the submission's demotion callback with it, as the renderer's request does
};

struct RouteResult {
	bool ok = false;
	String failure;
	GaussianSplatSceneDirector::WorldSubmission submission;
	// The director's HLOD ownership predicate (an active submission by this owner, PR1b: whatever the nodes). The route
	// needs it and the snapshot; without a snapshot the publisher never runs, so it is only asserted where one exists.
	bool hlod_world_submission = false;
	bool has_snapshot = false; // read before the snapshot (and the paged file it reads) is released
	bool snapshot_paged = false;
	bool still_paged = false;
	int world_count = 0;
	bool had_demotion_callback = false; // the first submission carried the node's demotion callback
	bool before_had_snapshot = false; // the first submission (before demote_cause) carried a snapshot
};

static RouteResult run_case(const RouteCase &p_case) {
	RouteResult result;
	SceneTree *tree = SceneTree::get_singleton();
	Window *root = tree ? tree->get_root() : nullptr;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!root || !ps || root->get_world_3d().is_null()) { result.failure = "SceneTree root with a World3D required"; return result; }
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = director == nullptr;
	if (owns_director) { director = memnew(GaussianSplatSceneDirector); }
	// No flag set: both keys at their registered defaults, the opt-out and the policy only where the case says so.
	const bool had_cut = ps->has_setting(CUT_KEY), had_route = ps->has_setting(ROUTE_KEY);
	const Variant prev_cut = had_cut ? ps->get_setting(CUT_KEY) : Variant(), prev_route = had_route ? ps->get_setting(ROUTE_KEY) : Variant();
	ps->set_setting(CUT_KEY, p_case.opt_out ? Variant(false) : ps->property_get_revert(CUT_KEY));
	ps->set_setting(ROUTE_KEY, p_case.route_policy_resident ? Variant(int64_t(gs::settings::GS_ROUTE_RESIDENT)) : ps->property_get_revert(ROUTE_KEY));
	String path;
	const bool previous_quantized = g_quantization_config.per_chunk_quantization;
	g_quantization_config.per_chunk_quantization = p_case.quantized;
	Ref<GaussianSplatWorld> world = load_world(p_case.tree, path, p_case.mixed_dc);
	LocalVector<Node3D *> nodes;
	GaussianSplatWorld3D *world_node = nullptr;
	if (world.is_null()) {
		result.failure = "world fixture save/load failed";
	} else {
		// Nodes first: the decision is made when the world submits, and nodes must not change it.
		Ref<GaussianSplatAsset> asset = node_asset();
		for (uint32_t i = 0; i < p_case.nodes; ++i) {
			Node3D *node = memnew(Node3D);
			root->add_child(node);
			nodes.push_back(node);
			Transform3D t;
			t.origin = Vector3(float(i), 0.0f, -6.0f);
			director->register_instance(node->get_instance_id(), asset, t, 1.0f, 0.0f, 0u);
		}
		world_node = memnew(GaussianSplatWorld3D);
		world_node->set_auto_apply_on_ready(false);
		world_node->set_lod_enabled(!p_case.lod_disabled);
		world_node->set_world(world);
		if (p_case.moved) { world_node->set_position(Vector3(1.0f, 0.0f, 0.0f)); }
		root->add_child(world_node);
		tree->process(0.0);
		world_node->apply_world();
		bool submitted = director->get_world_submission(world_node->get_instance_id(), &result.submission);
		result.had_demotion_callback = submitted && result.submission.hlod_demotion_callback.is_valid();
		result.before_had_snapshot = submitted && result.submission.hlod_snapshot.is_valid();
		if (submitted && !p_case.demote_cause.is_empty()) {
			if (!result.had_demotion_callback) {
				result.failure = "the submission carries no demotion callback";
				submitted = false;
			} else {
				// The main-thread half of the renderer's request (the director queues exactly this call).
				result.submission.hlod_demotion_callback.call(p_case.demote_cause);
				result.submission = GaussianSplatSceneDirector::WorldSubmission();
				submitted = director->get_world_submission(world_node->get_instance_id(), &result.submission);
			}
		}
		if (!submitted) {
			if (result.failure.is_empty()) { result.failure = "the world submitted nothing"; }
		} else {
			result.ok = true;
			result.hlod_world_submission = director->test_has_hlod_world_submission_for_scenario(root->get_world_3d()->get_scenario(),
					uint64_t(world_node->get_instance_id()));
			result.still_paged = world->has_hlod_paged_source();
			result.world_count = world->get_gaussian_data().is_valid() ? world->get_gaussian_data()->get_count() : 0;
		}
	}
	for (Node3D *node : nodes) {
		director->unregister_instance(node->get_instance_id());
		root->remove_child(node);
		memdelete(node);
	}
	if (world_node) {
		root->remove_child(world_node);
		memdelete(world_node);
	}
	tree->process(0.0);
	if (owns_director) { memdelete(director); }
	g_quantization_config.per_chunk_quantization = previous_quantized;
	if (had_cut) { ps->set_setting(CUT_KEY, prev_cut); } else { ps->clear(CUT_KEY); }
	if (had_route) { ps->set_setting(ROUTE_KEY, prev_route); } else { ps->clear(ROUTE_KEY); }
	result.has_snapshot = result.submission.hlod_snapshot.is_valid();
	result.snapshot_paged = result.has_snapshot && result.submission.hlod_snapshot->is_paged();
	result.submission.hlod_snapshot.unref();
	result.submission.payload_source.unref();
	result.submission.hlod_demotion_callback = Callable();
	world.unref();
	if (!path.is_empty()) { DirAccess::remove_absolute(path); }
	return result;
}

TEST_CASE("[GaussianSplatting][HLOD][SceneDirector][SceneTree] automatic route: a loaded world with an HLOD tree takes the HLOD route with no flag set, with or without nodes") {
	const int32_t RESIDENT = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
	const int32_t STREAMING = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING;
	for (Tree tree : { Tree::NONE, Tree::RESIDENT, Tree::PAGED, Tree::FILE_BACKED }) {
		for (bool opt_out : { false, true }) {
			for (uint32_t nodes : { 0u, 2u }) {
				RouteCase c;
				c.tree = tree;
				c.opt_out = opt_out;
				c.nodes = nodes;
				const RouteResult r = run_case(c);
				const String what = vformat("tree=%d opt_out=%d nodes=%d", int(tree), opt_out, nodes);
				if (!r.ok) { FAIL(what, ": ", r.failure); return; }
				const GaussianSplatSceneDirector::WorldSubmission &s = r.submission;
				CHECK_MESSAGE(s.has_desired_residency_hint, what);
				if (tree == Tree::NONE || tree == Tree::FILE_BACKED) {
					// A world without a snapshot-capable tree is unchanged: route_policy's streaming hint, no snapshot, no
					// reason, also with the cut on (a file-backed tree used to report hlod_requires_resident_complete_uninstanced_tree).
					CHECK_MESSAGE(!r.has_snapshot, what);
					CHECK_MESSAGE(s.hlod_reason.is_empty(), what);
					CHECK_MESSAGE(s.desired_residency_hint == STREAMING, what);
				} else if (!opt_out) {
					// The automatic route: a snapshot and the resident backend, the world's HLOD submission whatever the nodes.
					if (!r.has_snapshot) { FAIL(what, ": no HLOD snapshot, reason ", s.hlod_reason); continue; }
					CHECK_MESSAGE(r.snapshot_paged == (tree == Tree::PAGED), what);
					CHECK_MESSAGE(s.hlod_reason.is_empty(), what);
					CHECK_MESSAGE(s.desired_residency_hint == RESIDENT, what);
					CHECK_MESSAGE(r.hlod_world_submission, what);
					CHECK_MESSAGE(r.still_paged == (tree == Tree::PAGED), what);
					CHECK_MESSAGE(r.had_demotion_callback, what); // the renderer can demote it
				} else if (tree == Tree::RESIDENT) {
					// The opt-out: the routing from before, exactly (no snapshot, no reason, route_policy's hint).
					CHECK_MESSAGE(!r.has_snapshot, what);
					CHECK_MESSAGE(s.hlod_reason.is_empty(), what);
					CHECK_MESSAGE(s.desired_residency_hint == STREAMING, what);
				} else {
					// A paged package with the opt-out loads its payload and draws on route_policy's route, with the reason.
					CHECK_MESSAGE(!r.has_snapshot, what);
					CHECK_MESSAGE(s.hlod_reason == String("hlod_paged_requires_runtime_cut"), what);
					CHECK_MESSAGE(s.desired_residency_hint == STREAMING, what);
					CHECK_MESSAGE(!r.still_paged, what);
					CHECK_MESSAGE(r.world_count > 0, what);
					CHECK_MESSAGE((s.gaussian_data.is_valid() && s.gaussian_data->get_count() > 0), what);
				}
			}
		}
	}
}

TEST_CASE("[GaussianSplatting][HLOD][SceneDirector][SceneTree] automatic route: route_policy, a refused domain and a moved world keep their routes") {
	const int32_t RESIDENT = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
	const int32_t STREAMING = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING;
	{
		// route_policy = resident with the opt-out: the resident backend, no snapshot (unchanged).
		RouteCase c;
		c.tree = Tree::RESIDENT;
		c.opt_out = true;
		c.route_policy_resident = true;
		const RouteResult r = run_case(c);
		if (!r.ok) { FAIL("resident opt-out: ", r.failure); return; }
		CHECK_FALSE(r.has_snapshot);
		CHECK(r.submission.desired_residency_hint == RESIDENT);
	}
	{
		// LOD off under the default policy: the HLOD route never serves it, so the world keeps the chunk route and says why.
		RouteCase c;
		c.tree = Tree::RESIDENT;
		c.nodes = 2;
		c.lod_disabled = true;
		const RouteResult r = run_case(c);
		if (!r.ok) { FAIL("lod off: ", r.failure); return; }
		CHECK_FALSE(r.has_snapshot);
		CHECK(r.submission.hlod_reason == String("hlod_auto_route_unavailable:lod_disabled"));
		CHECK(r.submission.desired_residency_hint == STREAMING);
	}
	{
		// The same under route_policy = resident: the snapshot is captured as before, and the publisher draws the
		// canonical leaves until LOD is enabled again.
		RouteCase c;
		c.tree = Tree::RESIDENT;
		c.lod_disabled = true;
		c.route_policy_resident = true;
		const RouteResult r = run_case(c);
		if (!r.ok) { FAIL("lod off resident: ", r.failure); return; }
		CHECK(r.has_snapshot);
		CHECK(r.submission.desired_residency_hint == RESIDENT);
	}
	{
		// A paged package with LOD off loads its payload (its existing fallback); it never gets the auto-route reason.
		RouteCase c;
		c.tree = Tree::PAGED;
		c.lod_disabled = true;
		const RouteResult r = run_case(c);
		if (!r.ok) { FAIL("paged lod off: ", r.failure); return; }
		CHECK_FALSE(r.has_snapshot);
		CHECK(r.submission.hlod_reason == String("hlod_paged_lod_disabled"));
		CHECK_FALSE(r.still_paged);
		CHECK(r.world_count > 0);
		CHECK(r.submission.desired_residency_hint == STREAMING);
	}
	{
		// A moved world cannot take the route: no snapshot, the transform reason, route_policy's hint.
		RouteCase c;
		c.tree = Tree::RESIDENT;
		c.moved = true;
		const RouteResult r = run_case(c);
		if (!r.ok) { FAIL("moved: ", r.failure); return; }
		CHECK_FALSE(r.has_snapshot);
		CHECK(r.submission.hlod_reason == String("hlod_world_transform_unsupported"));
		CHECK(r.submission.desired_residency_hint == STREAMING);
	}
}

} // namespace TestHlodAutoRoute

namespace TestHlodAutoRoute {

TEST_CASE("[GaussianSplatting][HLOD] automatic route: the demotion state machine - which refusals demote, once per snapshot") {
	using gs_hlod_auto_route::DemotionLatch;
	using gs_hlod_auto_route::RefusalInputs;
	using gs_hlod_auto_route::RefusalStage;
	using gs_hlod_auto_route::demotion_cause;
	auto cause = [](RefusalStage p_stage, const char *p_reason, bool p_main, bool p_no_copy, bool p_edit) -> String {
		RefusalInputs in;
		in.stage = p_stage;
		in.reason = p_reason;
		in.main_view = p_main;
		in.no_canonical_copy = p_no_copy;
		in.committed_edit = p_edit;
		const char *c = demotion_cause(in);
		return c ? String(c) : String();
	};
	const RefusalStage F = RefusalStage::FRAME, P = RefusalStage::PUBLISH;
	// Per frame, on a streamed or paged world (no canonical copy), in the main view: the view domains and late domains.
	CHECK(cause(F, "unsupported_view_or_material", true, true, false) == "unsupported_view_or_material");
	CHECK(cause(F, "deformation_or_instance_domain", true, true, false) == "deformation_or_instance_domain");
	CHECK(cause(F, "hlod_paged_deformation_or_instance_domain", true, true, false) == "deformation_or_instance_domain");
	CHECK(cause(F, "lod_disabled_canonical_leaves", true, true, false) == "lod_disabled");
	CHECK(cause(F, "hlod_paged_lod_disabled", true, true, false) == "lod_disabled");
	// Never: a whole-tree world (its canonical copy draws the frame), a shadow or manual pass, an edited world, a transient
	// or budget refusal, no reason.
	CHECK(cause(F, "unsupported_view_or_material", true, false, false).is_empty());
	CHECK(cause(F, "unsupported_view_or_material", false, true, false).is_empty());
	CHECK(cause(F, "unsupported_view_or_material", true, true, true).is_empty());
	for (const char *transient : { "tile_device_unavailable", "resource_ledger_unavailable", "hlod_world_submission_lost",
				 "hlod_paged_world_submission_lost", "hlod_instance_rows_inconsistent", "active" }) {
		CHECK_MESSAGE(cause(F, transient, true, true, false).is_empty(), transient);
		CHECK_MESSAGE(cause(P, transient, true, true, false).is_empty(), transient);
	}
	CHECK(cause(F, nullptr, true, true, false).is_empty());
	// At publish, a paged world or one too large for its whole tree, in a setup that cannot stream.
	CHECK(cause(P, "lod_disabled_canonical_leaves", true, true, false) == "lod_disabled");
	CHECK(cause(P, "shadow_instance_filter", true, true, false) == "shadow_instance_filter");
	CHECK(cause(P, "unsupported_view_or_material", true, true, false) == "painterly_enabled");
	CHECK(cause(P, "deformation_or_instance_domain", true, true, false) == "deformation_or_instance_domain");
	CHECK(cause(P, "unsupported_instance_transform_domain", true, true, false) == "unsupported_instance_transform_domain");
	CHECK(cause(P, "shadow_instance_filter", true, false, false).is_empty()); // a whole tree that fits builds and draws
	CHECK(cause(P, "shadow_instance_filter", true, true, true).is_empty());
	// Quantization over a mixed-DC tree demotes at publish whatever the copy (the copy is the resident one); never per frame.
	CHECK(cause(P, "hlod_quantized_mixed_dc_node", true, false, false) == "quantized_mixed_dc_node");
	CHECK(cause(P, "hlod_quantized_mixed_dc_node", true, true, false) == "quantized_mixed_dc_node");
	CHECK(cause(P, "hlod_quantized_mixed_dc_node", true, true, true).is_empty());
	CHECK(cause(F, "hlod_quantized_mixed_dc_node", true, true, false).is_empty());
	// The latch: the first demoting refusal asks, later frames do not; a new snapshot re-arms it.
	DemotionLatch latch;
	CHECK_FALSE(latch.request(nullptr));
	CHECK_FALSE(latch.requested);
	CHECK(latch.request("unsupported_view_or_material"));
	CHECK(latch.requested);
	CHECK(latch.cause == String("unsupported_view_or_material"));
	CHECK_FALSE(latch.request("lod_disabled"));
	CHECK(latch.cause == String("unsupported_view_or_material"));
	latch.rearm();
	CHECK_FALSE(latch.requested);
	CHECK(latch.cause.is_empty());
	CHECK(latch.request("quantized_mixed_dc_node"));
	// A demoted world plans no snapshot and no reason of its own (the node states hlod_auto_route_demoted:<cause>).
	WorldInputs in;
	in.has_hlod_tree = true;
	in.resident_payload = true;
	in.demoted = true;
	for (bool paged : { false, true }) {
		in.paged = paged;
		const WorldPlan plan = plan_world(in);
		CHECK_FALSE(plan.capture_snapshot);
		CHECK(plan.reason.is_empty());
	}
	in.cut_enabled = false; // The opt-out stays the opt-out.
	CHECK_FALSE(plan_world(in).capture_snapshot);
}

TEST_CASE("[GaussianSplatting][HLOD][SceneDirector][SceneTree] automatic route: a demoted world resubmits on the route it has without the automatic route and stays there") {
	const int32_t RESIDENT = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
	const int32_t STREAMING = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING;
	for (Tree tree : { Tree::RESIDENT, Tree::PAGED }) {
		for (bool resident_policy : { false, true }) {
			// The renderer's request after an orthographic main view refused a streamed or paged world.
			RouteCase c;
			c.tree = tree;
			c.nodes = 2;
			c.route_policy_resident = resident_policy;
			c.demote_cause = "unsupported_view_or_material";
			const RouteResult r = run_case(c);
			const String what = vformat("tree=%d resident_policy=%d", int(tree), resident_policy);
			if (!r.ok) { FAIL(what, ": ", r.failure); return; }
			CHECK_MESSAGE(r.before_had_snapshot, what);
			CHECK_MESSAGE(!r.has_snapshot, what);
			CHECK_MESSAGE(r.submission.hlod_reason == String("hlod_auto_route_demoted:unsupported_view_or_material"), what);
			CHECK_MESSAGE(!r.submission.hlod_demotion_callback.is_valid(), what); // nothing left to demote
			// master's route: route_policy's backend; a paged world loaded its payload into memory.
			CHECK_MESSAGE(r.submission.desired_residency_hint == (resident_policy ? RESIDENT : STREAMING), what);
			CHECK_MESSAGE(!r.still_paged, what);
			CHECK_MESSAGE(r.world_count > 0, what);
		}
	}
	{
		// Quantization over a mixed-DC tree is known at submission: a paged package is loaded into memory right away
		// (master's route), never submitted to a route that would draw nothing.
		RouteCase c;
		c.tree = Tree::PAGED;
		c.mixed_dc = true;
		c.quantized = true;
		const RouteResult r = run_case(c);
		if (!r.ok) { FAIL("paged mixed dc: ", r.failure); return; }
		CHECK_FALSE(r.has_snapshot);
		CHECK(r.submission.hlod_reason == String("hlod_auto_route_demoted:quantized_mixed_dc_node"));
		CHECK(r.submission.desired_residency_hint == STREAMING);
		CHECK_FALSE(r.still_paged);
		CHECK(r.world_count > 0);
	}
	{
		// The same tree without quantization takes the route; a uniform tree with quantization takes it too.
		RouteCase c;
		c.tree = Tree::PAGED;
		c.mixed_dc = true;
		const RouteResult r = run_case(c);
		if (!r.ok) { FAIL("paged mixed dc unquantized: ", r.failure); return; }
		CHECK(r.has_snapshot);
		CHECK(r.had_demotion_callback);
		RouteCase q;
		q.tree = Tree::PAGED;
		q.quantized = true;
		const RouteResult rq = run_case(q);
		if (!rq.ok) { FAIL("paged uniform quantized: ", rq.failure); return; }
		CHECK(rq.has_snapshot);
	}
	{
		// A resident world with a mixed-DC tree under quantization: demoted at submission as well.
		RouteCase c;
		c.tree = Tree::RESIDENT;
		c.mixed_dc = true;
		c.quantized = true;
		const RouteResult r = run_case(c);
		if (!r.ok) { FAIL("resident mixed dc: ", r.failure); return; }
		CHECK_FALSE(r.has_snapshot);
		CHECK(r.submission.hlod_reason == String("hlod_auto_route_demoted:quantized_mixed_dc_node"));
		CHECK(r.submission.desired_residency_hint == STREAMING);
	}
}

} // namespace TestHlodAutoRoute

namespace TestHlodAutoRoute {

TEST_CASE("[GaussianSplatting][HLOD] automatic route: installing a replacement snapshot re-arms the demotion latch, before the held one is overwritten") {
	using gs_hlod_auto_route::DemotionLatch;
	using gs_hlod_auto_route::install_snapshot;
	Ref<RefCounted> first;
	first.instantiate();
	Ref<RefCounted> replacement;
	replacement.instantiate();
	Ref<RefCounted> held;
	DemotionLatch latch;
	CHECK(install_snapshot(held, first, latch));
	CHECK(held == first);
	CHECK(latch.request("unsupported_view_or_material"));
	// The same snapshot again (a same-input resubmission): still requested, never every frame.
	CHECK_FALSE(install_snapshot(held, first, latch));
	CHECK(latch.requested);
	// A replacement world on the same renderer (no demoted resubmission between): re-armed, a later refusal asks again.
	CHECK(install_snapshot(held, replacement, latch));
	CHECK(held == replacement);
	CHECK_FALSE(latch.requested);
	CHECK(latch.request("unsupported_view_or_material"));
	// The demoted world resubmits without a snapshot: the latch and its cause stay (the render stat reports it).
	CHECK(install_snapshot(held, Ref<RefCounted>(), latch));
	CHECK(held.is_null());
	CHECK(latch.requested);
	CHECK(latch.cause == String("unsupported_view_or_material"));
	// apply_world() again: a new snapshot re-arms it.
	CHECK(install_snapshot(held, first, latch));
	CHECK_FALSE(latch.requested);
	CHECK(latch.cause.is_empty());
}

// A demotion request belongs to the submission whose snapshot was refused. Codex P2 on #1309: a request queued from the
// render thread and delivered after set_world()/apply_world() demoted the newly applied submission instead (a supported
// replacement world moved off the HLOD route for good, a paged one loaded into memory). Two halves: the director queues
// only for the record's own snapshot, and the node drops a call bound to an older submission generation.
TEST_CASE("[GaussianSplatting][HLOD][SceneDirector][SceneTree] automatic route: a demotion request for a replaced submission is dropped") {
	SceneTree *tree = SceneTree::get_singleton();
	Window *root = tree ? tree->get_root() : nullptr;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!root || !ps || root->get_world_3d().is_null()) { FAIL("SceneTree root with a World3D required"); return; }
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = director == nullptr;
	if (owns_director) { director = memnew(GaussianSplatSceneDirector); }
	const bool had_cut = ps->has_setting(CUT_KEY), had_route = ps->has_setting(ROUTE_KEY);
	const Variant prev_cut = had_cut ? ps->get_setting(CUT_KEY) : Variant(), prev_route = had_route ? ps->get_setting(ROUTE_KEY) : Variant();
	ps->set_setting(CUT_KEY, ps->property_get_revert(CUT_KEY));
	ps->set_setting(ROUTE_KEY, ps->property_get_revert(ROUTE_KEY));
	const String CAUSE = "unsupported_view_or_material";
	const String DEMOTED = String("hlod_auto_route_demoted:") + CAUSE;
	const RID scenario = root->get_world_3d()->get_scenario();
	for (Tree kind : { Tree::RESIDENT, Tree::PAGED }) {
		const String what = vformat("tree=%d", int(kind));
		String path, replacement_path;
		Ref<GaussianSplatWorld> world = load_world(kind, path);
		Ref<GaussianSplatWorld> replacement = load_world(kind, replacement_path);
		GaussianSplatWorld3D *world_node = nullptr;
		if (world.is_null() || replacement.is_null()) {
			FAIL(what, ": world fixture save/load failed");
		} else {
			world_node = memnew(GaussianSplatWorld3D);
			world_node->set_auto_apply_on_ready(false);
			world_node->set_world(world);
			root->add_child(world_node);
			tree->process(0.0);
			world_node->apply_world();
			const uint64_t owner = uint64_t(world_node->get_instance_id());
			auto submitted = [&](GaussianSplatSceneDirector::WorldSubmission &r_s) {
				r_s = GaussianSplatSceneDirector::WorldSubmission();
				return director->get_world_submission(world_node->get_instance_id(), &r_s);
			};
			GaussianSplatSceneDirector::WorldSubmission first;
			if (!submitted(first) || first.hlod_snapshot.is_null() || !first.hlod_demotion_callback.is_valid()) {
				FAIL(what, ": the world did not take the HLOD route (reason ", first.hlod_reason, ")");
			} else {
				// 1. The exact race: the renderer's request is queued for the current snapshot, then apply_world() runs
				//    before the deferred call is delivered. The replacement submission keeps its snapshot.
				CHECK_MESSAGE(director->test_request_hlod_auto_route_demotion_for_scenario(scenario, owner, first.hlod_snapshot, CAUSE), what);
				world_node->apply_world();
				MessageQueue::get_singleton()->flush();
				GaussianSplatSceneDirector::WorldSubmission second;
				CHECK_MESSAGE(submitted(second), what);
				CHECK_MESSAGE(second.hlod_snapshot.is_valid(), String(what + ": a request queued before apply_world() demoted its replacement"));
				CHECK_MESSAGE(second.hlod_reason != DEMOTED, what);
				CHECK_MESSAGE(second.hlod_snapshot != first.hlod_snapshot, what);
				if (kind == Tree::PAGED) {
					CHECK_MESSAGE(world->has_hlod_paged_source(), String(what + ": a stale request loaded the paged world into memory"));
				}
				// 2. The renderer still holds the replaced snapshot (the new contract has not reached it): the director
				//    queues nothing for the new submission.
				CHECK_FALSE_MESSAGE(director->test_request_hlod_auto_route_demotion_for_scenario(scenario, owner, first.hlod_snapshot, CAUSE), what);
				CHECK_FALSE_MESSAGE(director->test_request_hlod_auto_route_demotion_for_scenario(scenario, owner, Ref<GaussianSplatHlodRuntimeData>(), CAUSE), what);
				// 3. The previous submission's callback, called directly, is dropped as well.
				first.hlod_demotion_callback.call(CAUSE);
				GaussianSplatSceneDirector::WorldSubmission after_stale;
				CHECK_MESSAGE((submitted(after_stale) && after_stale.hlod_snapshot.is_valid() && after_stale.hlod_reason != DEMOTED), what);
				// 4. A replacement world (set_world()) with a request of the old world in flight: the same.
				CHECK_MESSAGE(director->test_request_hlod_auto_route_demotion_for_scenario(scenario, owner, after_stale.hlod_snapshot, CAUSE), what);
				world_node->set_world(replacement);
				MessageQueue::get_singleton()->flush();
				GaussianSplatSceneDirector::WorldSubmission replaced;
				CHECK_MESSAGE((submitted(replaced) && replaced.hlod_snapshot.is_valid() && replaced.hlod_reason != DEMOTED), String(what + ": a request of the old world demoted the replacement"));
				if (kind == Tree::PAGED) {
					CHECK_MESSAGE(replacement->has_hlod_paged_source(), String(what + ": a stale request loaded the replacement paged world into memory"));
				}
				// 5. A request for the current submission still demotes it (discrimination: the drop is not a blanket no).
				CHECK_MESSAGE(director->test_request_hlod_auto_route_demotion_for_scenario(scenario, owner, replaced.hlod_snapshot, CAUSE), what);
				MessageQueue::get_singleton()->flush();
				GaussianSplatSceneDirector::WorldSubmission demoted;
				CHECK_MESSAGE((submitted(demoted) && demoted.hlod_snapshot.is_null() && demoted.hlod_reason == DEMOTED), String(what + ": a current request did not demote"));
				first = second = after_stale = replaced = demoted = GaussianSplatSceneDirector::WorldSubmission();
			}
		}
		if (world_node) {
			root->remove_child(world_node);
			memdelete(world_node);
		}
		tree->process(0.0);
		MessageQueue::get_singleton()->flush();
		world.unref();
		replacement.unref();
		if (!path.is_empty()) { DirAccess::remove_absolute(path); }
		if (!replacement_path.is_empty()) { DirAccess::remove_absolute(replacement_path); }
	}
	if (owns_director) { memdelete(director); }
	if (had_cut) { ps->set_setting(CUT_KEY, prev_cut); } else { ps->clear(CUT_KEY); }
	if (had_route) { ps->set_setting(ROUTE_KEY, prev_route); } else { ps->clear(ROUTE_KEY); }
}

} // namespace TestHlodAutoRoute
