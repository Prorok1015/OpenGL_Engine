#pragma once
#include "common.h"
#include "res_tag.h"
#include "desc_system.h"
#include "res_system.h"
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

		// Load model from absolute FS path, store results in memory://
		import_result import(const std::filesystem::path& abs_path);

	private:
		desc::desc_system&    m_desc;
		res::resource_system& m_res;
		rnd::render_system&   m_rnd;
	};

} // namespace edt
