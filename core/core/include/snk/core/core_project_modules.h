#pragma once
#include <snk/application/app_module_entry.h>
#include <snk/common/snk_api.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
		// Whether the manifest's editor modules (kind "editor", #588) are loaded, after the runtime ones: only with
		// `check_editor_module`, which the editor gives. Without it they are only reported.
		bool with_editor_modules = false;
		// Checks an editor module's entry (its editor api version, module_entry::editor_api_version) before its module is
		// created: empty when the editor can serve it, else why not. A module it refuses is not loaded and the rest runs.
		std::function<std::string(const app::modules::module_entry&)> check_editor_module;
		// Why the editor modules cannot be unloaded now (one of their callbacks is on the stack: unloading would
		// destroy code that runs); empty when they can. Asked before every unload and before a reload swaps the game.
		std::function<std::string()> editor_unload_refusal;
		// A static host (the shipping `game`, #591): only `compiled_in` modules run, nothing is opened from the
		// project's build output, and reload() from it is refused.
		bool compiled_only = false;
		// The engine's layout key (gs::engine_module_layout_key, #614): a module whose snk_module_layout_key differs is
		// refused before it is created, and a reload to it before the running module goes. 0: not checked.
		std::uint64_t layout_key = 0;
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
		// The module the swap was for; empty when nothing was swapped.
		std::string module{};
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
		// A build revert() took back is refused until the build output changes, unless `retry_taken_back` (a person asked
		// for this reload; live mode never does).
		reload_report reload(const std::string& name, const reload_hooks& hooks = {}, bool retry_taken_back = false);
		// Same with `next` instead of the build output (compiled-in modules, tests); `previous` gives the source of the
		// running module again for a rollback (empty: no rollback).
		reload_report reload(const std::string& name, std::unique_ptr<app::modules::module_source> next,
			std::function<std::unique_ptr<app::modules::module_source>()> previous, const reload_hooks& hooks = {});
		// Takes back the last reload() of `name` that put a new build in place: the build that ran before it is loaded
		// again through the same swap (hooks, editor modules), as when Play could not be restored in the new build
		// (HOT_RELOAD_PLAY.md 3.5). Afterwards reload() refuses the build taken back until the build output changes, so
		// live mode does not load it again. If the previous build fails to load (lost), its copy stays and no module runs.
		// What can be taken back is set by a reload that ends `reloaded` over a running module and dropped by one that
		// swapped and ended otherwise, by revert() itself and by load_all(); a refusal before the swap keeps it (the
		// running module did not change). Refused without calling the hooks: a reload runs, an editor module, nothing
		// to take back, or the module is not loaded. A compiled-in module can be taken back too.
		reload_report revert(const std::string& name, const reload_hooks& hooks = {});
		// Drops what revert() could take back for `name` (a revert asked for and then cancelled): a later revert() refuses.
		void forget_revert(const std::string& name);

		// The library reload() would copy, if the project's build output has one.
		std::optional<std::filesystem::path> build_output(const std::string& name) const;
		// The copy in use was made from the library the build output has now (same file, size and write time). True
		// for a compiled-in module; false for one that runs from no copy.
		bool is_current(const std::string& name) const;
		// Loads the manifest's editor modules (#588), each from <root>/build/bin like a runtime module (copied, checked
		// with options.check_editor_module, its name against the manifest's). Call it after load_all: a module of kind
		// "editor" is loaded after `game`. One that is missing, refused or fails to register is reported and logged; the
		// editor keeps running without it. reload() unloads them before the game module goes and loads them again after it.
		project_modules_report load_editor_modules();
		// Takes the new build of the editor module `name` (#664) without touching the game module or the other editor
		// modules: the copy is made from the build output and checked next to the running module (entry, name, editor
		// api version, layout key) before anything is unloaded; then before_swap, the running module goes (refused, nothing
		// touched, when editor_unload_refusal says one of the module's callbacks is on the stack, or another loaded module
		// depends on it), the new one is loaded in its place, and if it fails the previous copy is loaded again
		// (rolled_back; lost when that fails too: the module stays unloaded until the next build). A module that is not
		// loaded (lost, refused at start) is loaded by this. Then after_swap. Nothing is taken back: an editor module has
		// no Play to restore. Same rules as reload(): main thread, never from one of the module's callbacks (it would be
		// refused), a compiled-in module is not reloaded by name.
		reload_report reload_editor_module(const std::string& name, const reload_hooks& hooks = {});
		// Unloads the loaded editor modules, the last loaded first (each one's registrations go with it). Empty when
		// done (or none was loaded); otherwise why nothing was unloaded.
		std::string unload_editor_modules();
		bool has_editor_modules() const { return !m_editor_loaded.empty(); }
		// `name` is a module of kind "editor" (loaded, or named so by the manifest).
		bool is_editor_module(const std::string& name) const;
		// The runtime modules the project requires (not optional) that `report` did not load: missing or refused.
		std::vector<std::string> failed_required(const project_modules_report& report) const;

	private:
		struct loaded_copy
		{
			std::filesystem::path library;
			std::filesystem::path folder;
			// The libraries copied along, by file name: size and time of the build's file when copied.
			std::map<std::string, std::pair<std::uintmax_t, std::filesystem::file_time_type>> siblings;
			// The build's library the copy was made from, with its size and time then.
			std::filesystem::path source;
			std::uintmax_t source_size = 0;
			std::filesystem::file_time_type source_time{};
		};
		struct module_copies
		{
			std::optional<loaded_copy> current;
			std::optional<loaded_copy> previous;
		};

		// What revert() loads again, and the build it takes back.
		struct revert_point
		{
			std::function<std::unique_ptr<app::modules::module_source>()> source;
			std::optional<loaded_copy> previous;
			std::optional<loaded_copy> reverted;
		};
		// A build revert() took back, by the build output file it was copied from.
		struct rejected_build
		{
			std::filesystem::path source;
			std::uintmax_t size = 0;
			std::filesystem::file_time_type time{};
		};

		// `keep_copy`: a failed load leaves the folder of `copy` (a previous build revert() loads again).
		reload_report swap(const std::string& name, std::unique_ptr<app::modules::module_source> next,
			const std::function<std::unique_ptr<app::modules::module_source>()>& previous, const reload_hooks& hooks,
			const std::optional<loaded_copy>& copy, bool keep_copy = false);
		// The swap of an editor module (reload_editor_module): `copy` is null for a compiled-in or test source.
		reload_report swap_editor(const std::string& name, std::unique_ptr<app::modules::module_source> next,
			const std::function<std::unique_ptr<app::modules::module_source>()>& previous, const reload_hooks& hooks,
			const std::optional<loaded_copy>& copy);
		void remove_unused_copies() const;

		app::modules::module_loader& m_loader;
		ds::app_data_storage& m_data;
		project_modules_options m_options;
		std::map<std::string, module_copies> m_copies;
		std::map<std::string, revert_point> m_revert;
		std::map<std::string, rejected_build> m_rejected;
		// Editor modules in the order they were loaded.
		std::vector<std::string> m_editor_loaded;
		// A reload runs: one asked for from inside it (a hook, the module's own code) is refused.
		bool m_reloading = false;
	};

	// project_module_host::load_all with a host of its own (no reload afterwards).
	SNK_API project_modules_report load_project_modules(app::modules::module_loader& loader, ds::app_data_storage& data,
		const project_modules_options& options);
}
