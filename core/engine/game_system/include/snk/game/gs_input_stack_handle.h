#pragma once
#include <snk/input/inp_input_stack.h>
#include <snk/common/snk_api.h>
#include <string>
#include <string_view>

namespace inp { class input_router; }

namespace gs
{
	// The stack of the input layers of one set, for code that runs outside the systems (a module's setup, an event handler of
	// the engine); the systems of a world use inp::input_stack_control instead. The commands are not applied now: they wait for
	// the next frame's gs::route_input (INPUT_STACK.md 3.3). The layers it pushes belong to the module (inp::layer_owner::module):
	// it can change or remove only those, and a command on another owner's layer is logged and skipped. Empty (false) when
	// the set did not exist: every call then does nothing.
	class SNK_API input_stack_handle
	{
	public:
		// A module holds one by value (the result of module_context::input_stack) and builds the layers it passes in its own code,
		// so its layout and the layout of stack_layer_desc are in the module layout key (gs_module_layout.h, #614). The
		// constructor, the moves and the destructor are the engine's (exported); what is allocated on one side and freed on
		// the other (the set name, the strings and lists of a layer) is sound only because module and engine share one CRT,
		// which the loader checks by the build flags, as for every std type in the module API.
		input_stack_handle() = default;
		input_stack_handle(inp::input_router* router, std::string set);
		input_stack_handle(input_stack_handle&& other) noexcept;
		input_stack_handle& operator=(input_stack_handle&& other) noexcept;
		input_stack_handle(const input_stack_handle&) = delete;
		input_stack_handle& operator=(const input_stack_handle&) = delete;
		~input_stack_handle();
		explicit operator bool() const { return m_router != nullptr; }

		// A layer of `world`; one of the module with the same name is replaced, so a repeated setup makes no second layer.
		void push(std::string_view world, inp::stack_layer_desc layer);
		// Like push, but the layer must exist already.
		void replace(std::string_view world, inp::stack_layer_desc layer);
		void remove(std::string_view world, std::string_view layer);
		void set_active(std::string_view world, std::string_view layer, bool active);
		void set_order(std::string_view world, std::string_view layer, int order);
		// Every layer of the world, of any owner, on or off (the base layer too).
		void set_world_active(std::string_view world, bool active);

	private:
		void send(inp::stack_command command);

		inp::input_router* m_router = nullptr;
		std::string m_set;
	};
}
