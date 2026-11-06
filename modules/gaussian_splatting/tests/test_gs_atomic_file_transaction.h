#pragma once

// GSAtomicFileTransaction (BV-02, docs/architecture/adr-beyond-vram-worlds.md
// §4.3, W1/W9) and the sliced plain-v3 SH write. CPU only; no GPU.

#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_hlod_job.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/gs_atomic_file_transaction.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#include <cstring>

namespace TestGSAtomicFileTransaction {

inline String txn_path(const String &p_suffix) {
	static uint32_t s_seq = 0;
	const uint64_t ticks = OS::get_singleton()->get_ticks_usec();
	return OS::get_singleton()->get_temp_path().path_join(
			"gs_txn_" + p_suffix + "_" + itos(ticks) + "_" + itos(s_seq++) + ".bin");
}

inline PackedByteArray txn_pattern(uint64_t p_size, uint8_t p_seed) {
	PackedByteArray out;
	out.resize(int64_t(p_size));
	uint8_t *w = out.ptrw();
	for (uint64_t i = 0; i < p_size; i++) {
		w[i] = uint8_t((i * 131u + p_seed * 7u + (i >> 8u)) & 0xFFu);
	}
	return out;
}

inline bool txn_write_raw(const String &p_path, const PackedByteArray &p_bytes) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) {
		return false;
	}
	f->store_buffer(p_bytes.ptr(), p_bytes.size());
	return f->get_error() == OK;
}

inline PackedByteArray txn_read_raw(const String &p_path) {
	PackedByteArray out;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		return out;
	}
	out.resize(int64_t(f->get_length()));
	if (!out.is_empty()) {
		f->get_buffer(out.ptrw(), out.size());
	}
	return out;
}

// Files in p_path's directory named "<file>.tmp.*": the transaction's temps.
inline int txn_count_temps(const String &p_path) {
	Ref<DirAccess> da = DirAccess::open(p_path.get_base_dir());
	if (da.is_null()) {
		return -1;
	}
	const String prefix = p_path.get_file() + ".tmp.";
	int count = 0;
	da->list_dir_begin();
	for (String name = da->get_next(); !name.is_empty(); name = da->get_next()) {
		if (name.begins_with(prefix)) {
			count++;
		}
	}
	da->list_dir_end();
	return count;
}

inline Error txn_append(GSAtomicFileTransaction &p_txn, const PackedByteArray &p_bytes) {
	return p_txn.append(p_bytes.ptr(), uint64_t(p_bytes.size()));
}

} // namespace TestGSAtomicFileTransaction

TEST_CASE("[GaussianSplatting][AtomicWrite][Transaction][BV02] commit publishes appended bytes and the header rewrite") {
	using namespace TestGSAtomicFileTransaction;
	const String path = txn_path("commit");
	const PackedByteArray a = txn_pattern(4096, 1);
	const PackedByteArray b = txn_pattern(70000, 2);
	const PackedByteArray header = txn_pattern(64, 3);

	GSAtomicFileTransaction txn;
	REQUIRE(txn.begin(path) == OK);
	CHECK(txn.is_open());
	CHECK(FileAccess::exists(txn.get_temp_path()));
	CHECK(txn.get_temp_path() != path);
	CHECK(txn_append(txn, a) == OK);
	CHECK(txn_append(txn, b) == OK);
	CHECK(txn.get_size() == uint64_t(a.size() + b.size()));
	CHECK(txn.write_at(0, header.ptr(), uint64_t(header.size())) == OK);
	// W1: nothing at the final path before commit.
	CHECK_FALSE(FileAccess::exists(path));

	CHECK(txn.commit() == OK);
	CHECK(txn.get_state() == GSAtomicFileTransaction::State::COMMITTED);
	CHECK(txn.get_error() == OK);
	CHECK(txn_count_temps(path) == 0);

	PackedByteArray expected = a;
	expected.append_array(b);
	memcpy(expected.ptrw(), header.ptr(), size_t(header.size()));
	CHECK(txn_read_raw(path) == expected);

	// Terminal: further writes and a second commit are refused without effect.
	CHECK(txn_append(txn, a) == ERR_UNCONFIGURED);
	CHECK(txn.commit() == ERR_UNCONFIGURED);
	CHECK(txn_read_raw(path) == expected);
	DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][AtomicWrite][Transaction][BV02] commit replaces a larger existing file with exactly the new bytes") {
	using namespace TestGSAtomicFileTransaction;
	const String path = txn_path("replace");
	REQUIRE(txn_write_raw(path, txn_pattern(200000, 9)));
	const PackedByteArray fresh = txn_pattern(1000, 4);

	GSAtomicFileTransaction txn;
	REQUIRE(txn.begin(path) == OK);
	CHECK(txn_append(txn, fresh) == OK);
	// The old file is untouched while the transaction is open.
	CHECK(txn_read_raw(path) == txn_pattern(200000, 9));
	CHECK(txn.commit() == OK);
	CHECK(txn_read_raw(path) == fresh);
	CHECK(txn_count_temps(path) == 0);

	// The same object can run a second transaction once the first is terminal.
	const PackedByteArray again = txn_pattern(10, 5);
	REQUIRE(txn.begin(path) == OK);
	CHECK(txn_append(txn, again) == OK);
	CHECK(txn.commit() == OK);
	CHECK(txn_read_raw(path) == again);
	DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][AtomicWrite][Transaction][BV02] abort removes the temp and leaves the target byte-identical") {
	using namespace TestGSAtomicFileTransaction;
	const String path = txn_path("abort");
	const PackedByteArray old_bytes = txn_pattern(5000, 6);
	REQUIRE(txn_write_raw(path, old_bytes));

	GSAtomicFileTransaction txn;
	REQUIRE(txn.begin(path) == OK);
	const String temp = txn.get_temp_path();
	CHECK(txn_append(txn, txn_pattern(9000, 7)) == OK);
	txn.abort();
	CHECK(txn.get_state() == GSAtomicFileTransaction::State::ABORTED);
	CHECK_FALSE(FileAccess::exists(temp));
	CHECK(txn_count_temps(path) == 0);
	CHECK(txn_read_raw(path) == old_bytes);

	txn.abort(); // idempotent
	CHECK(txn_append(txn, old_bytes) == ERR_UNCONFIGURED);
	CHECK(txn.commit() == ERR_UNCONFIGURED);
	CHECK(txn_read_raw(path) == old_bytes);
	DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][AtomicWrite][Transaction][BV02] destructor aborts: no temp, existing target intact, new target never created") {
	using namespace TestGSAtomicFileTransaction;
	const String existing = txn_path("dtor_existing");
	const String absent = txn_path("dtor_absent");
	const PackedByteArray old_bytes = txn_pattern(3000, 8);
	REQUIRE(txn_write_raw(existing, old_bytes));

	String temp_existing;
	String temp_absent;
	{
		GSAtomicFileTransaction t1;
		GSAtomicFileTransaction t2;
		REQUIRE(t1.begin(existing) == OK);
		REQUIRE(t2.begin(absent) == OK);
		temp_existing = t1.get_temp_path();
		temp_absent = t2.get_temp_path();
		CHECK(txn_append(t1, txn_pattern(4000, 1)) == OK);
		CHECK(txn_append(t2, txn_pattern(4000, 2)) == OK);
		CHECK(FileAccess::exists(temp_existing));
		CHECK(FileAccess::exists(temp_absent));
	}
	CHECK_FALSE(FileAccess::exists(temp_existing));
	CHECK_FALSE(FileAccess::exists(temp_absent));
	CHECK(txn_read_raw(existing) == old_bytes);
	CHECK_FALSE(FileAccess::exists(absent));
	CHECK(txn_count_temps(existing) == 0);
	CHECK(txn_count_temps(absent) == 0);
	DirAccess::remove_absolute(existing);
}

TEST_CASE("[GaussianSplatting][AtomicWrite][Transaction][BV02] a crash-like abort mid-write leaves the old target byte-identical") {
	using namespace TestGSAtomicFileTransaction;
	// A multi-slice write (larger than the 1 MiB cancellation granularity the
	// writer will use) plus a header rewrite, then an abandonment before commit:
	// the prior good copy must survive untouched at every observation point.
	const String path = txn_path("crash");
	const PackedByteArray old_bytes = txn_pattern(1500000, 10);
	REQUIRE(txn_write_raw(path, old_bytes));

	GSAtomicFileTransaction *txn = memnew(GSAtomicFileTransaction);
	REQUIRE(txn->begin(path) == OK);
	const PackedByteArray block = txn_pattern(1u << 20u, 11);
	for (int i = 0; i < 3; i++) {
		CHECK(txn_append(*txn, block) == OK);
		CHECK(txn_read_raw(path) == old_bytes);
	}
	const PackedByteArray header = txn_pattern(128, 12);
	CHECK(txn->write_at(0, header.ptr(), uint64_t(header.size())) == OK);
	CHECK(txn_read_raw(path) == old_bytes);
	const String temp = txn->get_temp_path();
	memdelete(txn); // abandoned without commit or abort
	CHECK_FALSE(FileAccess::exists(temp));
	CHECK(txn_read_raw(path) == old_bytes);
	CHECK(txn_count_temps(path) == 0);
	DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][AtomicWrite][Transaction][BV02] begin in a missing directory fails, reports and creates nothing") {
	using namespace TestGSAtomicFileTransaction;
	const String dir = txn_path("missing_dir");
	REQUIRE_FALSE(DirAccess::exists(dir));
	const String path = dir.path_join("target.bin");

	GSAtomicFileTransaction txn;
	const Error err = txn.begin(path);
	CHECK(err == ERR_FILE_BAD_PATH);
	CHECK(txn.get_state() == GSAtomicFileTransaction::State::FAILED);
	CHECK(txn.get_error() == err);
	CHECK(txn.get_error_message().contains(path));
	CHECK_FALSE(txn.is_open());
	// Poisoned: the original error is returned, nothing is written.
	CHECK(txn_append(txn, txn_pattern(16, 1)) == err);
	CHECK(txn.commit() == err);
	CHECK_FALSE(DirAccess::exists(dir));
	CHECK_FALSE(FileAccess::exists(path));
}

TEST_CASE("[GaussianSplatting][AtomicWrite][Transaction][BV02] a replace that the OS refuses fails, removes the temp and leaves the target") {
	using namespace TestGSAtomicFileTransaction;
	// The final path is an existing directory: neither MoveFileExW nor rename()
	// may replace it with a file.
	const String dir_target = txn_path("dir_target");
	REQUIRE(DirAccess::make_dir_absolute(dir_target) == OK);
	const String marker = dir_target.path_join("keep.bin");
	REQUIRE(txn_write_raw(marker, txn_pattern(32, 2)));

	GSAtomicFileTransaction txn;
	REQUIRE(txn.begin(dir_target) == OK);
	const String temp = txn.get_temp_path();
	CHECK(txn_append(txn, txn_pattern(512, 3)) == OK);
	const Error err = txn.commit();
	CHECK(err != OK);
	CHECK(txn.get_state() == GSAtomicFileTransaction::State::FAILED);
	CHECK(txn.get_error() == err);
	CHECK_FALSE(txn.get_error_message().is_empty());
	CHECK_FALSE(FileAccess::exists(temp));
	CHECK(DirAccess::exists(dir_target));
	CHECK(txn_read_raw(marker) == txn_pattern(32, 2));
	CHECK(txn.commit() == err);

	DirAccess::remove_absolute(marker);
	DirAccess::remove_absolute(dir_target);
}

TEST_CASE("[GaussianSplatting][AtomicWrite][Transaction][BV02] argument errors are refused without poisoning the transaction") {
	using namespace TestGSAtomicFileTransaction;
	GSAtomicFileTransaction empty;
	CHECK(empty.begin(String()) == ERR_INVALID_PARAMETER);
	CHECK(empty.get_state() == GSAtomicFileTransaction::State::FAILED);

	const String path = txn_path("args");
	GSAtomicFileTransaction txn;
	CHECK(txn_append(txn, txn_pattern(4, 1)) == ERR_UNCONFIGURED); // never begun
	REQUIRE(txn.begin(path) == OK);
	CHECK(txn.begin(path) == ERR_ALREADY_IN_USE);
	const PackedByteArray data = txn_pattern(100, 2);
	CHECK(txn_append(txn, data) == OK);
	// No holes: a rewrite may not extend past the written end.
	CHECK(txn.write_at(1, data.ptr(), 100) == ERR_INVALID_PARAMETER);
	CHECK(txn.write_at(101, data.ptr(), 0) == ERR_INVALID_PARAMETER);
	CHECK(txn.append(nullptr, 4) == ERR_INVALID_PARAMETER);
	CHECK(txn.write_at(100, data.ptr(), 0) == OK);
	CHECK(txn.is_open());
	CHECK(txn.get_size() == 100);
	CHECK(txn.commit() == OK);
	CHECK(txn_read_raw(path) == data);
	DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][WorldIO][BV02] plain v3 SH section is written in slices and lands byte-exact") {
	using namespace TestGSAtomicFileTransaction;
	// The plain saver's SH section used to be one unsliced store_buffer. Under an
	// offline job _write_buffer_sliced uses 1 MiB slices and calls the write test
	// hook once per slice, so the SH section's slice count is observable as the
	// difference between an SH and an SH-free save of the same gaussians.
	constexpr uint32_t kCount = 8000;
	constexpr uint32_t kHigh = 12; // SH degree 3: 5 + 7 high-order coefficients
	LocalVector<Gaussian> gaussians;
	gaussians.resize(kCount);
	LocalVector<Vector3> sh;
	sh.resize(kCount * kHigh);
	for (uint32_t i = 0; i < kCount; i++) {
		Gaussian g;
		g.position = Vector3(float(i % 97), float((i / 97) % 89), float(i / 8633));
		g.scale = Vector3(0.1f, 0.1f, 0.1f);
		g.rotation = Quaternion();
		g.opacity = 0.5f;
		gaussians[i] = g;
		for (uint32_t k = 0; k < kHigh; k++) {
			sh[i * kHigh + k] = Vector3(float(i) * 0.001f, float(k) * 0.01f, -float(i + k) * 0.0001f);
		}
	}
	auto make_world = [&](bool p_with_sh) {
		Ref<GaussianData> data;
		data.instantiate();
		LocalVector<Vector3> none;
		data->set_gaussian_payload(gaussians, p_with_sh ? sh : none, 3u, p_with_sh ? kHigh : 0u, false, false);
		Ref<GaussianSplatWorld> world;
		world.instantiate();
		world->set_gaussian_data(data);
		world->set_bounds(data->get_aabb());
		return world;
	};

	struct Counter {
		static void hook(void *p_data) { (*static_cast<uint32_t *>(p_data))++; }
	};
	auto save_counting = [&](const Ref<GaussianSplatWorld> &p_world, const String &p_path, uint32_t &r_calls) {
		gs_hlod::OfflineJob job;
		job.output = uint64_t(1) << 40u;
		job.deadline = UINT64_MAX;
		job.cancel_path = p_path + ".cancel_never";
		gs_hlod::OfflineJobScope scope(job);
		if (!scope.acquired) {
			return ERR_BUSY;
		}
		r_calls = 0;
		ResourceFormatSaverGaussianSplatWorld::offline_write_test_userdata = &r_calls;
		ResourceFormatSaverGaussianSplatWorld::offline_write_test_hook = &Counter::hook;
		const Error err = p_world->save_to_file(p_path);
		ResourceFormatSaverGaussianSplatWorld::offline_write_test_hook = nullptr;
		ResourceFormatSaverGaussianSplatWorld::offline_write_test_userdata = nullptr;
		return err;
	};

	const String with_path = txn_path("sh_sliced") + ".gsplatworld";
	const String without_path = txn_path("sh_none") + ".gsplatworld";
	uint32_t with_calls = 0;
	uint32_t without_calls = 0;
	REQUIRE(save_counting(make_world(true), with_path, with_calls) == OK);
	REQUIRE(save_counting(make_world(false), without_path, without_calls) == OK);

	const uint64_t sh_bytes = uint64_t(kCount) * kHigh * sizeof(Vector3);
	const uint64_t expected_sh_slices = (sh_bytes + (uint64_t(1) << 20u) - 1u) >> 20u;
	REQUIRE(expected_sh_slices >= 2u); // the fixture must span more than one slice
	CHECK(uint64_t(with_calls) - uint64_t(without_calls) == expected_sh_slices);

	// The SH section is byte-exact at the offset the header declares.
	const PackedByteArray bytes = txn_read_raw(with_path);
	REQUIRE(bytes.size() > 72);
	uint64_t sh_offset = 0;
	memcpy(&sh_offset, bytes.ptr() + 64, sizeof(sh_offset)); // header: 7 x u32, 2 x vec3, u32, gaussian_offset u64, sh_offset u64
	REQUIRE(sh_offset + sh_bytes <= uint64_t(bytes.size()));
	CHECK(memcmp(bytes.ptr() + sh_offset, sh.ptr(), size_t(sh_bytes)) == 0);

	DirAccess::remove_absolute(with_path);
	DirAccess::remove_absolute(without_path);
}
