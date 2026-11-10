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

// What is physically down, as the router keeps it.
struct physical_state
{
	physical_state& down(KEYBOARD_BUTTONS key) { state.keyboard[key_index(key)] = true; return *this; }
	physical_state& up(KEYBOARD_BUTTONS key) { state.keyboard[key_index(key)] = false; return *this; }
	physical_state& down(inp::MOUSE_BUTTONS button) { state.mouse[static_cast<std::size_t>(button)] = true; return *this; }
	physical_state& up(inp::MOUSE_BUTTONS button) { state.mouse[static_cast<std::size_t>(button)] = false; return *this; }
	inp::input_state state{};
};

// resolve and arm_actions of one frame.
inp::stack_resolution frame_of(inp::input_stack& stack, const std::vector<std::string>& fed, glm::vec2 cursor, bool engaged,
	const physical_state& physical, const inp::action_map& map)
{
	inp::stack_resolution resolved = inp::resolve(stack, fed, cursor, engaged, {}, &map);
	inp::arm_actions(stack, resolved, physical.state, &map);
	return resolved;
}

bool armed(const inp::stack_resolution& resolved, const char* world, inp::action_id action, KEYBOARD_BUTTONS key)
{
	const inp::world_input* in = resolved.find(world);
	return in && action < in->actions.size() && in->actions[action].armed_keys.test(key_index(key));
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
	stack.put("game", 0, declaring("world", 0, { "Jump", "Fire" }));
	inp::stack_layer_desc menu = layer("menu", 1);
	menu.block_below = true;
	stack.put("game", 0, std::move(menu));
	const inp::stack_resolution resolved = inp::resolve(stack, { "game" }, outside, false, {}, &map);

	BOOST_TEST(!reached(resolved, "game", JUMP, KEYBOARD_BUTTONS::SPACE));
	// The modal layer is the world's own: the world still sees the key, just not through the layer that declares Jump.
	BOOST_TEST(resolved.find("game")->keys[key_index(KEYBOARD_BUTTONS::SPACE)]);

	// The click lands on the modal layer of the world: the world gets it, the base layer's mouse action does not.
	const inp::stack_resolution clicked = inp::resolve(stack, { "game" }, outside, true, {}, &map);
	BOOST_TEST(clicked.find("game")->buttons);
	BOOST_TEST(!clicked.find("game")->actions[FIRE].buttons);

	// The same click with no modal layer reaches Fire.
	inp::input_stack plain;
	plain.put("game", 0, declaring("world", 0, { "Fire" }));
	BOOST_TEST(inp::resolve(plain, { "game" }, outside, true, {}, &map).find("game")->actions[FIRE].buttons);
}

BOOST_AUTO_TEST_CASE(Actions_ADragKeepsTheMouseActionsOfTheLayerThatTookThePress)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	stack.put("ui", 0, region_layer("world", 5, button));
	stack.put("game", 1, declaring("world", 0, { "Fire" }));
	// The press landed on the game's layer: the drag is the game's and Fire is reached, the ui's region does not matter.
	const inp::drag_hold drag{ true, true, "game", "world" };
	const inp::stack_resolution resolved = inp::resolve(stack, both, inside, true, drag, &map);
	BOOST_TEST(resolved.find("game")->actions[FIRE].buttons);
	BOOST_TEST(!resolved.find("ui")->buttons);
}

BOOST_AUTO_TEST_CASE(Actions_TheDefaultReaderLayerDeclaresEveryAction)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	inp::stack_layer_desc all = layer("world", 5);
	all.all_actions = true;
	stack.put("ui", 0, std::move(all));
	stack.put("game", 1, layer("world", 0));
	const inp::stack_resolution resolved = inp::resolve(stack, both, outside, false, {}, &map);

	BOOST_TEST(reached(resolved, "ui", JUMP, KEYBOARD_BUTTONS::SPACE));
	BOOST_TEST(reached(resolved, "ui", MOVE_LEFT, KEYBOARD_BUTTONS::A));
	BOOST_TEST(reached(resolved, "ui", MOVE_LEFT, KEYBOARD_BUTTONS::LEFT));
	// All of them are taken from the world below.
	BOOST_TEST(!resolved.find("game")->keys[key_index(KEYBOARD_BUTTONS::SPACE)]);
	BOOST_TEST(!resolved.find("game")->keys[key_index(KEYBOARD_BUTTONS::LEFT)]);
	BOOST_TEST(resolved.find("game")->keys[key_index(KEYBOARD_BUTTONS::W)]);
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

BOOST_AUTO_TEST_CASE(Fronts_AKeyHeldWhileTakenIsNotArmedWhenItReachesAgain_UntilItIsLetGo)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	inp::stack_layer_desc menu = layer("world", 5);
	menu.keys.push_back(KEYBOARD_BUTTONS::SPACE);
	stack.put("ui", 0, menu);
	stack.put("game", 1, declaring("world", 0, { "Jump" }));
	physical_state physical;

	// Up: armed everywhere.
	frame_of(stack, both, outside, false, physical, map);
	// Held while the menu takes it: not reached, and the game's layer forgets that it was armed.
	physical.down(KEYBOARD_BUTTONS::SPACE);
	auto taken = frame_of(stack, both, outside, false, physical, map);
	BOOST_TEST(!reached(taken, "game", JUMP, KEYBOARD_BUTTONS::SPACE));
	BOOST_TEST(!armed(taken, "game", JUMP, KEYBOARD_BUTTONS::SPACE));

	// The menu goes while the key is still held: it reaches the game, but is not armed.
	BOOST_REQUIRE(stack.remove("ui", "world"));
	auto given_back = frame_of(stack, both, outside, false, physical, map);
	BOOST_TEST(reached(given_back, "game", JUMP, KEYBOARD_BUTTONS::SPACE));
	BOOST_TEST(!armed(given_back, "game", JUMP, KEYBOARD_BUTTONS::SPACE));

	// Let go: armed again; pressed again: reached and armed, a press.
	physical.up(KEYBOARD_BUTTONS::SPACE);
	frame_of(stack, both, outside, false, physical, map);
	physical.down(KEYBOARD_BUTTONS::SPACE);
	BOOST_TEST(armed(frame_of(stack, both, outside, false, physical, map), "game", JUMP, KEYBOARD_BUTTONS::SPACE));
}

BOOST_AUTO_TEST_CASE(Fronts_ANewLayerDoesNotSeeAKeyThatIsAlreadyHeldAsAPress)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	stack.put("game", 0, declaring("world", 0, { "Jump" }));
	physical_state physical;
	frame_of(stack, { "game" }, outside, false, physical, map);

	// The menu opens on a key that is still down (Escape that opened it must not close it): the new layer starts disarmed.
	physical.down(KEYBOARD_BUTTONS::ESCAPE);
	inp::stack_layer_desc menu = declaring("menu", 5, { "Close" });
	const inp::action_map closing = [] {
		inp::action_map out;
		out.add("Close", { KEYBOARD_BUTTONS::ESCAPE });
		return out;
	}();
	stack.put("game", 0, menu);
	auto opened = frame_of(stack, { "game" }, outside, false, physical, closing);
	BOOST_TEST(reached(opened, "game", 0, KEYBOARD_BUTTONS::ESCAPE));
	BOOST_TEST(!armed(opened, "game", 0, KEYBOARD_BUTTONS::ESCAPE));

	physical.up(KEYBOARD_BUTTONS::ESCAPE);
	frame_of(stack, { "game" }, outside, false, physical, closing);
	physical.down(KEYBOARD_BUTTONS::ESCAPE);
	BOOST_TEST(armed(frame_of(stack, { "game" }, outside, false, physical, closing), "game", 0, KEYBOARD_BUTTONS::ESCAPE));
}

BOOST_AUTO_TEST_CASE(Fronts_OneBindingHeldAndTakenForGood_DoesNotDisarmTheOther)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	inp::stack_layer_desc top = layer("world", 5);
	top.keys.push_back(KEYBOARD_BUTTONS::A);
	stack.put("ui", 0, top);
	stack.put("game", 1, declaring("world", 0, { "MoveLeft" }));
	physical_state physical;
	frame_of(stack, both, outside, false, physical, map);

	physical.down(KEYBOARD_BUTTONS::A);
	frame_of(stack, both, outside, false, physical, map);
	physical.down(KEYBOARD_BUTTONS::LEFT);
	auto resolved = frame_of(stack, both, outside, false, physical, map);
	BOOST_TEST(!armed(resolved, "game", MOVE_LEFT, KEYBOARD_BUTTONS::A));
	BOOST_TEST(armed(resolved, "game", MOVE_LEFT, KEYBOARD_BUTTONS::LEFT));
}

BOOST_AUTO_TEST_CASE(Fronts_TheMouseOverAnotherLayerWithoutAButton_LeavesTheClickArmed)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	stack.put("ui", 0, region_layer("world", 5, button));
	stack.put("game", 1, declaring("world", 0, { "Fire" }));
	physical_state physical;

	// The cursor is over the ui's region, no button down: Fire does not reach, and stays armed.
	frame_of(stack, both, inside, false, physical, map);
	frame_of(stack, both, inside, false, physical, map);
	physical.down(inp::MOUSE_BUTTONS::LEFT);
	const auto clicked = frame_of(stack, both, outside, true, physical, map);
	const inp::action_reach& fire = clicked.find("game")->actions[FIRE];
	BOOST_TEST(fire.buttons);
	BOOST_TEST(fire.armed_buttons.test(static_cast<std::size_t>(inp::MOUSE_BUTTONS::LEFT)));
}

BOOST_AUTO_TEST_CASE(Fronts_TheMemoryGoesWithItsLayerItsWorldAndItsOwner)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	stack.put("game", 0, declaring("world", 0, { "Jump" }));
	inp::stack_layer_desc modules_menu = layer("menu", 5);
	modules_menu.owner = inp::layer_owner::module;
	stack.put("game", 0, modules_menu);
	stack.put("hud", 1, declaring("world", 0, { "Jump" }));
	physical_state physical;
	frame_of(stack, { "game", "hud" }, outside, false, physical, map);
	BOOST_TEST(stack.memory_count() == 3u);

	// A module unloads (or reloads): its layers and their memory go, the engine's stay.
	stack.remove_owner(inp::layer_owner::module);
	BOOST_TEST(stack.memory_count() == 2u);
	BOOST_TEST(stack.find_memory("game", "menu") == nullptr);
	BOOST_TEST(stack.find_memory("game", "world") != nullptr);

	// A world dies: the memory of its layers goes.
	stack.retain_worlds({ "game" });
	BOOST_TEST(stack.memory_count() == 1u);
	BOOST_TEST(stack.find_memory("hud", "world") == nullptr);

	// One layer is removed.
	BOOST_TEST(stack.remove("game", "world"));
	BOOST_TEST(stack.memory_count() == 0u);
	BOOST_TEST(!stack.remove("game", "world"));
}

BOOST_AUTO_TEST_CASE(Fronts_TheSameLayerPutAgainKeepsItsMemory_AndAWorldMadeAgainStartsAnew)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	stack.put("game", 3, declaring("world", 0, { "Jump" }));
	physical_state physical;
	frame_of(stack, { "game" }, outside, false, physical, map);

	// The base layer of every world is put every frame: it is the same layer.
	physical.down(KEYBOARD_BUTTONS::SPACE);
	stack.put("game", 3, declaring("world", 0, { "Jump" }));
	auto held = frame_of(stack, { "game" }, outside, false, physical, map);
	BOOST_TEST(armed(held, "game", JUMP, KEYBOARD_BUTTONS::SPACE));

	// A world of the same name made again (another desc_index) while the key is held starts disarmed for it.
	stack.put("game", 4, declaring("world", 0, { "Jump" }));
	BOOST_TEST(stack.find_memory("game", "world") == nullptr);
	auto again = frame_of(stack, { "game" }, outside, false, physical, map);
	BOOST_TEST(reached(again, "game", JUMP, KEYBOARD_BUTTONS::SPACE));
	BOOST_TEST(!armed(again, "game", JUMP, KEYBOARD_BUTTONS::SPACE));
}

BOOST_AUTO_TEST_CASE(Fronts_AWorldThatDiesWhileItTakesAHeldKey_LeavesTheOnesBelowDisarmedUntilItIsLetGo)
{
	const inp::action_map map = make_actions();
	inp::input_stack stack;
	inp::stack_layer_desc menu = layer("world", 5);
	menu.block_below = true;
	stack.put("menu", 0, menu);
	stack.put("game", 1, declaring("world", 0, { "Jump" }));
	const std::vector<std::string> fed{ "game", "menu" };
	physical_state physical;
	frame_of(stack, fed, outside, false, physical, map);

	physical.down(KEYBOARD_BUTTONS::SPACE);
	frame_of(stack, fed, outside, false, physical, map);

	// The menu world dies with the key held: its memory goes, the game's layer is still disarmed for that key.
	stack.retain_worlds({ "game" });
	BOOST_TEST(stack.find_memory("menu", "world") == nullptr);
	auto after = frame_of(stack, { "game" }, outside, false, physical, map);
	BOOST_TEST(reached(after, "game", JUMP, KEYBOARD_BUTTONS::SPACE));
	BOOST_TEST(!armed(after, "game", JUMP, KEYBOARD_BUTTONS::SPACE));

	physical.up(KEYBOARD_BUTTONS::SPACE);
	frame_of(stack, { "game" }, outside, false, physical, map);
	physical.down(KEYBOARD_BUTTONS::SPACE);
	BOOST_TEST(armed(frame_of(stack, { "game" }, outside, false, physical, map), "game", JUMP, KEYBOARD_BUTTONS::SPACE));
}

BOOST_AUTO_TEST_SUITE_END()
