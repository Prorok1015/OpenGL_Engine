#pragma once

namespace edt
{
	// Counts the callbacks of one game_editor module that run now (main thread only): a panel's draw, an action, an inspector
	// renderer, an asset kind, an event handler. Unloading the module from inside one of them would destroy the code that
	// runs it, so editor_context::module_unload_refusal refuses while the count is above zero. The registries and
	// editor_module_context wrap the module's callbacks in a guard; the gate belongs to the module's registration_scope.
	class callback_gate
	{
	public:
		class guard
		{
		public:
			explicit guard(callback_gate& gate) : m_gate(gate) { ++m_gate.m_running; }
			~guard() { --m_gate.m_running; }
			guard(const guard&) = delete;
			guard& operator=(const guard&) = delete;

		private:
			callback_gate& m_gate;
		};

		int running() const { return m_running; }

	private:
		int m_running = 0;
	};
}
