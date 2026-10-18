#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/application/app_frame_stage.h>
#include <snk/application/app_loop_service.h>
#include <snk/application/application.h>
#include <snk/game/gs_engine_frame_stages.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/scene/level/scn_level_manager.h>
#include <string>
#include <vector>

// The engine's frame loop without editor_module. What is real: the core registry, begin body and
// loop_service, application::run, and the engine's scene stage bodies (level_manager::update,
// input delta reset) running on a real level_manager. What is NOT here: the window and the GPU
// (GLFW/GL need a display, which unit tests do not have), so the platform stages (poll events,
// extract, render, present) are not registered; every editor-only part (ImGui ui body, editor
// camera) lives in editor_module, which these tests never touch.

namespace {

constexpr const char* LEVEL_TWO_WORLDS = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":[],"children":{"c1":{"components":{"stat":{"__type":"stat_desc","hp":1}}}}},
	{"__type":"world_desc","name":"w1","systems":[]}]})";

constexpr const char* LEVEL_ONE_WORLD = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"only","systems":[],"children":{"c3":{"components":{"stat":{"__type":"stat_desc","hp":3}}}}}]})";

struct engine_frame_fixture : test_util::scene_env
{
	engine_frame_fixture()
		: hot(std::make_shared<scn::hot_reload_manager>(*assembler, *rs))
	{
		auto& stages = storage.construct<app::frame_stage_registry>();
		app::register_core_frame_stages(stages);
		gs::register_scene_frame_stages(stages);
		storage.construct<inp::ecs_input_manager>();
		storage.construct<scn::level_manager>(*rs, *assembler, sf, hot);
		storage.construct<app::app_loop_service_interface, app::loop_service>(stages);
	}

	app::frame_stage_registry& stages() { return storage.require<app::frame_stage_registry>(); }
	scn::level_manager& levels() { return storage.require<scn::level_manager>(); }

	// Runs `frames` frames through application::run: a stage after `present` counts them and stops the loop.
	int run_frames(int frames)
	{
		int done = 0;
		stages().add_stage("test_frame_counter", app::builtin_stage::present, app::stage_side::after,
			[&done, frames](app::stage_context& c) {
				if (++done >= frames) {
					c.storage.require<app::app_loop_service_interface>().request_stop();
				}
			});
		app::application application;
		BOOST_TEST(application.run(storage) == 0);
		stages().remove_stage("test_frame_counter");
		return done;
	}

	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	ds::app_data_storage storage;
};

}

BOOST_FIXTURE_TEST_SUITE(AppEngineWithoutEditorTests, engine_frame_fixture)

BOOST_AUTO_TEST_CASE(FrameRunsWithoutAnyEditorService)
{
	BOOST_TEST(run_frames(3) == 3);
}

BOOST_AUTO_TEST_CASE(OnlyBuiltinStagesArePresentWithoutTheEditor)
{
	const std::vector<std::string> expected{ "begin", "input", "update", "post_update", "extract", "render", "ui", "present" };
	BOOST_TEST(stages().stage_names() == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(UpdateStageDrivesTheLevelManager)
{
	const auto tag = put("lv.desc", LEVEL_TWO_WORLDS);
	BOOST_REQUIRE(levels().load(tag));
	BOOST_REQUIRE(levels().get_level().get_world_count() == 2u);

	// A changed level file is only applied from level_manager::update: seeing it applied after
	// frames proves the engine's `update` body ran.
	put("lv.desc", LEVEL_ONE_WORLD);
	rs->signal_changed(tag);
	run_frames(2);
	BOOST_TEST(levels().get_level().get_world_count() == 1u);
	BOOST_TEST(levels().get_level().has_world("only"));
}

BOOST_AUTO_TEST_CASE(PostUpdateToleratesMissingInputManager)
{
	storage.destruct<app::app_loop_service_interface>();
	storage.destruct<scn::level_manager>();
	storage.destruct<inp::ecs_input_manager>();
	storage.construct<scn::level_manager>(*rs, *assembler, sf, hot);
	storage.construct<app::app_loop_service_interface, app::loop_service>(stages());
	BOOST_TEST(run_frames(1) == 1);
}

BOOST_AUTO_TEST_SUITE_END()
