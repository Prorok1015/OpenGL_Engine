#include <boost/test/unit_test.hpp>
#include "../test_module_env.hpp"
#include "../test_temp_dir.hpp"
#include "test_game_dll_api.h"
#include "edt_data_document.h"
#include <snk/application/app_module_source.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/scn_scene_layer.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// #800 (S56-11, F33): a game config of the module is edited in a data document and the game reads the new value after the
// save, with no rebuild of the library. The module's system asks for the config every frame (test_game_dll.cpp).

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

const fs::path g_module_path{ SNK_TEST_GAME_DLL_PATH };

constexpr const char* CONFIG_LEVEL = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":["dll::read_config"]}]})";

struct game_config_fixture : test_util::module_env
{
	game_config_fixture()
	{
		fs::create_directories(res_root / "config");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		services.ctx_manager = root.require_shared<scn::context_manager>();
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		loader.load(std::make_unique<app::modules::native_library_source>(g_module_path, g_module_path.parent_path()), root);
		BOOST_REQUIRE(contexts().game_layer() != nullptr);
	}

	~game_config_fixture() { rs->unregistrate_resolver(res::tag::default_protocol()); }

	void write_config(double speed) const
	{
		std::ofstream out(res_root / "config" / "game.desc", std::ios::binary);
		out << R"({"__type":"dll.config","speed":)" << speed << "}";
	}

	void start_game()
	{
		{
			std::ofstream out(res_root / "lv.desc", std::ios::binary);
			out << CONFIG_LEVEL;
		}
		BOOST_REQUIRE(contexts().load_level(res::tag{ "res://lv.desc" }));
		pump();
	}

	void run_frames(int count = 4)
	{
		for (int frame = 0; frame < count; ++frame) {
			contexts().update(std::chrono::duration<float>(0.016f));
			pump();
		}
	}

	float speed_the_game_has()
	{
		scn::world_context* world = contexts().level_set().find("w0");
		if (!world) return -2.0f;
		auto view = world->state().view<dll_game::dll_config_seen>();
		return view.size() == 1u ? view.get<dll_game::dll_config_seen>(view.front()).speed : -2.0f;
	}

	edt::desc_file_layout layout() const
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return res_root; };
		out.layers = [this] { return std::vector<fs::path>{ res_root }; };
		out.open_level_files = [] { return std::vector<std::string>{}; };
		return out;
	}

	test_util::temp_dir dir{ "edt_game_config_dll" };
	const fs::path res_root = dir.path() / "res";
	edt::editor_services services;
};

}

BOOST_FIXTURE_TEST_SUITE(edt_game_config_dll_tests, game_config_fixture)

BOOST_AUTO_TEST_CASE(a_config_type_of_the_module_is_a_document_the_editor_sees)
{
	const std::vector<std::string> documents = services.descs()->document_types();
	BOOST_TEST(std::ranges::count(documents, std::string{ dll_game::document_type }) == 1);
	BOOST_TEST(!std::ranges::count(descs->document_types(), std::string{ dll_game::document_type }));
}

BOOST_AUTO_TEST_CASE(a_save_of_the_config_document_reaches_the_game_and_a_dropped_edit_does_not)
{
	write_config(4.0);
	start_game();
	run_frames();
	BOOST_TEST(speed_the_game_has() == 4.0f);

	edt::data_document document(services, layout());
	const edt::open_result opened = document.open(res::tag{ std::string{ dll_game::config_tag } });
	BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);
	BOOST_TEST(document.type() == dll_game::document_type);

	BOOST_REQUIRE(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	BOOST_REQUIRE(document.is_dirty());
	run_frames();
	BOOST_TEST(speed_the_game_has() == 4.0f); // an edit alone does not reach the game (R30)

	BOOST_REQUIRE(document.reload().opened);
	BOOST_TEST(!document.is_dirty());
	run_frames();
	BOOST_TEST(speed_the_game_has() == 4.0f); // the edit was dropped: the game still has the old value

	BOOST_REQUIRE(document.edit([](json::value& shown) {
		shown.as_object()["speed"] = 9.0;
		return true;
	}));
	const edt::save_result saved = document.save();
	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	run_frames();
	BOOST_TEST(speed_the_game_has() == 9.0f);
}

BOOST_AUTO_TEST_SUITE_END()
