#include <boost/test/unit_test.hpp>
#include <snk/input/inp_actions.h>
#include <snk/input/inp_input_stack.h>
#include <string>
#include <utility>
#include <vector>

// The input stack (INPUT_STACK.md §3, §4): which layer is above which, and what each world receives after the walk.
// Pure data, no worlds: the stack knows them by name.
namespace {

using inp::KEYBOARD_BUTTONS;

constexpr auto key_index(KEYBOARD_BUTTONS k) { return static_cast<std::size_t>(k); }

inp::stack_layer_desc layer(std::string name, int order = 0)
{
	inp::stack_layer_desc out;
	out.name = std::move(name);
	out.order = order;
	return out;
}

inp::stack_layer_desc region_layer(std::string name, int order, glm::ivec4 rect)
{
	inp::stack_layer_desc out = layer(std::move(name), order);
	out.regions.push_back(rect);
	return out;
}

std::vector<std::string> names_of(const inp::input_stack& stack)
{
	std::vector<std::string> out;
	for (const inp::stack_entry* entry : stack.ordered()) out.push_back(entry->world + "/" + entry->layer.name);
	return out;
}

// Jump on Space, MoveLeft on A or the left arrow, Fire on the left mouse button.
inp::action_map make_actions()
{
	inp::action_map map;
	map.add("Jump", { KEYBOARD_BUTTONS::SPACE });
	map.add("MoveLeft", { KEYBOARD_BUTTONS::A, KEYBOARD_BUTTONS::LEFT });
	map.add("Fire", { inp::MOUSE_BUTTONS::LEFT });
	return map;
}

constexpr inp::action_id JUMP = 0;
constexpr inp::action_id MOVE_LEFT = 1;
constexpr inp::action_id FIRE = 2;

inp::stack_layer_desc declaring(std::string name, int order, std::vector<std::string> actions)
{
	inp::stack_layer_desc out = layer(std::move(name), order);
	out.actions = std::move(actions);
	return out;
}

bool reached(const inp::stack_resolution& resolved, const char* world, inp::action_id action, KEYBOARD_BUTTONS key)
{
	const inp::world_input* in = resolved.find(world);
	return in && action < in->actions.size() && in->actions[action].keys.test(key_index(key));
}

const std::vector<std::string> both{ "game", "ui" };
const glm::vec2 outside{ 300.f, 300.f };
const glm::vec2 inside{ 20.f, 20.f };
const glm::ivec4 button{ 0, 0, 100, 100 };

}

BOOST_AUTO_TEST_SUITE(InpInputStackTests)

BOOST_AUTO_TEST_CASE(Order_IsTheLayerOrderThenTheWorldsDescIndexThenTheLaterAddedFirst)
{
	inp::input_stack stack;
	stack.put("late", 2, layer("world", 0));
	stack.put("early", 1, layer("world", 0));
	stack.put("early", 1, layer("map", 0));
	stack.put("top", 9, layer("world", 3));
	const std::vector<std::string> expected{ "top/world", "early/map", "early/world", "late/world" };
	BOOST_TEST(names_of(stack) == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Put_ReplacesALayerOfTheSameNameInPlace_AndRetainDropsTheWorldsThatAreGone)
{
	inp::input_stack stack;
	stack.put("a", 0, layer("world", 0));
	stack.put("a", 0, layer("menu", 0));
	stack.put("a", 0, layer("world", 7));
	BOOST_TEST(stack.size() == 2u);
	BOOST_TEST(stack.find("a", "world")->order == 7);
	BOOST_TEST(names_of(stack).front() == "a/world");

	stack.put("b", 1, layer("world"));
	stack.retain_worlds({ "b" });
	BOOST_TEST(stack.size() == 1u);
	BOOST_TEST(stack.find("a", "world") == nullptr);
}

BOOST_AUTO_TEST_CASE(NothingTaken_EveryWorldGetsEverything)
{
	inp::input_stack stack;
	stack.put("game", 0, layer("world"));
	stack.put("ui", 1, layer("world", 1));
	const auto result = inp::resolve(stack, both, outside, false, {});
	for (const char* world : { "game", "ui" }) {
		const inp::world_input* in = result.find(world);
		BOOST_REQUIRE(in);
		BOOST_TEST(in->buttons);
		BOOST_TEST(in->motion);
		BOOST_TEST(in->mouse_focus);
		BOOST_TEST(in->keys[key_index(KEYBOARD_BUTTONS::W)]);
	}
}

BOOST_AUTO_TEST_CASE(BlockBelow_TakesEverythingFromTheLayersBelowAndLeavesTheOnesAbove)
{
	inp::input_stack stack;
	stack.put("game", 0, layer("world", 0));
	inp::stack_layer_desc modal = layer("world", 5);
	modal.block_below = true;
	stack.put("menu", 1, modal);
	stack.put("hud", 2, layer("world", 9));
	const std::vector<std::string> fed{ "game", "menu", "hud" };

	const auto result = inp::resolve(stack, fed, outside, false, {});
	BOOST_TEST(result.find("hud")->buttons);
	BOOST_TEST(result.find("hud")->keys[key_index(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST(result.find("menu")->buttons);
	BOOST_TEST(result.find("menu")->motion);
	BOOST_TEST(!result.find("game")->buttons);
	BOOST_TEST(!result.find("game")->motion);
	BOOST_TEST(!result.find("game")->mouse_focus);
	BOOST_TEST(!result.find("game")->keys[key_index(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST(result.find("menu")->keys[key_index(KEYBOARD_BUTTONS::W)]);
}

BOOST_AUTO_TEST_CASE(ARegionTakesClicksAndWheelButNotTheMouseMovement)
{
	inp::input_stack stack;
	stack.put("game", 0, layer("world"));
	stack.put("ui", 1, region_layer("world", 1, button));

	const auto over = inp::resolve(stack, both, inside, false, {});
	BOOST_TEST(over.find("ui")->buttons);
	BOOST_TEST(!over.find("game")->buttons);
	BOOST_TEST(over.find("game")->motion);
	BOOST_TEST(over.find("ui")->motion);
	BOOST_TEST(over.find("game")->mouse_focus); // hovering a button does not stop the camera

	const auto clicking = inp::resolve(stack, both, inside, true, {});
	BOOST_TEST(!clicking.find("game")->mouse_focus); // a click or the wheel over it is not the game's

	const auto beside = inp::resolve(stack, both, outside, true, {});
	BOOST_TEST(beside.find("game")->buttons);
	BOOST_TEST(beside.find("ui")->buttons);
}

BOOST_AUTO_TEST_CASE(ADragTheLayerHoldsKeepsTheMouseWithItAndHidesTheMovementFromTheOthers)
{
	inp::input_stack stack;
	stack.put("game", 0, layer("world"));
	stack.put("ui", 1, region_layer("world", 1, button));
	const inp::drag_hold held{ true, true, "ui" };

	const auto result = inp::resolve(stack, both, outside, true, held); // dragged out of the button
	BOOST_TEST(result.find("ui")->buttons);
	BOOST_TEST(result.find("ui")->motion);
	BOOST_TEST(!result.find("game")->buttons);
	BOOST_TEST(!result.find("game")->motion);
}

BOOST_AUTO_TEST_CASE(ADragWhoseHolderIsGoneIsNobodys_AndADragFromOutsideNeverReachesTheLayerItCrosses)
{
	inp::input_stack stack;
	stack.put("game", 0, layer("world"));
	stack.put("ui", 1, region_layer("world", 1, button));

	const auto orphaned = inp::resolve(stack, both, outside, true, { true, true, "gone" });
	for (const char* world : { "game", "ui" }) {
		BOOST_TEST(!orphaned.find(world)->buttons);
		BOOST_TEST(!orphaned.find(world)->motion);
	}

	const auto crossing = inp::resolve(stack, both, inside, true, { true, false, "" });
	BOOST_TEST(crossing.find("game")->buttons);
	BOOST_TEST(!crossing.find("ui")->buttons);
}

BOOST_AUTO_TEST_CASE(AnInactiveLayerReceivesNothingAndTakesNothing)
{
	inp::input_stack stack;
	stack.put("game", 0, layer("world"));
	inp::stack_layer_desc off = layer("world", 4);
	off.active = false;
	off.block_below = true;
	stack.put("menu", 1, off);
	const std::vector<std::string> fed{ "game", "menu" };

	const auto result = inp::resolve(stack, fed, outside, false, {});
	BOOST_TEST(result.find("game")->buttons);
	BOOST_TEST(result.find("game")->keys[key_index(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST(!result.find("menu")->buttons);
	BOOST_TEST(!result.find("menu")->motion);
	BOOST_TEST(!result.find("menu")->keys[key_index(KEYBOARD_BUTTONS::W)]);
}

BOOST_AUTO_TEST_CASE(AWorldThatIsNotFedIsNotInTheResultAndItsLayersTakeNothing)
{
	inp::input_stack stack;
	stack.put("game", 0, layer("world"));
	inp::stack_layer_desc modal = layer("world", 5);
	modal.block_below = true;
	stack.put("menu", 1, modal);

	const auto result = inp::resolve(stack, { "game" }, outside, false, {});
	BOOST_TEST(result.find("menu") == nullptr);
	BOOST_TEST(result.find("game")->buttons);
}

BOOST_AUTO_TEST_CASE(Keys_ALayerTakesItsKeysAndThePrintableOnesAndTheWholeKeyboard)
{
	inp::input_stack stack;
	stack.put("game", 0, layer("world"));
	inp::stack_layer_desc chat = layer("world", 2);
	chat.keys.push_back(KEYBOARD_BUTTONS::TAB);
	chat.text_keys = true;
	stack.put("chat", 1, chat);
	const std::vector<std::string> fed{ "game", "chat" };

	auto result = inp::resolve(stack, fed, outside, false, {});
	const auto& game = result.find("game")->keys;
	const auto& chat_keys = result.find("chat")->keys;
	BOOST_TEST(!game[key_index(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST(!game[key_index(KEYBOARD_BUTTONS::TAB)]);
	BOOST_TEST(game[key_index(KEYBOARD_BUTTONS::ESCAPE)]);
	BOOST_TEST(game[key_index(KEYBOARD_BUTTONS::ENTER)]);
	BOOST_TEST(game[key_index(KEYBOARD_BUTTONS::F5)]);
	BOOST_TEST(game[key_index(KEYBOARD_BUTTONS::LEFT_SHIFT)]);
	BOOST_TEST(chat_keys[key_index(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST(result.find("chat")->keyboard_focus);
	BOOST_TEST(!result.find("game")->keyboard_focus);

	inp::stack_layer_desc whole = layer("world", 2);
	whole.whole_keyboard = true;
	stack.put("chat", 1, whole);
	result = inp::resolve(stack, fed, outside, false, {});
	BOOST_TEST(!result.find("game")->keys[key_index(KEYBOARD_BUTTONS::ESCAPE)]);
}

BOOST_AUTO_TEST_CASE(TextKeys_AreTheOnesOfTheDesignAndNotEscapeEnterTabFunctionKeysOrModifiers)
{
	for (const auto key : { KEYBOARD_BUTTONS::A, KEYBOARD_BUTTONS::Z, KEYBOARD_BUTTONS::K_0, KEYBOARD_BUTTONS::K_9, KEYBOARD_BUTTONS::SPACE,
		KEYBOARD_BUTTONS::SLASH, KEYBOARD_BUTTONS::SEMICOLON, KEYBOARD_BUTTONS::EQUAL, KEYBOARD_BUTTONS::GRAVE_ACCENT,
		KEYBOARD_BUTTONS::LEFT_BRACKET, KEYBOARD_BUTTONS::BACKSLASH, KEYBOARD_BUTTONS::RIGHT_BRACKET, KEYBOARD_BUTTONS::BACKSPACE,
		KEYBOARD_BUTTONS::DELETE, KEYBOARD_BUTTONS::LEFT, KEYBOARD_BUTTONS::UP, KEYBOARD_BUTTONS::HOME, KEYBOARD_BUTTONS::END,
		KEYBOARD_BUTTONS::KP_0, KEYBOARD_BUTTONS::KP_ADD, KEYBOARD_BUTTONS::KP_EQUAL }) {
		BOOST_TEST(inp::is_text_key(key));
	}
	for (const auto key : { KEYBOARD_BUTTONS::ESCAPE, KEYBOARD_BUTTONS::ENTER, KEYBOARD_BUTTONS::KP_ENTER, KEYBOARD_BUTTONS::TAB,
		KEYBOARD_BUTTONS::F1, KEYBOARD_BUTTONS::F25, KEYBOARD_BUTTONS::LEFT_SHIFT, KEYBOARD_BUTTONS::RIGHT_CONTROL, KEYBOARD_BUTTONS::MENU,
		KEYBOARD_BUTTONS::INSERT, KEYBOARD_BUTTONS::PAGE_UP, KEYBOARD_BUTTONS::CAPS_LOCK }) {
		BOOST_TEST(!inp::is_text_key(key));
	}
}

BOOST_AUTO_TEST_CASE(KeyNames_AreTheNamesOfTheEnumAndEveryKeyHasOne)
{
	BOOST_TEST((inp::key_from_name("W").value() == KEYBOARD_BUTTONS::W));
	BOOST_TEST((inp::key_from_name("K_0").value() == KEYBOARD_BUTTONS::K_0));
	BOOST_TEST((inp::key_from_name("ESCAPE").value() == KEYBOARD_BUTTONS::ESCAPE));
	BOOST_TEST((inp::key_from_name("KP_EQUAL").value() == KEYBOARD_BUTTONS::KP_EQUAL));
	BOOST_TEST((inp::key_from_name("MENU").value() == KEYBOARD_BUTTONS::MENU));
	BOOST_TEST((inp::key_from_name("WORLD_2").value() == KEYBOARD_BUTTONS::WORLD_2));
	BOOST_TEST(!inp::key_from_name("w").has_value());
	BOOST_TEST(!inp::key_from_name("").has_value());
}

BOOST_AUTO_TEST_CASE(MouseTaker_IsTheTopLayerThatBlocksOrHoldsTheCursorInARegion)
{
	inp::input_stack stack;
	stack.put("game", 0, layer("world"));
	stack.put("ui", 1, region_layer("world", 1, button));
	BOOST_TEST(inp::mouse_taker(stack, both, inside).value() == "ui");
	BOOST_TEST(!inp::mouse_taker(stack, both, outside).has_value());
	BOOST_TEST(!inp::mouse_taker(stack, { "game" }, inside).has_value());

	inp::stack_layer_desc modal = layer("world", 5);
	modal.block_below = true;
	stack.put("menu", 2, modal);
	BOOST_TEST(inp::mouse_taker(stack, { "game", "ui", "menu" }, outside).value() == "menu");
}

BOOST_AUTO_TEST_CASE(Actions_ALayerThatDeclaresAnActionTakesItsKeys_AndOnlyItsWorldGetsThem)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	stack.put("ui", 0, declaring("world", 5, { "Jump" }));
	stack.put("game", 1, declaring("world", 0, { "Jump" }));
	const inp::stack_resolution resolved = inp::resolve(stack, both, outside, false, {}, &map);

	BOOST_TEST(reached(resolved, "ui", JUMP, KEYBOARD_BUTTONS::SPACE));
	BOOST_TEST(!reached(resolved, "game", JUMP, KEYBOARD_BUTTONS::SPACE));
	// The layer sees the key it takes, the one below does not.
	BOOST_TEST(resolved.find("ui")->keys[key_index(KEYBOARD_BUTTONS::SPACE)]);
	BOOST_TEST(!resolved.find("game")->keys[key_index(KEYBOARD_BUTTONS::SPACE)]);
	// The other keys go through.
	BOOST_TEST(resolved.find("game")->keys[key_index(KEYBOARD_BUTTONS::W)]);
}

BOOST_AUTO_TEST_CASE(Actions_AHalfTakenAction_IsStillThereThroughTheOtherBinding)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	inp::stack_layer_desc top = layer("world", 5);
	top.keys.push_back(KEYBOARD_BUTTONS::A);
	stack.put("ui", 0, std::move(top));
	stack.put("game", 1, declaring("world", 0, { "MoveLeft" }));
	const inp::stack_resolution resolved = inp::resolve(stack, both, outside, false, {}, &map);

	BOOST_TEST(!reached(resolved, "game", MOVE_LEFT, KEYBOARD_BUTTONS::A));
	BOOST_TEST(reached(resolved, "game", MOVE_LEFT, KEYBOARD_BUTTONS::LEFT));
}

BOOST_AUTO_TEST_CASE(Actions_AModalLayerOfTheSameWorldHidesTheActionsOfItsBaseLayer)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	stack.put("game", 0, declaring("world", 0, { "Jump" }));
	inp::stack_layer_desc menu = layer("menu", 1);
	menu.block_below = true;
	stack.put("game", 0, std::move(menu));
	const inp::stack_resolution resolved = inp::resolve(stack, { "game" }, outside, false, {}, &map);

	BOOST_TEST(!reached(resolved, "game", JUMP, KEYBOARD_BUTTONS::SPACE));
	// The modal layer is the world's own: the world still sees the key, just not through the layer that declares Jump.
	BOOST_TEST(resolved.find("game")->keys[key_index(KEYBOARD_BUTTONS::SPACE)]);
}

BOOST_AUTO_TEST_CASE(Actions_AnActionNoLayerDeclaresIsNeverReached_AndWithoutAMapThereAreNone)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	stack.put("game", 0, declaring("world", 0, { "Jump", "NoSuchAction" }));
	const inp::stack_resolution resolved = inp::resolve(stack, { "game" }, outside, false, {}, &map);
	const inp::world_input& game = *resolved.find("game");
	BOOST_REQUIRE_EQUAL(game.actions.size(), map.size());
	BOOST_TEST(game.actions[JUMP].keys.test(key_index(KEYBOARD_BUTTONS::SPACE)));
	BOOST_TEST(game.actions[MOVE_LEFT].keys.none());
	BOOST_TEST(!game.actions[FIRE].buttons);

	BOOST_TEST(inp::resolve(stack, { "game" }, outside, false, {}).find("game")->actions.empty());
}

BOOST_AUTO_TEST_CASE(Actions_AnInactiveLayerDeclaresNothingAndTakesNothing)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	inp::stack_layer_desc top = declaring("world", 5, { "Jump" });
	top.active = false;
	stack.put("ui", 0, std::move(top));
	stack.put("game", 1, declaring("world", 0, { "Jump" }));
	const inp::stack_resolution resolved = inp::resolve(stack, both, outside, false, {}, &map);

	BOOST_TEST(!reached(resolved, "ui", JUMP, KEYBOARD_BUTTONS::SPACE));
	BOOST_TEST(reached(resolved, "game", JUMP, KEYBOARD_BUTTONS::SPACE));
}

BOOST_AUTO_TEST_CASE(Actions_AMouseBindingFollowsTheClickRulesOfTheLayers)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	stack.put("ui", 0, region_layer("world", 5, button));
	stack.put("game", 1, declaring("world", 0, { "Fire" }));

	// Outside the button the game gets the click, over it the ui does and Fire is not reached.
	BOOST_TEST(inp::resolve(stack, both, outside, true, {}, &map).find("game")->actions[FIRE].buttons);
	BOOST_TEST(!inp::resolve(stack, both, inside, true, {}, &map).find("game")->actions[FIRE].buttons);

	// A modal layer over the game takes the click too.
	inp::stack_layer_desc menu = layer("menu", 9);
	menu.block_below = true;
	stack.put("ui", 0, std::move(menu));
	BOOST_TEST(!inp::resolve(stack, both, outside, true, {}, &map).find("game")->actions[FIRE].buttons);
}

BOOST_AUTO_TEST_SUITE_END()
