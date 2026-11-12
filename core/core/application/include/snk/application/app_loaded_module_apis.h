#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace app::modules
{
	// The api version (SNK_MODULE_API_VERSION) each module the host loaded was built against, by module name. The host
	// keeps it in its storage so the engine can tell a project's data a module of an older minor cannot read in full (a
	// gamepad binding of actions.desc for a module of 3.15). A reloaded module takes its entry again; an unloaded one
	// stays until it is loaded again.
	struct loaded_module_apis
	{
		std::map<std::string, std::uint32_t, std::less<>> api;
	};
}
