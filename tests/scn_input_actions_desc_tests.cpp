#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/scn_input_actions_desc.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <boost/json.hpp>
#include <string>
#include <variant>
#include <vector>

// res/input/actions.desc (#604, WORLD_BUS.md 4.1): read leniently, checked by build_action_map, which warns with the
// JSON path of every problem and keeps the rest.

namespace {

struct actions_fixture : test_util::scene_env
{
	actions_fixture()
	{
		sink = engine::add_log_sink([this](std::string_view, engine::log_level level, std::string_view message) {
			if (level == engine::log_level::warn) warnings.emplace_back(message);
		});
	}

	scn::input_actions_desc read(std::string_view text)
	{
		scn::input_actions_desc desc;
		desc.deserialize(*descs, boost::json::parse(text).as_object());
		return desc;
	}

	bool warned(std::string_view part) const
	{
		for (const auto& w : warnings) {
			if (w.find(part) != std::string::npos) return true;
		}
		return false;
	}

	std::vector<std::string> warnings;
	engine::log_sink_handle sink; // last: removed first
};

constexpr const char* GOOD = R"({ "__type": "input_actions_desc", "__version": 1, "actions": [
	{ "name": "MoveLeft", "bindings": [ { "key": "A" }, { "key": "LEFT" } ] },
	{ "name": "Fire", "bindings": [ { "mouse": "LEFT" } ] } ] })";

} // namespace

BOOST_FIXTURE_TEST_SUITE(ScnInputActionsDescTests, actions_fixture)

BOOST_AUTO_TEST_CASE(AGoodFileGivesItsActionsInOrder)
{
	const auto map = scn::build_action_map(read(GOOD), "res://input/actions.desc");
	BOOST_TEST(warnings.empty());
	BOOST_REQUIRE(map.size() == 2u);
	BOOST_TEST(map.actions()[0].name == "MoveLeft");
	BOOST_TEST(map.actions()[0].bindings.size() == 2u);
	BOOST_TEST((map.actions()[0].bindings[1].key() == inp::KEYBOARD_BUTTONS::LEFT));
	BOOST_TEST((map.actions()[1].bindings[0].mouse_button() == inp::MOUSE_BUTTONS::LEFT));
}

BOOST_AUTO_TEST_CASE(TheDescSavesWhatItRead)
{
	const auto desc = read(GOOD);
	boost::json::object saved;
	desc.serialize(saved);
	BOOST_TEST((saved.at("actions") == boost::json::parse(GOOD).as_object().at("actions")));
}

BOOST_AUTO_TEST_CASE(AGamepadBindingTakesAThreshold_WhichTheDescSavesBack_AndAWrongOneIsWarnedAbout)
{
	const char* text = R"({ "actions": [ { "name": "Aim", "bindings": [
		{ "axis": "RightTrigger", "threshold": 0.3 }, { "gamepad": "A", "threshold": 0.5 }, { "axis": "LeftX-" }, { "axis": "LeftX+", "threshold": 2 } ] } ] })";
	const auto desc = read(text);
	const auto map = scn::build_action_map(desc, "res://input/actions.desc");
	BOOST_REQUIRE(map.size() == 1u);
	const auto& bindings = map.actions()[0].bindings;
	BOOST_REQUIRE(bindings.size() == 4u);
	BOOST_TEST(bindings[0].threshold() == 0.3f);
	BOOST_TEST(bindings[1].threshold() == 0.5f);
	BOOST_TEST(bindings[2].threshold() == 0.0f);
	BOOST_TEST(bindings[3].threshold() == 0.0f);
	BOOST_TEST(warned("/actions/0/bindings/3/threshold: not a number in 0..1"));

	boost::json::object saved;
	desc.serialize(saved);
	BOOST_TEST((saved.at("actions") == boost::json::parse(text).as_object().at("actions")));
}

BOOST_AUTO_TEST_CASE(AThresholdOnAKeyIsWarnedAboutOnce_AndABindingSkippedForItsNameAddsNoSecondWarning)
{
	const auto map = scn::build_action_map(read(R"({ "actions": [
		{ "name": "A", "bindings": [ { "key": "SPACE", "threshold": 0.4 }, { "mouse": "LEFT", "threshold": "x" },
			{ "gamepad": "TURBO", "threshold": 9 }, { "axis": "LeftX-", "threshold": 0.2 } ] } ] })"), "res://input/actions.desc");
	BOOST_REQUIRE(map.size() == 1u);
	BOOST_TEST(map.actions()[0].bindings.size() == 3u);
	std::size_t about_threshold = 0;
	for (const auto& w : warnings) {
		if (w.find("threshold") != std::string::npos) ++about_threshold;
	}
	BOOST_TEST(about_threshold == 2u);
	BOOST_TEST(warned("/actions/0/bindings/0/threshold: ignored for a key or a mouse button"));
	BOOST_TEST(warned("/actions/0/bindings/1/threshold: ignored for a key or a mouse button"));
	BOOST_TEST(!warned("/actions/0/bindings/2/threshold"));
}

BOOST_AUTO_TEST_CASE(ProblemsAreWarnedWithTheirPathAndSkipped)
{
	const auto map = scn::build_action_map(read(R"({ "actions": [
		{ "name": "Jump", "bindings": [ { "key": "SPACEBAR" }, { "key": "SPACE" }, { "mouse": "BTN_1" }, {}, { "key": "A", "mouse": "LEFT" }, { "key": 5 } ] },
		{ "name": "", "bindings": [ { "key": "B" } ] },
		{ "bindings": [ { "key": "C" } ] },
		{ "name": "Jump", "bindings": [ { "key": "J" } ] },
		"not an object",
		{ "name": "Ok", "bindings": [ { "key": "K" } ] } ] })"), "res://input/actions.desc");

	BOOST_TEST(warned("res://input/actions.desc: /actions/0/bindings/0/key: unknown key 'SPACEBAR'"));
	BOOST_TEST(warned("/actions/0/bindings/2/mouse: unknown mouse button 'BTN_1'"));
	BOOST_TEST(warned("/actions/0/bindings/3: a binding names exactly one"));
	BOOST_TEST(warned("/actions/0/bindings/4: a binding names exactly one"));
	BOOST_TEST(warned("/actions/0/bindings/5/key: not a string"));
	BOOST_TEST(warned("/actions/1: an action without a name"));
	BOOST_TEST(warned("/actions/2: an action without a name"));
	BOOST_TEST(warned("/actions/3/name: action 'Jump' is already declared"));
	BOOST_TEST(warned("/actions/4: an action without a name"));

	BOOST_REQUIRE(map.size() == 2u);
	BOOST_TEST(map.actions()[0].name == "Jump");
	BOOST_TEST(map.actions()[0].bindings.size() == 1u); // only SPACE
	BOOST_TEST(map.actions()[1].name == "Ok");
}

BOOST_AUTO_TEST_CASE(AFileWithMistakesIsSavedAsItWasUntilItIsEdited)
{
	constexpr const char* broken = R"({ "actions": [
		{ "name": "Jump", "bindings": [ { "key": 5 }, { "key": "SPACEBAR", "extra": true }, 7 ] },
		"not an object", { "bindings": null } ] })";
	auto desc = read(broken);
	boost::json::object saved;
	desc.serialize(saved);
	BOOST_TEST((saved.at("actions") == boost::json::parse(broken).as_object().at("actions")));
	BOOST_TEST(boost::json::serialize(saved).find("not a string") == std::string::npos);

	auto not_an_array = read(R"({ "actions": 5 })");
	boost::json::object kept;
	not_an_array.serialize(kept);
	BOOST_TEST((kept.at("actions") == boost::json::value(5)));

	// An edit writes what the desc now holds.
	desc.actions.entries.resize(1);
	desc.actions.entries[0].bindings = { scn::input_binding_entry{ .key = "SPACE" } };
	boost::json::object edited;
	desc.serialize(edited);
	BOOST_TEST((edited.at("actions") == boost::json::parse(R"([{"name":"Jump","bindings":[{"key":"SPACE"}]}])")));
}

BOOST_AUTO_TEST_CASE(ActionsThatAreNotAnArrayAreWarnedAboutAndEmpty)
{
	BOOST_TEST(scn::build_action_map(read(R"({ "actions": 5 })")).size() == 0u);
	BOOST_TEST(warned("/actions: expected an array"));
}

BOOST_AUTO_TEST_CASE(AFileWithoutActionsHasNone)
{
	BOOST_TEST(scn::build_action_map(read(R"({ "__type": "input_actions_desc", "__version": 1 })")).size() == 0u);
	BOOST_TEST(warnings.empty());
}

BOOST_AUTO_TEST_CASE(GamepadButtonsAndAxesAreBindings_AndTheirMistakesAreWarnedWithThePath)
{
	const auto map = scn::build_action_map(read(R"({ "actions": [
		{ "name": "Jump", "bindings": [ { "key": "SPACE" }, { "gamepad": "A" }, { "axis": "LeftX-" }, { "gamepad": "TURBO" }, { "axis": "LeftZ" },
			{ "gamepad": "B", "axis": "LeftX+" }, { "gamepad": 3 }, { "axis": true } ] } ] })"), "res://input/actions.desc");

	BOOST_REQUIRE(map.size() == 1u);
	const auto& bindings = map.actions()[0].bindings;
	BOOST_REQUIRE(bindings.size() == 3u);
	BOOST_TEST((bindings[1].gamepad_button() == inp::GAMEPAD_BUTTONS::A));
	BOOST_TEST((bindings[2].gamepad_axis() == inp::GAMEPAD_AXES::LEFT_X_NEG));
	BOOST_TEST(warned("/actions/0/bindings/3/gamepad: unknown gamepad button 'TURBO'"));
	BOOST_TEST(warned("/actions/0/bindings/4/axis: unknown gamepad axis 'LeftZ'"));
	BOOST_TEST(warned("/actions/0/bindings/5: a binding names exactly one"));
	BOOST_TEST(warned("/actions/0/bindings/6/gamepad: not a string"));
	BOOST_TEST(warned("/actions/0/bindings/7/axis: not a string"));
}

BOOST_AUTO_TEST_CASE(AFileWithGamepadBindingsIsSavedAsItWas)
{
	constexpr const char* text = R"({ "__type": "input_actions_desc", "__version": 1, "actions": [
		{ "name": "Move", "bindings": [ { "axis": "LeftY+" }, { "gamepad": "DPAD_DOWN" } ] } ],
		"gamepad": { "deadzone": 0.3 } })";
	const auto desc = read(text);
	boost::json::object saved;
	desc.serialize(saved);
	BOOST_TEST((saved.at("actions") == boost::json::parse(text).as_object().at("actions")));
	BOOST_TEST((saved.at("gamepad") == boost::json::parse(text).as_object().at("gamepad")));
}

BOOST_AUTO_TEST_CASE(TheGamepadSetting_IsReadWithDefaultsForWhatIsMissing_AndBadNumbersAreWarned)
{
	const auto defaults = scn::build_gamepad_config(read(GOOD));
	BOOST_TEST((defaults == inp::gamepad_config{}));
	BOOST_TEST(warnings.empty());

	const auto some = scn::build_gamepad_config(read(R"({ "gamepad": { "deadzone": 0.4 } })"));
	BOOST_TEST(some.deadzone == 0.4f);
	BOOST_TEST(some.press_threshold == inp::gamepad_config{}.press_threshold);

	const auto bad = scn::build_gamepad_config(read(R"({ "gamepad": { "deadzone": 2, "press_threshold": "high" } })"), "res://input/actions.desc");
	BOOST_TEST((bad == inp::gamepad_config{}));
	BOOST_TEST(warned("res://input/actions.desc: /gamepad/deadzone: 2 is out of 0..1"));
	BOOST_TEST(warned("/gamepad/press_threshold: not a number"));
}

BOOST_AUTO_TEST_CASE(AFileWithoutTheGamepadKey_IsSavedWithoutIt)
{
	const auto desc = read(GOOD);
	boost::json::object saved;
	desc.serialize(saved);
	BOOST_TEST(!saved.contains("gamepad"));
}

BOOST_AUTO_TEST_CASE(AGamepadSettingThatIsNoObjectOrAHugeNumber_IsWarnedAboutWithTheFile)
{
	const auto notobject = scn::build_gamepad_config(read(R"({ "gamepad": 5 })"), "res://input/actions.desc");
	BOOST_TEST((notobject == inp::gamepad_config{}));
	BOOST_TEST(warned("res://input/actions.desc: /gamepad: expected an object"));

	const auto huge = scn::build_gamepad_config(read(R"({ "gamepad": { "deadzone": 1e300 } })"), "res://input/actions.desc");
	BOOST_TEST((huge == inp::gamepad_config{}));
	BOOST_TEST(warned("/gamepad/deadzone:"));
}

BOOST_AUTO_TEST_SUITE_END()
