#include <boost/test/unit_test.hpp>
#include <snk/input/inp_actions.h>
#include <snk/input/inp_input_stack.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <cstddef>
#include <set>
#include <string>
#include <variant>
#include <vector>

// Key and mouse button names and the action map of the game actions (#604, WORLD_BUS.md 4.1).

BOOST_AUTO_TEST_SUITE(InpActionsTests)

BOOST_AUTO_TEST_CASE(EveryKeyHasOneNameThatReadsBack)
{
	std::set<std::string> names;
	for (std::size_t i = 0; i < inp::key_count; ++i) {
		const auto key = static_cast<inp::KEYBOARD_BUTTONS>(i);
		const auto name = inp::key_code_name(key);
		BOOST_TEST_INFO("key " << i);
		BOOST_TEST(!name.empty());
		BOOST_TEST(names.emplace(name).second);
		BOOST_TEST((inp::key_from_name(name) == key));
	}
	BOOST_TEST(inp::key_code_name(inp::KEYBOARD_BUTTONS::COUNT).empty());
	BOOST_TEST(!inp::key_from_name("a").has_value()); // names are upper case
}

BOOST_AUTO_TEST_CASE(EveryMouseButtonHasOneNameWithoutTheAliases)
{
	std::set<std::string> names;
	for (std::size_t i = 0; i < static_cast<std::size_t>(inp::MOUSE_BUTTONS::LAST); ++i) {
		const auto button = static_cast<inp::MOUSE_BUTTONS>(i);
		const auto name = inp::mouse_button_name(button);
		BOOST_TEST_INFO("button " << i);
		BOOST_TEST(!name.empty());
		BOOST_TEST(names.emplace(name).second);
		BOOST_TEST((inp::mouse_button_from_name(name) == button));
	}
	BOOST_TEST((inp::mouse_button_from_name("LEFT") == inp::MOUSE_BUTTONS::LEFT));
	BOOST_TEST((inp::mouse_button_from_name("BTN_4") == inp::MOUSE_BUTTONS::BTN_4));
	BOOST_TEST(!inp::mouse_button_from_name("BTN_1").has_value());
	BOOST_TEST(!inp::mouse_button_from_name("LAST").has_value());
	BOOST_TEST(inp::mouse_button_name(inp::MOUSE_BUTTONS::LAST).empty());
}

BOOST_AUTO_TEST_CASE(AnActionMapKeepsTheOrderAndRefusesEmptyOrRepeatedNames)
{
	inp::action_map map;
	BOOST_TEST(map.add("Jump", { inp::KEYBOARD_BUTTONS::SPACE }));
	BOOST_TEST(map.add("Fire", { inp::MOUSE_BUTTONS::LEFT, inp::KEYBOARD_BUTTONS::F }));
	BOOST_TEST(!map.add("Jump", {}));
	BOOST_TEST(!map.add("", {}));
	BOOST_TEST(map.size() == 2u);

	BOOST_TEST(map.id("Jump") == 0u);
	BOOST_TEST(map.id("Fire") == 1u);
	const auto* fire = map.find(map.id("Fire"));
	BOOST_REQUIRE(fire);
	BOOST_TEST(fire->bindings.size() == 2u);
	BOOST_TEST(std::holds_alternative<inp::MOUSE_BUTTONS>(fire->bindings[0]));
	BOOST_TEST(!map.find(inp::invalid_action));
}

BOOST_AUTO_TEST_CASE(AnUnknownActionWarnsOncePerName)
{
	std::vector<std::string> warnings;
	auto sink = engine::add_log_sink([&](std::string_view, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::warn) warnings.emplace_back(message);
	});
	inp::action_map map;
	map.add("Jump", { inp::KEYBOARD_BUTTONS::SPACE });
	BOOST_TEST(map.id("Dash") == inp::invalid_action);
	BOOST_TEST(map.id("Dash") == inp::invalid_action);
	BOOST_TEST(map.id("Roll") == inp::invalid_action);
	BOOST_TEST(warnings.size() == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
