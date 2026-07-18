#ifndef GAUSSIAN_RESOURCE_PREVIEW_GENERATOR_H
#define GAUSSIAN_RESOURCE_PREVIEW_GENERATOR_H

#ifdef TOOLS_ENABLED

#include "editor/inspector/editor_resource_preview.h"

#include "core/templates/safe_refcount.h"

class GaussianThumbnailGenerator;
class Image;

class GaussianSplatAssetPreviewGenerator : public EditorResourcePreviewGenerator {
	GDCLASS(GaussianSplatAssetPreviewGenerator, EditorResourcePreviewGenerator);

	mutable Ref<GaussianThumbnailGenerator> thumbnail_generator;
	// Set from abort() (called on every generator by EditorResourcePreview::stop()
	// during editor shutdown) so the worker-thread wait below can bail promptly
	// instead of blocking on a main queue that will no longer flush.
	SafeFlag aborted;

	// Turns an Image produced on a worker thread into a Texture2D. Texture
	// creation must happen on the main thread, so off the main thread this
	// bounces a request through the main message queue and waits for the result
	// with a bounded, abortable wait (never an unbounded block).
	Ref<Texture2D> _create_preview_texture_from_image(const Ref<Image> &p_image) const;

protected:
	static void _bind_methods();

public:
	virtual bool handles(const String &p_type) const override;
	virtual Ref<Texture2D> generate(const Ref<Resource> &p_from, const Size2 &p_size, Dictionary &p_metadata) const override;
	virtual Ref<Texture2D> generate_from_path(const String &p_path, const Size2 &p_size, Dictionary &p_metadata) const override;
	virtual bool generate_small_preview_automatically() const override;
	virtual void abort() override;
};

#endif // TOOLS_ENABLED

#endif // GAUSSIAN_RESOURCE_PREVIEW_GENERATOR_H
