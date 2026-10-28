#pragma once
#include <snk/application/app_module_entry.h>
#include <snk/common/snk_api.h>
#include <map>
#include <string>
#include <vector>

namespace app::modules { class module_loader; }
namespace ds { class app_data_storage; }

// Loads the game modules the open project's manifest names (#428, MODULE_LAYER.md). The host calls it once its own
// modules are initialized: the editor in its main(), a player (#23) in its own.
namespace core
{
	struct project_modules_options
	{
		// Modules compiled into the host, by manifest name; looked up before the project's build output.
		std::map<std::string, app::modules::module_entry> compiled_in;
		// Whether the manifest's editor modules count. None is loaded yet (Extension API, #22): they are only reported.
		bool with_editor_modules = false;
	};

	struct project_modules_report
	{
		std::vector<std::string> loaded;
		std::vector<std::string> missing_required;
		std::vector<std::string> missing_optional;
		// Found but not loaded: the source could not be opened or copied, the entry or the name did not match, or the
		// module failed to register or initialize. Each is logged with the reason.
		std::vector<std::string> refused;
	};

	// Without a prj::project_context in `data` nothing is loaded. A runtime module comes from `compiled_in`, else from
	// <root>/build/bin (<name>.dll; lib<name>.so, then <name>.so): that file is copied to
	// <root>/.cache/modules/<name>.<n>.<ext>, with n unused so far, and the copy is loaded, so the build output stays
	// free to be rebuilt. Both folders must lie inside the project once symlinks are resolved. A module that is
	// refused is unloaded and its copy removed; the host keeps running without it. Never throws for a module.
	SNK_API project_modules_report load_project_modules(app::modules::module_loader& loader, ds::app_data_storage& data,
		const project_modules_options& options);
}
