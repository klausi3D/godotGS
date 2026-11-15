extern "C" int test_splat_count_sample_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../interfaces/splat_count_sample.h"

TEST_CASE("[GaussianSplatting][WorldIO] splat count provenance starts unknown and follows the accepted request") {
	gs_count_provenance::Sample sample;
	CHECK(sample.source_frame() == -1);
	CHECK(sample.accept(123, 71));
	CHECK(sample.count == 123);
	CHECK(sample.source_frame() == 71);
	CHECK_FALSE(sample.accept(999, 70));
	CHECK(sample.count == 123);
	CHECK(sample.source_frame() == 71);
	CHECK(sample.accept(0, 72)); // A measured zero still has provenance.
	CHECK(sample.count == 0);
	CHECK(sample.source_frame() == 72);
	sample.source_valid = false; // An overwrite without a measurement invalidates only provenance.
	CHECK(sample.source_frame() == -1);
	CHECK(sample.valid);
	CHECK(sample.frame == 72);
	CHECK(sample.accept(456, 73));
	CHECK(sample.source_frame() == 73);
	sample = {};
	CHECK(sample.source_frame() == -1);
}
