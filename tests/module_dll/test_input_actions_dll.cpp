#include <snk/game/gs_input_actions.h>
#include <snk/input/inp_actions.h>
#include <cstddef>
#include <variant>

// A module as its own shared library, linked as a game.dll is (US-542-C): it reads the action map inline and calls the
// exports of gs::input_actions, so a change of a signature or of a layout it relies on shows up as a library that does not
// load or reads wrong. gs_input_actions_dll_tests.cpp loads it.

#if defined(_WIN32)
#define SNK_TEST_INPUT_EXPORT extern "C" __declspec(dllexport)
#else
#define SNK_TEST_INPUT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

const inp::action* find_action(const inp::action_map* map, const char* name)
{
	// actions() is inline: the offset of the vector and the size of an action are the module's own copy.
	for (const inp::action& action : map->actions()) {
		if (action.name == name) return &action;
	}
	return nullptr;
}

}

// The keyboard key at a slot of an action; -1 for none, -2 for a binding that is not a key, -3 for an action not there.
SNK_TEST_INPUT_EXPORT int snk_test_key_at(const inp::action_map* map, const char* action, int slot)
{
	const inp::action* found = find_action(map, action);
	if (!found) return -3;
	if (slot < 0 || static_cast<std::size_t>(slot) >= found->bindings.size()) return -1;
	const auto* key = std::get_if<inp::KEYBOARD_BUTTONS>(&found->bindings[static_cast<std::size_t>(slot)]);
	return key ? static_cast<int>(*key) : -2;
}

SNK_TEST_INPUT_EXPORT int snk_test_binding_count(const inp::action_map* map, const char* action)
{
	const inp::action* found = find_action(map, action);
	return found ? static_cast<int>(found->bindings.size()) : -3;
}

// allow < 0: the three-argument rebind of 3.16 (a module built for it imports that symbol); else the four-argument one.
SNK_TEST_INPUT_EXPORT int snk_test_rebind(gs::input_actions* actions, const char* action, int slot, int key, int allow)
{
	const inp::action_binding binding = static_cast<inp::KEYBOARD_BUTTONS>(key);
	const auto slot_index = static_cast<std::size_t>(slot);
	const gs::rebind_outcome outcome = allow < 0 ? actions->rebind(action, slot_index, binding)
		: actions->rebind(action, slot_index, binding, allow != 0);
	return static_cast<int>(outcome.result.status);
}

SNK_TEST_INPUT_EXPORT int snk_test_remove(gs::input_actions* actions, const char* action, int slot)
{
	return static_cast<int>(actions->remove_binding(action, static_cast<std::size_t>(slot)).result.status);
}
