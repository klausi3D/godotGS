#ifdef TOOLS_ENABLED

#include "gaussian_resource_preview_generator.h"

#include "core/io/resource_loader.h"
#include "core/math/math_funcs.h"
#include "core/object/message_queue.h"
#include "core/object/object.h"
#include "core/object/ref_counted.h"
#include "core/os/os.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/texture.h"

#include "../core/gaussian_splat_asset.h"
#include "gaussian_thumbnail_generator.h"

void GaussianSplatAssetPreviewGenerator::_bind_methods() {}

// Backstop timeout for the worker's wait on main-thread texture creation. Under
// normal operation the main message queue is flushed every frame, so the request
// completes within a frame or two; this bound only trips on genuine starvation
// (the main loop no longer flushing) so the preview worker can never block
// forever.
//
// Why 10s, and why editor shutdown does not actually pay this cost (#592
// residual-gap assessment): EditorResourcePreview::stop() -- the only caller
// of abort() -- calls abort() on every generator FIRST, then loops flushing
// MessageQueue::get_singleton() every 10ms until its own worker thread
// reports exited (see editor/inspector/editor_resource_preview.cpp). So a
// real shutdown resolves this wait one of two ways, both fast: (a) the
// poll loop below observes aborted.is_set() within one
// PREVIEW_TEXTURE_WAIT_POLL_USEC tick (~2ms), or (b) even if that racy check
// is missed, stop()'s own flush loop lets the queued create_texture()
// message run within ~10ms anyway. The full 10s is only ever spent when the
// main thread is starved for a reason OTHER than a normal
// EditorResourcePreview::stop() shutdown (e.g. a genuine deadlock/hang
// elsewhere) -- a scenario where the editor is already unresponsive for an
// unrelated reason, so shortening this bound would buy no user-visible
// responsiveness. It would, however, raise the odds of a false-positive
// blank preview during an ordinary transient main-thread stall (heavy
// import, shader compile, etc.) that is longer than the shortened bound but
// well under 10s. 10s is kept as the conservative choice; see
// test_set_wait_timeout_usec() below for how the starvation/abort paths are
// still covered by a fast regression test despite that.
//
// Defined as a class member (not an anonymous-namespace constant) so
// test_set_wait_timeout_usec() can override it per instance for a fast,
// deterministic starvation regression test; production code never calls that
// setter, so every real instance uses this value unless told otherwise.
const uint64_t GaussianSplatAssetPreviewGenerator::DEFAULT_WAIT_TIMEOUT_USEC = 10 * 1000 * 1000; // 10 s.

namespace {
constexpr uint64_t PREVIEW_TEXTURE_WAIT_POLL_USEC = 2000; // 2 ms.
} // namespace

GaussianSplatAssetPreviewGenerator::GaussianSplatAssetPreviewGenerator() :
		wait_timeout_usec(DEFAULT_WAIT_TIMEOUT_USEC) {
}

// Reference-counted shared state for a cross-thread texture-creation request.
// It is a heap RefCounted (not a bare stack Object) so its lifetime can be
// shared safely between the preview worker and the main-thread callable: the
// queued callable owns an independent strong reference (bound as its argument),
// which keeps this object alive even if the worker abandons the wait on
// timeout/abort. See GaussianSplatAssetPreviewGenerator::_create_preview_texture_from_image()
// for the full lifetime argument.
class GaussianPreviewTextureCreateRequest : public RefCounted {
public:
	Ref<Image> image;
	Ref<Texture2D> texture;
	Semaphore done;

	// Runs on the main thread. The bound argument is this same request; it is
	// there only so the queued message holds a strong reference that keeps this
	// object alive for the duration of (and after) the call. Its value is
	// intentionally unused.
	void create_texture(const Variant &p_keepalive) {
		if (image.is_valid() && !image->is_empty()) {
			texture = ImageTexture::create_from_image(image);
		}
		done.post();
	}
};

Ref<Texture2D> GaussianSplatAssetPreviewGenerator::_create_preview_texture_from_image(const Ref<Image> &p_image) const {
	if (p_image.is_null() || p_image->is_empty()) {
		return Ref<Texture2D>();
	}
	if (Thread::is_main_thread()) {
		return ImageTexture::create_from_image(p_image);
	}

	CallQueue *main_queue = MessageQueue::get_main_singleton();
	ERR_FAIL_NULL_V_MSG(main_queue, Ref<Texture2D>(),
			"Gaussian preview texture creation must run on the main thread, but the main message queue is not available.");

	Ref<GaussianPreviewTextureCreateRequest> request;
	request.instantiate();
	request->image = p_image;

	// Bind the request itself as the callable's argument. push_callable() copies
	// the argument into the queued message as a Variant, and a Variant holding a
	// RefCounted keeps a strong reference. So the message owns the request for as
	// long as it lives, independently of this worker's own Ref below — this is
	// what makes abandoning the wait use-after-free-safe.
	const Error err = main_queue->push_callable(
			callable_mp(request.ptr(), &GaussianPreviewTextureCreateRequest::create_texture), request);
	ERR_FAIL_COND_V_MSG(err != OK, Ref<Texture2D>(), "Failed to queue Gaussian preview texture creation on the main thread.");

	// Bounded, non-blocking wait. We poll try_wait() and never block in wait(),
	// so this thread is never registered as a semaphore awaiter (avoiding the
	// "destroyed while a thread waits" UB) and can never hang forever: it gives
	// up on abort (editor shutdown, via abort()) or after the timeout (main-queue
	// starvation) and fails the preview gracefully.
	const uint64_t deadline_usec = OS::get_singleton()->get_ticks_usec() + wait_timeout_usec.load(std::memory_order_acquire);
	bool completed = false;
	while (true) {
		if (request->done.try_wait()) {
			completed = true;
			break;
		}
		if (aborted.is_set()) {
			break;
		}
		if (OS::get_singleton()->get_ticks_usec() >= deadline_usec) {
			WARN_PRINT_ONCE("Gaussian preview texture creation timed out waiting for the main thread; skipping preview.");
			break;
		}
		OS::get_singleton()->delay_usec(PREVIEW_TEXTURE_WAIT_POLL_USEC);
	}

	if (!completed) {
		// The wait was abandoned. The queued callable still owns a strong
		// reference to `request`, so the object stays alive until the main queue
		// runs or clears the message; we return without reading request->texture.
		return Ref<Texture2D>();
	}
	// try_wait() succeeded, so create_texture() has finished (texture assigned,
	// then done.post()); the semaphore's mutex orders that write before this read.
	// Our own `request` Ref is still held, so the object cannot be freed here.
	return request->texture;
}

void GaussianSplatAssetPreviewGenerator::abort() {
	// Called on the main thread by EditorResourcePreview::stop(), which runs on
	// editor shutdown (editor/editor_node.cpp) AND on every reimport
	// (editor/docks/import_dock.cpp:589 -- "Don't try to re-create previews
	// after import"). It is not shutdown-only.
	//
	// INVARIANT (documented rather than defended in code -- see below):
	// `aborted` is one-way. Nothing ever clears it, so the first stop() latches
	// this generator into the abort fast-path permanently.
	//
	// That is currently correct, but only because of a cross-file property that
	// is invisible from this file: EditorResourcePreview::stop() joins and
	// clears its worker thread, and EditorResourcePreview::start() has exactly
	// ONE caller (editor/editor_node.cpp, at editor startup). The preview
	// worker is therefore never restarted after a stop(), so no generate() call
	// that matters can observe a stale `aborted`. If start() ever gains a
	// second caller -- i.e. the preview worker becomes restartable after an
	// import -- this flag MUST be cleared as part of that restart, or every
	// preview generated afterwards will silently take the abort path and return
	// no texture.
	//
	// Why documented instead of reset here: clearing `aborted` at the top of
	// generate()/generate_from_path() would be wrong -- it would defeat an
	// abort() that arrives while a generate() is in flight, which is precisely
	// the shutdown race this flag exists to win. A correct reset needs a
	// "resume" hook invoked from EditorResourcePreview::start(), which is
	// upstream Godot code; adding one to defend a condition no current caller
	// can reach would grow the upstream delta for no present benefit (see
	// AGENTS.md, "Upstream Godot boundary"). The invariant is stated here, at
	// the flag it constrains, so the next person to touch start() finds it.
	aborted.set();
}

bool GaussianSplatAssetPreviewGenerator::handles(const String &p_type) const {
	return ClassDB::is_parent_class(p_type, "GaussianSplatAsset");
}

Ref<Texture2D> GaussianSplatAssetPreviewGenerator::generate(const Ref<Resource> &p_from, const Size2 &p_size, Dictionary &p_metadata) const {
	Ref<GaussianSplatAsset> asset = p_from;
	if (asset.is_null()) {
		return Ref<Texture2D>();
	}

	if (Thread::is_main_thread()) {
		Ref<Texture2D> existing_thumbnail = asset->get_preview_texture();
		if (existing_thumbnail.is_valid()) {
			p_metadata[StringName("gaussian_preview_source")] = String("stored_thumbnail");
			return existing_thumbnail;
		}
	} else {
		Ref<Texture2D> existing_thumbnail = _create_preview_texture_from_image(asset->get_preview_image());
		if (existing_thumbnail.is_valid()) {
			p_metadata[StringName("gaussian_preview_source")] = String("stored_thumbnail");
			return existing_thumbnail;
		}
	}

	if (thumbnail_generator.is_null()) {
		thumbnail_generator.instantiate();
	}
	if (thumbnail_generator.is_null()) {
		return Ref<Texture2D>();
	}

	const int thumbnail_size = MAX(64, int(MAX(p_size.x, p_size.y)));
	p_metadata[StringName("gaussian_preview_source")] = String("generated_thumbnail");
	if (Thread::is_main_thread()) {
		return thumbnail_generator->generate_thumbnail(asset, thumbnail_size, GaussianThumbnailGenerator::THUMBNAIL_STYLE_COLOR);
	}
	return _create_preview_texture_from_image(
			thumbnail_generator->generate_thumbnail_image(asset, thumbnail_size, GaussianThumbnailGenerator::THUMBNAIL_STYLE_COLOR));
}

Ref<Texture2D> GaussianSplatAssetPreviewGenerator::generate_from_path(const String &p_path, const Size2 &p_size, Dictionary &p_metadata) const {
	Ref<GaussianSplatAsset> asset = ResourceLoader::load(p_path, "GaussianSplatAsset");
	if (asset.is_null()) {
		return Ref<Texture2D>();
	}
	p_metadata[StringName("source_path")] = p_path;
	return generate(asset, p_size, p_metadata);
}

bool GaussianSplatAssetPreviewGenerator::generate_small_preview_automatically() const {
	return true;
}

#endif // TOOLS_ENABLED
