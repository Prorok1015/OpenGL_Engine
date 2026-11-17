#pragma once
#include <snk/core_input/inp_gamepad.hpp>

// Where the state of a gamepad comes from (EPIC-42 #542, US-542-B0): an interface without GLFW types, so the engine reads a
// device the same way from the real backend, from nothing and from a test. Only the first device is read. Nothing polls it
// into input_state yet; that is the gamepad story proper.
namespace wnd
{
	class gamepad_source
	{
	public:
		virtual ~gamepad_source() = default;

		// Fills `out` with the state of the device now. False, and `out` empty and not connected, when there is none.
		virtual bool poll(core::inp::gamepad_state& out) = 0;
	};

	// No device: what a platform without gamepad support gives.
	class null_gamepad_source final : public gamepad_source
	{
	public:
		bool poll(core::inp::gamepad_state& out) override
		{
			out = {};
			return false;
		}
	};

	// A device a test drives.
	class fake_gamepad_source final : public gamepad_source
	{
	public:
		void set_state(const core::inp::gamepad_state& state) { m_state = state; }
		void press(core::inp::GAMEPAD_BUTTONS button, bool down = true)
		{
			m_state.connected = true;
			m_state.buttons[static_cast<std::size_t>(button)] = down;
		}
		void set_axis(core::inp::GAMEPAD_AXIS_IDS axis, float value)
		{
			m_state.connected = true;
			m_state.axes[static_cast<std::size_t>(axis)] = value;
		}
		void disconnect() { m_state = {}; }

		bool poll(core::inp::gamepad_state& out) override
		{
			out = m_state;
			return m_state.connected;
		}

	private:
		core::inp::gamepad_state m_state{};
	};
}
