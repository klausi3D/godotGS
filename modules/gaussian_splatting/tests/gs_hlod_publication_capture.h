#pragma once

#include "test_macros.h"
#include "../renderer/hlod_node_streamer.h"

// F10: HlodNodeStreamer publication capture is opt-in and bounded. Tests read it only through these helpers, so a
// capture that was never enabled (or was reset by a later init()) or that truncated fails the test. Neither can pass
// as a clean, empty history: many cases assert "no publication" as their negative evidence.
namespace TestHlodPublicationCapture {

// Call after every init()/init_with_page_store(): init() installs a fresh ledger with capture off.
inline bool observe_publications(HlodNodeStreamer &p_streamer,
		uint32_t p_capacity = HlodNodeStreamer::TEST_PUBLICATION_DEFAULT_CAPACITY) {
	if (p_streamer._test_observe_publications(p_capacity) != OK) {
		FAIL_CHECK("F10: publication capture could not be enabled (call it after init)");
		return false;
	}
	return true;
}

inline Vector<HlodNodeStreamer::TestPublication> publications(const HlodNodeStreamer &p_streamer) {
	Vector<HlodNodeStreamer::TestPublication> result;
	uint64_t dropped = 0;
	if (p_streamer._test_publications(result, &dropped) != OK) {
		FAIL_CHECK("F10: publication capture is off; an empty history here would be vacuous");
		return result;
	}
	if (dropped != 0) {
		FAIL_CHECK("F10: publication capture truncated ", dropped, " entries; the history is incomplete");
	}
	return result;
}

} // namespace TestHlodPublicationCapture
