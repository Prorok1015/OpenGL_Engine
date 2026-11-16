#pragma once
#include "edt_async_import_task.h"
#include "edt_decoder_sandbox.h"
#include "edt_model_file_access.h"
#include <snk/common/ds/ds_task_system.hpp>
#if SNK_HAS_ASSIMP
#include <snk/model_import/mdl_import.h>
#endif
#include <boost/json/object.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// The editor's import of a model without a desc or resource system (#1077): what the decoder process runs, and what
// model_importer runs over the resource system in the editor's own tests. Links no engine code but snk_model_codec.
namespace edt {

	// The largest model file an import a person starts reads into memory (an agent's import has its own, smaller limit).
	inline constexpr std::uintmax_t k_max_person_source_bytes = 256ull * 1024ull * 1024ull;
	// How long each decoder step (listing the files, importing) may take: an agent's import, and the one a person starts
	// (larger models, nobody waiting on a deadline of their own).
	inline constexpr std::chrono::seconds k_agent_parse_time_limit{ 60 };
	inline constexpr std::chrono::seconds k_person_parse_time_limit{ 120 };

	// What an import reads. `preloaded`: the bytes of the model file, already read. `allowed`: set for an agent's
	// import, the files the model may read besides it (what a person was shown). `provided`: set in the decoder process
	// (#1043), the files the editor read for it; nothing else is read, and `allowed` is not used.
	struct import_source {
		std::filesystem::path                 path;
		const std::vector<std::byte>*         preloaded = nullptr;
		const std::vector<referenced_file>*   allowed = nullptr;
		const std::vector<provided_file>*     provided = nullptr;
		model_file_limits                     limits;
		// Zero: no limit. Assimp is told to stop when it passes, and the import fails with a message.
		std::chrono::steady_clock::time_point deadline{};
		// Empty: the model's folder. The folder side files may come from (model_file_access's boundary).
		std::filesystem::path boundary;
		// Empty: "<stem>/". The memory:// folder the import's resources go to, ending in '/'.
		std::string memory_folder;
		// False: only `preloaded` is the model, `path` names no file (model_file_access's on_disk).
		bool on_disk = true;
		// The decoder process's OS sandbox (#1043) this import asks for at least; off asks for nothing beyond the setting.
		sandbox_mode sandbox = sandbox_mode::off;
	};

	// The files a model would read (needs `source.preloaded`): parses it, stores nothing. `error` says why the model
	// may not be imported by an agent (a reference outside its folder, a hidden file, no image, too many or too big).
	model_file_scan scan_model_files(const import_source& source, ds::task_context* ctx = nullptr);

	// The decoder process's scan (#1043): parses the model with only `source.provided` (none when null) and returns the
	// names it asked for that it was not given, side files and the textures scan_model_files reads. Reads no disk. A parse
	// that failed while a side file was missing is not an error: the editor hands the file over and asks again.
	scan_requests list_model_requests(const import_source& source, ds::task_context* ctx = nullptr);

#if SNK_HAS_ASSIMP
	// mdl::import_model (assimp) of `source` into `sink`, under memory://<memory_folder>: textures, materials and the
	// geometry as it goes, the prefab last. `stamp` (empty: none) is given the prefab's JSON before it is stored, so the
	// desc system that will read it stamps its versions; the decoder process has none and stores it unstamped, the editor
	// stamps what it accepts (accept_import_answer). The result has every field but `created_tags` (the sink's to know).
	// `ctx` (null: not cancellable) is checked between the steps and by Assimp while it parses; a stop request throws
	// ds::task_cancelled, as does whatever `sink` throws. With `source.allowed` set, anything the model refers to outside
	// that list fails the import; with `source.provided` set, a side file it was not given does.
	import_intermediate import_model_into(const import_source& source, mdl::import_sink& sink, ds::task_context* ctx = nullptr,
		const std::function<void(boost::json::object&)>& stamp = {});
#endif

} // namespace edt
