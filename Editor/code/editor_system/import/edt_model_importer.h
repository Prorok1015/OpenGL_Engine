#pragma once
#include "common.h"
#include "res_tag.h"
#include "desc_system.h"
#include "res_system.h"
#include "edt_async_import_task.h"
#include "level/scn_prefab_desc.h"
#include <filesystem>
#include <vector>
#include <memory>

namespace rnd { class render_system; }

namespace edt {

	struct import_result {
		std::shared_ptr<scn::prefab_desc> prefab;
		res::tag                          root_tag;     // memory://name/name.prefab.desc
		std::vector<res::tag>             all_tags;     // all memory:// tags created
		std::filesystem::path             source_path;  // original file on disk
	};

	class model_importer {
	public:
		model_importer(desc::desc_system& ds, res::resource_system& rs, rnd::render_system& rnd);

		// Synchronous import (blocks caller). Kept for backward compat.
		import_result import(const std::filesystem::path& abs_path);

		// Background-safe phase: Assimp parse + geometry + materials + store.
		// Does NOT touch desc_sys.create_instance or skinning_manager.
		import_intermediate import_background(const std::filesystem::path& abs_path);

		// Main-thread finalization: desc_sys.create_instance + deserialize + skinning.
		import_result finalize_on_main(import_intermediate&& intermediate);

	private:
		desc::desc_system&    m_desc;
		res::resource_system& m_res;
		rnd::render_system&   m_rnd;
	};

} // namespace edt
