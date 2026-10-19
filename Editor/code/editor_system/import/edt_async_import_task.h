#pragma once
#include <snk/resource/res_tag.h>
#include <boost/json.hpp>
#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace edt {

	// Intermediate result produced on background thread.
	// All res_sys.store() calls done; main thread does desc_sys + skinning finalization.
	struct import_intermediate {
		res::tag                          geom_tag;
		res::tag                          prefab_tag;
		std::vector<res::tag>             all_tags;
		std::filesystem::path             source_path;

		// Owned copy of prefab JSON for main-thread desc_sys.create_instance + deserialize.
		boost::json::object               prefab_json;

		// Skinning weights to register on main thread.
		std::vector<std::vector<uint32_t>> skin_weights;

		// Non-empty means failure.
		std::string                        error;
	};

} // namespace edt
