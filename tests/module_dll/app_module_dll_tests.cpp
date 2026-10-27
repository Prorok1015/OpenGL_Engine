#include <boost/test/unit_test.hpp>
#include "../test_module_env.hpp"
#include "test_game_dll_api.h"
#include <snk/application/app_module_source.h>
#include <snk/scene/scn_scene_layer.h>
#include <entt/entt.hpp>
#include <chrono>
#include <filesystem>
#include <memory>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// The game module of test_game_dll.cpp, loaded from disk with native_library_source (#220 PR-7): it fills its
// layer, its worlds run, and unloading releases the library image. Shared configurations only: the module links
// snk_engine as a game.dll does.

namespace {

const std::filesystem::path g_module_path{ SNK_TEST_GAME_DLL_PATH };

// Whether the image is still mapped (a leftover reference, or GNU_UNIQUE symbols pinning it on Linux).
bool resident(const std::filesystem::path& path)
{
#if defined(_WIN32)
	return ::GetModuleHandleW(path.c_str()) != nullptr;
#else
	void* again = ::dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
	if (again) {
		::dlclose(again);
	}
	return again != nullptr;
#endif
}

constexpr const char* DLL_LEVEL = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":["dll::move"],
	 "components":{"g":{"__type":"dll_desc","value":2}}}]})";

}

BOOST_FIXTURE_TEST_SUITE(app_module_dll_tests, test_util::module_env)

BOOST_AUTO_TEST_CASE(a_module_library_fills_its_layer_and_its_worlds_run)
{
	const std::size_t root_size = root.size();
	loader.load(std::make_unique<app::modules::native_library_source>(g_module_path), root);
	BOOST_TEST(root.size() == root_size);

	auto* layer = contexts().game_layer();
	BOOST_REQUIRE(layer != nullptr);
	BOOST_TEST(layer->descs().is_registered(dll_game::desc_type));
	BOOST_TEST(!descs->is_registered(dll_game::desc_type));
	BOOST_TEST((layer->components().find(dll_game::component_name) == entt::type_hash<dll_game::dll_component>::value()));

	BOOST_REQUIRE(contexts().load_level(put("lv.desc", DLL_LEVEL)));
	pump();
	auto* world = contexts().level_set().find("w0");
	BOOST_REQUIRE(world != nullptr);
	auto view = world->state().view<dll_game::dll_component>();
	BOOST_REQUIRE(view.size() == 1u);
	contexts().update(std::chrono::duration<float>(0.016f));
	BOOST_TEST(view.get<dll_game::dll_component>(view.front()).value == 3);
}

BOOST_AUTO_TEST_CASE(unloading_releases_the_library_and_it_loads_again)
{
	for (int round = 0; round < 2; ++round) {
		BOOST_TEST_CONTEXT("round " << round) {
			loader.load(std::make_unique<app::modules::native_library_source>(g_module_path), root);
			BOOST_REQUIRE(contexts().load_level(put("lv.desc", DLL_LEVEL)));
			pump();
			auto* world = contexts().level_set().find("w0");
			BOOST_REQUIRE(world != nullptr);
			BOOST_TEST(world->state().view<dll_game::dll_component>().size() == 1u);

			loader.unload(dll_game::module_name, root);
			BOOST_TEST(!resident(g_module_path));
			BOOST_TEST(contexts().level_set().size() == 0u);
			BOOST_TEST(!contexts().game_layer()->descs().is_registered(dll_game::desc_type));
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()
