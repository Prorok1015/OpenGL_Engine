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
#include <string>
#include <string_view>
#include <vector>

namespace edt {

	struct export_progress {
		std::atomic<int> done{ 0 };
		std::atomic<int> total{ 0 };
		std::atomic<bool> finished{ false };
		std::atomic<bool> success{ false };
		res::tag exported_tag;
		// Why the export stopped (which file, what was refused); written before `finished` is set.
		std::string error;
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
		// `written` (null: not told) gets each res:// tag as its file is stored. `wait_for_writes` stores each file with
		// store_sync (the file is on disk when the call returns; false when a write is refused), the way a caller that may
		// have to take the files back needs it. `on_file` (empty: none) is called after each file with (done, total).
		bool export_to_project(
			const import_result& result,
			const std::unordered_map<res::tag, res::tag>& remap,
			export_progress* progress = nullptr,
			ds::task_context* ctx = nullptr,
			std::vector<res::tag>* written = nullptr,
			bool wait_for_writes = false,
			const std::function<void(int, int)>& on_file = {}) const;

		// Empty when everything the remap would write is something an import of `asset_name` makes, below `target_folder`, with the
		// extension it had (an agent's import: nothing it chose may turn a resource into a model file). Else why not.
		static std::string remap_problem(const std::unordered_map<res::tag, res::tag>& remap, std::string_view asset_name, std::string_view target_folder);

		// Empty when `target_folder` is a folder path relative to res:// ("assets/tree"): no drive, root, '.' or '..' component,
		// empty component, backslash, ':' or control character. Else why not, for the person or the agent who chose it.
		static std::string folder_problem(std::string_view target_folder);

		// Empty when `asset_name` is one file name (no '/', '\\', ':', control character, not "." or ".."). Else why not.
		static std::string asset_name_problem(std::string_view asset_name);

		// What a person's export is held to: the folder and the name are plain, and everything the remap writes lands below
		// `target_folder`. Empty when so, else why not. remap_problem adds the agent's rules on names and extensions.
		static std::string destination_problem(const std::unordered_map<res::tag, res::tag>& remap, std::string_view asset_name, std::string_view target_folder);

	private:
		res::resource_system& m_res;

		// The file holds these bytes already (an import of an unchanged model): it is not written again, so file watchers and
		// git see nothing (#559).
		bool unchanged_on_disk(const res::tag& tag, const std::vector<std::byte>& bytes) const;

		// Remap all string values in a JSON tree that match memory:// tags
		static void remap_json_strings(
			boost::json::value& val,
			const std::unordered_map<res::tag, res::tag>& remap);
	};

} // namespace edt
