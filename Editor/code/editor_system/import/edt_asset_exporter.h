#pragma once
#include <snk/common/common.h>
#include <snk/model_codec/res_tag.h>
#include <snk/resource/res_system.h>
#include "edt_model_importer.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <unordered_map>
#include <filesystem>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt {

	// What a re-import (#1194) finds at a file it would overwrite, against the `made` of the prefab's record.
	enum class edit_state : std::uint8_t { unchanged, changed, unknown };

	// One file the export writes (a geometry desc stands for its `.bin` too), as the re-import's check saw it.
	struct reimport_file_check {
		res::tag tag{};
		bool exists = false;
		// The disk holds exactly what the export would write: it is not listed.
		bool same_as_new = false;
		edit_state state = edit_state::unknown;
	};

	struct reimport_scan {
		std::vector<reimport_file_check> files{};
		// Every file the export may write (a geometry desc's `.bin` too) -> the SHA-256 seen, empty when there was
		// none.
		std::unordered_map<res::tag, std::string> seen{};
		// A file that could not be read: nothing may be overwritten.
		std::string error{};
	};

	// What the earlier import made that this one does not (#1194), and why the list may be incomplete.
	struct no_longer_made_list {
		std::vector<res::tag> tags{};
		std::string note{};
	};

	struct export_progress {
		// Set right before the export starts writing its first file: from then on a re-import runs to its end.
		std::atomic<bool> writing{ false };
		std::atomic<int> done{ 0 };
		std::atomic<int> total{ 0 };
		std::atomic<bool> finished{ false };
		std::atomic<bool> success{ false };
		res::tag exported_tag;
		// Why the export stopped (which file, what was refused); written before `finished` is set.
		std::string error;
		// Resources the import lists but never stored (an external texture it did not find): not written, the export goes on.
		// Written before `finished` is set.
		std::vector<res::tag> skipped;
		// A material written over one of an earlier export whose slot named another embedded texture (#1206 renamed them):
		// "<material> <slot>: <old> -> <new>", each also a warning in the log. The old file stays in the project. Written
		// before `finished` is set.
		std::vector<std::string> replaced_textures;
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
		// A geometry desc is written with its binary file (<name>.geom.bin, EPIC-79) whatever form memory:// holds it in,
		// always waited for; `written` gets both. One that replaces a file in JSON arrays logs the conversion (Editor/Save).
		// `written` (null: not told) gets each res:// tag as its file is stored. `wait_for_writes` stores each file with
		// store_sync (the file is on disk when the call returns; false when a write is refused), the way a caller that may
		// have to take the files back needs it. `on_file` (empty: none) is called after each file with (done, total).
		// A resource with no remap, one that cannot be read, a missing root and a desc that is not JSON stop the export too
		// (false, `progress->error` says which). A leaf the import lists but never stored is left out and named in
		// `progress->skipped`.
		// With `result.source_sha256` set (#1195) the prefab's `.resource` (plan_model_meta, edt_model_import_meta.h) is
		// checked before any file is written and written right before the prefab (taken back when the prefab is not);
		// `written` gets it, below `meta_top` (empty: the resource system's top layer, where the files go). An existing
		// one is rewritten only with `may_replace_meta` (a person's overwrite). That `.resource` records in `made` the
		// SHA-256 of every file the export wrote or left as it was (the prefab and a geometry desc's `.bin` too,
		// #1194).
		// `seen` (null: none; a re-import's check): before a file in it is overwritten it is hashed again, and one that
		// is no longer what the check saw stops the export. `finish_once_writing` (a re-import): `ctx` is heeded only
		// until the first write begins (`progress->writing`); from then on the export writes to its end, so a stop at
		// exit leaves no half-new asset and no `.resource` with the earlier `made`.
		bool export_to_project(
			const import_result& result,
			const std::unordered_map<res::tag, res::tag>& remap,
			export_progress* progress = nullptr,
			ds::task_context* ctx = nullptr,
			std::vector<res::tag>* written = nullptr,
			bool wait_for_writes = false,
			const std::function<void(int, int)>& on_file = {},
			const std::filesystem::path& meta_top = {},
			bool may_replace_meta = true,
			const std::unordered_map<res::tag, std::string>* seen = nullptr,
			bool finish_once_writing = false) const;

		// The bytes the export writes for `mem_tag` (a desc remapped and pretty-printed, else as it is); nullopt for a
		// geometry desc (written with its binary file), and with `error` set when it cannot be read.
		std::optional<std::vector<std::byte>> planned_bytes(const res::tag& mem_tag,
			const std::unordered_map<res::tag, res::tag>& remap, std::string& error) const;

		// The re-import's look at what the export would overwrite below `top` (#1194): each remap target there is
		// hashed and compared with `made` (the prefab's record, paths below `target_folder`) and with what the export
		// would write.
		reimport_scan scan_reimport(const std::unordered_map<res::tag, res::tag>& remap,
			const std::filesystem::path& top, std::string_view target_folder, const std::optional<made_files>& made,
			ds::task_context* ctx = nullptr) const;

		// Empty when everything the remap would write is something an import of `asset_name` makes, below `target_folder`, with the
		// extension it had (an agent's import: nothing it chose may turn a resource into a model file). Else why not.
		static std::string remap_problem(const std::unordered_map<res::tag, res::tag>& remap, std::string_view asset_name, std::string_view target_folder);

		// Empty when `target_folder` is a folder path inside res:// ("assets/tree"; empty is the root) by the rules of
		// check_agent_folder (no drive, protocol, '.'/'..'/empty segment, name Windows reserves, trailing '.' or ' ', <>"|?*).
		// Else why not.
		static std::string folder_problem(std::string_view target_folder);

		// Empty when `asset_name` is one name by the rules of check_agent_name. Else why not.
		static std::string asset_name_problem(std::string_view asset_name);

		// The res:// files an earlier import made below `target_folder` and the remap no longer writes (#1194): left
		// in the project, only listed. `made` (its record) when there is one; else the files the old prefab (its text,
		// read before the re-import overwrote it; nullopt: it could not be read) refers to below the folder, nothing
		// for the root of res://.
		static no_longer_made_list no_longer_made(const std::optional<made_files>& made,
			const std::optional<std::string>& old_prefab, std::string_view target_folder,
			const std::unordered_map<res::tag, res::tag>& remap);

		// What a person's export is held to: the folder and the name are plain, and everything the remap writes lands below
		// `target_folder` under a path check_agent_folder accepts. Empty when so, else why not. remap_problem adds the agent's
		// rules on names and extensions.
		static std::string destination_problem(const std::unordered_map<res::tag, res::tag>& remap, std::string_view asset_name, std::string_view target_folder);

	private:
		res::resource_system& m_res;

		// The file holds these bytes already (an import of an unchanged model): it is not written again, so file watchers and
		// git see nothing (#559).
		bool unchanged_on_disk(const res::tag& tag, const std::vector<std::byte>& bytes) const;

		// Writes the geometry `desc` (memory:// `from`, references remapped) as `to` and its binary file; lines and points (the
		// desc names its topology) as one line of JSON arrays, as before EPIC-79. Empty when written or unchanged (`wrote`
		// says which), else why not; triangles the loader would refuse are not written.
		// `begin_write` is called right before a file is written.
		std::string export_geometry(const res::tag& from, const res::tag& to, const boost::json::object& desc,
			std::vector<res::tag>* written, bool& wrote, std::vector<res::tag>& files,
			const std::function<void()>& begin_write) const;

		// The slots of the material file at `to` (an earlier export) that named an embedded texture other than `desc` names.
		std::vector<std::string> replaced_embedded_textures(const res::tag& to, const boost::json::value& desc) const;

		// Remap all string values in a JSON tree that match memory:// tags
		static void remap_json_strings(
			boost::json::value& val,
			const std::unordered_map<res::tag, res::tag>& remap);
	};

} // namespace edt
