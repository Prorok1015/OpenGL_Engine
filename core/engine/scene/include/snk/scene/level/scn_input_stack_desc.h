#pragma once
#include <optional>
#include <string>
#include <vector>

namespace scn
{
	// What a level file says about the base layer ("world") of a world's input stack (INPUT_STACK.md sec. 3.1), as the
	// file holds it: key names stay text, inp resolves them.
	struct input_stack_desc
	{
		std::optional<int> order;
		bool active = true;
		bool block_below = false;
		bool text_keys = false;
		std::vector<std::string> keys;
		std::vector<std::string> actions;
		std::vector<std::string> listen;

		bool operator==(const input_stack_desc&) const = default;
	};
}
