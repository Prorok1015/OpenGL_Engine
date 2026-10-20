#pragma once
#include <snk/common/common.h>
#include <snk/resource/res_tag.h>
#include <snk/resource/res_system.h>
#include "edt_model_importer.h"
#include <snk/common/ds/ds_task_system.hpp>
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
		// `ctx` (null: not cancellable) is checked before each file; a stop request throws ds::task_cancelled.
		// The files written before that stay on disk: each write is as atomic as res::resource_system::store(), but a
		// cancelled export can leave a partial project (some leaves written, no root prefab).
		bool export_to_project(
			const import_result& result,
			const std::unordered_map<res::tag, res::tag>& remap,
			export_progress* progress = nullptr,
			ds::task_context* ctx = nullptr) const;

	private:
		res::resource_system& m_res;

		// Remap all string values in a JSON tree that match memory:// tags
		static void remap_json_strings(
			boost::json::value& val,
			const std::unordered_map<res::tag, res::tag>& remap);
	};

} // namespace edt
