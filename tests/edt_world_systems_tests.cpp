#include "edt_prefab_preview_kinds.hpp"
#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_scene_commands.h"
#include "edt_scene_document.h"
#include "edt_world_problems.h"
#include "edt_document_cast.h"
#include <snk/command/cmd_registry.h>
#include <snk/ecs/ecs_system.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_world_context.h>
#include <boost/json.hpp>
#include <optional>
#include <string>
#include <vector>

// The systems of a world (#549): world.systems.list/add/remove/move are scene edits (one commit each, undoable, the
// level dirty), the list is saved in worlds[].systems, and a name that is not registered is a problem of the level.

namespace {

namespace json = boost::json;

struct counter { int value = 0; };
void spin(counter& c) { ++c.value; }
void fade(counter& c) { --c.value; }

constexpr const char* WORLD = R"({ "__type": "world_desc", "__version": 4, "name": "w0", "systems": ["game::spin", "gone::system"], "children": [] })";
constexpr const char* WORLD_1 = R"({ "__type": "world_desc", "__version": 4, "name": "w1", "systems": [], "children": [] })";

struct systems_fixture : test_util::scene_env
{
	systems_fixture()
	{
		sfactory.register_system<&spin>("game::spin", { .phase = ecs::phase::update });
		sfactory.register_system<&fade>("game::fade", { .phase = ecs::phase::update, .run_in_edit = true });
		for (const auto& [name, text] : { std::pair{ "w0", WORLD }, std::pair{ "w1", WORLD_1 } }) {
			auto world = load<scn::world_desc>(put(std::string(name) + ".desc", text));
			BOOST_REQUIRE(world);
			scene.world_descs.push_back(*world);
			scene.world_names.emplace_back(name);
			scene.worlds_systems_list.push_back(world->get_systems());
		}
		scene.active_world_idx = 0;
		scene.reset_history();
		scene.mark_clean();
		scene.set_pusher([this] { ++pushes; return true; });
		scene.set_world_reloader([this](std::size_t world) { reloaded.push_back(world); return true; });
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sfactory;
		registrations = edt::register_scene_commands(commands, { documents, selection, services, [] { return false; } });
	}

	cmd::result call(const std::string& id, const std::string& args, cmd::origin from = cmd::origin::agent("s1"))
	{
		return commands.invoke(id, json::parse(args), from);
	}

	std::vector<std::string> systems_of(std::size_t world = 0) const { return scene.world_descs[world].get_systems(); }

	static std::vector<std::string> names_in(const cmd::result& result)
	{
		std::vector<std::string> out;
		for (const json::value& name : result.value.at("systems").as_array()) out.emplace_back(name.as_string());
		return out;
	}

	ecs::system_factory sfactory;
	edt::editor_services services;
	edt::scene_document scene;
	edt::editor_events events;
	edt::document_manager documents{ scene, events };
	test_util::prefab_preview_kinds prefab_kinds{ documents, services };
	edt::selection_service selection{ events };
	cmd::command_registry commands;
	int pushes = 0;
	std::vector<std::size_t> reloaded;
	std::vector<edt::registration> registrations;
};

using names = std::vector<std::string>;

}

BOOST_FIXTURE_TEST_SUITE(EdtWorldSystemsTests, systems_fixture)

BOOST_AUTO_TEST_CASE(TheCommands_AreSceneEditsForAnAgent_AndTheListIsARead)
{
	for (const char* id : { "world.systems.add", "world.systems.remove", "world.systems.move" }) {
		BOOST_TEST_CONTEXT(id) {
			const cmd::command_desc* command = commands.find(id);
			BOOST_REQUIRE(command != nullptr);
			BOOST_TEST(command->mutates);
			BOOST_TEST((command->category == cmd::agent_category::scene_edit));
		}
	}
	const cmd::command_desc* list = commands.find("world.systems.list");
	BOOST_REQUIRE(list != nullptr);
	BOOST_TEST(!list->mutates);
	BOOST_TEST((list->category == cmd::agent_category::read));
}

BOOST_AUTO_TEST_CASE(List_ShowsTheOrderWhetherEachIsRegisteredAndWhatCouldBeAdded)
{
	const auto listed = call("world.systems.list", "{}");
	BOOST_REQUIRE_MESSAGE(listed.ok, listed.error);
	const json::array& systems = listed.value.at("systems").as_array();
	BOOST_REQUIRE_EQUAL(systems.size(), 2u);
	BOOST_TEST(std::string(systems[0].at("name").as_string()) == std::string("game::spin"));
	BOOST_TEST(systems[0].at("known").as_bool());
	BOOST_TEST(std::string(systems[0].at("phase").as_string()) == std::string("update"));
	BOOST_TEST(std::string(systems[1].at("name").as_string()) == std::string("gone::system"));
	BOOST_TEST(!systems[1].at("known").as_bool());
	BOOST_REQUIRE_EQUAL(listed.value.at("available").as_array().size(), 1u);
	BOOST_TEST(std::string(listed.value.at("available").as_array()[0].at("name").as_string()) == std::string("game::fade"));
	BOOST_TEST(listed.value.at("available").as_array()[0].at("run_in_edit").as_bool());

	const auto other = call("world.systems.list", R"({"world":"w1"})");
	BOOST_REQUIRE(other.ok);
	BOOST_TEST(other.value.at("systems").as_array().size() == 0u);
	BOOST_TEST(other.value.at("available").as_array().size() == 2u);
	BOOST_TEST(call("world.systems.list", R"({"world":"nowhere"})").code == cmd::codes::not_found);
}

BOOST_AUTO_TEST_CASE(Add_PutsARegisteredSystemInTheListAtAPlace)
{
	const auto last = call("world.systems.add", R"({"system":"game::fade"})");
	BOOST_REQUIRE_MESSAGE(last.ok, last.error);
	BOOST_TEST((names_in(last) == names{ "game::spin", "gone::system", "game::fade" }));
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system", "game::fade" }));
	BOOST_TEST((scene.worlds_systems_list[0] == systems_of()));

	const auto first = call("world.systems.add", R"({"world":"w1","system":"game::spin","index":0})");
	BOOST_REQUIRE(first.ok);
	BOOST_TEST((systems_of(1) == names{ "game::spin" }));
	const auto before = call("world.systems.add", R"({"world":"w1","system":"game::fade","index":0})");
	BOOST_REQUIRE(before.ok);
	BOOST_TEST((systems_of(1) == names{ "game::fade", "game::spin" }));
}

BOOST_AUTO_TEST_CASE(Add_RefusesAnUnknownNameWithTheRegisteredOnes_AndADuplicate)
{
	const auto unknown = call("world.systems.add", R"({"system":"crystal_run::update"})");
	BOOST_TEST(unknown.code == cmd::codes::invalid_args);
	BOOST_TEST(unknown.error.find("game::spin") != std::string::npos);
	BOOST_TEST(unknown.error.find("game::fade") != std::string::npos);
	const auto duplicate = call("world.systems.add", R"({"system":"game::spin"})");
	BOOST_TEST(duplicate.code == cmd::codes::invalid_args);
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system" }));
	BOOST_TEST(!scene.is_dirty());
}

BOOST_AUTO_TEST_CASE(Remove_TakesOutAnyNameInTheList_AlsoOneThatIsNoLongerRegistered)
{
	const auto gone = call("world.systems.remove", R"({"system":"gone::system"})");
	BOOST_REQUIRE_MESSAGE(gone.ok, gone.error);
	BOOST_TEST((names_in(gone) == names{ "game::spin" }));
	const auto missing = call("world.systems.remove", R"({"system":"game::fade"})");
	BOOST_TEST(missing.code == cmd::codes::not_found);
	BOOST_TEST(missing.error.find("game::spin") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Move_ChecksTheNameAndThePlace_AndSamePlaceChangesNothing)
{
	call("world.systems.add", R"({"system":"game::fade"})");
	const auto moved = call("world.systems.move", R"({"system":"game::fade","to":0})");
	BOOST_REQUIRE_MESSAGE(moved.ok, moved.error);
	BOOST_TEST((systems_of() == names{ "game::fade", "game::spin", "gone::system" }));
	const std::uint64_t generation = scene.generation();
	const auto same = call("world.systems.move", R"({"system":"game::fade","to":0})");
	BOOST_REQUIRE(same.ok);
	BOOST_TEST(same.value.at("unchanged").as_bool());
	BOOST_TEST(scene.generation() == generation);
	BOOST_TEST(call("world.systems.move", R"({"system":"game::fade","to":3})").code == cmd::codes::invalid_args);
	BOOST_TEST(call("world.systems.move", R"({"system":"game::fade","to":-1})").code == cmd::codes::invalid_args);
	BOOST_TEST(call("world.systems.move", R"({"system":"nothing","to":0})").code == cmd::codes::not_found);
	BOOST_TEST((systems_of() == names{ "game::fade", "game::spin", "gone::system" }));
}

BOOST_AUTO_TEST_CASE(DuringPlay_EveryEditIsBusy)
{
	scene.set_edits_locked(true);
	BOOST_TEST(call("world.systems.add", R"({"system":"game::fade"})").code == cmd::codes::busy);
	BOOST_TEST(call("world.systems.remove", R"({"system":"game::spin"})").code == cmd::codes::busy);
	BOOST_TEST(call("world.systems.move", R"({"system":"game::spin","to":1})").code == cmd::codes::busy);
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system" }));
	BOOST_TEST(call("world.systems.list", "{}").ok);
}

BOOST_AUTO_TEST_CASE(AnEdit_IsOneUndoableCommit_ThatMakesTheLevelDirty_AndUndoAndRedoKeepTheList)
{
	BOOST_TEST(!scene.is_dirty());
	BOOST_REQUIRE(call("world.systems.add", R"({"system":"game::fade"})").ok);
	BOOST_TEST(scene.is_dirty());
	BOOST_REQUIRE(scene.history().next_undo() != nullptr);
	BOOST_TEST(scene.history().next_undo()->label == "Add world system");
	BOOST_TEST((scene.history().next_undo()->origin.kind == cmd::origin_kind::agent));

	reloaded.clear();
	pushes = 0;
	BOOST_REQUIRE(scene.undo());
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system" }));
	BOOST_TEST((scene.worlds_systems_list[0] == systems_of()));
	BOOST_TEST(!scene.is_dirty());
	BOOST_TEST(pushes == 1);

	BOOST_REQUIRE(scene.redo());
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system", "game::fade" }));
	BOOST_TEST((scene.worlds_systems_list[0] == systems_of()));
	BOOST_TEST(scene.is_dirty());
	BOOST_TEST(pushes == 2);
}

BOOST_AUTO_TEST_CASE(AnEditOfAWorldThatIsNotActive_IsUndoneThroughTheWorldReloader)
{
	BOOST_REQUIRE(call("world.systems.add", R"({"world":"w1","system":"game::spin"})").ok);
	reloaded.clear();
	BOOST_REQUIRE(scene.undo());
	BOOST_TEST((systems_of(1) == names{}));
	BOOST_REQUIRE_EQUAL(reloaded.size(), 1u);
	BOOST_TEST(reloaded[0] == 1u);
}

BOOST_AUTO_TEST_CASE(RemoveThenMove_UndoneOneByOne_ComeBackInOrder)
{
	BOOST_REQUIRE(call("world.systems.add", R"({"system":"game::fade"})").ok);
	BOOST_REQUIRE(call("world.systems.move", R"({"system":"game::fade","to":0})").ok);
	BOOST_REQUIRE(call("world.systems.remove", R"({"system":"game::spin"})").ok);
	BOOST_TEST((systems_of() == names{ "game::fade", "gone::system" }));
	BOOST_REQUIRE(scene.undo());
	BOOST_TEST((systems_of() == names{ "game::fade", "game::spin", "gone::system" }));
	BOOST_REQUIRE(scene.undo());
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system", "game::fade" }));
	BOOST_REQUIRE(scene.undo());
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system" }));
	BOOST_TEST(!scene.undo());
	BOOST_TEST(!scene.is_dirty());
	BOOST_REQUIRE(scene.redo());
	BOOST_REQUIRE(scene.redo());
	BOOST_REQUIRE(scene.redo());
	BOOST_TEST((systems_of() == names{ "game::fade", "gone::system" }));
}

BOOST_AUTO_TEST_CASE(TheSavedLevel_HoldsTheSystemsInWorldsSystems_AndReadingItBringsThemBack)
{
	BOOST_REQUIRE(call("world.systems.add", R"({"system":"game::fade","index":0})").ok);
	BOOST_REQUIRE(call("world.systems.remove", R"({"system":"gone::system"})").ok);
	BOOST_REQUIRE(scene.undo());
	BOOST_REQUIRE(scene.redo());

	const json::object level = scn::level_desc::build_json("lvl", scene.world_descs, *descs);
	const json::array& worlds = level.at("worlds").as_array();
	BOOST_REQUIRE_EQUAL(worlds.size(), 2u);
	BOOST_TEST((json::value_to<names>(worlds[0].at("systems")) == names{ "game::fade", "game::spin" }));

	scn::world_desc reread;
	reread.deserialize(*descs, worlds[0].as_object());
	BOOST_TEST((reread.get_systems() == names{ "game::fade", "game::spin" }));
}

BOOST_AUTO_TEST_CASE(AnUnknownSystem_IsAProblemOfTheLevel_NotASilentSkip)
{
	scn::context_set level("level", nullptr, nullptr, scn::set_options{ .hot_reload = false });
	for (std::size_t i = 0; i < scene.world_descs.size(); ++i) {
		scn::world_context& context = level.create_context(scene.world_names[i], static_cast<std::uint32_t>(i + 1));
		level.add_systems(context, scene.world_descs[i].get_systems(), sfactory);
	}
	scene.level_tag = res::tag("res://levels/main.desc");

	const std::vector<edt::problem> found = edt::world_problem_publisher::collect(scene, &level);
	BOOST_REQUIRE_EQUAL(found.size(), 1u);
	BOOST_TEST((found[0].severity == edt::problem_severity::error));
	BOOST_TEST(found[0].file == "levels/main.desc");
	BOOST_TEST(found[0].code == "unknown_system");
	BOOST_TEST(found[0].message.find("w0") != std::string::npos);
	BOOST_TEST(found[0].message.find("gone::system") != std::string::npos);
	BOOST_REQUIRE_EQUAL(found[0].notes.size(), 1u);
	BOOST_TEST(found[0].notes[0] == "/worlds/0/systems/1");

	edt::editor_events problem_events;
	edt::problem_list problems(&problem_events);
	edt::world_problem_publisher publisher;
	int announced = 0;
	const edt::connection watch = problem_events.problems_changed.subscribe([&announced](const std::string&) { ++announced; });
	publisher.refresh(problems, scene, &level);
	publisher.refresh(problems, scene, &level);
	BOOST_TEST(problems.size() == 1u);
	BOOST_TEST(announced == 1);
	// Problems' "Go to" (#596): every row knows its world, whatever its kind.
	const std::optional<std::string> world = publisher.world_of(found[0]);
	BOOST_REQUIRE(world.has_value());
	BOOST_TEST(*world == "w0");
	edt::problem other = found[0];
	other.message += " (another)";
	BOOST_TEST(!publisher.world_of(other).has_value());

	// Removing the name from the list clears the problem once the plan is rebuilt from the desc.
	BOOST_REQUIRE(call("world.systems.remove", R"({"system":"gone::system"})").ok);
	level.add_systems(level.get("w0"), scene.world_descs[0].get_systems(), sfactory);
	publisher.refresh(problems, scene, &level);
	BOOST_TEST(problems.size() == 0u);
	BOOST_TEST(!publisher.world_of(found[0]).has_value());
	publisher.refresh(problems, scene, nullptr);
	BOOST_TEST(problems.size() == 0u);
}

BOOST_AUTO_TEST_CASE(ASystemsEdit_LeavesTheNodeSelectionAlone)
{
	edt::selection picked;
	picked.world = "w0";
	picked.path = { 0xa4 };
	selection.select(picked);
	BOOST_REQUIRE(selection.has_selection());
	BOOST_REQUIRE(call("world.systems.add", R"({"system":"game::fade"})").ok);
	BOOST_REQUIRE(call("world.systems.move", R"({"system":"game::fade","to":0})").ok);
	BOOST_REQUIRE(call("world.systems.remove", R"({"system":"game::spin"})").ok);
	BOOST_TEST(selection.has_selection());
	// A node edit still clears it.
	BOOST_REQUIRE(call("scene.node.create", R"({"parent":{"context":"w0","path":[]},"name":"Box"})").ok);
	BOOST_TEST(!selection.has_selection());
}

BOOST_AUTO_TEST_CASE(ANodeEditAndASystemsEdit_AreUndoneAndRedoneInTheirOwnOrder)
{
	BOOST_REQUIRE(call("world.systems.add", R"({"system":"game::fade"})").ok);
	BOOST_REQUIRE(call("scene.node.create", R"({"parent":{"context":"w0","path":[]},"name":"Box"})").ok);
	BOOST_TEST(scene.world_descs[0].get_root().children.size() == 1u);
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system", "game::fade" }));

	BOOST_REQUIRE(scene.undo());
	BOOST_TEST(scene.world_descs[0].get_root().children.size() == 0u);
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system", "game::fade" }));
	BOOST_REQUIRE(scene.undo());
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system" }));
	BOOST_REQUIRE(scene.redo());
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system", "game::fade" }));
	BOOST_TEST(scene.world_descs[0].get_root().children.size() == 0u);
	BOOST_REQUIRE(scene.redo());
	BOOST_TEST(scene.world_descs[0].get_root().children.size() == 1u);

	// One commit that changes both is one record and comes back as one.
	scene.world_descs[0].set_systems({ "game::fade" });
	scene.world_descs[0].get_root().children.clear();
	scene.commit("both");
	BOOST_REQUIRE(scene.undo());
	BOOST_TEST((systems_of() == names{ "game::spin", "gone::system", "game::fade" }));
	BOOST_TEST(scene.world_descs[0].get_root().children.size() == 1u);
}

BOOST_AUTO_TEST_CASE(ARollbackOfTheAgentsGroup_StopsAtAHumansLaterEditOfTheSameList)
{
	const auto agent = cmd::origin::agent("s1");
	const auto group = scene.history().begin_group("Agent session", agent);
	BOOST_REQUIRE(call("world.systems.add", R"({"system":"game::fade"})").ok);
	scene.world_descs[0].set_systems({ "game::spin" });
	scene.commit("human edit of the systems");

	const auto result = scene.rollback_group(group);
	BOOST_TEST(result.rolled_back == 0u);
	BOOST_TEST(result.remaining == 1u);
	BOOST_REQUIRE(result.conflict_key.has_value());
	BOOST_TEST(*result.conflict_key == "world:0:systems");
	BOOST_TEST(result.conflict_label == "human edit of the systems");
	BOOST_TEST((systems_of() == names{ "game::spin" }));
}

BOOST_AUTO_TEST_CASE(ALayerRegistry_MergesWithItsParent_AndTheCommandsSeeTheLayersSystem)
{
	ecs::system_factory layer(&sfactory);
	layer.register_system<&spin>("mod::only", { .phase = ecs::phase::update });
	BOOST_TEST((layer.registry().names() == names{ "game::fade", "game::spin", "mod::only" }));
	BOOST_TEST((sfactory.registry().names() == names{ "game::fade", "game::spin" }));
	services.base_sfactory = &layer;

	const auto listed = call("world.systems.list", R"({"world":"w1"})");
	BOOST_REQUIRE(listed.ok);
	BOOST_TEST(listed.value.at("available").as_array().size() == 3u);
	const auto added = call("world.systems.add", R"({"world":"w1","system":"mod::only"})");
	BOOST_REQUIRE_MESSAGE(added.ok, added.error);
	BOOST_TEST((systems_of(1) == names{ "mod::only" }));
	BOOST_TEST(call("world.systems.add", R"({"world":"w1","system":"mod::absent"})").error.find("mod::only") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheProblem_ComesBackWhenAnUndoPutsTheNameBack_AndOnlyTheNamedSystemsGetAPointer)
{
	scn::context_set level("level", nullptr, nullptr, scn::set_options{ .hot_reload = false });
	scn::world_context& context = level.create_context("w0", 1);
	level.add_systems(context, systems_of(), sfactory);
	scene.level_tag = res::tag("res://levels/main.desc");
	edt::problem_list problems;
	edt::world_problem_publisher publisher;
	const auto rebuild = [&] {
		level.add_systems(level.get("w0"), systems_of(), sfactory);
		publisher.refresh(problems, scene, &level);
	};

	publisher.refresh(problems, scene, &level);
	BOOST_TEST(problems.size() == 1u);
	BOOST_REQUIRE(call("world.systems.remove", R"({"system":"gone::system"})").ok);
	rebuild();
	BOOST_TEST(problems.size() == 0u);
	BOOST_REQUIRE(scene.undo());
	rebuild();
	BOOST_TEST(problems.size() == 1u);
	BOOST_REQUIRE(scene.redo());
	rebuild();
	BOOST_TEST(problems.size() == 0u);

	// A duplicate points at the second entry; a fault that names no one entry of the list has no pointer.
	scene.world_descs[0].set_systems({ "game::spin", "game::fade", "game::spin" });
	scene.commit("duplicate");
	level.add_systems(level.get("w0"), systems_of(), sfactory);
	const std::vector<edt::problem> duplicate = edt::world_problem_publisher::collect(scene, &level);
	BOOST_REQUIRE_EQUAL(duplicate.size(), 1u);
	BOOST_TEST(duplicate[0].code == "duplicate_system");
	BOOST_REQUIRE_EQUAL(duplicate[0].notes.size(), 1u);
	BOOST_TEST(duplicate[0].notes[0] == "/worlds/0/systems/2");
}

BOOST_AUTO_TEST_SUITE_END()
