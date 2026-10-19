#include <snk/gui/gui_platform_backend.h>
#include <snk/core_input/inp_keyboard_event.hpp>
#include <snk/window/wnd_coordinates.h>
#include <algorithm>
#include <cfloat>
#include <imgui.h>

namespace {
	using core::inp::KEYBOARD_BUTTONS;
	using core::inp::KEY_ACTION;

	constexpr std::uint8_t shift_bits = 0b0001'0001;
	constexpr std::uint8_t ctrl_bits = 0b0010'0010;
	constexpr std::uint8_t alt_bits = 0b0100'0100;
	constexpr std::uint8_t super_bits = 0b1000'1000;

	std::uint8_t modifier_bit(KEYBOARD_BUTTONS key)
	{
		switch (key) {
		case KEYBOARD_BUTTONS::LEFT_SHIFT:    return 1u << 0;
		case KEYBOARD_BUTTONS::LEFT_CONTROL:  return 1u << 1;
		case KEYBOARD_BUTTONS::LEFT_ALT:      return 1u << 2;
		case KEYBOARD_BUTTONS::LEFT_SUPER:    return 1u << 3;
		case KEYBOARD_BUTTONS::RIGHT_SHIFT:   return 1u << 4;
		case KEYBOARD_BUTTONS::RIGHT_CONTROL: return 1u << 5;
		case KEYBOARD_BUTTONS::RIGHT_ALT:     return 1u << 6;
		case KEYBOARD_BUTTONS::RIGHT_SUPER:   return 1u << 7;
		default: return 0;
		}
	}

	ImGuiKey to_imgui_key(KEYBOARD_BUTTONS key)
	{
		using K = KEYBOARD_BUTTONS;
		switch (key) {
		case K::A: return ImGuiKey_A;
		case K::B: return ImGuiKey_B;
		case K::C: return ImGuiKey_C;
		case K::D: return ImGuiKey_D;
		case K::E: return ImGuiKey_E;
		case K::F: return ImGuiKey_F;
		case K::G: return ImGuiKey_G;
		case K::H: return ImGuiKey_H;
		case K::I: return ImGuiKey_I;
		case K::J: return ImGuiKey_J;
		case K::K: return ImGuiKey_K;
		case K::L: return ImGuiKey_L;
		case K::M: return ImGuiKey_M;
		case K::N: return ImGuiKey_N;
		case K::O: return ImGuiKey_O;
		case K::P: return ImGuiKey_P;
		case K::Q: return ImGuiKey_Q;
		case K::R: return ImGuiKey_R;
		case K::S: return ImGuiKey_S;
		case K::T: return ImGuiKey_T;
		case K::U: return ImGuiKey_U;
		case K::V: return ImGuiKey_V;
		case K::W: return ImGuiKey_W;
		case K::X: return ImGuiKey_X;
		case K::Y: return ImGuiKey_Y;
		case K::Z: return ImGuiKey_Z;
		case K::K_0: return ImGuiKey_0;
		case K::K_1: return ImGuiKey_1;
		case K::K_2: return ImGuiKey_2;
		case K::K_3: return ImGuiKey_3;
		case K::K_4: return ImGuiKey_4;
		case K::K_5: return ImGuiKey_5;
		case K::K_6: return ImGuiKey_6;
		case K::K_7: return ImGuiKey_7;
		case K::K_8: return ImGuiKey_8;
		case K::K_9: return ImGuiKey_9;
		case K::SPACE: return ImGuiKey_Space;
		case K::APOSTROPHE: return ImGuiKey_Apostrophe;
		case K::COMMA: return ImGuiKey_Comma;
		case K::MINUS: return ImGuiKey_Minus;
		case K::PERIOD: return ImGuiKey_Period;
		case K::SLASH: return ImGuiKey_Slash;
		case K::SEMICOLON: return ImGuiKey_Semicolon;
		case K::EQUAL: return ImGuiKey_Equal;
		case K::ESCAPE: return ImGuiKey_Escape;
		case K::ENTER: return ImGuiKey_Enter;
		case K::TAB: return ImGuiKey_Tab;
		case K::BACKSPACE: return ImGuiKey_Backspace;
		case K::INSERT: return ImGuiKey_Insert;
		case K::DELETE: return ImGuiKey_Delete;
		case K::RIGHT: return ImGuiKey_RightArrow;
		case K::LEFT: return ImGuiKey_LeftArrow;
		case K::DOWN: return ImGuiKey_DownArrow;
		case K::UP: return ImGuiKey_UpArrow;
		case K::PAGE_UP: return ImGuiKey_PageUp;
		case K::PAGE_DOWN: return ImGuiKey_PageDown;
		case K::HOME: return ImGuiKey_Home;
		case K::END: return ImGuiKey_End;
		case K::CAPS_LOCK: return ImGuiKey_CapsLock;
		case K::SCROLL_LOCK: return ImGuiKey_ScrollLock;
		case K::NUM_LOCK: return ImGuiKey_NumLock;
		case K::PRINT_SCREEN: return ImGuiKey_PrintScreen;
		case K::PAUSE: return ImGuiKey_Pause;
		case K::F1: return ImGuiKey_F1;
		case K::F2: return ImGuiKey_F2;
		case K::F3: return ImGuiKey_F3;
		case K::F4: return ImGuiKey_F4;
		case K::F5: return ImGuiKey_F5;
		case K::F6: return ImGuiKey_F6;
		case K::F7: return ImGuiKey_F7;
		case K::F8: return ImGuiKey_F8;
		case K::F9: return ImGuiKey_F9;
		case K::F10: return ImGuiKey_F10;
		case K::F11: return ImGuiKey_F11;
		case K::F12: return ImGuiKey_F12;
		case K::F13: return ImGuiKey_F13;
		case K::F14: return ImGuiKey_F14;
		case K::F15: return ImGuiKey_F15;
		case K::F16: return ImGuiKey_F16;
		case K::F17: return ImGuiKey_F17;
		case K::F18: return ImGuiKey_F18;
		case K::F19: return ImGuiKey_F19;
		case K::F20: return ImGuiKey_F20;
		case K::F21: return ImGuiKey_F21;
		case K::F22: return ImGuiKey_F22;
		case K::F23: return ImGuiKey_F23;
		case K::F24: return ImGuiKey_F24;
		case K::KP_0: return ImGuiKey_Keypad0;
		case K::KP_1: return ImGuiKey_Keypad1;
		case K::KP_2: return ImGuiKey_Keypad2;
		case K::KP_3: return ImGuiKey_Keypad3;
		case K::KP_4: return ImGuiKey_Keypad4;
		case K::KP_5: return ImGuiKey_Keypad5;
		case K::KP_6: return ImGuiKey_Keypad6;
		case K::KP_7: return ImGuiKey_Keypad7;
		case K::KP_8: return ImGuiKey_Keypad8;
		case K::KP_9: return ImGuiKey_Keypad9;
		case K::KP_DECIMAL: return ImGuiKey_KeypadDecimal;
		case K::KP_DIVIDE: return ImGuiKey_KeypadDivide;
		case K::KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
		case K::KP_SUBTRACT: return ImGuiKey_KeypadSubtract;
		case K::KP_ADD: return ImGuiKey_KeypadAdd;
		case K::KP_ENTER: return ImGuiKey_KeypadEnter;
		case K::KP_EQUAL: return ImGuiKey_KeypadEqual;
		case K::LEFT_SHIFT: return ImGuiKey_LeftShift;
		case K::LEFT_CONTROL: return ImGuiKey_LeftCtrl;
		case K::LEFT_ALT: return ImGuiKey_LeftAlt;
		case K::LEFT_SUPER: return ImGuiKey_LeftSuper;
		case K::RIGHT_SHIFT: return ImGuiKey_RightShift;
		case K::RIGHT_CONTROL: return ImGuiKey_RightCtrl;
		case K::RIGHT_ALT: return ImGuiKey_RightAlt;
		case K::RIGHT_SUPER: return ImGuiKey_RightSuper;
		case K::MENU: return ImGuiKey_Menu;
		// F25 and COUNT have no ImGui counterpart.
		default: return ImGuiKey_None;
		}
	}

	wnd::cursor_shape to_cursor_shape(ImGuiMouseCursor cursor)
	{
		switch (cursor) {
		case ImGuiMouseCursor_TextInput:  return wnd::cursor_shape::text_input;
		case ImGuiMouseCursor_ResizeAll:  return wnd::cursor_shape::resize_all;
		case ImGuiMouseCursor_ResizeNS:   return wnd::cursor_shape::resize_ns;
		case ImGuiMouseCursor_ResizeEW:   return wnd::cursor_shape::resize_ew;
		case ImGuiMouseCursor_ResizeNESW: return wnd::cursor_shape::resize_nesw;
		case ImGuiMouseCursor_ResizeNWSE: return wnd::cursor_shape::resize_nwse;
		case ImGuiMouseCursor_Hand:       return wnd::cursor_shape::hand;
		case ImGuiMouseCursor_NotAllowed: return wnd::cursor_shape::not_allowed;
		default:                          return wnd::cursor_shape::arrow;
		}
	}
}

gui::platform_backend::platform_backend(wnd::window& window)
	: m_window(window)
{
}

gui::platform_backend::~platform_backend() = default;

void gui::platform_backend::init()
{
	ImGuiIO& io = ImGui::GetIO();
	io.BackendPlatformName = "snk_wnd";
	io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

	ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
	platform_io.Platform_GetClipboardTextFn = &platform_backend::get_clipboard_text;
	platform_io.Platform_SetClipboardTextFn = &platform_backend::set_clipboard_text;
	platform_io.Platform_ClipboardUserData = this;
	m_initialized = true;
}

void gui::platform_backend::shutdown()
{
	if (!m_initialized) {
		return;
	}
	ImGuiIO& io = ImGui::GetIO();
	io.BackendPlatformName = nullptr;
	io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;

	ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
	platform_io.Platform_GetClipboardTextFn = nullptr;
	platform_io.Platform_SetClipboardTextFn = nullptr;
	platform_io.Platform_ClipboardUserData = nullptr;

	if (m_cursor_hidden) {
		m_window.set_cursor_mode(wnd::CURSOR_MODE::NORMAL);
		m_cursor_hidden = false;
	}
	m_initialized = false;
}

const char* gui::platform_backend::get_clipboard_text(ImGuiContext*)
{
	auto* self = static_cast<platform_backend*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
	self->m_clipboard = self->m_window.get_clipboard_text();
	return self->m_clipboard.c_str();
}

void gui::platform_backend::set_clipboard_text(ImGuiContext*, const char* text)
{
	auto* self = static_cast<platform_backend*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
	self->m_window.set_clipboard_text(text ? text : "");
}

void gui::platform_backend::new_frame()
{
	ImGuiIO& io = ImGui::GetIO();

	const glm::ivec2 window_size = m_window.get_size();
	const glm::vec2 fb_scale = wnd::framebuffer_scale(m_window.get_backbuffer_size(), window_size);
	io.DisplaySize = ImVec2{ static_cast<float>(window_size.x), static_cast<float>(window_size.y) };
	io.DisplayFramebufferScale = ImVec2{ fb_scale.x, fb_scale.y };

	const auto now = std::chrono::steady_clock::now();
	const float dt = m_has_last_frame ? std::chrono::duration<float>(now - m_last_frame).count() : 1.f / 60.f;
	io.DeltaTime = m_fixed_delta > 0.f ? m_fixed_delta : std::max(dt, 1.f / 1000000.f);
	m_last_frame = now;
	m_has_last_frame = true;

	update_mouse_cursor();
}

void gui::platform_backend::update_mouse_cursor()
{
	const ImGuiIO& io = ImGui::GetIO();
	if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) {
		return;
	}

	const ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
	const bool hide = cursor == ImGuiMouseCursor_None || io.MouseDrawCursor;
	if (hide != m_cursor_hidden) {
		// Only our own transitions touch the mode, so a mode set elsewhere (e.g. DISABLE) is not undone.
		m_window.set_cursor_mode(hide ? wnd::CURSOR_MODE::HIDDEN : wnd::CURSOR_MODE::NORMAL);
		m_cursor_hidden = hide;
	}
	if (hide) {
		return;
	}

	const wnd::cursor_shape shape = to_cursor_shape(cursor);
	if (!m_cursor_shape_valid || shape != m_cursor_shape) {
		m_window.set_cursor_shape(shape);
		m_cursor_shape = shape;
		m_cursor_shape_valid = true;
	}
}

void gui::platform_backend::update_modifiers(std::uint8_t previous)
{
	ImGuiIO& io = ImGui::GetIO();
	const auto submit = [&](std::uint8_t bits, ImGuiKey mod) {
		const bool was_down = (previous & bits) != 0;
		const bool is_down = (m_modifier_keys & bits) != 0;
		if (was_down != is_down) {
			io.AddKeyEvent(mod, is_down);
		}
	};
	submit(ctrl_bits, ImGuiMod_Ctrl);
	submit(shift_bits, ImGuiMod_Shift);
	submit(alt_bits, ImGuiMod_Alt);
	submit(super_bits, ImGuiMod_Super);
}

void gui::platform_backend::on_input_event(wnd::handle win, const inp::input_event& evt)
{
	if (!m_initialized || (win && win != &m_window)) {
		return;
	}

	ImGuiIO& io = ImGui::GetIO();

	if (const auto* key = evt.get_payload<core::inp::keyboard_event>()) {
		if (key->action == KEY_ACTION::NONE) {
			return;
		}
		const bool down = key->action == KEY_ACTION::DOWN;
		if (const std::uint8_t bit = modifier_bit(key->key)) {
			const std::uint8_t previous = m_modifier_keys;
			m_modifier_keys = static_cast<std::uint8_t>(down ? (m_modifier_keys | bit) : (m_modifier_keys & ~bit));
			update_modifiers(previous);
		}
		const ImGuiKey imgui_key = to_imgui_key(key->key);
		if (imgui_key != ImGuiKey_None) {
			io.AddKeyEvent(imgui_key, down);
		}
	} else if (const auto* ch = evt.get_payload<core::inp::char_event>()) {
		io.AddInputCharacter(static_cast<unsigned int>(ch->codepoint));
	} else if (const auto* click = evt.get_payload<core::inp::mouse_click_event>()) {
		const int button = static_cast<int>(click->key);
		if (button < ImGuiMouseButton_COUNT && click->action != KEY_ACTION::NONE) {
			io.AddMouseButtonEvent(button, click->action == KEY_ACTION::DOWN);
		}
	} else if (const auto* scroll = evt.get_payload<core::inp::scroll_move_event>()) {
		io.AddMouseWheelEvent(scroll->direction.x, scroll->direction.y);
	} else if (const auto* move = evt.get_payload<core::inp::cursor_move_event>()) {
		// Events carry framebuffer pixels; ImGui works in window units.
		const glm::vec2 scale = wnd::framebuffer_scale(m_window.get_backbuffer_size(), m_window.get_size());
		io.AddMousePosEvent(move->pos.x / scale.x, move->pos.y / scale.y);
	} else if (const auto* enter = evt.get_payload<core::inp::cursor_entered_event>()) {
		if (!enter->entered) {
			io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
		}
	}
}

void gui::platform_backend::on_window_focus_gained(wnd::handle win)
{
	if (m_initialized && (!win || win == &m_window)) {
		ImGui::GetIO().AddFocusEvent(true);
	}
}

void gui::platform_backend::on_window_focus_lost(wnd::handle win)
{
	if (m_initialized && (!win || win == &m_window)) {
		m_modifier_keys = 0;
		ImGui::GetIO().AddFocusEvent(false);
	}
}
