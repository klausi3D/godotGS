#pragma once

#include "../renderer/hlod_node_streamer.h"
#include "core/os/thread.h"

namespace TestGaussianSplatting {

// Deadline pumps can poll workers millions of times per second. A poll is not
// a new logical frame: aging a staged edit on every poll can exhaust its frame
// timeout before a worker gets scheduled. Keep calling update() in the current
// frame while awaiting asynchronous work; yield outside the streamer's locks.
// Upload/metadata progress needs a fresh allowance, retirement and retry delays
// need frame advancement, and a ready edit needs its begin_frame commit boundary.
// No-edit streams retain their original frame cadence. Only a test deliberately
// exercising the frame-based timeout should force advancement while workers wait.
inline HlodNodeStreamer::EditCommitResult gs_hlod_edit_begin_frame(HlodNodeStreamer &p_streamer, RenderingDevice *p_device = nullptr,
		const HlodNodeStreamer::Buffers *p_buffers = nullptr, bool p_force_advance = false) {
	const auto &telemetry = p_streamer.get_telemetry();
	if (p_force_advance || !p_streamer.is_edit_staged() || p_streamer.is_edit_ready() ||
			telemetry.uploaded_bytes_this_frame != 0 || telemetry.uploaded_this_frame != 0 ||
			telemetry.evicted_this_frame != 0 || telemetry.pages_retiring != 0 || telemetry.failed > telemetry.permanently_failed) {
		p_streamer.begin_frame(p_device, p_buffers);
		return p_streamer.get_last_commit_result();
	}
#ifdef THREADS_ENABLED
	Thread::yield();
#endif
	return HlodNodeStreamer::COMMIT_NOTHING; // No new boundary, so never repeat a previous commit result.
}

} // namespace TestGaussianSplatting
