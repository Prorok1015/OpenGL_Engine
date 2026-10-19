#pragma once
#include <snk/resource/res_tag.h>
#include <boost/json.hpp>
#include <filesystem>
#include <cstdint>
#include <optional>
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

	// Held by editor_system while an import job runs; filled by the job's on_done on the main thread.
	struct async_import_token {
		std::uint64_t                     job = 0;
		std::string                       display_name;
		float                             spinner_t = 0.f;
		bool                              finished = false;
		std::optional<import_intermediate> intermediate;
		// Set when the job itself failed (exception); a failed import reports through intermediate->error.
		std::string                       failure;
	};

} // namespace edt
