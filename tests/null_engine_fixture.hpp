#pragma once
// The engine frame on the null render backend: render_init picks drv_null because the null window has no
// GL surface, and the real platform stages (extract, render, present) run the engine's passes over the
// shipped smoke level without a display or a GPU. Only the GUI is absent: its backend is still GL.
// The backend runs under the validation decorator (render/validate) unless the fixture is built without it,
// so every frame is also checked against the rules of docs/roadmap/design/RHI.md section 3.

#include "test_scene_env.hpp"
#include "rnd_null_driver.h"
#include <snk/driver_null/rnd_null_swapchain.h>
#include <snk/driver_validation/rnd_validation_driver.h>
#include <snk/application/app_frame_stage.h>
#include <snk/application/app_loop_service.h>
#include <snk/application/application.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/config/cfg_variables.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_system.h>
#include <snk/game/gs_engine_frame_stages.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/render/rnd_render_system.h>
#include <snk/resource/res_system.h>
#if SNK_HAS_ASSIMP
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#endif
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_registration_scope.h>
#include <snk/scene/scn_scene_service_init.h>
#include <snk/window/wnd_null_window_system.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#ifndef SNK_TEST_PROJECT_DIR
#error "SNK_TEST_PROJECT_DIR must point at projects/sandbox"
#endif
#ifndef SNK_EDITOR_DATA_DIR
#error "SNK_EDITOR_DATA_DIR must point at Editor/data"
#endif
#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace test_util {

namespace fs = std::filesystem;

namespace vl = rnd::driver::validation;

// How long a part of a case takes, in the log as "test/timing": where the null-backend suites spend their time on a
// slow runner (#898).
class phase_timer
{
public:
	explicit phase_timer(std::string phase) : m_phase(std::move(phase)) {}
	phase_timer(const phase_timer&) = delete;
	phase_timer& operator=(const phase_timer&) = delete;
	~phase_timer()
	{
		using std::chrono::milliseconds;
		const auto took = std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - m_start);
		egLOG_INFO("test/timing", "{}: {} ms", m_phase, took.count());
	}

private:
	std::string m_phase;
	std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

inline void set_render_validate(bool on)
{
	std::string program = "unit_tests";
	std::string flag = "--set";
	std::string name = "render/validate";
	std::string value = on ? "true" : "false";
	char* argv[] = { program.data(), flag.data(), name.data(), value.data() };
	cfg::initialize_configs(static_cast<int>(std::size(argv)), argv);
}

// Serves the repository's res:// files (core/res, overridden by the test project projects/sandbox/res) from RAM.
inline void mount_dir(test_util::ram_resolver& ram, const fs::path& root, std::string_view protocol)
{
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) continue;
		// One read per file: a byte at a time took 2 s of every case under TSan's -O0 build (#524). A file that
		// cannot be opened or sized stays empty, as before.
		std::error_code size_error;
		const std::uintmax_t size = entry.file_size(size_error);
		std::vector<std::byte> bytes(size_error ? 0 : static_cast<std::size_t>(size));
		std::ifstream in(entry.path(), std::ios::binary);
		in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		bytes.resize(static_cast<std::size_t>(in.gcount()));
		ram.files[res::tag{ protocol, fs::relative(entry.path(), root).generic_string() }] = std::move(bytes);
	}
}

inline void mount_repo_res(test_util::ram_resolver& ram)
{
	for (const fs::path& root : { fs::path{ SNK_CORE_RES_DIR }, (fs::path{ SNK_TEST_PROJECT_DIR } / "res") }) {
		mount_dir(ram, root, res::tag::default_protocol());
	}
}

struct null_engine_fixture
{
	explicit null_engine_fixture(bool validate = true)
	{
		const phase_timer whole("fixture setup");
		fs::create_directories("./res"); // resource_system's vfs root
		fs::create_directories("./sandbox");

		storage.construct<wnd::window_system, wnd::null_window_system>();
		auto& rs = storage.construct<res::resource_system>(std::ref(tasks));
		rs.unregistrate_resolver(res::tag::memory);
		rs.registrate_resolver<test_util::ram_resolver>(res::tag::memory);
		rs.unregistrate_resolver(res::tag::default_protocol());
		{
			const phase_timer mount("mount res:// and edt://");
			mount_repo_res(rs.registrate_resolver<test_util::ram_resolver>(res::tag::default_protocol()));
			mount_dir(rs.registrate_resolver<test_util::ram_resolver>("edt"), SNK_EDITOR_DATA_DIR, "edt");
		}

		auto& descs = storage.construct<desc::desc_system>(std::ref(rs));
		rs.registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(rs), std::ref(descs));
#if SNK_HAS_ASSIMP
		rs.registrate_adapter<scn::model_importer_adapter>(scn::model_importer_adapter::INFO, std::ref(descs), std::ref(rs));
#endif
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
		{
			const phase_timer init("render_init + scene_init");
			engine::render::render_init(storage);
			scn::scene_init(storage);
		}
		gs::register_scene_frame_stages(stages);
		gs::register_platform_frame_stages(stages);
		scene_types.emplace(scn::register_scene_types(descs, storage.require<scn::ecs_assembler>(), storage.require<rnd::skinning_manager>()));
		m_ready = std::chrono::steady_clock::now();
	}

	~null_engine_fixture()
	{
		const phase_timer whole("fixture teardown");
		unregister_scene_types();
		scn::scene_term(storage);
		engine::render::render_term(storage);
		set_render_validate(false);
		auto& rs = storage.require<res::resource_system>();
#if SNK_HAS_ASSIMP
		rs.unregistrate_adapter(scn::model_importer_adapter::INFO);
#endif
		rs.unregistrate_adapter(core::desc::desc_adapter_t::INFO);
	}

	// The first step of engine::engine_module::shutdown_services, in the same order. Idempotent.
	void unregister_scene_types()
	{
		scn::stop_scene_loads(storage);
		scene_types.reset();
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
		// What the case did before its frames: the level's load in most cases.
		using std::chrono::milliseconds;
		const auto before = std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - m_ready);
		egLOG_INFO("test/timing", "before run_frames({}): {} ms since the setup or the last run_frames", frames,
			before.count());
		const phase_timer timer(std::format("run_frames({})", frames));
		int done = 0;
		auto& stages = storage.require<app::frame_stage_registry>();
		auto frame_start = std::chrono::steady_clock::now();
		stages.add_stage("test_frame_counter", app::builtin_stage::present, app::stage_side::after,
			[&done, &frame_start, frames](app::stage_context& c) {
				const auto now = std::chrono::steady_clock::now();
				egLOG_INFO("test/timing", "frame {}: {} ms", done,
					std::chrono::duration_cast<std::chrono::milliseconds>(now - frame_start).count());
				frame_start = now;
				if (++done >= frames) {
					c.storage.require<app::app_loop_service_interface>().request_stop();
				}
			});
		app::application application;
		BOOST_TEST(application.run(storage) == 0);
		stages.remove_stage("test_frame_counter");
		m_ready = std::chrono::steady_clock::now();
		return done;
	}

	ds::task_system tasks{ test_util::manual_tasks() }; // outlives the services in `storage`
	ds::app_data_storage storage;
	std::optional<scn::registration_scope> scene_types;

private:
	// The end of the setup or of the last run_frames.
	std::chrono::steady_clock::time_point m_ready = std::chrono::steady_clock::now();
};

}
