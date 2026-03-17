#pragma once
#include "common.h"
#include "res_tag.h"
#include "res_system.h"
#include "edt_model_importer.h"
#include <unordered_map>
#include <filesystem>

namespace edt {

	class asset_exporter {
	public:
		asset_exporter(res::resource_system& res);

		// Build tag remapping: memory://... → res://target_folder/...
		std::unordered_map<res::tag, res::tag> build_remap(
			const import_result& result,
			const std::string& asset_name,
			const std::string& target_folder) const;

		// Write all resources to disk, remapping memory:// → res:// in JSON files
		bool export_to_project(
			const import_result& result,
			const std::unordered_map<res::tag, res::tag>& remap) const;

	private:
		res::resource_system& m_res;

		// Remap all string values in a JSON tree that match memory:// tags
		static void remap_json_strings(
			boost::json::value& val,
			const std::unordered_map<res::tag, res::tag>& remap);
	};

} // namespace edt
