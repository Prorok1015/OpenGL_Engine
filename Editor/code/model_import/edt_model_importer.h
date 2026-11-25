#pragma once
#include <snk/common/common.h>
#include <snk/model_codec/res_tag.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include "edt_async_import_task.h"
#include "edt_decoder_sandbox.h"
#include "edt_model_file_access.h"
#include "edt_model_import_core.h"
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
		// What the `.resource` of the exported prefab records (#1195): the settings and the SHA-256 (lowercase hex) of
		// the model file. An empty sha256: the export writes no `.resource` for the prefab.
		model_import_settings             settings{};
		std::string                       source_sha256{};
	};

	class model_importer {
	public:
		model_importer(desc::desc_system& ds, res::resource_system& rs);

		// Background-safe phase: import_model_into over the resource system, the prefab stamped by this desc system. The
		// editor itself imports through process_decoder (#925, #987); the decoder process runs import_model_into alone (#1077).
		// Does NOT touch desc_sys.create_instance or skinning_manager.
		// `ctx` (null: not cancellable) is checked between the phases and by Assimp while it parses; a stop request
		// throws ds::task_cancelled. The helpers store textures, descs and materials as they go and the geometry
		// and prefab at the end; when the job is cancelled or throws, every tag stored so far is erased again.
		// With `source.allowed` set, anything the model refers to outside that list fails the import; with `source.provided`
		// set, a side file it was not given does.
		import_intermediate import_background(const import_source& source, ds::task_context* ctx = nullptr);

		// scan_model_files and list_model_requests (edt_model_import_core.h).
		static model_file_scan scan_files(const import_source& source, ds::task_context* ctx = nullptr);
		static scan_requests list_requests(const import_source& source, ds::task_context* ctx = nullptr);

		// Main-thread finalization: desc_sys.create_instance + deserialize + skinning.
		import_result finalize_on_main(import_intermediate&& intermediate);

	private:
		desc::desc_system&    m_desc;
		res::resource_system& m_res;
	};

} // namespace edt
