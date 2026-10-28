#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <snk/common/snk_api.h>

namespace cfg {

	// Read-only view of the configuration, what a game module gets (gs::module_context::config()). Defining or
	// writing a variable is base-only (<snk/config/cfg_variables.h>).
	class SNK_API config_view
	{
	public:
		// The value of a registered variable as text; nullopt when no variable has that name. Main thread: the
		// configuration has no lock (variables are registered and set on the main thread), so a worker must not call it.
		std::optional<std::string> value(std::string_view name) const;
		bool has(std::string_view name) const { return value(name).has_value(); }
	};

} // namespace cfg
