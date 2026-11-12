#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace app::modules
{
	// The api version (SNK_MODULE_API_VERSION) each module the host loaded was built against, by module name. The host
	// keeps it in its storage so the engine can tell a project's data a module of an older minor cannot read in full (a
	// gamepad binding of actions.desc for a module of 3.15). A load (a reload, a rollback) records the entry of the build
	// that runs; an unload removes it. `revision` changes with each, so a reader notices without comparing the map.
	struct loaded_module_apis
	{
		std::map<std::string, std::uint32_t, std::less<>> api;
		std::uint64_t revision = 0;

		void set(const std::string& module, std::uint32_t version)
		{
			api[module] = version;
			++revision;
		}

		void erase(const std::string& module)
		{
			if (api.erase(module) != 0) ++revision;
		}
	};
}
