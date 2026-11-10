#include <boost/test/unit_test.hpp>
#include "../test_module_env.hpp"
#include "test_dll_project.hpp"
#include "test_game_dll_api.h"
#include "edt_editor_camera.h"
#include "edt_input_target.h"
#include "edt_level_controller.h"
#include "edt_module_swap_gate.h"
#include "edt_play_session.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <snk/core/core_project_modules.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_prefab_spawn.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_scene_layer.h>
#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

// #600 (HOT_RELOAD_PLAY.md 11): Play keeps its world across a reload of the game module built as a real library, the
// way the editor reloads it (project_module_host with the editor's swap gate), and runs the code of the build that
// came in. CI runs it in the ASan+UBSan job: the swap frees the library the snapshot was taken from.

namespace {

const std::filesystem::path g_module_path{ SNK_TEST_GAME_DLL_PATH };
const std::filesystem::path g_v2_path{ SNK_TEST_GAME_DLL_V2_PATH };
const std::filesystem::path g_failing_path{ SNK_TEST_GAME_DLL_FAILING_PATH };
const std::filesystem::path g_bad_restore_path{ SNK_TEST_GAME_DLL_BAD_RESTORE_PATH };

constexpr const char* PLAY_LEVEL = R"json({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":["dll::move","dll::keeper"],
	 "components":{"g":{"__type":"dll_desc","value":2}}}]})json";

// #551: the module's snake, alone in the world.
constexpr const char* SNAKE_LEVEL = R"json({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":["dll::snake"]}]})json";
constexpr const char* SEGMENT = R"json({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000b1","name":"scale"}]})json";

struct dll_play_fixture : test_util::module_env
{
	dll_play_fixture()
	{
		services.ctx_manager = root.require_shared<scn::context_manager>();
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
		contexts().set_on_reload([this] { level_ctrl.resync_worlds_after_reload(); });
		contexts().set_on_game_layer_change([this](scn::context_manager::game_layer_change change) {
			gate.on_game_layer_change(change == scn::context_manager::game_layer_change::before);
		});
		// The trap is set around Play's restore only: the level, rebuilt in the new layer just before, starts its keeper.
		gate.set_play_hooks([this] { play.before_module_swap(); }, [this](const core::reload_report& report) {
			if (trap_restore) std::ofstream{ dll_game::keeper_trap_file } << "trap";
			play.after_module_swap(report);
			std::error_code ec;
			std::filesystem::remove(dll_game::keeper_trap_file, ec);
		});
		contexts().level_set().set_mode(ecs::run_mode::edit);
		auto& catalog = contexts().catalog();
		if (!catalog.find(entt::type_hash<scn::name_component>::value())) scn::register_engine_components(catalog);
		if (!catalog.find("edt.editor_camera")) catalog.add_editor_only<edt::editor_camera_tag>("edt.editor_camera");
	}

	~dll_play_fixture()
	{
		play.stop();
		loader.unload(dll_game::module_name, root);
		contexts().set_on_game_layer_change(nullptr);
		contexts().set_on_reload(nullptr);
		root.destruct<prj::project_context>();
	}

	void open_level(const char* level = PLAY_LEVEL)
	{
		const res::tag tag = put("lv.desc", level);
		state.editor_tag = tag;
		state.level_tag = tag;
		auto handle = services.resources()->require_sync<scn::level_desc>(tag);
		BOOST_REQUIRE(level_ctrl.finish_level_load(handle));
	}

	// The editor's reload: the snapshot, the swap through the gate, then the frames until the restored set runs.
	core::reload_report reload_play()
	{
		BOOST_REQUIRE(play.capture_for_reload().empty());
		const core::reload_report report = host.reload(std::string{ dll_game::module_name }, gate.hooks());
		play.end_reload();
		run_reload_polls();
		return report;
	}

	// UI frames until the restored set runs.
	void run_reload_polls()
	{
		for (int frame = 0; frame < 10 && play.reloading(); ++frame) {
			pump();
			auto polls = std::exchange(reload_polls, {});
			for (auto& poll : polls) {
				if (poll()) reload_polls.push_back(std::move(poll));
			}
		}
	}

	void run_frame() { contexts().update(std::chrono::duration<float>(0.016f)); }

	int value_in_play()
	{
		scn::context_set* set = contexts().find_set(edt::k_play_set);
		scn::world_context* world = set ? set->find("w0") : nullptr;
		if (!world) return -1;
		auto view = world->state().view<dll_game::dll_component>();
		return view.size() == 1u ? view.get<dll_game::dll_component>(view.front()).value : -1;
	}

	scn::world_context* play_world()
	{
		scn::context_set* set = contexts().find_set(edt::k_play_set);
		return set ? set->find("w0") : nullptr;
	}

	std::size_t segments_in_play()
	{
		scn::world_context* world = play_world();
		if (!world) return 0;
		std::size_t count = 0;
		for (auto&& [e, name] : world->state().view<scn::name_component>().each()) {
			if (name.name == dll_game::segment_name) ++count;
		}
		return count;
	}

	boost::json::object read_report() const
	{
		std::ifstream in(project.game / ".cache" / "hot_reload" / "last_play_snapshot.json", std::ios::binary);
		const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		return text.empty() ? boost::json::object{} : boost::json::parse(text).as_object();
	}

	test_util::dll_project project{ "dll_play_reload", root };
	edt::editor_services services;
	edt::scene_document state;
	edt::editor_events events;
	edt::selection_service selection{ events };
	edt::scene_editor scene_ed{ state, services };
	edt::world_controller world_ctrl{ state, services };
	edt::level_controller level_ctrl{ state, services, scene_ed, world_ctrl };
	std::vector<std::function<bool()>> reload_polls;
	std::vector<std::string> notices;
	int revert_requests = 0;
	bool trap_restore = false;
	edt::play_session play{ state, services, world_ctrl, selection, events, edt::play_session_options{
		.each_frame = [this](const std::string&, std::function<bool()> poll) { reload_polls.push_back(std::move(poll)); },
		.project_root = project.game, .report_full_snapshot = true,
		.notify = [this](const std::string& message) { notices.push_back(message); },
		.revert_module = [this](const std::string&) { ++revert_requests; } } };
	edt::module_swap_gate gate{ [this](bool before) {
		if (before) level_ctrl.detach_from_game_layer();
		else level_ctrl.attach_to_game_layer();
	} };
	core::project_module_host host{ loader, root, core::project_modules_options{} };
};

}

BOOST_FIXTURE_TEST_SUITE(edt_play_reload_dll_tests, dll_play_fixture)

BOOST_AUTO_TEST_CASE(play_keeps_its_world_across_a_reload_of_the_library_and_runs_the_new_build)
{
	project.build(g_module_path);
	BOOST_REQUIRE(host.load_all().loaded.size() == 1u);
	open_level();
	BOOST_REQUIRE(play.start());
	run_frame();
	BOOST_REQUIRE(value_in_play() == 3);

	project.rebuild(g_v2_path);
	const core::reload_report report = reload_play();

	BOOST_CHECK_MESSAGE(report.status == core::reload_status::reloaded, report.message);
	// Nothing keeps the first build: Play's set, the snapshot and the level hold no reference into it.
	BOOST_TEST(test_util::resident(project.copy(1)));
	BOOST_TEST(!test_util::resident(project.copy(0)));
	BOOST_TEST(!play.reloading());
	BOOST_TEST((play.state() == edt::play_state::playing));
	BOOST_TEST(play.reload_number() == 1u);
	BOOST_TEST(value_in_play() == 3); // the value of before the swap, not the level's 2
	run_frame();
	BOOST_TEST(value_in_play() == 5); // the new build moves by 2
	const boost::json::object written = read_report();
	BOOST_REQUIRE(written.contains("play"));
	BOOST_TEST(written.at("play").as_string() == "kept");
	BOOST_REQUIRE(written.at("worlds").as_array().size() == 1u);
	BOOST_TEST(written.at("worlds").as_array()[0].at("state").is_object()); // report_full_snapshot
}

// #551: the game spawns and removes a prefab with one call each; after a reload in Play the world spawns with the new
// layer's assembler, not the unloaded library's.
BOOST_AUTO_TEST_CASE(a_snake_spawns_and_despawns_with_one_call_each_and_spawns_again_after_a_reload_in_play)
{
	project.build(g_module_path);
	BOOST_REQUIRE(host.load_all().loaded.size() == 1u);
	put("dll_segment.prefab.desc", SEGMENT);
	open_level(SNAKE_LEVEL);
	BOOST_REQUIRE(play.start());
	for (std::size_t frame = 0; frame < dll_game::snake_length + 3; ++frame) run_frame();
	BOOST_TEST(segments_in_play() == dll_game::snake_length);

	project.rebuild(g_v2_path);
	const core::reload_report report = reload_play();
	BOOST_REQUIRE_MESSAGE(report.status == core::reload_status::reloaded, report.message);
	scn::world_context* world = play_world();
	BOOST_REQUIRE(world);
	const auto* spawning = world->state().ctx().find<scn::prefab_spawning>();
	BOOST_REQUIRE(spawning);
	BOOST_REQUIRE(contexts().game_layer());
	BOOST_TEST(spawning->assembler == &contexts().game_layer()->assembler());

	const entt::entity spawned = scn::spawn_prefab(world->state(), res::tag{ std::string{ dll_game::segment_tag } });
	BOOST_REQUIRE(spawned != entt::null);
	BOOST_TEST(world->state().all_of<scn::children_component>(spawned));
	scn::despawn(world->state(), spawned);
	run_frame(); // the new build's snake runs too
	BOOST_TEST(!world->state().valid(spawned));
	BOOST_TEST(segments_in_play() >= dll_game::snake_length);
}

// The new build fails to register: the previous copy is loaded again and Play goes on in it.
BOOST_AUTO_TEST_CASE(play_keeps_its_world_when_the_reload_rolls_back_to_the_previous_build)
{
	project.build(g_module_path);
	BOOST_REQUIRE(host.load_all().loaded.size() == 1u);
	open_level();
	BOOST_REQUIRE(play.start());
	run_frame();
	run_frame();
	BOOST_REQUIRE(value_in_play() == 4);

	project.rebuild(g_failing_path);
	const core::reload_report report = reload_play();

	BOOST_CHECK_MESSAGE(report.status == core::reload_status::rolled_back, report.message);
	// The previous copy is loaded again; the failing build's copy is gone with its folder.
	BOOST_TEST(test_util::resident(project.copy(0)));
	BOOST_TEST(!std::filesystem::exists(project.copy_folder(1)));
	BOOST_TEST(play.active());
	BOOST_TEST(value_in_play() == 4);
	run_frame();
	BOOST_TEST(value_in_play() == 5); // the previous build moves by 1
	const boost::json::object written = read_report();
	BOOST_REQUIRE(written.contains("module_reload"));
	BOOST_TEST(written.at("module_reload").at("status").as_string() == "rolled_back");
}

// #601 (HOT_RELOAD_PLAY.md 3.5): the new build cannot restore Play (a system of it throws from its constructor); the
// previous copy is loaded again, Play goes on in it, and the build taken back is not loaded again.
BOOST_AUTO_TEST_CASE(a_build_play_cannot_be_restored_in_is_taken_back_and_play_goes_on_in_the_previous_copy)
{
	project.build(g_module_path);
	BOOST_REQUIRE(host.load_all().loaded.size() == 1u);
	open_level();
	BOOST_REQUIRE(play.start());
	run_frame();
	BOOST_REQUIRE(value_in_play() == 3);

	project.rebuild(g_bad_restore_path);
	trap_restore = true;
	const core::reload_report report = reload_play();

	BOOST_CHECK_MESSAGE(report.status == core::reload_status::reloaded, report.message);
	BOOST_TEST(revert_requests == 1);
	BOOST_TEST(play.reloading());

	// What the editor does on the next UI frame.
	const core::reload_report reverted = host.revert(std::string{ dll_game::module_name }, gate.hooks());
	play.end_revert(reverted);
	trap_restore = false;
	run_reload_polls();

	BOOST_CHECK_MESSAGE(reverted.status == core::reload_status::reloaded, reverted.message);
	BOOST_TEST(revert_requests == 1);
	BOOST_TEST(!play.reloading());
	BOOST_TEST((play.state() == edt::play_state::playing));
	BOOST_TEST(test_util::resident(project.copy(0)));
	BOOST_TEST(!test_util::resident(project.copy(1)));
	BOOST_TEST(value_in_play() == 3);
	run_frame();
	BOOST_TEST(value_in_play() == 4); // the previous copy moves by 1
	const boost::json::object written = read_report();
	BOOST_REQUIRE(written.contains("play"));
	BOOST_TEST(written.at("play").as_string() == "reverted");
	BOOST_REQUIRE(!notices.empty());
	BOOST_TEST(notices.back().find("cannot start") != std::string::npos);

	const core::reload_report again = host.reload(std::string{ dll_game::module_name }, gate.hooks());
	BOOST_TEST((again.status == core::reload_status::refused));
	BOOST_TEST(play.active());
	BOOST_TEST(value_in_play() == 4);
}

BOOST_AUTO_TEST_SUITE_END()
