#include <boost/test/unit_test.hpp>
#include "../test_module_env.hpp"
#include "../test_temp_dir.hpp"
#include "test_dll_project.hpp"
#include "test_game_dll_api.h"
#include <snk/application/app_module_source.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/core/core_project_modules.h>
#include <snk/platform/plt_process.h>
#include <snk/desc/desc_base.hpp>
#include <snk/game/gs_module_layout.h>
#include <snk/project/prj_project.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_animation.h>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/scn_animation_job.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_scene_layer.h>
#include <snk/scene/scn_schema_components.h>
#include <entt/entt.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
const std::filesystem::path g_v2_path{ SNK_TEST_GAME_DLL_V2_PATH };
const std::filesystem::path g_failing_path{ SNK_TEST_GAME_DLL_FAILING_PATH };
const std::filesystem::path g_bad_api_path{ SNK_TEST_GAME_DLL_BAD_API_PATH };
const std::filesystem::path g_bad_layout_path{ SNK_TEST_GAME_DLL_BAD_LAYOUT_PATH };

// The editor's options (#614): the engine's layout key is checked.
core::project_modules_options checked_layouts()
{
	core::project_modules_options options;
	options.layout_key = gs::engine_module_layout_key();
	return options;
}

std::unique_ptr<app::modules::native_library_source> game_library()
{
	return std::make_unique<app::modules::native_library_source>(g_module_path, g_module_path.parent_path());
}

using test_util::project_library_name;
using test_util::resident;

constexpr const char* DLL_LEVEL = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":["dll::move"],
	 "components":{"g":{"__type":"dll_desc","value":2}}}]})";

}

namespace {

using test_util::dll_project;

// The level of DLL_LEVEL after one update: 2 plus the step of the build that runs.
int moved_value(test_util::module_env& env)
{
	auto& contexts = env.contexts();
	if (!contexts.load_level(env.put("lv.desc", DLL_LEVEL))) {
		return -1;
	}
	env.pump();
	auto* world = contexts.level_set().find("w0");
	if (!world) {
		return -1;
	}
	auto view = world->state().view<dll_game::dll_component>();
	if (view.size() != 1u) {
		return -1;
	}
	contexts.update(std::chrono::duration<float>(0.016f));
	return view.get<dll_game::dll_component>(view.front()).value;
}

}

// The loader opens the canonical path; GetModuleHandleW matches it only with backslashes.
bool test_util::resident(const std::filesystem::path& file)
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

// #558: a prefab_node copied in a module linked as the template's `game` (no Boost.JSON of its own) links, and the
// copy, with JSON the module allocated, is read and freed by the engine's code after the module is gone.
BOOST_AUTO_TEST_CASE(a_prefab_node_copied_in_a_module_outlives_the_module)
{
	scn::prefab_desc::prefab_node node;
	node.name = "root";
	node.components["mesh_node_desc"].overrides["value"] = 7;
	scn::prefab_desc::prefab_node child;
	child.name = "child";
	node.children.push_back(child);

	using copy_fn = scn::prefab_desc::prefab_node* (*)(const scn::prefab_desc::prefab_node*);
	scn::prefab_desc::prefab_node* copy = nullptr;
#if defined(_WIN32)
	HMODULE library = ::LoadLibraryW(std::filesystem::path(SNK_TEST_COPY_PREFAB_DLL_PATH).c_str());
	BOOST_REQUIRE(library != nullptr);
	const auto copy_node = reinterpret_cast<copy_fn>(::GetProcAddress(library, "snk_test_copy_prefab_node"));
	BOOST_REQUIRE(copy_node != nullptr);
	copy = copy_node(&node);
	BOOST_REQUIRE(::FreeLibrary(library));
#else
	void* library = ::dlopen(SNK_TEST_COPY_PREFAB_DLL_PATH, RTLD_NOW | RTLD_LOCAL);
	BOOST_REQUIRE(library != nullptr);
	const auto copy_node = reinterpret_cast<copy_fn>(::dlsym(library, "snk_test_copy_prefab_node"));
	BOOST_REQUIRE(copy_node != nullptr);
	copy = copy_node(&node);
	BOOST_REQUIRE(::dlclose(library) == 0);
#endif
	BOOST_REQUIRE(!resident(SNK_TEST_COPY_PREFAB_DLL_PATH));
	std::unique_ptr<scn::prefab_desc::prefab_node> owned(copy);
	BOOST_REQUIRE(owned != nullptr);
	BOOST_TEST(owned->name == "root");
	BOOST_REQUIRE(owned->children.size() == 1u);
	BOOST_TEST(owned->children[0].name == "child");
	BOOST_TEST(owned->components.at("mesh_node_desc").overrides.at("value").to_number<int>() == 7);
	const json::value& written = owned->components.at("mod").overrides.at("written_in_module");
	BOOST_TEST((written == json::array{ 1, 2, 3 }));
	// Grown and freed by the engine's code: the default storage names no resource of the unloaded module.
	owned->components.at("mod").overrides["written_in_module"].as_array().push_back(4);
	owned.reset();
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
	BOOST_TEST(!contexts().catalog().find(dll_game::component_name));

	BOOST_REQUIRE(contexts().load_level(put("lv.desc", DLL_LEVEL)));
	pump();
	auto* world = contexts().level_set().find("w0");
	BOOST_REQUIRE(world != nullptr);
	auto view = world->state().view<dll_game::dll_component>();
	BOOST_REQUIRE(view.size() == 1u);
	contexts().update(std::chrono::duration<float>(0.016f));
	BOOST_TEST(view.get<dll_game::dll_component>(view.front()).value == 3);

	// The library's catalog entry writes the component through the schema the library built.
	const scn::component_entry* entry = layer->catalog().find(dll_game::component_name);
	BOOST_REQUIRE(entry != nullptr);
	BOOST_REQUIRE(entry->saved());
	json::object saved;
	BOOST_REQUIRE(entry->write(*entry->schema, world->state(), view.front(), saved));
	BOOST_TEST(saved.at("value").to_number<int>() == 3);
}

BOOST_AUTO_TEST_CASE(module_code_switches_the_clips_of_a_model_with_play_animation)
{
	loader.load(game_library(), root);
	BOOST_REQUIRE(contexts().load_level(put("anim_lv.desc", R"({"__type":"level_desc","name":"lv","worlds":[
		{"__type":"world_desc","name":"w0","systems":["dll::animate"]}]})")));
	pump();
	auto* world = contexts().level_set().find("w0");
	BOOST_REQUIRE(world != nullptr);
	auto& reg = world->state();

	const entt::entity model = reg.create();
	scn::animation_collection_component collection;
	collection.clips = load<scn::animation_collection_desc>(put("dll_clips.desc", R"({"__type":"animation_collection_desc","clips":[
		{"__type":"animation_clip_desc","name":"idle","duration":20.0,"ticks_per_second":10.0},
		{"__type":"animation_clip_desc","name":"walk","duration":10.0,"ticks_per_second":20.0},
		{"__type":"animation_clip_desc","name":"jump","duration":6.0,"ticks_per_second":12.0}]})"))->clips;
	reg.emplace<scn::animation_collection_component>(model, std::move(collection));
	auto& controller = reg.emplace<scn::animation_controller_component>(model);
	controller.current_animation = "idle";
	controller.playing = true;

	contexts().update(std::chrono::duration<float>(0.016f));
	BOOST_TEST(controller.current_animation == "walk");
	BOOST_TEST(controller.duration == 10.0f);
	BOOST_TEST(controller.loop);

	contexts().update(std::chrono::duration<float>(0.016f));
	BOOST_TEST(controller.current_animation == "jump");
	BOOST_TEST(controller.duration == 6.0f);
	BOOST_TEST(controller.ticks_per_second == 12.0f);
	BOOST_TEST(!controller.loop);
	BOOST_TEST(controller.current_time == 0.0f);
}

BOOST_AUTO_TEST_CASE(module_code_hears_the_end_of_a_clip_and_plays_idle_again)
{
	scn::init_animation_system(sf);
	loader.load(game_library(), root);
	BOOST_REQUIRE(contexts().load_level(put("anim_end_lv.desc", R"({"__type":"level_desc","name":"lv","worlds":[
		{"__type":"world_desc","name":"w0","systems":["scn::animation_controller_update","dll::back_to_idle"]}]})")));
	pump();
	auto* world = contexts().level_set().find("w0");
	BOOST_REQUIRE(world != nullptr);
	contexts().level_set().set_mode(ecs::run_mode::play);
	auto& reg = world->state();

	const entt::entity model = reg.create();
	scn::animation_collection_component collection;
	collection.clips = load<scn::animation_collection_desc>(put("dll_end_clips.desc", R"({"__type":"animation_collection_desc","clips":[
		{"__type":"animation_clip_desc","name":"idle","duration":20.0,"ticks_per_second":10.0},
		{"__type":"animation_clip_desc","name":"jump","duration":6.0,"ticks_per_second":12.0}]})"))->clips;
	reg.emplace<scn::animation_collection_component>(model, std::move(collection));
	reg.emplace<scn::animation_controller_component>(model);
	BOOST_REQUIRE(scn::play_animation(reg, model, "jump", false)); // 0.5 s

	// Under max_frame_dt (0.25 s): 0.2 + 0.2 stays inside the clip.
	contexts().update(std::chrono::duration<float>(0.2f));
	contexts().update(std::chrono::duration<float>(0.2f));
	BOOST_TEST(reg.get<scn::animation_controller_component>(model).current_animation == "jump");
	// The clip ends in this frame; the module's system after the delivery point hears it in the same frame.
	contexts().update(std::chrono::duration<float>(0.2f));
	const auto& controller = reg.get<scn::animation_controller_component>(model);
	BOOST_TEST(controller.current_animation == "idle");
	BOOST_TEST(controller.playing);
	BOOST_TEST(controller.loop);
	BOOST_TEST(controller.current_time == 0.0f);
}

// #606: a component the library describes by a schema alone lives in the worlds as a blob of the engine's code, and
// goes with its pools when the library is unloaded.
BOOST_AUTO_TEST_CASE(a_component_the_library_registers_by_its_schema_spawns_and_goes_with_it)
{
	loader.load(game_library(), root);
	auto* layer = contexts().game_layer();
	BOOST_REQUIRE(layer != nullptr);
	const scn::component_entry* entry = layer->catalog().find(dll_game::schema_component_name);
	BOOST_REQUIRE(entry != nullptr);
	BOOST_TEST(scn::is_schema_component(*entry));

	BOOST_REQUIRE(contexts().load_level(put("lv.desc", R"json({"__type":"level_desc","name":"lv","worlds":[
		{"__type":"world_desc","name":"w0",
		 "components":{"h":{"__type":"dll.schema_health","hp":7}}}]})json")));
	pump();
	auto* world = contexts().level_set().find("w0");
	BOOST_REQUIRE(world != nullptr);
	const entt::id_type id = scn::schema_component_id(dll_game::schema_component_name);
	const auto* pool = std::as_const(world->state()).storage<desc::dyn_blob>(id);
	BOOST_REQUIRE(pool != nullptr);
	BOOST_REQUIRE(pool->size() == 1u);
	json::object saved;
	const entt::entity holder = *static_cast<const entt::sparse_set&>(*pool).begin();
	BOOST_REQUIRE(entry->write(*entry->schema, world->state(), holder, saved));
	BOOST_TEST(saved.at("hp").to_number<double>() == 7.0);
	// No value in the desc: no entity (left out or null), not entity 0.
	BOOST_TEST((!saved.contains("target") || saved.at("target").is_null()));

	loader.unload(dll_game::module_name, root);
	BOOST_TEST(!resident(g_module_path));
	auto* after = contexts().game_layer();
	BOOST_REQUIRE(after != nullptr);
	BOOST_TEST(!after->catalog().find(dll_game::schema_component_name));
	BOOST_TEST(!after->descs().is_registered(dll_game::schema_component_name));
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
			BOOST_TEST(!contexts().game_layer()->catalog().find(dll_game::component_name));
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

BOOST_AUTO_TEST_CASE(a_project_module_runs_from_a_copy_and_its_build_output_stays_free)
{
	namespace fs = std::filesystem;
	test_util::temp_dir dir("dll_project_module");
	fs::create_directories(dir.path() / "game" / "build" / "bin");
	const fs::path game = fs::canonical(dir.path() / "game");
	const fs::path built = game / "build" / "bin" / project_library_name(dll_game::module_name);
	fs::copy_file(g_module_path, built);
	BOOST_REQUIRE_MESSAGE(fs::is_regular_file(built), "not at the lookup path: " << built.string());

	prj::project_context project;
	project.root = game;
	project.data.name = "Game";
	project.data.modules.emplace_back();
	project.data.modules.back().name = std::string{ dll_game::module_name };
	root.construct<prj::project_context>(std::move(project));

	const auto report = core::load_project_modules(loader, root, core::project_modules_options{});
	BOOST_TEST(report.loaded == std::vector<std::string>{ std::string{ dll_game::module_name } });
	auto* layer = contexts().game_layer();
	BOOST_REQUIRE(layer != nullptr);
	BOOST_TEST(layer->descs().is_registered(dll_game::desc_type));

	const fs::path copy = game / ".cache" / "modules" / std::format("{}-0", platform::current_process_id())
		/ project_library_name(dll_game::module_name);
	BOOST_TEST(resident(copy));
	BOOST_TEST(!resident(built));
	{
		// What a rebuild does while the editor runs: Windows refuses it for a loaded image.
		std::ofstream rebuilt(built, std::ios::binary | std::ios::trunc);
		rebuilt << "rebuilt";
		BOOST_TEST(rebuilt.good());
	}

	loader.unload(dll_game::module_name, root);
	BOOST_TEST(!resident(copy));
	root.destruct<prj::project_context>();
}

BOOST_AUTO_TEST_CASE(a_project_library_that_is_another_module_is_refused_and_its_copy_removed)
{
	namespace fs = std::filesystem;
	test_util::temp_dir dir("dll_project_wrong_module");
	fs::create_directories(dir.path() / "game" / "build" / "bin");
	const fs::path game = fs::canonical(dir.path() / "game");
	// The library of test_game_dll under the file name the manifest's "other_game" looks for.
	const fs::path built = game / "build" / "bin" / project_library_name("other_game");
	fs::copy_file(g_module_path, built);
	BOOST_REQUIRE_MESSAGE(fs::is_regular_file(built), "not at the lookup path: " << built.string());

	prj::project_context project;
	project.root = game;
	project.data.name = "Game";
	project.data.modules.emplace_back();
	project.data.modules.back().name = "other_game";
	root.construct<prj::project_context>(std::move(project));

	const std::size_t root_size = root.size();
	const auto report = core::load_project_modules(loader, root, core::project_modules_options{});
	BOOST_TEST(report.refused == std::vector<std::string>{ "other_game" });
	BOOST_TEST(!loader.state(dll_game::module_name).has_value());
	BOOST_TEST(root.size() == root_size);
	const fs::path copy = game / ".cache" / "modules" / std::format("{}-0", platform::current_process_id());
	// Windows does not delete a mapped image: gone means released too.
	BOOST_TEST(!fs::exists(copy));
	root.destruct<prj::project_context>();
}

BOOST_AUTO_TEST_CASE(a_project_module_reloads_from_a_new_build_and_keeps_the_previous_copy)
{
	namespace fs = std::filesystem;
	dll_project project("dll_reload", root);
	project.build(g_module_path);
	core::project_module_host host(loader, root, core::project_modules_options{});
	BOOST_REQUIRE(host.load_all().loaded.size() == 1u);
	BOOST_TEST(moved_value(*this) == 3);
	BOOST_TEST(host.is_current(std::string{ dll_game::module_name }));

	project.build(g_v2_path);
	// A copy within the file system's time resolution keeps the old time: the new build must differ.
	fs::last_write_time(project.built, fs::last_write_time(project.built) + std::chrono::hours(1));
	BOOST_TEST(!host.is_current(std::string{ dll_game::module_name }));
	// What goes with the module into its copy: its symbols and the libraries next to it.
	const fs::path symbols = fs::path{ project_library_name(dll_game::module_name) }.replace_extension(".pdb");
	const std::string sibling = project_library_name("helper");
	{
		std::ofstream(project.built.parent_path() / symbols) << "symbols";
		std::ofstream(project.built.parent_path() / sibling) << "a library next to the module";
	}
	const auto report = host.reload(std::string{ dll_game::module_name });

	BOOST_CHECK_MESSAGE(report.status == core::reload_status::reloaded, report.message);
	BOOST_TEST(host.is_current(std::string{ dll_game::module_name }));
	BOOST_TEST(resident(project.copy(1)));
	BOOST_TEST(!resident(project.copy(0)));
	BOOST_TEST(fs::is_directory(project.copy_folder(0))); // the previous copy stays for a rollback
#if defined(_WIN32)
	BOOST_TEST(fs::exists(project.copy_folder(1) / symbols));
#endif
	BOOST_TEST(fs::exists(project.copy_folder(1) / sibling));
	BOOST_TEST(moved_value(*this) == 4);
	{
		std::ofstream rebuilt(project.built, std::ios::binary | std::ios::trunc);
		rebuilt << "rebuilt";
		BOOST_TEST(rebuilt.good());
	}
	loader.unload(dll_game::module_name, root);
	root.destruct<prj::project_context>();
}

// #601 (HOT_RELOAD_PLAY.md 3.5): the previous copy runs again, the build taken back is not loaded again until a new one.
BOOST_AUTO_TEST_CASE(a_revert_runs_the_previous_copy_again_and_the_build_taken_back_waits_for_a_new_build)
{
	namespace fs = std::filesystem;
	dll_project project("dll_revert", root);
	project.build(g_module_path);
	core::project_module_host host(loader, root, core::project_modules_options{});
	BOOST_REQUIRE(host.load_all().loaded.size() == 1u);
	project.rebuild(g_v2_path);
	const auto reloaded = host.reload(std::string{ dll_game::module_name });
	BOOST_REQUIRE_MESSAGE(reloaded.status == core::reload_status::reloaded, reloaded.message);
	BOOST_REQUIRE(moved_value(*this) == 4);

	const auto report = host.revert(std::string{ dll_game::module_name });

	BOOST_CHECK_MESSAGE(report.status == core::reload_status::reloaded, report.message);
	BOOST_TEST(resident(project.copy(0)));
	BOOST_TEST(!resident(project.copy(1)));
	BOOST_TEST(fs::is_directory(project.copy_folder(1))); // the build taken back is the previous copy now
	BOOST_TEST(moved_value(*this) == 3);

	// Live mode reloads after every build: the same output is refused, so no reload/revert loop starts.
	const auto same = host.reload(std::string{ dll_game::module_name });
	BOOST_TEST((same.status == core::reload_status::refused));
	BOOST_CHECK_MESSAGE(same.message.find("taken back") != std::string::npos, same.message);
	BOOST_TEST(moved_value(*this) == 3);

	project.rebuild(g_v2_path);
	// A copy keeps the source's time on Windows: the new build must not look like the one taken back.
	fs::last_write_time(project.built, fs::last_write_time(project.built) + std::chrono::hours(1));
	const auto rebuilt = host.reload(std::string{ dll_game::module_name });
	BOOST_CHECK_MESSAGE(rebuilt.status == core::reload_status::reloaded, rebuilt.message);
	BOOST_TEST(moved_value(*this) == 4);

	// A person asks for the build taken back again (Build > Reload Game Module): it is loaded.
	BOOST_REQUIRE((host.revert(std::string{ dll_game::module_name }).status == core::reload_status::reloaded));
	BOOST_TEST((host.reload(std::string{ dll_game::module_name }).status == core::reload_status::refused));
	const auto retried = host.reload(std::string{ dll_game::module_name }, {}, true);
	BOOST_CHECK_MESSAGE(retried.status == core::reload_status::reloaded, retried.message);
	BOOST_TEST(moved_value(*this) == 4);
	loader.unload(dll_game::module_name, root);
	root.destruct<prj::project_context>();
}

// A previous copy that no longer loads leaves no module: no copy counts as current, so the same build loads again.
BOOST_AUTO_TEST_CASE(a_revert_whose_previous_copy_fails_leaves_no_current_copy)
{
	dll_project project("dll_revert_lost", root);
	project.build(g_module_path);
	core::project_module_host host(loader, root, core::project_modules_options{});
	BOOST_REQUIRE(host.load_all().loaded.size() == 1u);
	project.rebuild(g_v2_path);
	BOOST_REQUIRE((host.reload(std::string{ dll_game::module_name }).status == core::reload_status::reloaded));
	BOOST_REQUIRE(host.is_current(std::string{ dll_game::module_name }));
	// The previous copy is not mapped any more: it becomes a build that passes the check and fails to register.
	std::filesystem::copy_file(g_failing_path, project.copy(0), std::filesystem::copy_options::overwrite_existing);

	const auto report = host.revert(std::string{ dll_game::module_name });

	BOOST_CHECK_MESSAGE(report.status == core::reload_status::lost, report.message);
	BOOST_TEST((loader.state(dll_game::module_name) != app::modules::module_state::initialized));
	BOOST_TEST(!host.is_current(std::string{ dll_game::module_name }));
	BOOST_TEST(std::filesystem::exists(project.copy(0))); // kept for a look
	const auto again = host.reload(std::string{ dll_game::module_name });
	BOOST_CHECK_MESSAGE(again.status == core::reload_status::reloaded, again.message);
	BOOST_TEST(host.is_current(std::string{ dll_game::module_name }));
	BOOST_TEST(moved_value(*this) == 4);
	loader.unload(dll_game::module_name, root);
	root.destruct<prj::project_context>();
}

BOOST_AUTO_TEST_CASE(a_build_of_another_api_is_refused_and_the_running_module_stays)
{
	dll_project project("dll_reload_bad_api", root);
	project.build(g_module_path);
	core::project_module_host host(loader, root, core::project_modules_options{});
	BOOST_REQUIRE(host.load_all().loaded.size() == 1u);

	project.build(g_bad_api_path);
	const auto report = host.reload(std::string{ dll_game::module_name });

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_CHECK_MESSAGE(report.message.find("api") != std::string::npos, report.message);
	BOOST_TEST(resident(project.copy(0)));
	BOOST_TEST(!std::filesystem::exists(project.copy_folder(1)));
	BOOST_TEST(moved_value(*this) == 3);
	loader.unload(dll_game::module_name, root);
	root.destruct<prj::project_context>();
}

// #614: a library built with the engine's headers reports the engine's layout key, and the host lets it in.
BOOST_AUTO_TEST_CASE(a_library_built_with_the_engine_headers_reports_its_layout_key_and_loads)
{
	auto source = game_library();
	BOOST_REQUIRE(source->open().empty());
	BOOST_REQUIRE(source->entry().layout_key != nullptr);
	BOOST_TEST(source->entry().layout_key() == gs::engine_module_layout_key());
	source->close();

	dll_project project("dll_layout_same", root);
	project.build(g_module_path);
	core::project_module_host host(loader, root, checked_layouts());
	BOOST_TEST(host.load_all().loaded.size() == 1u);
	BOOST_TEST(moved_value(*this) == 3);
	loader.unload(dll_game::module_name, root);
	root.destruct<prj::project_context>();
}

// A library built against other layouts of the engine's types is not created: refused at load, its copy removed.
BOOST_AUTO_TEST_CASE(a_library_built_against_other_layouts_is_not_loaded)
{
	dll_project project("dll_layout_load", root);
	project.build(g_bad_layout_path);
	core::project_module_host host(loader, root, checked_layouts());
	const auto report = host.load_all();

	BOOST_TEST(report.loaded.empty());
	BOOST_TEST(report.refused == std::vector<std::string>{ std::string{ dll_game::module_name } });
	BOOST_TEST(!loader.state(dll_game::module_name).has_value());
	BOOST_TEST(!std::filesystem::exists(project.copy_folder(0)));
	root.destruct<prj::project_context>();
}

// A reload to such a build is refused before the running module goes: it stays and keeps running.
BOOST_AUTO_TEST_CASE(a_reload_to_a_build_against_other_layouts_is_refused_and_the_running_module_stays)
{
	dll_project project("dll_layout_reload", root);
	project.build(g_module_path);
	core::project_module_host host(loader, root, checked_layouts());
	BOOST_REQUIRE(host.load_all().loaded.size() == 1u);

	project.rebuild(g_bad_layout_path);
	int hooks_called = 0;
	core::reload_hooks hooks;
	hooks.before_swap = [&] { ++hooks_called; };
	hooks.after_swap = [&](const core::reload_report&) { ++hooks_called; };
	const auto report = host.reload(std::string{ dll_game::module_name }, hooks);

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_CHECK_MESSAGE(report.message.find("rebuild the project") != std::string::npos, report.message);
	BOOST_TEST(hooks_called == 0); // refused before the running module was touched
	BOOST_TEST(resident(project.copy(0)));
	BOOST_TEST(!std::filesystem::exists(project.copy_folder(1)));
	BOOST_TEST(moved_value(*this) == 3);
	loader.unload(dll_game::module_name, root);
	root.destruct<prj::project_context>();
}

// A build that is refused for its api and has another layout too is reported for the api: the version check comes first.
BOOST_AUTO_TEST_CASE(a_build_of_another_api_is_reported_for_its_api_even_with_another_layout)
{
	dll_project project("dll_layout_api", root);
	project.build(g_module_path);
	core::project_module_host host(loader, root, checked_layouts());
	BOOST_REQUIRE(host.load_all().loaded.size() == 1u);

	project.rebuild(g_bad_api_path);
	const auto report = host.reload(std::string{ dll_game::module_name });

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_CHECK_MESSAGE(report.message.find("api major") != std::string::npos, report.message);
	BOOST_CHECK_MESSAGE(report.message.find("another layout") == std::string::npos, report.message);
	BOOST_TEST(resident(project.copy(0)));
	loader.unload(dll_game::module_name, root);
	root.destruct<prj::project_context>();
}

BOOST_AUTO_TEST_CASE(a_build_that_fails_to_register_rolls_back_to_the_previous_copy)
{
	dll_project project("dll_reload_failing", root);
	project.build(g_module_path);
	core::project_module_host host(loader, root, core::project_modules_options{});
	BOOST_REQUIRE(host.load_all().loaded.size() == 1u);

	project.build(g_failing_path);
	const auto report = host.reload(std::string{ dll_game::module_name });

	BOOST_CHECK_MESSAGE(report.status == core::reload_status::rolled_back, report.message);
	BOOST_CHECK_MESSAGE(report.message.find("fails to register") != std::string::npos, report.message);
	// The previous copy runs again: the failing build is still the newer one in build/bin.
	std::filesystem::last_write_time(project.built, std::filesystem::last_write_time(project.built) + std::chrono::hours(1));
	BOOST_TEST(!host.is_current(std::string{ dll_game::module_name }));
	BOOST_TEST(resident(project.copy(0)));
	BOOST_TEST(!std::filesystem::exists(project.copy_folder(1)));
	BOOST_TEST(moved_value(*this) == 3);
	loader.unload(dll_game::module_name, root);
	root.destruct<prj::project_context>();
}

BOOST_AUTO_TEST_SUITE_END()
