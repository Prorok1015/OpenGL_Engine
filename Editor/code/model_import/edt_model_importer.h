#pragma once
#include <snk/common/common.h>
#include <snk/resource/res_tag.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include "edt_async_import_task.h"
#include "edt_decoder_sandbox.h"
#include "edt_model_file_access.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/scene/level/scn_prefab_desc.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>

namespace edt {

	struct import_result {
		std::shared_ptr<scn::prefab_desc> prefab;
		res::tag                          root_tag;     // memory://name/name.prefab.desc
		std::vector<res::tag>             all_tags;     // all memory:// tags created
		std::filesystem::path             source_path;  // original file on disk
	};

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

	class model_importer {
	public:
		model_importer(desc::desc_system& ds, res::resource_system& rs);

		// Background-safe phase: mdl::import_model (assimp, snk_model_import_assimp) + store, in this process: the decoder
		// process runs it (#647); the editor itself imports through process_decoder (#925, #987).
		// Does NOT touch desc_sys.create_instance or skinning_manager.
		// `ctx` (null: not cancellable) is checked between the phases and by Assimp while it parses; a stop request
		// throws ds::task_cancelled. The helpers store textures, descs and materials as they go and the geometry
		// and prefab at the end; when the job is cancelled or throws, every tag stored so far is erased again.
		// With `source.allowed` set, anything the model refers to outside that list fails the import; with `source.provided`
		// set, a side file it was not given does.
		import_intermediate import_background(const import_source& source, ds::task_context* ctx = nullptr);

		// The files a model would read (needs `source.preloaded`): parses it, stores nothing. `error` says why the model
		// may not be imported by an agent (a reference outside its folder, a hidden file, no image, too many or too big).
		static model_file_scan scan_files(const import_source& source, ds::task_context* ctx = nullptr);

		// The decoder process's scan (#1043): parses the model with only `source.provided` (none when null) and returns the
		// names it asked for that it was not given, side files and the textures scan_files reads. Reads no disk. A parse
		// that failed while a side file was missing is not an error: the editor hands the file over and asks again.
		static scan_requests list_requests(const import_source& source, ds::task_context* ctx = nullptr);

		// Main-thread finalization: desc_sys.create_instance + deserialize + skinning.
		import_result finalize_on_main(import_intermediate&& intermediate);

	private:
		desc::desc_system&    m_desc;
		res::resource_system& m_res;
	};

} // namespace edt
