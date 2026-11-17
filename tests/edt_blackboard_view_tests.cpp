#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_blackboard_panel.h"
#include "edt_blackboard_view.h"
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/editor/edt_panel_registry.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The Blackboard panel (W5 #273) is a table of the committed blackboard of a set. Everything the panel decides
// is in edt_blackboard_view.h, which has no ImGui: the rows, their text, the filter and the choice of the set.

namespace {

constexpr ecs::bb_key<int> k_health{ "hud.health" };
constexpr ecs::bb_key<bool> k_inside{ "zone.door.player_inside" };
constexpr ecs::bb_key<float> k_volume{ "settings.volume" };
constexpr ecs::bb_key<std::string> k_title{ "hud.title" };
constexpr ecs::bb_key<glm::vec3> k_marker{ "hud.marker" };
constexpr ecs::bb_key<glm::vec2> k_size{ "hud.size" };
constexpr ecs::bb_key<glm::vec4> k_tint{ "hud.tint" };
constexpr ecs::bb_key<std::uint64_t> k_seed{ "world.seed" };
constexpr ecs::bb_key<double> k_time{ "world.time" };

struct opaque_value
{
	int a = 0;
};
constexpr ecs::bb_key<opaque_value> k_opaque{ "mod.opaque" };

constexpr auto one_frame = std::chrono::duration<float>(0.001f);

struct view_fixture
{
	view_fixture()
		: set(std::make_unique<scn::context_set>("level", nullptr))
	{
		add("world", 0);
		add("ui", 1);
	}

	scn::world_context& add(std::string_view name, uint32_t id)
	{
		auto& context = set->create_context(name, id);
		set->add_systems(context, {}, sf);
		return context;
	}

	ecs::blackboard_writer& writer_of(std::string_view context) { return set->get(context).state().ctx().get<ecs::blackboard_writer>(); }

	void commit() { set->update(one_frame); }

	std::vector<edt::blackboard_view::row> rows(std::string_view filter = {}) const { return edt::blackboard_view::collect_rows(*set, filter); }

	ecs::system_factory sf;
	std::unique_ptr<scn::context_set> set;
};

std::vector<std::string> keys_of(const std::vector<edt::blackboard_view::row>& rows)
{
	std::vector<std::string> keys;
	for (const auto& r : rows) keys.push_back(r.key);
	return keys;
}

const edt::blackboard_view::row* find_row(const std::vector<edt::blackboard_view::row>& rows, std::string_view key)
{
	for (const auto& r : rows) {
		if (r.key == key) return &r;
	}
	return nullptr;
}

}

BOOST_FIXTURE_TEST_SUITE(EditorBlackboardViewTests, view_fixture)

BOOST_AUTO_TEST_CASE(NothingIsShownBeforeTheFirstExchange)
{
	BOOST_TEST(rows().empty());
	writer_of("world").set(k_health, 10); // written into the back buffer: the systems cannot read it yet
	BOOST_TEST(rows().empty());
	commit();
	BOOST_TEST(rows().size() == 1u);
}

BOOST_AUTO_TEST_CASE(RowsAreSortedByKeyAndKnowTheirOwner)
{
	writer_of("world").set(k_inside, true);
	writer_of("ui").set(k_health, 10);
	writer_of("world").set(k_volume, 0.5f);
	commit();
	const auto all = rows();
	BOOST_TEST((keys_of(all) == std::vector<std::string>{ "hud.health", "settings.volume", "zone.door.player_inside" }));
	BOOST_TEST(all[0].owner == "hud");
	BOOST_TEST(all[1].owner == "settings");
	BOOST_TEST(all[2].owner == "zone");
	BOOST_TEST(edt::blackboard_view::owner_of("nodot").empty());
	BOOST_TEST(edt::blackboard_view::owner_of(".lead") == "");
	BOOST_TEST(edt::blackboard_view::owner_of("a.b.c") == "a");
}

BOOST_AUTO_TEST_CASE(TheWriterIsTheContextByNameOrExternal)
{
	writer_of("world").set(k_health, 10);
	writer_of("ui").set(k_volume, 0.5f);
	set->channel().external_blackboard().set(k_inside, true);
	commit();
	const auto all = rows();
	BOOST_REQUIRE(find_row(all, "hud.health"));
	BOOST_TEST(find_row(all, "hud.health")->writer == "world");
	BOOST_TEST(find_row(all, "settings.volume")->writer == "ui");
	BOOST_TEST(find_row(all, "zone.door.player_inside")->writer == "external");
}

BOOST_AUTO_TEST_CASE(TheGreaterWriterOfAKeyIsTheOneShown)
{
	set->channel().external_blackboard().set(k_health, 1);
	writer_of("world").set(k_health, 2);
	writer_of("ui").set(k_health, 3);
	commit();
	const auto all = rows();
	BOOST_REQUIRE_EQUAL(all.size(), 1u);
	BOOST_TEST(all[0].value == "3");
	BOOST_TEST(all[0].writer == "ui");
}

BOOST_AUTO_TEST_CASE(KnownTypesShowTheirValue)
{
	auto& w = writer_of("world");
	w.set(k_health, 42);
	w.set(k_inside, true);
	w.set(k_volume, 0.5f);
	w.set(k_title, std::string{ "Level 1" });
	w.set(k_marker, glm::vec3{ 1, 2, 3 });
	w.set(k_size, glm::vec2{ 4, 5 });
	w.set(k_tint, glm::vec4{ 1, 0, 0, 1 });
	w.set(k_seed, std::uint64_t{ 9 });
	w.set(k_time, 1.5);
	commit();
	const auto all = rows();
	const auto value = [&](std::string_view key) { return find_row(all, key)->value; };
	const auto type = [&](std::string_view key) { return find_row(all, key)->type; };
	BOOST_REQUIRE_EQUAL(all.size(), 9u);
	BOOST_TEST(value("hud.health") == "42");
	BOOST_TEST(type("hud.health") == "int");
	BOOST_TEST(value("zone.door.player_inside") == "true");
	BOOST_TEST(type("zone.door.player_inside") == "bool");
	BOOST_TEST(value("settings.volume") == "0.5");
	BOOST_TEST(type("settings.volume") == "float");
	BOOST_TEST(value("hud.title") == "\"Level 1\"");
	BOOST_TEST(type("hud.title") == "string");
	BOOST_TEST(value("hud.marker") == "(1, 2, 3)");
	BOOST_TEST(type("hud.marker") == "vec3");
	BOOST_TEST(value("hud.size") == "(4, 5)");
	BOOST_TEST(value("hud.tint") == "(1, 0, 0, 1)");
	BOOST_TEST(value("world.seed") == "9");
	BOOST_TEST(type("world.seed") == "uint64");
	BOOST_TEST(value("world.time") == "1.5");
	BOOST_TEST(type("world.time") == "double");
}

BOOST_AUTO_TEST_CASE(AnUnknownTypeShowsItsNameInsteadOfAValue)
{
	writer_of("world").set(k_opaque, opaque_value{ 5 });
	commit();
	const auto all = rows();
	BOOST_REQUIRE_EQUAL(all.size(), 1u);
	BOOST_TEST(all[0].value.front() == '<');
	BOOST_TEST(all[0].value.back() == '>');
	BOOST_TEST(all[0].value.find("opaque_value") != std::string::npos);
	BOOST_TEST(all[0].type.find("opaque_value") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheFilterKeepsKeysThatContainTheTextIgnoringCase)
{
	writer_of("world").set(k_health, 1);
	writer_of("world").set(k_title, std::string{ "x" });
	writer_of("world").set(k_volume, 0.5f);
	commit();
	BOOST_TEST((keys_of(rows("HUD")) == std::vector<std::string>{ "hud.health", "hud.title" }));
	BOOST_TEST((keys_of(rows("vol")) == std::vector<std::string>{ "settings.volume" }));
	BOOST_TEST(rows("nothing like this").empty());
	BOOST_TEST(rows("").size() == 3u);
}

BOOST_AUTO_TEST_CASE(AValueStaysUntilItIsReplacedAndFollowsTheNewWrite)
{
	writer_of("world").set(k_health, 10);
	commit();
	commit();
	BOOST_TEST(rows()[0].value == "10");
	writer_of("world").set(k_health, 7);
	BOOST_TEST(rows()[0].value == "10"); // the committed value is what the systems read
	commit();
	BOOST_TEST(rows()[0].value == "7");
}

BOOST_AUTO_TEST_CASE(AClearedChannelIsEmpty)
{
	writer_of("world").set(k_health, 10);
	commit();
	set->channel().clear();
	BOOST_TEST(rows().empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- the sets and the panel

namespace {

struct manager_fixture : test_util::scene_env
{
	manager_fixture()
		: mgr(*rs, *assembler, sf)
	{
	}

	ecs::system_factory sf;
	scn::context_manager mgr;
};

}

BOOST_FIXTURE_TEST_SUITE(EditorBlackboardPanelTests, manager_fixture)

BOOST_AUTO_TEST_CASE(SetNamesAreInCreationOrder)
{
	mgr.create_set("play");
	mgr.create_preview("hero.prefab.desc", "preview");
	const auto names = edt::blackboard_view::set_names(mgr);
	BOOST_REQUIRE_EQUAL(names.size(), 3u);
	BOOST_TEST(names[0] == "level");
	BOOST_TEST(names[1] == "play");
	BOOST_TEST(names[2] == scn::context_manager::preview_set_name("hero.prefab.desc"));
}

BOOST_AUTO_TEST_CASE(PickSetKeepsTheChosenOneThenTheLevelThenTheFirst)
{
	using edt::blackboard_view::pick_set;
	BOOST_TEST(pick_set("play", { "level", "play" }) == "play");
	BOOST_TEST(pick_set("gone", { "play", "level" }) == "level"); // the set was destroyed: back to the level
	BOOST_TEST(pick_set("", { "level", "play" }) == "level");
	BOOST_TEST(pick_set("gone", { "play", "other" }) == "play");
	BOOST_TEST(pick_set("play", {}).empty());
}

BOOST_AUTO_TEST_CASE(TheViewReadsTheChannelOfTheSetItIsGiven)
{
	mgr.create_set("play");
	mgr.blackboard("level").set(k_health, 1);
	mgr.blackboard("play").set(k_health, 2);
	mgr.create_context("level", "w", scn::context_kind::runtime);
	mgr.create_context("play", "w", scn::context_kind::runtime);
	mgr.update(one_frame);
	const auto level_rows = edt::blackboard_view::collect_rows(mgr.get_set("level"));
	const auto play_rows = edt::blackboard_view::collect_rows(mgr.get_set("play"));
	BOOST_REQUIRE_EQUAL(level_rows.size(), 1u);
	BOOST_REQUIRE_EQUAL(play_rows.size(), 1u);
	BOOST_TEST(level_rows[0].value == "1");
	BOOST_TEST(play_rows[0].value == "2");
	BOOST_TEST(level_rows[0].writer == "external");
}

BOOST_AUTO_TEST_CASE(ThePanelRegistersLikeAnyOtherAndStartsClosed)
{
	edt::panel_registry registry;
	auto panel = std::make_shared<edt::blackboard_panel>();
	BOOST_TEST(panel->get_id() == "blackboard");
	BOOST_TEST(panel->get_title() == "Blackboard");
	auto registration = registry.add({ .id = panel->get_id(), .title = panel->get_title(), .slot = edt::dock_slot::bottom_right,
									   .open_by_default = false, .panel = panel });
	BOOST_TEST(registration.active());
	BOOST_TEST(registry.contains("blackboard"));
	BOOST_TEST(!registry.is_open("blackboard"));
	registry.render_all(); // closed: no ImGui involved
}

BOOST_AUTO_TEST_SUITE_END()
