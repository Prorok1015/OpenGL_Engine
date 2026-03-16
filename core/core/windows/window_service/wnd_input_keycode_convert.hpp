#pragma once
#include "inp_keyboard_event.hpp"
#include <GLFW/glfw3.h>

namespace wnd::convert
{
    core::inp::KEY_ACTION to_action(int action)
    {
        switch (action)
        {
        case GLFW_RELEASE: return core::inp::KEY_ACTION::UP; break;
        case GLFW_PRESS: return core::inp::KEY_ACTION::DOWN; break;
            //case GLFW_REPEAT:;
        }

        return core::inp::KEY_ACTION::NONE;
    }

    int to_glfw_keycode(core::inp::KEYBOARD_BUTTONS button) {
        switch (button) {
        case core::inp::KEYBOARD_BUTTONS::SPACE: return GLFW_KEY_SPACE;
        case core::inp::KEYBOARD_BUTTONS::APOSTROPHE: return GLFW_KEY_APOSTROPHE;
        case core::inp::KEYBOARD_BUTTONS::COMMA: return GLFW_KEY_COMMA;
        case core::inp::KEYBOARD_BUTTONS::MINUS: return GLFW_KEY_MINUS;
        case core::inp::KEYBOARD_BUTTONS::PERIOD: return GLFW_KEY_PERIOD;
        case core::inp::KEYBOARD_BUTTONS::SLASH: return GLFW_KEY_SLASH;
        case core::inp::KEYBOARD_BUTTONS::K_0: return GLFW_KEY_0;
        case core::inp::KEYBOARD_BUTTONS::K_1: return GLFW_KEY_1;
        case core::inp::KEYBOARD_BUTTONS::K_2: return GLFW_KEY_2;
        case core::inp::KEYBOARD_BUTTONS::K_3: return GLFW_KEY_3;
        case core::inp::KEYBOARD_BUTTONS::K_4: return GLFW_KEY_4;
        case core::inp::KEYBOARD_BUTTONS::K_5: return GLFW_KEY_5;
        case core::inp::KEYBOARD_BUTTONS::K_6: return GLFW_KEY_6;
        case core::inp::KEYBOARD_BUTTONS::K_7: return GLFW_KEY_7;
        case core::inp::KEYBOARD_BUTTONS::K_8: return GLFW_KEY_8;
        case core::inp::KEYBOARD_BUTTONS::K_9: return GLFW_KEY_9;
        case core::inp::KEYBOARD_BUTTONS::SEMICOLON: return GLFW_KEY_SEMICOLON;
        case core::inp::KEYBOARD_BUTTONS::EQUAL: return GLFW_KEY_EQUAL;
        case core::inp::KEYBOARD_BUTTONS::A: return GLFW_KEY_A;
        case core::inp::KEYBOARD_BUTTONS::B: return GLFW_KEY_B;
        case core::inp::KEYBOARD_BUTTONS::C: return GLFW_KEY_C;
        case core::inp::KEYBOARD_BUTTONS::D: return GLFW_KEY_D;
        case core::inp::KEYBOARD_BUTTONS::E: return GLFW_KEY_E;
        case core::inp::KEYBOARD_BUTTONS::F: return GLFW_KEY_F;
        case core::inp::KEYBOARD_BUTTONS::G: return GLFW_KEY_G;
        case core::inp::KEYBOARD_BUTTONS::H: return GLFW_KEY_H;
        case core::inp::KEYBOARD_BUTTONS::I: return GLFW_KEY_I;
        case core::inp::KEYBOARD_BUTTONS::J: return GLFW_KEY_J;
        case core::inp::KEYBOARD_BUTTONS::K: return GLFW_KEY_K;
        case core::inp::KEYBOARD_BUTTONS::L: return GLFW_KEY_L;
        case core::inp::KEYBOARD_BUTTONS::M: return GLFW_KEY_M;
        case core::inp::KEYBOARD_BUTTONS::N: return GLFW_KEY_N;
        case core::inp::KEYBOARD_BUTTONS::O: return GLFW_KEY_O;
        case core::inp::KEYBOARD_BUTTONS::P: return GLFW_KEY_P;
        case core::inp::KEYBOARD_BUTTONS::Q: return GLFW_KEY_Q;
        case core::inp::KEYBOARD_BUTTONS::R: return GLFW_KEY_R;
        case core::inp::KEYBOARD_BUTTONS::S: return GLFW_KEY_S;
        case core::inp::KEYBOARD_BUTTONS::T: return GLFW_KEY_T;
        case core::inp::KEYBOARD_BUTTONS::U: return GLFW_KEY_U;
        case core::inp::KEYBOARD_BUTTONS::V: return GLFW_KEY_V;
        case core::inp::KEYBOARD_BUTTONS::W: return GLFW_KEY_W;
        case core::inp::KEYBOARD_BUTTONS::X: return GLFW_KEY_X;
        case core::inp::KEYBOARD_BUTTONS::Y: return GLFW_KEY_Y;
        case core::inp::KEYBOARD_BUTTONS::Z: return GLFW_KEY_Z;
        case core::inp::KEYBOARD_BUTTONS::ESCAPE: return GLFW_KEY_ESCAPE;
        case core::inp::KEYBOARD_BUTTONS::ENTER: return GLFW_KEY_ENTER;
        case core::inp::KEYBOARD_BUTTONS::TAB: return GLFW_KEY_TAB;
        case core::inp::KEYBOARD_BUTTONS::BACKSPACE: return GLFW_KEY_BACKSPACE;
        case core::inp::KEYBOARD_BUTTONS::INSERT: return GLFW_KEY_INSERT;
        case core::inp::KEYBOARD_BUTTONS::DELETE: return GLFW_KEY_DELETE;
        case core::inp::KEYBOARD_BUTTONS::RIGHT: return GLFW_KEY_RIGHT;
        case core::inp::KEYBOARD_BUTTONS::LEFT: return GLFW_KEY_LEFT;
        case core::inp::KEYBOARD_BUTTONS::DOWN: return GLFW_KEY_DOWN;
        case core::inp::KEYBOARD_BUTTONS::UP: return GLFW_KEY_UP;
        case core::inp::KEYBOARD_BUTTONS::PAGE_UP: return GLFW_KEY_PAGE_UP;
        case core::inp::KEYBOARD_BUTTONS::PAGE_DOWN: return GLFW_KEY_PAGE_DOWN;
        case core::inp::KEYBOARD_BUTTONS::HOME: return GLFW_KEY_HOME;
        case core::inp::KEYBOARD_BUTTONS::END: return GLFW_KEY_END;
        case core::inp::KEYBOARD_BUTTONS::CAPS_LOCK: return GLFW_KEY_CAPS_LOCK;
        case core::inp::KEYBOARD_BUTTONS::SCROLL_LOCK: return GLFW_KEY_SCROLL_LOCK;
        case core::inp::KEYBOARD_BUTTONS::NUM_LOCK: return GLFW_KEY_NUM_LOCK;
        case core::inp::KEYBOARD_BUTTONS::PRINT_SCREEN: return GLFW_KEY_PRINT_SCREEN;
        case core::inp::KEYBOARD_BUTTONS::PAUSE: return GLFW_KEY_PAUSE;
        case core::inp::KEYBOARD_BUTTONS::F1: return GLFW_KEY_F1;
        case core::inp::KEYBOARD_BUTTONS::F2: return GLFW_KEY_F2;
        case core::inp::KEYBOARD_BUTTONS::F3: return GLFW_KEY_F3;
        case core::inp::KEYBOARD_BUTTONS::F4: return GLFW_KEY_F4;
        case core::inp::KEYBOARD_BUTTONS::F5: return GLFW_KEY_F5;
        case core::inp::KEYBOARD_BUTTONS::F6: return GLFW_KEY_F6;
        case core::inp::KEYBOARD_BUTTONS::F7: return GLFW_KEY_F7;
        case core::inp::KEYBOARD_BUTTONS::F8: return GLFW_KEY_F8;
        case core::inp::KEYBOARD_BUTTONS::F9: return GLFW_KEY_F9;
        case core::inp::KEYBOARD_BUTTONS::F10: return GLFW_KEY_F10;
        case core::inp::KEYBOARD_BUTTONS::F11: return GLFW_KEY_F11;
        case core::inp::KEYBOARD_BUTTONS::F12: return GLFW_KEY_F12;
        case core::inp::KEYBOARD_BUTTONS::F13: return GLFW_KEY_F13;
        case core::inp::KEYBOARD_BUTTONS::F14: return GLFW_KEY_F14;
        case core::inp::KEYBOARD_BUTTONS::F15: return GLFW_KEY_F15;
        case core::inp::KEYBOARD_BUTTONS::F16: return GLFW_KEY_F16;
        case core::inp::KEYBOARD_BUTTONS::F17: return GLFW_KEY_F17;
        case core::inp::KEYBOARD_BUTTONS::F18: return GLFW_KEY_F18;
        case core::inp::KEYBOARD_BUTTONS::F19: return GLFW_KEY_F19;
        case core::inp::KEYBOARD_BUTTONS::F20: return GLFW_KEY_F20;
        case core::inp::KEYBOARD_BUTTONS::F21: return GLFW_KEY_F21;
        case core::inp::KEYBOARD_BUTTONS::F22: return GLFW_KEY_F22;
        case core::inp::KEYBOARD_BUTTONS::F23: return GLFW_KEY_F23;
        case core::inp::KEYBOARD_BUTTONS::F24: return GLFW_KEY_F24;
        case core::inp::KEYBOARD_BUTTONS::F25: return GLFW_KEY_F25;
        case core::inp::KEYBOARD_BUTTONS::KP_0: return GLFW_KEY_KP_0;
        case core::inp::KEYBOARD_BUTTONS::KP_1: return GLFW_KEY_KP_1;
        case core::inp::KEYBOARD_BUTTONS::KP_2: return GLFW_KEY_KP_2;
        case core::inp::KEYBOARD_BUTTONS::KP_3: return GLFW_KEY_KP_3;
        case core::inp::KEYBOARD_BUTTONS::KP_4: return GLFW_KEY_KP_4;
        case core::inp::KEYBOARD_BUTTONS::KP_5: return GLFW_KEY_KP_5;
        case core::inp::KEYBOARD_BUTTONS::KP_6: return GLFW_KEY_KP_6;
        case core::inp::KEYBOARD_BUTTONS::KP_7: return GLFW_KEY_KP_7;
        case core::inp::KEYBOARD_BUTTONS::KP_8: return GLFW_KEY_KP_8;
        case core::inp::KEYBOARD_BUTTONS::KP_9: return GLFW_KEY_KP_9;
        case core::inp::KEYBOARD_BUTTONS::KP_DECIMAL: return GLFW_KEY_KP_DECIMAL;
        case core::inp::KEYBOARD_BUTTONS::KP_MULTIPLY: return GLFW_KEY_KP_MULTIPLY;
        case core::inp::KEYBOARD_BUTTONS::KP_SUBTRACT: return GLFW_KEY_KP_SUBTRACT;
        case core::inp::KEYBOARD_BUTTONS::KP_ADD: return GLFW_KEY_KP_ADD;
        case core::inp::KEYBOARD_BUTTONS::KP_ENTER: return GLFW_KEY_KP_ENTER;
        case core::inp::KEYBOARD_BUTTONS::KP_EQUAL: return GLFW_KEY_KP_EQUAL;
        case core::inp::KEYBOARD_BUTTONS::LEFT_SHIFT: return GLFW_KEY_LEFT_SHIFT;
        case core::inp::KEYBOARD_BUTTONS::LEFT_CONTROL: return GLFW_KEY_LEFT_CONTROL;
        case core::inp::KEYBOARD_BUTTONS::LEFT_ALT: return GLFW_KEY_LEFT_ALT;
        case core::inp::KEYBOARD_BUTTONS::LEFT_SUPER: return GLFW_KEY_LEFT_SUPER;
        case core::inp::KEYBOARD_BUTTONS::RIGHT_SHIFT: return GLFW_KEY_RIGHT_SHIFT;
        case core::inp::KEYBOARD_BUTTONS::RIGHT_CONTROL: return GLFW_KEY_RIGHT_CONTROL;
        case core::inp::KEYBOARD_BUTTONS::RIGHT_ALT: return GLFW_KEY_RIGHT_ALT;
        case core::inp::KEYBOARD_BUTTONS::RIGHT_SUPER: return GLFW_KEY_RIGHT_SUPER;
        case core::inp::KEYBOARD_BUTTONS::MENU: return GLFW_KEY_MENU;
        }
        return -1;
    }

    std::optional<core::inp::KEYBOARD_BUTTONS> to_keyboard_keycode(int glfw_value) {
        switch (glfw_value) {
        case GLFW_KEY_SPACE:       return core::inp::KEYBOARD_BUTTONS::SPACE;
        case GLFW_KEY_APOSTROPHE:  return core::inp::KEYBOARD_BUTTONS::APOSTROPHE;
        case GLFW_KEY_COMMA:       return core::inp::KEYBOARD_BUTTONS::COMMA;
        case GLFW_KEY_MINUS:       return core::inp::KEYBOARD_BUTTONS::MINUS;
        case GLFW_KEY_PERIOD:      return core::inp::KEYBOARD_BUTTONS::PERIOD;
        case GLFW_KEY_SLASH:       return core::inp::KEYBOARD_BUTTONS::SLASH;
        case GLFW_KEY_0:           return core::inp::KEYBOARD_BUTTONS::K_0;
        case GLFW_KEY_1:           return core::inp::KEYBOARD_BUTTONS::K_1;
        case GLFW_KEY_2:           return core::inp::KEYBOARD_BUTTONS::K_2;
        case GLFW_KEY_3:           return core::inp::KEYBOARD_BUTTONS::K_3;
        case GLFW_KEY_4:           return core::inp::KEYBOARD_BUTTONS::K_4;
        case GLFW_KEY_5:           return core::inp::KEYBOARD_BUTTONS::K_5;
        case GLFW_KEY_6:           return core::inp::KEYBOARD_BUTTONS::K_6;
        case GLFW_KEY_7:           return core::inp::KEYBOARD_BUTTONS::K_7;
        case GLFW_KEY_8:           return core::inp::KEYBOARD_BUTTONS::K_8;
        case GLFW_KEY_9:           return core::inp::KEYBOARD_BUTTONS::K_9;
        case GLFW_KEY_SEMICOLON:   return core::inp::KEYBOARD_BUTTONS::SEMICOLON;
        case GLFW_KEY_EQUAL:       return core::inp::KEYBOARD_BUTTONS::EQUAL;
        case GLFW_KEY_A:           return core::inp::KEYBOARD_BUTTONS::A;
        case GLFW_KEY_B:           return core::inp::KEYBOARD_BUTTONS::B;
        case GLFW_KEY_C:           return core::inp::KEYBOARD_BUTTONS::C;
        case GLFW_KEY_D:           return core::inp::KEYBOARD_BUTTONS::D;
        case GLFW_KEY_E:           return core::inp::KEYBOARD_BUTTONS::E;
        case GLFW_KEY_F:           return core::inp::KEYBOARD_BUTTONS::F;
        case GLFW_KEY_G:           return core::inp::KEYBOARD_BUTTONS::G;
        case GLFW_KEY_H:           return core::inp::KEYBOARD_BUTTONS::H;
        case GLFW_KEY_I:           return core::inp::KEYBOARD_BUTTONS::I;
        case GLFW_KEY_J:           return core::inp::KEYBOARD_BUTTONS::J;
        case GLFW_KEY_K:           return core::inp::KEYBOARD_BUTTONS::K;
        case GLFW_KEY_L:           return core::inp::KEYBOARD_BUTTONS::L;
        case GLFW_KEY_M:           return core::inp::KEYBOARD_BUTTONS::M;
        case GLFW_KEY_N:           return core::inp::KEYBOARD_BUTTONS::N;
        case GLFW_KEY_O:           return core::inp::KEYBOARD_BUTTONS::O;
        case GLFW_KEY_P:           return core::inp::KEYBOARD_BUTTONS::P;
        case GLFW_KEY_Q:           return core::inp::KEYBOARD_BUTTONS::Q;
        case GLFW_KEY_R:           return core::inp::KEYBOARD_BUTTONS::R;
        case GLFW_KEY_S:           return core::inp::KEYBOARD_BUTTONS::S;
        case GLFW_KEY_T:           return core::inp::KEYBOARD_BUTTONS::T;
        case GLFW_KEY_U:           return core::inp::KEYBOARD_BUTTONS::U;
        case GLFW_KEY_V:           return core::inp::KEYBOARD_BUTTONS::V;
        case GLFW_KEY_W:           return core::inp::KEYBOARD_BUTTONS::W;
        case GLFW_KEY_X:           return core::inp::KEYBOARD_BUTTONS::X;
        case GLFW_KEY_Y:           return core::inp::KEYBOARD_BUTTONS::Y;
        case GLFW_KEY_Z:           return core::inp::KEYBOARD_BUTTONS::Z;
        case GLFW_KEY_ESCAPE:      return core::inp::KEYBOARD_BUTTONS::ESCAPE;
        case GLFW_KEY_ENTER:       return core::inp::KEYBOARD_BUTTONS::ENTER;
        case GLFW_KEY_TAB:         return core::inp::KEYBOARD_BUTTONS::TAB;
        case GLFW_KEY_BACKSPACE:   return core::inp::KEYBOARD_BUTTONS::BACKSPACE;
        case GLFW_KEY_INSERT:      return core::inp::KEYBOARD_BUTTONS::INSERT;
        case GLFW_KEY_DELETE:      return core::inp::KEYBOARD_BUTTONS::DELETE;
        case GLFW_KEY_RIGHT:       return core::inp::KEYBOARD_BUTTONS::RIGHT;
        case GLFW_KEY_LEFT:        return core::inp::KEYBOARD_BUTTONS::LEFT;
        case GLFW_KEY_DOWN:        return core::inp::KEYBOARD_BUTTONS::DOWN;
        case GLFW_KEY_UP:          return core::inp::KEYBOARD_BUTTONS::UP;
        case GLFW_KEY_PAGE_UP:     return core::inp::KEYBOARD_BUTTONS::PAGE_UP;
        case GLFW_KEY_PAGE_DOWN:   return core::inp::KEYBOARD_BUTTONS::PAGE_DOWN;
        case GLFW_KEY_HOME:        return core::inp::KEYBOARD_BUTTONS::HOME;
        case GLFW_KEY_END:         return core::inp::KEYBOARD_BUTTONS::END;
        case GLFW_KEY_CAPS_LOCK:   return core::inp::KEYBOARD_BUTTONS::CAPS_LOCK;
        case GLFW_KEY_SCROLL_LOCK: return core::inp::KEYBOARD_BUTTONS::SCROLL_LOCK;
        case GLFW_KEY_NUM_LOCK:    return core::inp::KEYBOARD_BUTTONS::NUM_LOCK;
        case GLFW_KEY_PRINT_SCREEN:return core::inp::KEYBOARD_BUTTONS::PRINT_SCREEN;
        case GLFW_KEY_PAUSE:       return core::inp::KEYBOARD_BUTTONS::PAUSE;
        case GLFW_KEY_F1:          return core::inp::KEYBOARD_BUTTONS::F1;
        case GLFW_KEY_F2:          return core::inp::KEYBOARD_BUTTONS::F2;
        case GLFW_KEY_F3:          return core::inp::KEYBOARD_BUTTONS::F3;
        case GLFW_KEY_F4:          return core::inp::KEYBOARD_BUTTONS::F4;
        case GLFW_KEY_F5:          return core::inp::KEYBOARD_BUTTONS::F5;
        case GLFW_KEY_F6:          return core::inp::KEYBOARD_BUTTONS::F6;
        case GLFW_KEY_F7:          return core::inp::KEYBOARD_BUTTONS::F7;
        case GLFW_KEY_F8:          return core::inp::KEYBOARD_BUTTONS::F8;
        case GLFW_KEY_F9:          return core::inp::KEYBOARD_BUTTONS::F9;
        case GLFW_KEY_F10:         return core::inp::KEYBOARD_BUTTONS::F10;
        case GLFW_KEY_F11:         return core::inp::KEYBOARD_BUTTONS::F11;
        case GLFW_KEY_F12:         return core::inp::KEYBOARD_BUTTONS::F12;
        case GLFW_KEY_F13:         return core::inp::KEYBOARD_BUTTONS::F13;
        case GLFW_KEY_F14:         return core::inp::KEYBOARD_BUTTONS::F14;
        case GLFW_KEY_F15:         return core::inp::KEYBOARD_BUTTONS::F15;
        case GLFW_KEY_F16:         return core::inp::KEYBOARD_BUTTONS::F16;
        case GLFW_KEY_F17:         return core::inp::KEYBOARD_BUTTONS::F17;
        case GLFW_KEY_F18:         return core::inp::KEYBOARD_BUTTONS::F18;
        case GLFW_KEY_F19:         return core::inp::KEYBOARD_BUTTONS::F19;
        case GLFW_KEY_F20:         return core::inp::KEYBOARD_BUTTONS::F20;
        case GLFW_KEY_F21:         return core::inp::KEYBOARD_BUTTONS::F21;
        case GLFW_KEY_F22:         return core::inp::KEYBOARD_BUTTONS::F22;
        case GLFW_KEY_F23:         return core::inp::KEYBOARD_BUTTONS::F23;
        case GLFW_KEY_F24:         return core::inp::KEYBOARD_BUTTONS::F24;
        case GLFW_KEY_F25:         return core::inp::KEYBOARD_BUTTONS::F25;
        case GLFW_KEY_KP_0:        return core::inp::KEYBOARD_BUTTONS::KP_0;
        case GLFW_KEY_KP_1:        return core::inp::KEYBOARD_BUTTONS::KP_1;
        case GLFW_KEY_KP_2:        return core::inp::KEYBOARD_BUTTONS::KP_2;
        case GLFW_KEY_KP_3:        return core::inp::KEYBOARD_BUTTONS::KP_3;
        case GLFW_KEY_KP_4:        return core::inp::KEYBOARD_BUTTONS::KP_4;
        case GLFW_KEY_KP_5:        return core::inp::KEYBOARD_BUTTONS::KP_5;
        case GLFW_KEY_KP_6:        return core::inp::KEYBOARD_BUTTONS::KP_6;
        case GLFW_KEY_KP_7:        return core::inp::KEYBOARD_BUTTONS::KP_7;
        case GLFW_KEY_KP_8:        return core::inp::KEYBOARD_BUTTONS::KP_8;
        case GLFW_KEY_KP_9:        return core::inp::KEYBOARD_BUTTONS::KP_9;
        case GLFW_KEY_KP_DECIMAL:  return core::inp::KEYBOARD_BUTTONS::KP_DECIMAL;
        case GLFW_KEY_KP_DIVIDE:   return core::inp::KEYBOARD_BUTTONS::KP_DIVIDE;
        case GLFW_KEY_KP_MULTIPLY: return core::inp::KEYBOARD_BUTTONS::KP_MULTIPLY;
        case GLFW_KEY_KP_SUBTRACT: return core::inp::KEYBOARD_BUTTONS::KP_SUBTRACT;
        case GLFW_KEY_KP_ADD:      return core::inp::KEYBOARD_BUTTONS::KP_ADD;
        case GLFW_KEY_KP_ENTER:    return core::inp::KEYBOARD_BUTTONS::KP_ENTER;
        case GLFW_KEY_KP_EQUAL:    return core::inp::KEYBOARD_BUTTONS::KP_EQUAL;
        case GLFW_KEY_LEFT_SHIFT:  return core::inp::KEYBOARD_BUTTONS::LEFT_SHIFT;
        case GLFW_KEY_LEFT_CONTROL:return core::inp::KEYBOARD_BUTTONS::LEFT_CONTROL;
        case GLFW_KEY_LEFT_ALT:    return core::inp::KEYBOARD_BUTTONS::LEFT_ALT;
        case GLFW_KEY_LEFT_SUPER:  return core::inp::KEYBOARD_BUTTONS::LEFT_SUPER;
        case GLFW_KEY_RIGHT_SHIFT: return core::inp::KEYBOARD_BUTTONS::RIGHT_SHIFT;
        case GLFW_KEY_RIGHT_CONTROL:return core::inp::KEYBOARD_BUTTONS::RIGHT_CONTROL;
        case GLFW_KEY_RIGHT_ALT:   return core::inp::KEYBOARD_BUTTONS::RIGHT_ALT;
        case GLFW_KEY_RIGHT_SUPER: return core::inp::KEYBOARD_BUTTONS::RIGHT_SUPER;
        case GLFW_KEY_MENU:        return core::inp::KEYBOARD_BUTTONS::MENU;
        }
        return std::nullopt;
    }

    int to_glfw_keycode(core::inp::MOUSE_BUTTONS button) {
        switch (button) {
        case core::inp::MOUSE_BUTTONS::LEFT:   return GLFW_MOUSE_BUTTON_LEFT;
        case core::inp::MOUSE_BUTTONS::RIGHT:  return GLFW_MOUSE_BUTTON_RIGHT;
        case core::inp::MOUSE_BUTTONS::MIDDLE: return GLFW_MOUSE_BUTTON_MIDDLE;
        case core::inp::MOUSE_BUTTONS::BTN_4:  return GLFW_MOUSE_BUTTON_4;
        case core::inp::MOUSE_BUTTONS::BTN_5:  return GLFW_MOUSE_BUTTON_5;
        case core::inp::MOUSE_BUTTONS::BTN_6:  return GLFW_MOUSE_BUTTON_6;
        case core::inp::MOUSE_BUTTONS::BTN_7:  return GLFW_MOUSE_BUTTON_7;
        case core::inp::MOUSE_BUTTONS::BTN_8:  return GLFW_MOUSE_BUTTON_8;
        }
        return -1;
    }

    std::optional<core::inp::MOUSE_BUTTONS> to_mouse_keycode(int glfw_value) {
        switch (glfw_value) {
        case GLFW_MOUSE_BUTTON_LEFT:   return core::inp::MOUSE_BUTTONS::LEFT;
        case GLFW_MOUSE_BUTTON_RIGHT:  return core::inp::MOUSE_BUTTONS::RIGHT;
        case GLFW_MOUSE_BUTTON_MIDDLE: return core::inp::MOUSE_BUTTONS::MIDDLE;
        case GLFW_MOUSE_BUTTON_4:  return core::inp::MOUSE_BUTTONS::BTN_4;
        case GLFW_MOUSE_BUTTON_5:  return core::inp::MOUSE_BUTTONS::BTN_5;
        case GLFW_MOUSE_BUTTON_6:  return core::inp::MOUSE_BUTTONS::BTN_6;
        case GLFW_MOUSE_BUTTON_7:  return core::inp::MOUSE_BUTTONS::BTN_7;
        case GLFW_MOUSE_BUTTON_8:  return core::inp::MOUSE_BUTTONS::BTN_8;
        default:                   return std::nullopt;
        }
    }
}