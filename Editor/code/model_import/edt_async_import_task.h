#pragma once
#include "edt_decoder_sandbox.h"
#include <snk/model_codec/res_tag.h>
#include <boost/json.hpp>
#include <array>
#include <filesystem>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace edt {

	// Intermediate result produced on background thread.
	// All res_sys.store() calls done; main thread does desc_sys + skinning finalization.
	struct import_intermediate {
		res::tag                          geom_tag;
		res::tag                          prefab_tag;
		std::vector<res::tag>             all_tags;
		// The tags this run stored itself; the rest of all_tags was there before.
		std::vector<res::tag>             created_tags;
		// The SHA-256 of each created resource as it was stored, when the importer knows it (the decoder process's, #999).
		std::vector<std::pair<res::tag, std::array<std::uint8_t, 32>>> created_digests;
		std::filesystem::path             source_path;

		// Owned copy of prefab JSON for main-thread desc_sys.create_instance + deserialize.
		boost::json::object               prefab_json;

		// Skinning weights to register on main thread.
		std::vector<std::vector<uint32_t>> skin_weights;

		// External textures as the model names them that were not loaded: their materials have no sampler for them.
		std::vector<std::string>           missing_textures;
		// How many more were not loaded than missing_textures lists (mdl::k_max_missing_textures).
		std::size_t                        missing_textures_not_listed = 0;

		// Non-empty means failure.
		std::string                        error;

		// What the decoder process said of its OS sandbox for this import (#1076); not reported by an importer in this process.
		decoder_sandbox_status             sandbox{};
	};

} // namespace edt
