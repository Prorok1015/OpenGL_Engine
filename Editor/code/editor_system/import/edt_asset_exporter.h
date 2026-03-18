#pragma once
#include "common.h"
#include "res_tag.h"
#include "res_system.h"
#include "edt_model_importer.h"
#include <unordered_map>
#include <filesystem>
#include <atomic>
#include <functional>

namespace edt {

	struct export_progress {
		std::atomic<int> done{ 0 };
		std::atomic<int> total{ 0 };
		std::atomic<bool> finished{ false };
		std::atomic<bool> success{ false };
		res::tag exported_tag;
	};

	class asset_exporter {
	public:
		asset_exporter(res::resource_system& res);

		// Build tag remapping: memory://... → res://target_folder/...
		std::unordered_map<res::tag, res::tag> build_remap(
			const import_result& result,
			const std::string& asset_name,
			const std::string& target_folder) const;

		// Write all resources to disk, remapping memory:// → res:// in JSON files.
		// Thread-safe — can run on background thread.
		bool export_to_project(
			const import_result& result,
			const std::unordered_map<res::tag, res::tag>& remap,
			export_progress* progress = nullptr) const;

	private:
		res::resource_system& m_res;

		// Remap all string values in a JSON tree that match memory:// tags
		static void remap_json_strings(
			boost::json::value& val,
			const std::unordered_map<res::tag, res::tag>& remap);
	};

} // namespace edt
