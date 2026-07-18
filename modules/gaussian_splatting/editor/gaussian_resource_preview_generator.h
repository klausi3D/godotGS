#ifndef GAUSSIAN_RESOURCE_PREVIEW_GENERATOR_H
#define GAUSSIAN_RESOURCE_PREVIEW_GENERATOR_H

#ifdef TOOLS_ENABLED

#include "editor/inspector/editor_resource_preview.h"

#include "core/templates/safe_refcount.h"

#include <atomic>

class GaussianThumbnailGenerator;
class Image;

class GaussianSplatAssetPreviewGenerator : public EditorResourcePreviewGenerator {
	GDCLASS(GaussianSplatAssetPreviewGenerator, EditorResourcePreviewGenerator);

	// Bounded-wait backstop for the worker's main-thread texture-creation
	// request (see _create_preview_texture_from_image()). Defaults to
	// DEFAULT_WAIT_TIMEOUT_USEC; test_set_wait_timeout_usec() lets a
	// starvation regression test shrink it so the bail-out can be proven
	// without waiting out the real production timeout. Mirrors
	// RenderThreadDispatcher::timeout_usec / GaussianSplatRenderer's
	// test_set_render_thread_dispatch_timeout_usec seam.
	mutable std::atomic<uint64_t> wait_timeout_usec;

	mutable Ref<GaussianThumbnailGenerator> thumbnail_generator;
	// Set from abort() (called on every generator by EditorResourcePreview::stop(),
	// on editor shutdown and on every reimport) so the worker-thread wait below
	// can bail promptly instead of blocking on a main queue that will no longer
	// flush. One-way: never cleared. That is safe only because the preview
	// worker is never restarted after a stop() -- see the invariant note on
	// abort() in the .cpp before adding a caller to EditorResourcePreview::start().
	SafeFlag aborted;

	// Turns an Image produced on a worker thread into a Texture2D. Texture
	// creation must happen on the main thread, so off the main thread this
	// bounces a request through the main message queue and waits for the result
	// with a bounded, abortable wait (never an unbounded block).
	Ref<Texture2D> _create_preview_texture_from_image(const Ref<Image> &p_image) const;

protected:
	static void _bind_methods();

public:
	// Production default for wait_timeout_usec; defined in the .cpp.
	static const uint64_t DEFAULT_WAIT_TIMEOUT_USEC;

	GaussianSplatAssetPreviewGenerator();

	virtual bool handles(const String &p_type) const override;
	virtual Ref<Texture2D> generate(const Ref<Resource> &p_from, const Size2 &p_size, Dictionary &p_metadata) const override;
	virtual Ref<Texture2D> generate_from_path(const String &p_path, const Size2 &p_size, Dictionary &p_metadata) const override;
	virtual bool generate_small_preview_automatically() const override;
	virtual void abort() override;

	// Test-only seam (used by modules/gaussian_splatting/tests/test_gaussian_importer.h):
	// override/query the bounded wait timeout. Never called from production
	// code paths (registration in gaussian_editor_plugin.cpp never touches it).
	void test_set_wait_timeout_usec(uint64_t p_timeout_usec) { wait_timeout_usec.store(p_timeout_usec, std::memory_order_release); }
	uint64_t test_get_wait_timeout_usec() const { return wait_timeout_usec.load(std::memory_order_acquire); }
};

#endif // TOOLS_ENABLED

#endif // GAUSSIAN_RESOURCE_PREVIEW_GENERATOR_H
