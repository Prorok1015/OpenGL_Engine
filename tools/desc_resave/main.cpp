// desc_resave: loads every .desc file under a resources directory through the engine (older formats are
// migrated on load) and writes it back in the current format, indented. Run it after a format change.
//
//   desc_resave <path to a resources directory named "res"> [--base <res dir>] [--check]
//
// --check writes nothing and exits with 1 when any file would change.
// --base mounts a lower layer under the directory (Editor/res needs core/res for its base descs);
// only the descs of the first directory are touched.
#include <snk/resource/res_system.h>
#include <snk/desc/desc_system.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/common/ds/ds_task_system.hpp>
#if SNK_HAS_ASSIMP
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#endif
#include <snk/config/cfg_api.h>
#include <snk/common/logger/engine_log.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_registration_scope.h>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

CFG_VAR_EXT_PATH(cfg_res_base_path);

namespace {

	std::vector<std::byte> read_file(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::vector<char> text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		std::vector<std::byte> bytes(text.size());
		std::transform(text.begin(), text.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
		return bytes;
	}

}

int main(int argc, char** argv)
{
	if (argc < 2) {
		egLOG_ERROR("desc_resave", "usage: desc_resave <resources directory named 'res'> [--check]");
		return 2;
	}
	const fs::path res_dir = fs::absolute(argv[1]).lexically_normal();
	bool check_only = false;
	fs::path base_dir;
	for (int i = 2; i < argc; ++i) {
		const std::string arg{ argv[i] };
		if (arg == "--check") {
			check_only = true;
		} else if (arg == "--base" && i + 1 < argc) {
			base_dir = fs::absolute(argv[++i]).lexically_normal();
		} else {
			egLOG_ERROR("desc_resave", "unknown argument '{}'", arg);
			return 2;
		}
	}
	if (!fs::is_directory(res_dir) || res_dir.filename() != "res") {
		egLOG_ERROR("desc_resave", "'{}' is not a directory named 'res'", res_dir.string());
		return 2;
	}

	// The resource system reads its root from the configured "./res"; run next to it, with the sandbox it expects.
	fs::current_path(res_dir.parent_path());
	if (!base_dir.empty()) {
		if (!fs::is_directory(base_dir)) {
			egLOG_ERROR("desc_resave", "base '{}' is not a directory", base_dir.string());
			return 2;
		}
		*cfg_res_base_path = base_dir;
	}
	std::error_code ec;
	fs::create_directories("./sandbox", ec);

	// No threads: get_sync() on a manual task_system runs the queued loads itself.
	ds::task_system_config tasks_config;
	tasks_config.mode = ds::policy::manual;
	ds::task_system tasks(tasks_config);
	res::resource_system res_sys(tasks);
	desc::desc_system desc_sys(res_sys);
	res_sys.registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(res_sys), std::ref(desc_sys));
	scn::ecs_assembler assembler(desc_sys, res_sys);
	rnd::skinning_manager skinning;
	auto scene_types = scn::register_scene_types(desc_sys, assembler, skinning);
	engine::render::register_render_descs(desc_sys);
#if SNK_HAS_ASSIMP
	// Levels reference models (.glb) that are imported into prefabs while they load.
	res_sys.registrate_adapter<scn::model_importer_adapter>(scn::model_importer_adapter::INFO, std::ref(desc_sys), std::ref(res_sys));
#endif
	core::desc::desc_adapter_t adapter(res_sys, desc_sys);

	std::vector<fs::path> files;
	for (const auto& entry : fs::recursive_directory_iterator(res_dir)) {
		if (entry.is_regular_file() && entry.path().extension() == ".desc") {
			files.push_back(entry.path());
		}
	}
	std::ranges::sort(files);

	int failed = 0;
	int changed = 0;
	for (const auto& path : files) {
		const std::string rel = fs::relative(path, res_dir).generic_string();
		const res::tag tag = res::tag::make(rel);
		auto handle = res_sys.require_sync<desc::desc_base>(tag);
		auto loaded = handle.get_sync();
		if (!loaded) {
			egLOG_ERROR("desc_resave", "{}: failed to load", rel);
			++failed;
			continue;
		}

		const auto written = adapter.serialize(tag, loaded);
		if (written == read_file(path)) {
			continue;
		}
		++changed;
		if (check_only) {
			egLOG_INFO("desc_resave", "{}: would change", rel);
			continue;
		}
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(written.data()), static_cast<std::streamsize>(written.size()));
		egLOG_INFO("desc_resave", "{}: rewritten", rel);
	}

	egLOG_INFO("desc_resave", "{} files, {} {}, {} failed", files.size(), changed, check_only ? "would change" : "rewritten", failed);
	return failed > 0 || (check_only && changed > 0) ? 1 : 0;
}
