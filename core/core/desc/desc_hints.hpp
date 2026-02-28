#pragma once
#include <variant>

namespace core::desc::hints {
	struct range
	{
		float min;
		float max;
	};

	struct color {};

	struct filter {
		std::string_view exts;
	};

	using hint_value = std::variant<range, color>;
}