#include "gaussian_splat_hlod_runtime_data.h"

#include "gaussian_splat_world.h"

bool gs_hlod_runtime::allocation_bytes(uint64_t p_count, uint64_t p_stride, uint64_t &r_bytes, uint64_t p_size_limit) {
	r_bytes = 0;
	if (p_count == 0) {
		return true;
	}
	if (p_stride == 0 || p_count > UINT64_MAX / p_stride) {
		return false;
	}
	const uint64_t payload = p_count * p_stride;
	uint64_t rounded = 1;
	while (rounded < payload) {
		if (rounded > UINT64_MAX / 2) {
			return false;
		}
		rounded *= 2;
	}
	if (p_size_limit < 16 || rounded > p_size_limit - 16) {
		return false;
	}
	r_bytes = rounded + 16;
	return true;
}

bool GaussianSplatHlodRuntimeData::is_current() const {
	const GaussianSplatWorld *world = Object::cast_to<GaussianSplatWorld>(source_world.ptr());
	return world && world->is_hlod_runtime_snapshot_current(*this);
}

const Gaussian &GaussianSplatHlodRuntimeData::gaussian(uint64_t p_index) const {
	static const Gaussian paged_placeholder;
	ERR_FAIL_COND_V_MSG(is_paged(), paged_placeholder, "A paged HLOD snapshot holds no resident payload; read the node through its paged source.");
	return p_index < leaf_count ? leaves.get_gaussians()[p_index] : parents[p_index - leaf_count];
}

const Vector3 *GaussianSplatHlodRuntimeData::sh(uint64_t p_index) const {
	ERR_FAIL_COND_V_MSG(is_paged(), nullptr, "A paged HLOD snapshot holds no resident payload; read the node through its paged source.");
	if (high_order_count == 0) {
		return nullptr;
	}
	return p_index < leaf_count ? leaves.get_sh_high_order().ptr() + p_index * high_order_count :
			parent_sh.ptr() + (p_index - leaf_count) * high_order_count;
}
