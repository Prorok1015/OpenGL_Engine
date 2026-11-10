#include <boost/test/unit_test.hpp>
#include "edt_desc_location.h"
#include <cstdint>
#include <string>
#include <vector>

// #596: what a row of the source "desc" leads to, read from the JSON pointer of its diagnostic.
namespace {

using node = scn::prefab_desc::prefab_node;

node make_node(std::uint64_t id, std::vector<node> children = {})
{
	node out;
	out.node_id = id;
	out.children = std::move(children);
	return out;
}

struct two_worlds
{
	two_worlds()
	{
		w0 = make_node(0x10, { make_node(0xa1), make_node(0xa2, { make_node(0xb1), make_node(0xb2) }) });
		w1 = make_node(0x20, { make_node(0xc1) });
		level.level = "res://levels/l.desc";
		level.names = { "memory://editor/loading/1/l.desc", "res://levels/l.desc" };
		level.worlds = { { "main", &w0, "memory://inline/w0" }, { "ui", &w1, "res://worlds/ui.desc" } };
	}

	node w0;
	node w1;
	edt::level_worlds level;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDescLocation, two_worlds)

BOOST_AUTO_TEST_CASE(ALevelsPointer_LeadsToTheNodeByItsIds)
{
	const auto target = edt::locate_desc(level, "memory://editor/loading/1/l.desc", "/worlds/0/children/1/children/0/components/x");
	BOOST_TEST(target.level == "res://levels/l.desc");
	BOOST_TEST(target.world == "main");
	BOOST_TEST(target.path == (std::vector<std::uint64_t>{ 0xa2, 0xb1 }), boost::test_tools::per_element());
	BOOST_TEST(target.resource.empty());
}

BOOST_AUTO_TEST_CASE(TheWalkStops_AtAnythingButAChild)
{
	BOOST_TEST(edt::locate_desc(level, "res://levels/l.desc", "/worlds/0/children/1/__parent").path
		== (std::vector<std::uint64_t>{ 0xa2 }), boost::test_tools::per_element());
	BOOST_TEST(edt::locate_desc(level, "res://levels/l.desc", "/worlds/0/children/1/overrides/0/children/0").path
		== (std::vector<std::uint64_t>{ 0xa2 }), boost::test_tools::per_element());
	BOOST_TEST(edt::locate_desc(level, "res://levels/l.desc", "/worlds/0/systems/3").path.empty());
	// An index the world does not have: the deepest node found.
	BOOST_TEST(edt::locate_desc(level, "res://levels/l.desc", "/worlds/0/children/1/children/7").path
		== (std::vector<std::uint64_t>{ 0xa2 }), boost::test_tools::per_element());
	BOOST_TEST(edt::locate_desc(level, "res://levels/l.desc", "/worlds/0/children/x").path.empty());
	BOOST_TEST(edt::locate_desc(level, "res://levels/l.desc", "/worlds/0/children/-1").path.empty());
}

BOOST_AUTO_TEST_CASE(AWorldOnly_IsTheWorldWithoutAPath)
{
	const auto target = edt::locate_desc(level, "res://levels/l.desc", "/worlds/1");
	BOOST_TEST(target.world == "ui");
	BOOST_TEST(target.path.empty());
}

BOOST_AUTO_TEST_CASE(AWorldsOwnFile_IsReadFromItsRoot)
{
	const auto target = edt::locate_desc(level, "res://worlds/ui.desc", "/children/0/components/y");
	BOOST_TEST(target.level == "res://levels/l.desc");
	BOOST_TEST(target.world == "ui");
	BOOST_TEST(target.path == (std::vector<std::uint64_t>{ 0xc1 }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(AnotherFile_IsAFileToOpen)
{
	const auto target = edt::locate_desc(level, "res://prefabs/p.prefab.desc", "/children/0/components/x");
	BOOST_TEST(target.world.empty());
	BOOST_TEST(target.resource == "res://prefabs/p.prefab.desc");
}

BOOST_AUTO_TEST_CASE(TheLevelWithoutAWorld_IsTheLevelsFile)
{
	BOOST_TEST(edt::locate_desc(level, "memory://editor/loading/1/l.desc", "/worlds/5").resource == "res://levels/l.desc");
	BOOST_TEST(edt::locate_desc(level, "memory://editor/loading/1/l.desc", "/name").resource == "res://levels/l.desc");
	BOOST_TEST(edt::locate_desc(level, "memory://editor/loading/1/l.desc", "").resource == "res://levels/l.desc");
}

BOOST_AUTO_TEST_CASE(AWorldThatDidNotLoad_LeadsNowhereButTheLevel)
{
	level.worlds[1].name.clear();
	level.worlds[1].root = nullptr;
	BOOST_TEST(edt::locate_desc(level, "res://levels/l.desc", "/worlds/1/children/0").world.empty());
	BOOST_TEST(edt::locate_desc(level, "res://worlds/ui.desc", "/children/0").resource == "res://worlds/ui.desc");
}

BOOST_AUTO_TEST_SUITE_END()
