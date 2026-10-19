#pragma once
// The engine frame on the null render backend: render_init picks drv_null because the null window has no
// GL surface, and the real platform stages (extract, render, present) run the engine's passes over the
// shipped smoke level without a display or a GPU. Only the GUI is absent: its backend is still GL.
// The backend runs under the validation decorator (render/validate) unless the fixture is built without it,
// so every frame is also checked against the rules of docs/roadmap/design/RHI.md section 3.

#include "test_scene_env.hpp"
#include "rnd_null_driver.h"
#include "rnd_null_swapchain.h"
#include "rnd_validation_driver.h"
#include <snk/application/app_frame_stage.h>
#include <snk/application/app_loop_service.h>
#include <snk/application/application.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/config/cfg_api.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_system.h>
#include <snk/game/gs_engine_frame_stages.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/render/rnd_render_system.h>
#include <snk/resource/res_system.h>
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_registration_scope.h>
#include <snk/scene/scn_scene_service_init.h>
#include <snk/window/wnd_null_window_system.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#ifndef SNK_EDITOR_RES_DIR
#error "SNK_EDITOR_RES_DIR must point at Editor/res"
#endif
#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace test_util {

namespace fs = std::filesystem;

namespace vl = rnd::driver::validation;

inline void set_render_validate(bool on)
{
	std::string program = "unit_tests";
	std::string flag = "--set";
	std::string name = "render/validate";
	std::string value = on ? "true" : "false";
	char* argv[] = { program.data(), flag.data(), name.data(), value.data() };
	cfg::initialize_configs(static_cast<int>(std::size(argv)), argv);
}

// Serves the repository's res:// files (core/res, overridden by Editor/res) from RAM.
inline void mount_repo_res(test_util::ram_resolver& ram)
{
	for (const fs::path& root : { fs::path{ SNK_CORE_RES_DIR }, fs::path{ SNK_EDITOR_RES_DIR } }) {
		for (const auto& entry : fs::recursive_directory_iterator(root)) {
			if (!entry.is_regular_file()) continue;
			std::ifstream in(entry.path(), std::ios::binary);
			const std::vector<char> text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
			std::vector<std::byte> bytes(text.size());
			std::memcpy(bytes.data(), text.data(), text.size());
			ram.files[res::tag::make(fs::relative(entry.path(), root).generic_string())] = std::move(bytes);
		}
	}
}

struct null_engine_fixture
{
	explicit null_engine_fixture(bool validate = true)
	{
		fs::create_directories("./res"); // resource_system's vfs root
		fs::create_directories("./sandbox");

		storage.construct<wnd::window_system, wnd::null_window_system>();
		auto& rs = storage.construct<res::resource_system>(std::ref(tasks));
		rs.unregistrate_resolver(res::tag::memory);
		rs.registrate_resolver<test_util::ram_resolver>(res::tag::memory);
		rs.unregistrate_resolver(res::tag::default_protocol());
		mount_repo_res(rs.registrate_resolver<test_util::ram_resolver>(res::tag::default_protocol()));

		auto& descs = storage.construct<desc::desc_system>(std::ref(rs));
		rs.registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(rs), std::ref(descs));
		rs.registrate_adapter<scn::model_importer_adapter>(scn::model_importer_adapter::INFO, std::ref(descs), std::ref(rs));
		auto& sfactory = storage.construct<ecs::system_factory>();

		auto& stages = storage.construct<app::frame_stage_registry>();
		app::register_core_frame_stages(stages);
		stages.add_stage("tasks", app::builtin_stage::input, app::stage_side::after, [this](app::stage_context&) {
			// Stands in for the production `tasks` stage; it also finishes the load tasks, as no pool thread does.
			test_util::pump(tasks);
		});
		storage.construct<app::app_loop_service_interface, app::loop_service>(stages);

		auto ecs_input = std::make_shared<inp::ecs_input_manager>();
		inp::register_ecs_input_system(sfactory, ecs_input);

		// The process-wide registry core_module makes: render_init registers viewport.capture into it.
		storage.construct<cmd::command_registry>();
		set_render_validate(validate);
		engine::render::render_init(storage);
		scn::scene_init(storage);
		gs::register_scene_frame_stages(stages);
		gs::register_platform_frame_stages(stages);
		scene_types.emplace(scn::register_scene_types(descs, storage.require<scn::ecs_assembler>(), storage.require<rnd::skinning_manager>()));
	}

	~null_engine_fixture()
	{
		scene_types.reset();
		scn::scene_term(storage);
		engine::render::render_term(storage);
		set_render_validate(false);
		auto& rs = storage.require<res::resource_system>();
		rs.unregistrate_adapter(scn::model_importer_adapter::INFO);
		rs.unregistrate_adapter(core::desc::desc_adapter_t::INFO);
	}

	vl::swapchain* validating_swapchain()
	{
		return dynamic_cast<vl::swapchain*>(&storage.require<rnd::driver::swapchain_interface>());
	}

	vl::driver* validating_driver()
	{
		return dynamic_cast<vl::driver*>(storage.require<rnd::render_system>().get_driver());
	}

	rnd::driver::null::swapchain* swapchain()
	{
		auto* validating = validating_swapchain();
		return validating ? dynamic_cast<rnd::driver::null::swapchain*>(&validating->inner()) : nullptr;
	}

	rnd::driver::null::driver* driver()
	{
		auto* validating = validating_driver();
		return validating ? dynamic_cast<rnd::driver::null::driver*>(&validating->inner()) : nullptr;
	}

	// Runs `frames` frames through application::run: a stage after `present` counts them and stops the loop.
	int run_frames(int frames)
	{
		int done = 0;
		auto& stages = storage.require<app::frame_stage_registry>();
		stages.add_stage("test_frame_counter", app::builtin_stage::present, app::stage_side::after,
			[&done, frames](app::stage_context& c) {
				if (++done >= frames) {
					c.storage.require<app::app_loop_service_interface>().request_stop();
				}
			});
		app::application application;
		BOOST_TEST(application.run(storage) == 0);
		stages.remove_stage("test_frame_counter");
		return done;
	}

	ds::task_system tasks{ test_util::manual_tasks() }; // outlives the services in `storage`
	ds::app_data_storage storage;
	std::optional<scn::registration_scope> scene_types;
};

}
