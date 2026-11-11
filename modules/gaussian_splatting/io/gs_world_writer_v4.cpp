#include "gs_world_writer_v4.h"

// Tests-first scaffolding only. No I/O or writer implementation in BV-18a's
// contract change; positive paths must go red until the next implementation step.
namespace gs_world_writer {

WriterV4::~WriterV4() = default;

Error WriterV4::_not_implemented() {
	report.error = ERR_UNAVAILABLE;
	report.reason = "not_implemented";
	return ERR_UNAVAILABLE;
}

Error WriterV4::begin_paged(const String &, const String &, const PagedV4Spec &, const CodecParams &, const Limits &) {
	return _not_implemented();
}

Error WriterV4::append_node(const V4BodyDesc &, const Gaussian *, const Vector3 *, uint32_t, V4BodyHandle &) {
	return _not_implemented();
}

Error WriterV4::get_spill_record(V4BodyHandle, V4SpillRecord &) const {
	return ERR_UNAVAILABLE;
}

Error WriterV4::finalize(const LocalVector<V4Node> &) {
	return _not_implemented();
}

void WriterV4::abort() {}
void WriterV4::request_cancel() {}
Progress WriterV4::get_progress() const { return Progress(); }

#ifdef TESTS_ENABLED
void WriterV4::set_assembly_observer(AssemblyObserver, void *) {}
#endif

} // namespace gs_world_writer
