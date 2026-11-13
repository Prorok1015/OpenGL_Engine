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

BOOST_AUTO_TEST_CASE(EveryGamepadButtonAndAxisHasOneNameThatReadsBack)
{
	std::set<std::string> names;
	for (std::size_t i = 0; i < core::inp::gamepad_button_count; ++i) {
		const auto button = static_cast<inp::GAMEPAD_BUTTONS>(i);
		const auto name = inp::gamepad_button_name(button);
		BOOST_TEST_INFO("button " << i);
		BOOST_TEST(!name.empty());
		BOOST_TEST(names.emplace(name).second);
		BOOST_TEST((inp::gamepad_button_from_name(name) == button));
	}
	BOOST_TEST(inp::gamepad_button_name(inp::GAMEPAD_BUTTONS::COUNT).empty());
	BOOST_TEST(!inp::gamepad_button_from_name("a").has_value());

	for (std::size_t i = 0; i < static_cast<std::size_t>(inp::GAMEPAD_AXES::COUNT); ++i) {
		const auto axis = static_cast<inp::GAMEPAD_AXES>(i);
		const auto name = inp::gamepad_axis_name(axis);
		BOOST_TEST_INFO("axis " << i);
		BOOST_TEST(!name.empty());
		BOOST_TEST(names.emplace(name).second);
		BOOST_TEST((inp::gamepad_axis_from_name(name) == axis));
		BOOST_TEST((inp::gamepad_axis_id(axis) != inp::GAMEPAD_AXIS_IDS::COUNT));
	}
	BOOST_TEST(inp::gamepad_axis_name(inp::GAMEPAD_AXES::COUNT).empty());
	BOOST_TEST(!inp::gamepad_axis_from_name("LeftX").has_value());
}

BOOST_AUTO_TEST_CASE(ADirectedAxisKnowsItsDeviceAxisAndItsDirection)
{
	BOOST_TEST((inp::gamepad_axis_id(inp::GAMEPAD_AXES::LEFT_Y_POS) == inp::GAMEPAD_AXIS_IDS::LEFT_Y));
	BOOST_TEST(inp::gamepad_axis_direction(inp::GAMEPAD_AXES::LEFT_Y_POS) == 1);
	BOOST_TEST(inp::gamepad_axis_direction(inp::GAMEPAD_AXES::RIGHT_X_NEG) == -1);
	BOOST_TEST((inp::gamepad_axis_id(inp::GAMEPAD_AXES::RIGHT_TRIGGER) == inp::GAMEPAD_AXIS_IDS::RIGHT_TRIGGER));
	BOOST_TEST(inp::gamepad_axis_direction(inp::GAMEPAD_AXES::RIGHT_TRIGGER) == 1);
}

BOOST_AUTO_TEST_CASE(ABindingKeepsItsKindCodeAndThreshold)
{
	const inp::action_binding key = inp::KEYBOARD_BUTTONS::SPACE;
	BOOST_TEST((key.kind == inp::binding_kind::keyboard));
	BOOST_TEST((key.key() == inp::KEYBOARD_BUTTONS::SPACE));
	BOOST_TEST(!key.mouse_button().has_value());
	BOOST_TEST(!key.is_gamepad());
	BOOST_TEST(key.threshold == 0.0f);

	const inp::action_binding trigger{ inp::GAMEPAD_AXES::RIGHT_TRIGGER, 0.4f };
	BOOST_TEST((trigger.kind == inp::binding_kind::gamepad_axis));
	BOOST_TEST((trigger.gamepad_axis() == inp::GAMEPAD_AXES::RIGHT_TRIGGER));
	BOOST_TEST(trigger.is_gamepad());
	BOOST_TEST(trigger.threshold == 0.4f);
	BOOST_TEST((trigger != inp::action_binding{ inp::GAMEPAD_AXES::RIGHT_TRIGGER }));
	BOOST_TEST((inp::action_binding{ inp::MOUSE_BUTTONS::LEFT } != inp::action_binding{ inp::KEYBOARD_BUTTONS::SPACE }));
}

BOOST_AUTO_TEST_CASE(ABracedListOfBindingsAndAnEmptyOneAreAdded)
{
	inp::action_map map;
	BOOST_TEST(map.add("Braced", { inp::KEYBOARD_BUTTONS::B, inp::GAMEPAD_BUTTONS::A }));
	BOOST_TEST(map.add("Empty", {}));
	BOOST_REQUIRE_EQUAL(map.size(), 2u);
	BOOST_TEST((map.actions()[0].bindings == std::vector<inp::action_binding>{ inp::KEYBOARD_BUTTONS::B, inp::GAMEPAD_BUTTONS::A }));
	BOOST_TEST(map.actions()[1].bindings.empty());
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
	BOOST_TEST(fire->bindings[0].mouse_button().has_value());
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
