#include <boost/test/unit_test.hpp>
#include "ecs_phase.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace {

std::vector<std::string> names(const std::vector<ecs::phase_id>& ids)
{
	std::vector<std::string> out;
	for (const auto& id : ids) out.push_back(id.name());
	return out;
}

}

BOOST_AUTO_TEST_SUITE(EcsPhaseTests)

BOOST_AUTO_TEST_CASE(BuiltinOrder_IsFixedAndMatchesTheDesign)
{
	ecs::phase_table table;
	BOOST_TEST(names(table.ordered()) == (std::vector<std::string>{
		"input", "pre_update", "fixed_update", "update", "post_update", "transform", "late_update" }),
		boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(PhaseId_BuiltinConvertsFromEnumAndName)
{
	ecs::phase_id from_enum = ecs::phase::transform;
	ecs::phase_id from_name = "transform";
	BOOST_TEST((from_enum == from_name));
	BOOST_TEST(from_name.is_builtin());
	BOOST_TEST((from_name.builtin() == ecs::phase::transform));

	ecs::phase_id game = "ai";
	BOOST_TEST(!game.is_builtin());
}

BOOST_AUTO_TEST_CASE(GamePhase_IsAnchoredBeforeOrAfterABuiltin)
{
	ecs::phase_table table;
	BOOST_TEST(table.add_game_phase("ai", ecs::phase::update, ecs::phase_side::before));
	BOOST_TEST(table.add_game_phase("camera_rig", ecs::phase::transform, ecs::phase_side::after));

	BOOST_TEST(names(table.ordered()) == (std::vector<std::string>{
		"input", "pre_update", "fixed_update", "ai", "update", "post_update", "transform", "camera_rig", "late_update" }),
		boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(GamePhase_SameAnchorKeepsRegistrationOrder)
{
	ecs::phase_table table;
	table.add_game_phase("first", ecs::phase::update, ecs::phase_side::after);
	table.add_game_phase("second", ecs::phase::update, ecs::phase_side::after);
	table.add_game_phase("pre_a", ecs::phase::update, ecs::phase_side::before);
	table.add_game_phase("pre_b", ecs::phase::update, ecs::phase_side::before);

	const auto order = names(table.ordered());
	auto at = [&](const char* n) { return std::ranges::find(order, n) - order.begin(); };
	BOOST_TEST(at("pre_a") < at("pre_b"));
	BOOST_TEST(at("pre_b") < at("update"));
	BOOST_TEST(at("update") < at("first"));
	BOOST_TEST(at("first") < at("second"));
}

BOOST_AUTO_TEST_CASE(GamePhase_CannotShadowBuiltinOrDuplicate)
{
	ecs::phase_table table;
	BOOST_TEST(!table.add_game_phase("update", ecs::phase::input, ecs::phase_side::before));
	BOOST_TEST(!table.add_game_phase("", ecs::phase::input, ecs::phase_side::before));
	BOOST_TEST(table.add_game_phase("ai", ecs::phase::input, ecs::phase_side::after));
	BOOST_TEST(!table.add_game_phase("ai", ecs::phase::late_update, ecs::phase_side::after));
	BOOST_TEST(table.ordered().size() == ecs::builtin_phase_count + 1);
	BOOST_TEST(table.contains("ai"));
	BOOST_TEST(!table.contains("nope"));
}

BOOST_AUTO_TEST_SUITE_END()
