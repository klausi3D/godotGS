#include "gaussian_splat_hlod_auto_tree.h"

#include "core/object/class_db.h"

void GaussianSplatHlodAutoTree::_bind_methods() {
	ClassDB::bind_static_method("GaussianSplatHlodAutoTree", D_METHOD("build", "input_path", "output_path", "options"), &GaussianSplatHlodAutoTree::build);
}

Dictionary GaussianSplatHlodAutoTree::build(const String &p_input_path, const String &p_output_path, const Dictionary &p_options) {
	return {};
}
