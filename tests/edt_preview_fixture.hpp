#pragma once
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_document.h"
#include "edt_preview_stage.h"
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/scn_model.h>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What the preview stage tests and the material preview tests share (#1196): a context manager, the preview template
// (a sky, no light: no pixels are drawn), the unit shapes and the engine's sky shader in a lower layer.

#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace test_util
{
	inline constexpr const char* k_preview_template = R"({
	"__type": "world_desc", "__version": 5, "name": "__prefab_preview", "systems": [],
	"components": { "anchor": { "__type": "anchor_desc", "__version": 1 } },
	"children": [
		{ "__id": "faa1152ccb1457e8", "name": "Skybox",
			"components": { "skybox_desc": { "__type": "skybox_desc", "__version": 1 } } }
	]
})";

	struct preview_env : scene_env
	{
		explicit preview_env(std::string_view dir_name)
			: scene_env(test_descs::without)
			, dir{ dir_name }
		{
			namespace fs = std::filesystem;
			fs::create_directories(res_root);
			fs::create_directories(lower / "shapes");
			// The unit shapes the material preview draws, in the lower layer as the engine's are.
			for (const char* name : { "sphere", "box", "plane" }) {
				for (const char* suffix : { ".geom.desc", ".geom.bin" }) {
					const std::string file = std::string(name) + suffix;
					fs::copy_file(fs::path(SNK_CORE_RES_DIR) / "shapes" / file, lower / "shapes" / file, fs::copy_options::overwrite_existing);
				}
			}
			// The engine's sky shader, as core/res has it (the stages are not compiled here).
			fs::create_directories(lower / "shaders");
			write(lower, "shaders/sky.vert", "#version 450\n");
			write(lower, "shaders/sky.frag", "#version 450\n");
			write(lower, "shaders/sky.shader.desc",
				R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/sky.vert","fragment":"res://shaders/sky.frag"})");
			rs->unregistrate_resolver(res::tag::default_protocol());
			rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
				res::mount_access::read_write, res::write_policy::allow_all());
			engine::render::register_render_descs(*descs);
			auto& edt_ram = rs->registrate_resolver<ram_resolver>("edt");
			edt_ram.store(res::tag("edt://templates/prefab_preview.desc"), to_bytes(k_preview_template));
			hot = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
			mgr = std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot);
			services.ctx_manager = mgr;
			services.base_descs = descs.get();
			services.base_resources = rs.get();
			services.base_assembler = assembler.get();
			services.base_sfactory = &sf;
		}

		~preview_env()
		{
			mgr.reset();
			hot.reset();
			engine::render::unregister_render_descs(*descs);
			rs->unregistrate_resolver(res::tag::default_protocol());
		}

		edt::desc_file_layout layout()
		{
			edt::desc_file_layout out;
			out.top_layer = [this] { return res_root; };
			out.layers = [this] { return std::vector<std::filesystem::path>{ lower, res_root }; };
			out.open_level_files = [] { return std::vector<std::string>{}; };
			return out;
		}

		static void write(const std::filesystem::path& root, std::string_view name, std::string_view content)
		{
			std::ofstream out(root / std::string(name), std::ios::binary);
			out << content;
		}

		res::tag put(std::string_view name, std::string_view content) const
		{
			write(res_root, name, content);
			return res::tag{ "res://" + std::string(name) };
		}

		// Collects the render target tags a stage gives up.
		edt::preview_stage::targets_release record_released()
		{
			return [this](const std::vector<res::tag>& tags) { released.insert(released.end(), tags.begin(), tags.end()); };
		}

		// The first mesh entity of the stage's world; null: none.
		static entt::entity shape_entity(edt::preview_stage& stage)
		{
			entt::registry* reg = stage.registry();
			if (!reg) return entt::null;
			const auto shapes = reg->view<scn::mesh_component>();
			if (shapes.begin() == shapes.end()) return entt::null;
			return *shapes.begin();
		}

		// The sky entities of the stage's world and how many of them are drawn.
		static std::pair<std::size_t, std::size_t> skies(edt::preview_stage& stage)
		{
			entt::registry* reg = stage.registry();
			if (!reg) return { 0u, 0u };
			std::size_t all = 0;
			std::size_t shown = 0;
			for (const entt::entity entity : reg->view<scn::sky_component>()) {
				++all;
				if (scn::is_visible(*reg, entity)) ++shown;
			}
			return { all, shown };
		}

		temp_dir dir;
		const std::filesystem::path res_root = dir.path() / "res";
		const std::filesystem::path lower = dir.path() / "lower";
		ecs::system_factory sf;
		edt::editor_services services;
		std::shared_ptr<scn::hot_reload_manager> hot;
		std::shared_ptr<scn::context_manager> mgr;
		std::vector<res::tag> released;
	};
}
