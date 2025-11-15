#pragma once
#include "inp_key_enums.hpp"
#include <GLFW/glfw3.h>

namespace wnd::convert
{
    inp::KEY_ACTION to_action(int action)
    {
        switch (action)
        {
        case GLFW_RELEASE: return inp::KEY_ACTION::UP; break;
        case GLFW_PRESS: return inp::KEY_ACTION::DOWN; break;
            //case GLFW_REPEAT:;
        }

        return inp::KEY_ACTION::NONE;
    }

    int to_glfw_keycode(inp::KEYBOARD_BUTTONS button) {
        using namespace inp;
        switch (button) {
        case KEYBOARD_BUTTONS::SPACE: return GLFW_KEY_SPACE;
        case KEYBOARD_BUTTONS::APOSTROPHE: return GLFW_KEY_APOSTROPHE;
        case KEYBOARD_BUTTONS::COMMA: return GLFW_KEY_COMMA;
        case KEYBOARD_BUTTONS::MINUS: return GLFW_KEY_MINUS;
        case KEYBOARD_BUTTONS::PERIOD: return GLFW_KEY_PERIOD;
        case KEYBOARD_BUTTONS::SLASH: return GLFW_KEY_SLASH;
        case KEYBOARD_BUTTONS::K_0: return GLFW_KEY_0;
        case KEYBOARD_BUTTONS::K_1: return GLFW_KEY_1;
        case KEYBOARD_BUTTONS::K_2: return GLFW_KEY_2;
        case KEYBOARD_BUTTONS::K_3: return GLFW_KEY_3;
        case KEYBOARD_BUTTONS::K_4: return GLFW_KEY_4;
        case KEYBOARD_BUTTONS::K_5: return GLFW_KEY_5;
        case KEYBOARD_BUTTONS::K_6: return GLFW_KEY_6;
        case KEYBOARD_BUTTONS::K_7: return GLFW_KEY_7;
        case KEYBOARD_BUTTONS::K_8: return GLFW_KEY_8;
        case KEYBOARD_BUTTONS::K_9: return GLFW_KEY_9;
        case KEYBOARD_BUTTONS::SEMICOLON: return GLFW_KEY_SEMICOLON;
        case KEYBOARD_BUTTONS::EQUAL: return GLFW_KEY_EQUAL;
        case KEYBOARD_BUTTONS::A: return GLFW_KEY_A;
        case KEYBOARD_BUTTONS::B: return GLFW_KEY_B;
        case KEYBOARD_BUTTONS::C: return GLFW_KEY_C;
        case KEYBOARD_BUTTONS::D: return GLFW_KEY_D;
        case KEYBOARD_BUTTONS::E: return GLFW_KEY_E;
        case KEYBOARD_BUTTONS::F: return GLFW_KEY_F;
        case KEYBOARD_BUTTONS::G: return GLFW_KEY_G;
        case KEYBOARD_BUTTONS::H: return GLFW_KEY_H;
        case KEYBOARD_BUTTONS::I: return GLFW_KEY_I;
        case KEYBOARD_BUTTONS::J: return GLFW_KEY_J;
        case KEYBOARD_BUTTONS::K: return GLFW_KEY_K;
        case KEYBOARD_BUTTONS::L: return GLFW_KEY_L;
        case KEYBOARD_BUTTONS::M: return GLFW_KEY_M;
        case KEYBOARD_BUTTONS::N: return GLFW_KEY_N;
        case KEYBOARD_BUTTONS::O: return GLFW_KEY_O;
        case KEYBOARD_BUTTONS::P: return GLFW_KEY_P;
        case KEYBOARD_BUTTONS::Q: return GLFW_KEY_Q;
        case KEYBOARD_BUTTONS::R: return GLFW_KEY_R;
        case KEYBOARD_BUTTONS::S: return GLFW_KEY_S;
        case KEYBOARD_BUTTONS::T: return GLFW_KEY_T;
        case KEYBOARD_BUTTONS::U: return GLFW_KEY_U;
        case KEYBOARD_BUTTONS::V: return GLFW_KEY_V;
        case KEYBOARD_BUTTONS::W: return GLFW_KEY_W;
        case KEYBOARD_BUTTONS::X: return GLFW_KEY_X;
        case KEYBOARD_BUTTONS::Y: return GLFW_KEY_Y;
        case KEYBOARD_BUTTONS::Z: return GLFW_KEY_Z;
        case KEYBOARD_BUTTONS::ESCAPE: return GLFW_KEY_ESCAPE;
        case KEYBOARD_BUTTONS::ENTER: return GLFW_KEY_ENTER;
        case KEYBOARD_BUTTONS::TAB: return GLFW_KEY_TAB;
        case KEYBOARD_BUTTONS::BACKSPACE: return GLFW_KEY_BACKSPACE;
        case KEYBOARD_BUTTONS::INSERT: return GLFW_KEY_INSERT;
        case KEYBOARD_BUTTONS::DELETE: return GLFW_KEY_DELETE;
        case KEYBOARD_BUTTONS::RIGHT: return GLFW_KEY_RIGHT;
        case KEYBOARD_BUTTONS::LEFT: return GLFW_KEY_LEFT;
        case KEYBOARD_BUTTONS::DOWN: return GLFW_KEY_DOWN;
        case KEYBOARD_BUTTONS::UP: return GLFW_KEY_UP;
        case KEYBOARD_BUTTONS::PAGE_UP: return GLFW_KEY_PAGE_UP;
        case KEYBOARD_BUTTONS::PAGE_DOWN: return GLFW_KEY_PAGE_DOWN;
        case KEYBOARD_BUTTONS::HOME: return GLFW_KEY_HOME;
        case KEYBOARD_BUTTONS::END: return GLFW_KEY_END;
        case KEYBOARD_BUTTONS::CAPS_LOCK: return GLFW_KEY_CAPS_LOCK;
        case KEYBOARD_BUTTONS::SCROLL_LOCK: return GLFW_KEY_SCROLL_LOCK;
        case KEYBOARD_BUTTONS::NUM_LOCK: return GLFW_KEY_NUM_LOCK;
        case KEYBOARD_BUTTONS::PRINT_SCREEN: return GLFW_KEY_PRINT_SCREEN;
        case KEYBOARD_BUTTONS::PAUSE: return GLFW_KEY_PAUSE;
        case KEYBOARD_BUTTONS::F1: return GLFW_KEY_F1;
        case KEYBOARD_BUTTONS::F2: return GLFW_KEY_F2;
        case KEYBOARD_BUTTONS::F3: return GLFW_KEY_F3;
        case KEYBOARD_BUTTONS::F4: return GLFW_KEY_F4;
        case KEYBOARD_BUTTONS::F5: return GLFW_KEY_F5;
        case KEYBOARD_BUTTONS::F6: return GLFW_KEY_F6;
        case KEYBOARD_BUTTONS::F7: return GLFW_KEY_F7;
        case KEYBOARD_BUTTONS::F8: return GLFW_KEY_F8;
        case KEYBOARD_BUTTONS::F9: return GLFW_KEY_F9;
        case KEYBOARD_BUTTONS::F10: return GLFW_KEY_F10;
        case KEYBOARD_BUTTONS::F11: return GLFW_KEY_F11;
        case KEYBOARD_BUTTONS::F12: return GLFW_KEY_F12;
        case KEYBOARD_BUTTONS::F13: return GLFW_KEY_F13;
        case KEYBOARD_BUTTONS::F14: return GLFW_KEY_F14;
        case KEYBOARD_BUTTONS::F15: return GLFW_KEY_F15;
        case KEYBOARD_BUTTONS::F16: return GLFW_KEY_F16;
        case KEYBOARD_BUTTONS::F17: return GLFW_KEY_F17;
        case KEYBOARD_BUTTONS::F18: return GLFW_KEY_F18;
        case KEYBOARD_BUTTONS::F19: return GLFW_KEY_F19;
        case KEYBOARD_BUTTONS::F20: return GLFW_KEY_F20;
        case KEYBOARD_BUTTONS::F21: return GLFW_KEY_F21;
        case KEYBOARD_BUTTONS::F22: return GLFW_KEY_F22;
        case KEYBOARD_BUTTONS::F23: return GLFW_KEY_F23;
        case KEYBOARD_BUTTONS::F24: return GLFW_KEY_F24;
        case KEYBOARD_BUTTONS::F25: return GLFW_KEY_F25;
        case KEYBOARD_BUTTONS::KP_0: return GLFW_KEY_KP_0;
        case KEYBOARD_BUTTONS::KP_1: return GLFW_KEY_KP_1;
        case KEYBOARD_BUTTONS::KP_2: return GLFW_KEY_KP_2;
        case KEYBOARD_BUTTONS::KP_3: return GLFW_KEY_KP_3;
        case KEYBOARD_BUTTONS::KP_4: return GLFW_KEY_KP_4;
        case KEYBOARD_BUTTONS::KP_5: return GLFW_KEY_KP_5;
        case KEYBOARD_BUTTONS::KP_6: return GLFW_KEY_KP_6;
        case KEYBOARD_BUTTONS::KP_7: return GLFW_KEY_KP_7;
        case KEYBOARD_BUTTONS::KP_8: return GLFW_KEY_KP_8;
        case KEYBOARD_BUTTONS::KP_9: return GLFW_KEY_KP_9;
        case KEYBOARD_BUTTONS::KP_DECIMAL: return GLFW_KEY_KP_DECIMAL;
        case KEYBOARD_BUTTONS::KP_MULTIPLY: return GLFW_KEY_KP_MULTIPLY;
        case KEYBOARD_BUTTONS::KP_SUBTRACT: return GLFW_KEY_KP_SUBTRACT;
        case KEYBOARD_BUTTONS::KP_ADD: return GLFW_KEY_KP_ADD;
        case KEYBOARD_BUTTONS::KP_ENTER: return GLFW_KEY_KP_ENTER;
        case KEYBOARD_BUTTONS::KP_EQUAL: return GLFW_KEY_KP_EQUAL;
        case KEYBOARD_BUTTONS::LEFT_SHIFT: return GLFW_KEY_LEFT_SHIFT;
        case KEYBOARD_BUTTONS::LEFT_CONTROL: return GLFW_KEY_LEFT_CONTROL;
        case KEYBOARD_BUTTONS::LEFT_ALT: return GLFW_KEY_LEFT_ALT;
        case KEYBOARD_BUTTONS::LEFT_SUPER: return GLFW_KEY_LEFT_SUPER;
        case KEYBOARD_BUTTONS::RIGHT_SHIFT: return GLFW_KEY_RIGHT_SHIFT;
        case KEYBOARD_BUTTONS::RIGHT_CONTROL: return GLFW_KEY_RIGHT_CONTROL;
        case KEYBOARD_BUTTONS::RIGHT_ALT: return GLFW_KEY_RIGHT_ALT;
        case KEYBOARD_BUTTONS::RIGHT_SUPER: return GLFW_KEY_RIGHT_SUPER;
        case KEYBOARD_BUTTONS::MENU: return GLFW_KEY_MENU;
        }
        return -1;
    }

    std::optional<inp::KEYBOARD_BUTTONS> to_keyboard_keycode(int glfw_value) {
        using namespace inp;
        switch (glfw_value) {
        case GLFW_KEY_SPACE:       return KEYBOARD_BUTTONS::SPACE;
        case GLFW_KEY_APOSTROPHE:  return KEYBOARD_BUTTONS::APOSTROPHE;
        case GLFW_KEY_COMMA:       return KEYBOARD_BUTTONS::COMMA;
        case GLFW_KEY_MINUS:       return KEYBOARD_BUTTONS::MINUS;
        case GLFW_KEY_PERIOD:      return KEYBOARD_BUTTONS::PERIOD;
        case GLFW_KEY_SLASH:       return KEYBOARD_BUTTONS::SLASH;
        case GLFW_KEY_0:           return KEYBOARD_BUTTONS::K_0;
        case GLFW_KEY_1:           return KEYBOARD_BUTTONS::K_1;
        case GLFW_KEY_2:           return KEYBOARD_BUTTONS::K_2;
        case GLFW_KEY_3:           return KEYBOARD_BUTTONS::K_3;
        case GLFW_KEY_4:           return KEYBOARD_BUTTONS::K_4;
        case GLFW_KEY_5:           return KEYBOARD_BUTTONS::K_5;
        case GLFW_KEY_6:           return KEYBOARD_BUTTONS::K_6;
        case GLFW_KEY_7:           return KEYBOARD_BUTTONS::K_7;
        case GLFW_KEY_8:           return KEYBOARD_BUTTONS::K_8;
        case GLFW_KEY_9:           return KEYBOARD_BUTTONS::K_9;
        case GLFW_KEY_SEMICOLON:   return KEYBOARD_BUTTONS::SEMICOLON;
        case GLFW_KEY_EQUAL:       return KEYBOARD_BUTTONS::EQUAL;
        case GLFW_KEY_A:           return KEYBOARD_BUTTONS::A;
        case GLFW_KEY_B:           return KEYBOARD_BUTTONS::B;
        case GLFW_KEY_C:           return KEYBOARD_BUTTONS::C;
        case GLFW_KEY_D:           return KEYBOARD_BUTTONS::D;
        case GLFW_KEY_E:           return KEYBOARD_BUTTONS::E;
        case GLFW_KEY_F:           return KEYBOARD_BUTTONS::F;
        case GLFW_KEY_G:           return KEYBOARD_BUTTONS::G;
        case GLFW_KEY_H:           return KEYBOARD_BUTTONS::H;
        case GLFW_KEY_I:           return KEYBOARD_BUTTONS::I;
        case GLFW_KEY_J:           return KEYBOARD_BUTTONS::J;
        case GLFW_KEY_K:           return KEYBOARD_BUTTONS::K;
        case GLFW_KEY_L:           return KEYBOARD_BUTTONS::L;
        case GLFW_KEY_M:           return KEYBOARD_BUTTONS::M;
        case GLFW_KEY_N:           return KEYBOARD_BUTTONS::N;
        case GLFW_KEY_O:           return KEYBOARD_BUTTONS::O;
        case GLFW_KEY_P:           return KEYBOARD_BUTTONS::P;
        case GLFW_KEY_Q:           return KEYBOARD_BUTTONS::Q;
        case GLFW_KEY_R:           return KEYBOARD_BUTTONS::R;
        case GLFW_KEY_S:           return KEYBOARD_BUTTONS::S;
        case GLFW_KEY_T:           return KEYBOARD_BUTTONS::T;
        case GLFW_KEY_U:           return KEYBOARD_BUTTONS::U;
        case GLFW_KEY_V:           return KEYBOARD_BUTTONS::V;
        case GLFW_KEY_W:           return KEYBOARD_BUTTONS::W;
        case GLFW_KEY_X:           return KEYBOARD_BUTTONS::X;
        case GLFW_KEY_Y:           return KEYBOARD_BUTTONS::Y;
        case GLFW_KEY_Z:           return KEYBOARD_BUTTONS::Z;
        case GLFW_KEY_ESCAPE:      return KEYBOARD_BUTTONS::ESCAPE;
        case GLFW_KEY_ENTER:       return KEYBOARD_BUTTONS::ENTER;
        case GLFW_KEY_TAB:         return KEYBOARD_BUTTONS::TAB;
        case GLFW_KEY_BACKSPACE:   return KEYBOARD_BUTTONS::BACKSPACE;
        case GLFW_KEY_INSERT:      return KEYBOARD_BUTTONS::INSERT;
        case GLFW_KEY_DELETE:      return KEYBOARD_BUTTONS::DELETE;
        case GLFW_KEY_RIGHT:       return KEYBOARD_BUTTONS::RIGHT;
        case GLFW_KEY_LEFT:        return KEYBOARD_BUTTONS::LEFT;
        case GLFW_KEY_DOWN:        return KEYBOARD_BUTTONS::DOWN;
        case GLFW_KEY_UP:          return KEYBOARD_BUTTONS::UP;
        case GLFW_KEY_PAGE_UP:     return KEYBOARD_BUTTONS::PAGE_UP;
        case GLFW_KEY_PAGE_DOWN:   return KEYBOARD_BUTTONS::PAGE_DOWN;
        case GLFW_KEY_HOME:        return KEYBOARD_BUTTONS::HOME;
        case GLFW_KEY_END:         return KEYBOARD_BUTTONS::END;
        case GLFW_KEY_CAPS_LOCK:   return KEYBOARD_BUTTONS::CAPS_LOCK;
        case GLFW_KEY_SCROLL_LOCK: return KEYBOARD_BUTTONS::SCROLL_LOCK;
        case GLFW_KEY_NUM_LOCK:    return KEYBOARD_BUTTONS::NUM_LOCK;
        case GLFW_KEY_PRINT_SCREEN:return KEYBOARD_BUTTONS::PRINT_SCREEN;
        case GLFW_KEY_PAUSE:       return KEYBOARD_BUTTONS::PAUSE;
        case GLFW_KEY_F1:          return KEYBOARD_BUTTONS::F1;
        case GLFW_KEY_F2:          return KEYBOARD_BUTTONS::F2;
        case GLFW_KEY_F3:          return KEYBOARD_BUTTONS::F3;
        case GLFW_KEY_F4:          return KEYBOARD_BUTTONS::F4;
        case GLFW_KEY_F5:          return KEYBOARD_BUTTONS::F5;
        case GLFW_KEY_F6:          return KEYBOARD_BUTTONS::F6;
        case GLFW_KEY_F7:          return KEYBOARD_BUTTONS::F7;
        case GLFW_KEY_F8:          return KEYBOARD_BUTTONS::F8;
        case GLFW_KEY_F9:          return KEYBOARD_BUTTONS::F9;
        case GLFW_KEY_F10:         return KEYBOARD_BUTTONS::F10;
        case GLFW_KEY_F11:         return KEYBOARD_BUTTONS::F11;
        case GLFW_KEY_F12:         return KEYBOARD_BUTTONS::F12;
        case GLFW_KEY_F13:         return KEYBOARD_BUTTONS::F13;
        case GLFW_KEY_F14:         return KEYBOARD_BUTTONS::F14;
        case GLFW_KEY_F15:         return KEYBOARD_BUTTONS::F15;
        case GLFW_KEY_F16:         return KEYBOARD_BUTTONS::F16;
        case GLFW_KEY_F17:         return KEYBOARD_BUTTONS::F17;
        case GLFW_KEY_F18:         return KEYBOARD_BUTTONS::F18;
        case GLFW_KEY_F19:         return KEYBOARD_BUTTONS::F19;
        case GLFW_KEY_F20:         return KEYBOARD_BUTTONS::F20;
        case GLFW_KEY_F21:         return KEYBOARD_BUTTONS::F21;
        case GLFW_KEY_F22:         return KEYBOARD_BUTTONS::F22;
        case GLFW_KEY_F23:         return KEYBOARD_BUTTONS::F23;
        case GLFW_KEY_F24:         return KEYBOARD_BUTTONS::F24;
        case GLFW_KEY_F25:         return KEYBOARD_BUTTONS::F25;
        case GLFW_KEY_KP_0:        return KEYBOARD_BUTTONS::KP_0;
        case GLFW_KEY_KP_1:        return KEYBOARD_BUTTONS::KP_1;
        case GLFW_KEY_KP_2:        return KEYBOARD_BUTTONS::KP_2;
        case GLFW_KEY_KP_3:        return KEYBOARD_BUTTONS::KP_3;
        case GLFW_KEY_KP_4:        return KEYBOARD_BUTTONS::KP_4;
        case GLFW_KEY_KP_5:        return KEYBOARD_BUTTONS::KP_5;
        case GLFW_KEY_KP_6:        return KEYBOARD_BUTTONS::KP_6;
        case GLFW_KEY_KP_7:        return KEYBOARD_BUTTONS::KP_7;
        case GLFW_KEY_KP_8:        return KEYBOARD_BUTTONS::KP_8;
        case GLFW_KEY_KP_9:        return KEYBOARD_BUTTONS::KP_9;
        case GLFW_KEY_KP_DECIMAL:  return KEYBOARD_BUTTONS::KP_DECIMAL;
        case GLFW_KEY_KP_DIVIDE:   return KEYBOARD_BUTTONS::KP_DIVIDE;
        case GLFW_KEY_KP_MULTIPLY: return KEYBOARD_BUTTONS::KP_MULTIPLY;
        case GLFW_KEY_KP_SUBTRACT: return KEYBOARD_BUTTONS::KP_SUBTRACT;
        case GLFW_KEY_KP_ADD:      return KEYBOARD_BUTTONS::KP_ADD;
        case GLFW_KEY_KP_ENTER:    return KEYBOARD_BUTTONS::KP_ENTER;
        case GLFW_KEY_KP_EQUAL:    return KEYBOARD_BUTTONS::KP_EQUAL;
        case GLFW_KEY_LEFT_SHIFT:  return KEYBOARD_BUTTONS::LEFT_SHIFT;
        case GLFW_KEY_LEFT_CONTROL:return KEYBOARD_BUTTONS::LEFT_CONTROL;
        case GLFW_KEY_LEFT_ALT:    return KEYBOARD_BUTTONS::LEFT_ALT;
        case GLFW_KEY_LEFT_SUPER:  return KEYBOARD_BUTTONS::LEFT_SUPER;
        case GLFW_KEY_RIGHT_SHIFT: return KEYBOARD_BUTTONS::RIGHT_SHIFT;
        case GLFW_KEY_RIGHT_CONTROL:return KEYBOARD_BUTTONS::RIGHT_CONTROL;
        case GLFW_KEY_RIGHT_ALT:   return KEYBOARD_BUTTONS::RIGHT_ALT;
        case GLFW_KEY_RIGHT_SUPER: return KEYBOARD_BUTTONS::RIGHT_SUPER;
        case GLFW_KEY_MENU:        return KEYBOARD_BUTTONS::MENU;
        }
        return std::nullopt;
    }

    int to_glfw_keycode(inp::MOUSE_BUTTONS button) {
        switch (button) {
        case inp::MOUSE_BUTTONS::LEFT:   return GLFW_MOUSE_BUTTON_LEFT;
        case inp::MOUSE_BUTTONS::RIGHT:  return GLFW_MOUSE_BUTTON_RIGHT;
        case inp::MOUSE_BUTTONS::MIDDLE: return GLFW_MOUSE_BUTTON_MIDDLE;
        case inp::MOUSE_BUTTONS::BTN_4:  return GLFW_MOUSE_BUTTON_4;
        case inp::MOUSE_BUTTONS::BTN_5:  return GLFW_MOUSE_BUTTON_5;
        case inp::MOUSE_BUTTONS::BTN_6:  return GLFW_MOUSE_BUTTON_6;
        case inp::MOUSE_BUTTONS::BTN_7:  return GLFW_MOUSE_BUTTON_7;
        case inp::MOUSE_BUTTONS::BTN_8:  return GLFW_MOUSE_BUTTON_8;
        }
        return -1;
    }

    std::optional<inp::MOUSE_BUTTONS> to_mouse_keycode(int glfw_value) {
        switch (glfw_value) {
        case GLFW_MOUSE_BUTTON_LEFT:   return inp::MOUSE_BUTTONS::LEFT;
        case GLFW_MOUSE_BUTTON_RIGHT:  return inp::MOUSE_BUTTONS::RIGHT;
        case GLFW_MOUSE_BUTTON_MIDDLE: return inp::MOUSE_BUTTONS::MIDDLE;
        case GLFW_MOUSE_BUTTON_4:  return inp::MOUSE_BUTTONS::BTN_4;
        case GLFW_MOUSE_BUTTON_5:  return inp::MOUSE_BUTTONS::BTN_5;
        case GLFW_MOUSE_BUTTON_6:  return inp::MOUSE_BUTTONS::BTN_6;
        case GLFW_MOUSE_BUTTON_7:  return inp::MOUSE_BUTTONS::BTN_7;
        case GLFW_MOUSE_BUTTON_8:  return inp::MOUSE_BUTTONS::BTN_8;
        default:                   return std::nullopt;
        }
    }
}