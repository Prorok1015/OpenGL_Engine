#include <boost/test/unit_test.hpp>
#include "../test_module_env.hpp"
#include "test_game_dll_api.h"
#include <snk/application/app_module_source.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/desc/desc_base.hpp>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/scene/scn_scene_layer.h>
#include <entt/entt.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

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

std::unique_ptr<app::modules::native_library_source> game_library()
{
	return std::make_unique<app::modules::native_library_source>(g_module_path, g_module_path.parent_path());
}

// Whether the image is still mapped (a leftover reference, or GNU_UNIQUE symbols pinning it on Linux).
// The loader opens the canonical path; GetModuleHandleW matches it only with backslashes.
bool resident(const std::filesystem::path& file)
{
	std::error_code ec;
	const std::filesystem::path path = std::filesystem::canonical(file, ec);
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
	loader.load(game_library(), root);
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
			loader.load(game_library(), root);
			BOOST_TEST(resident(g_module_path));
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

// #406: the library's callbacks on a resource still loading (a block of its layer) and on its task, then the library
// goes before either settles. What the engine holds of them afterwards is its own code: settling and draining after
// the library is unmapped runs nothing of it. Shared configurations only (Linux GCC Shared, Windows MSVC Shared
// Release), without a sanitizer; the static tests of the same paths run under ASan/TSan.
BOOST_AUTO_TEST_CASE(callbacks_of_an_unloaded_library_leave_nothing_of_it_in_the_engine)
{
	const auto waited = put("dll_waited.desc", R"({"__type":"dll_desc","value":1})");
	BOOST_REQUIRE(waited.view() == dll_game::waited_tag);
	loader.load(game_library(), root);
	BOOST_REQUIRE(resident(g_module_path));
	auto* layer = contexts().game_layer();
	BOOST_REQUIRE(layer != nullptr);
	// The task's continuation and the callback on the resource: both registered, both still waiting.
	BOOST_REQUIRE(ds::detail::held_count(layer->tasks()) == 2u);
	{
		const auto pending = layer->resources().require<desc::desc_base>(waited);
		BOOST_REQUIRE(!pending.is_ready());
		BOOST_REQUIRE(!pending.has_error());
	}
	loader.unload(dll_game::module_name, root);
	BOOST_TEST(!resident(g_module_path));
	pump();
	for (int i = 0; i < 4; ++i) {
		tasks.drain_main();
	}
}

// The resource the library waits for fails: its callback is dropped without a call (drop_held), the task's goes as
// usual, and the library unloads with nothing of it left held.
BOOST_AUTO_TEST_CASE(a_failed_resource_drops_the_library_callback_without_a_call)
{
	const auto failing = put("dll_failing.desc", R"({"__type":"no_such_desc_type"})");
	BOOST_REQUIRE(failing.view() == dll_game::failing_tag);
	loader.load(game_library(), root);
	auto* layer = contexts().game_layer();
	BOOST_REQUIRE(layer != nullptr);
	BOOST_REQUIRE(ds::detail::held_count(layer->tasks()) == 2u);
	{
		// The block the library waits on: a failed block is not kept once settled, so it is held from here (the
		// library's own request found it in the cache, it is the same block).
		const auto failed = layer->resources().require<desc::desc_base>(failing);
		for (int i = 0; i < 100 && !failed.is_ready() && !failed.has_error(); ++i) {
			pump();
		}
		BOOST_REQUIRE(failed.has_error());
	}
	pump(); // the task's continuation
	BOOST_TEST(ds::detail::held_count(layer->tasks()) == 0u);
	loader.unload(dll_game::module_name, root);
	BOOST_TEST(!resident(g_module_path));
	pump();
}

// #416: the library requests resources of the BASE (texts: the base's adapter) and waits for them; the blocks sit in
// the base's cache. Unloaded and unmapped before they settle, then they settle (one is ready, one fails) and the base
// drops them: the blocks are the engine's, so nothing of the library runs or is freed through its code.
BOOST_AUTO_TEST_CASE(base_resources_a_library_requested_settle_and_go_after_it_is_gone)
{
	const auto base = put("dll_base.txt", "base text");
	BOOST_REQUIRE(base.view() == dll_game::base_tag);
	const res::tag missing{ std::string{ dll_game::base_missing_tag } };
	const std::size_t base_live = rs->live_resources();
	loader.load(game_library(), root);
	auto* layer = contexts().game_layer();
	BOOST_REQUIRE(layer != nullptr);
	BOOST_REQUIRE(ds::detail::held_count(layer->tasks()) == 3u); // its task, the two texts
	// The library's two requests made their blocks in the BASE's cache, before the host asks for anything.
	BOOST_REQUIRE(rs->live_resources() == base_live + 2u);
	BOOST_REQUIRE(layer->resources().live_resources() == 0u);
	{
		// Both in the base's cache, both still loading.
		const auto pending = rs->require<res::text_resource>(base);
		const auto failing = rs->require<res::text_resource>(missing);
		BOOST_REQUIRE(!pending.is_ready() && !pending.has_error());
		BOOST_REQUIRE(!failing.is_ready() && !failing.has_error());
	}
	loader.unload(dll_game::module_name, root);
	BOOST_REQUIRE(!resident(g_module_path));

	pump();
	for (int i = 0; i < 4; ++i) {
		tasks.drain_main();
	}
	// The base lets go of the blocks: the last references go through their control blocks.
	rs->erase(base);
	rs->signal_changed(missing);
	pump();
	BOOST_TEST(rs->require_sync<res::text_resource>(base).has_error()); // erased: a fresh, engine-made block
}

BOOST_AUTO_TEST_SUITE_END()
