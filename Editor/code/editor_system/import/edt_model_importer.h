#pragma once
#include <snk/common/common.h>
#include <snk/resource/res_tag.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include "edt_async_import_task.h"
#include "edt_model_file_access.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/scene/level/scn_prefab_desc.h>
#include <chrono>
#include <filesystem>
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

	// What an import reads. `preloaded`: the bytes of the model file, already read. `allowed`: set for an agent's
	// import, the files the model may read besides it (what a person was shown).
	struct import_source {
		std::filesystem::path                 path;
		const std::vector<std::byte>*         preloaded = nullptr;
		const std::vector<referenced_file>*   allowed = nullptr;
		model_file_limits                     limits;
		// Zero: no limit. Assimp is told to stop when it passes, and the import fails with a message.
		std::chrono::steady_clock::time_point deadline{};
	};

	class model_importer {
	public:
		model_importer(desc::desc_system& ds, res::resource_system& rs);

		// Background-safe phase: Assimp parse + geometry + materials + store.
		// Does NOT touch desc_sys.create_instance or skinning_manager.
		// `ctx` (null: not cancellable) is checked between the phases and by Assimp while it parses; a stop request
		// throws ds::task_cancelled. The helpers store textures, descs and materials as they go and the geometry
		// and prefab at the end; when the job is cancelled or throws, every tag stored so far is erased again.
		import_intermediate import_background(const std::filesystem::path& abs_path, ds::task_context* ctx = nullptr);
		// With `source.allowed` set, anything the model refers to outside that list fails the import.
		import_intermediate import_background(const import_source& source, ds::task_context* ctx = nullptr);

		// The files a model would read (needs `source.preloaded`): parses it, stores nothing. `error` says why the model
		// may not be imported by an agent (a reference outside its folder, a hidden file, no image, too many or too big).
		static model_file_scan scan_files(const import_source& source, ds::task_context* ctx = nullptr);

		// Main-thread finalization: desc_sys.create_instance + deserialize + skinning.
		import_result finalize_on_main(import_intermediate&& intermediate);

	private:
		desc::desc_system&    m_desc;
		res::resource_system& m_res;
	};

} // namespace edt
