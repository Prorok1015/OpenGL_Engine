#pragma once
#include <snk/application/app_module_entry.h>
#include <snk/common/snk_api.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace app::modules { class module_loader; class module_source; }
namespace ds { class app_data_storage; }

// Loads the game modules the open project's manifest names (#428) and reloads one from a new build (#435,
// MODULE_LAYER.md, DISTRIBUTION.md "Hot reload game.dll"). The host makes a project_module_host once its own modules are
// initialized: the editor in its main(), a player (#23) in its own.
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

	enum class reload_status
	{
		// The new build runs.
		reloaded,
		// The new build was not taken (no build, no copy, entry or name refused, or the running module could not go):
		// the running module was not touched.
		refused,
		// The new build failed to register or initialize; the previous copy runs again.
		rolled_back,
		// The new build failed and so did the previous copy: no game module runs.
		lost,
	};

	constexpr std::string_view to_string(reload_status status)
	{
		switch (status) {
		case reload_status::reloaded: return "reloaded";
		case reload_status::refused: return "refused";
		case reload_status::rolled_back: return "rolled_back";
		case reload_status::lost: return "lost";
		}
		return "?";
	}

	struct reload_report
	{
		reload_status status = reload_status::refused;
		// Empty when reloaded; otherwise why, for the user.
		std::string message;
	};

	struct reload_hooks
	{
		// The new build passed its check and the running module is about to be unloaded.
		std::function<void()> before_swap;
		// With the outcome, exactly when before_swap was called.
		std::function<void(const reload_report&)> after_swap;
	};

	// A runtime module from `compiled_in`, else from <root>/build/bin (<name>.dll; lib<name>.so, then <name>.so): that
	// file, its debug symbols and the other libraries next to it are copied to <root>/.cache/modules/<pid>-<n>/ under
	// their own names, and the copy is loaded, so the build output stays free to be rebuilt and the debugger finds the
	// symbols the module names (built with /PDBALTPATH:%_PDB%). Both folders must lie inside the project once symlinks
	// are resolved. A folder of another process that still runs is left alone; of this one, the copy of each loaded
	// module and the one before it (a rollback reloads it) stay. Main thread only, never from one of a module's callbacks.
	class SNK_API project_module_host
	{
	public:
		// `loader` and `data` outlive the host; the host is gone before the loader shuts its modules down.
		project_module_host(app::modules::module_loader& loader, ds::app_data_storage& data, project_modules_options options);
		project_module_host(const project_module_host&) = delete;
		project_module_host& operator=(const project_module_host&) = delete;

		// Without a prj::project_context in `data` nothing is loaded. A module that is refused is unloaded and its copy
		// removed; the host keeps running without it. Never throws for a module.
		project_modules_report load_all();

		// Takes the new build of the runtime module `name`: the copy is loaded next to the running module and checked
		// (entry, name) before anything is unloaded; then before_swap, the running module goes, the new one is loaded,
		// and if it fails the previous copy is loaded again; then after_swap. Libraries next to the module are copied but
		// not reloaded: the system keeps the one already loaded under that name (logged). Never throws for a module.
		reload_report reload(const std::string& name, const reload_hooks& hooks = {});
		// Same with `next` instead of the build output (compiled-in modules, tests); `previous` gives the source of the
		// running module again for a rollback (empty: no rollback).
		reload_report reload(const std::string& name, std::unique_ptr<app::modules::module_source> next,
			std::function<std::unique_ptr<app::modules::module_source>()> previous, const reload_hooks& hooks = {});

		// The library reload() would copy, if the project's build output has one.
		std::optional<std::filesystem::path> build_output(const std::string& name) const;

	private:
		struct loaded_copy
		{
			std::filesystem::path library;
			std::filesystem::path folder;
		};
		struct module_copies
		{
			std::optional<loaded_copy> current;
			std::optional<loaded_copy> previous;
		};

		reload_report swap(const std::string& name, std::unique_ptr<app::modules::module_source> next,
			const std::function<std::unique_ptr<app::modules::module_source>()>& previous, const reload_hooks& hooks,
			const std::optional<loaded_copy>& copy);
		void remove_unused_copies() const;

		app::modules::module_loader& m_loader;
		ds::app_data_storage& m_data;
		project_modules_options m_options;
		std::map<std::string, module_copies> m_copies;
	};

	// project_module_host::load_all with a host of its own (no reload afterwards).
	SNK_API project_modules_report load_project_modules(app::modules::module_loader& loader, ds::app_data_storage& data,
		const project_modules_options& options);
}
