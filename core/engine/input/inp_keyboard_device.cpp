#include "inp_keyboard_device.h"
#include <timer.hpp>
#include <engine_log.h>

void inp::KeyboardDevice::on_key_action(KEYBOARD_BUTTONS keycode, int scancode, KEY_ACTION action, int mode)
{
    if (action == KEY_ACTION::NONE) {
        return;
    }

    on_key_keyboard_action(keycode, action, (float)Timer::now());
    onKeyStateChanged(keycode, action);
    egLOG("input/keyboard", "Key {0}: action {1}", (int)keycode, (int)action);
}

void inp::KeyboardDevice::on_key_keyboard_action(KEYBOARD_BUTTONS keycode, KEY_ACTION action, float time)
{
    Key& state = keys_[keycode];
    prev_keys_[keycode] = state;
    state.action = action;
}

inp::Key inp::KeyboardDevice::get_some_key(KEYBOARD_BUTTONS keycode, const KeyContainer& keys)
{
    auto it = keys.find(keycode);
    if (it != keys.end())
        return it->second;
    return {};
}
