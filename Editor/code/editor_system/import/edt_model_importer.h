#pragma once
#include <snk/common/common.h>
#include <snk/resource/res_tag.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include "edt_async_import_task.h"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/scene/level/scn_prefab_desc.h>
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

	class model_importer {
	public:
		model_importer(desc::desc_system& ds, res::resource_system& rs);

		// Background-safe phase: Assimp parse + geometry + materials + store.
		// Does NOT touch desc_sys.create_instance or skinning_manager.
		// `ctx` (null: not cancellable) is checked between the phases and by Assimp while it parses; a stop request
		// throws ds::task_cancelled. The helpers store textures, descs and materials as they go and the geometry
		// and prefab at the end; when the job is cancelled or throws, every tag stored so far is erased again.
		import_intermediate import_background(const std::filesystem::path& abs_path, ds::task_context* ctx = nullptr);

		// Main-thread finalization: desc_sys.create_instance + deserialize + skinning.
		import_result finalize_on_main(import_intermediate&& intermediate);

	private:
		desc::desc_system&    m_desc;
		res::resource_system& m_res;
	};

} // namespace edt
