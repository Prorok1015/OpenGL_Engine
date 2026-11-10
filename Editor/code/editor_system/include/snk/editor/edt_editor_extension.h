#pragma once
#include "edt_api.h"
#include "edt_editor_context.h"
#include "edt_registration_scope.h"
#include <snk/application/app_module_entry.h>
#include <snk/application/app_module_interface.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// The version of the editor API a game_editor module is built against (major << 16 | minor), exported by the module as
// snk_editor_api_version (SNK_DECLARE_EDITOR_MODULE) and checked by the host before it creates the module: the major
// must match, the module's minor must not exceed the host's. Raise the major for a change that breaks a module's
// build or binary, the minor for an addition.
// 1.1: editor_module_context::add_asset_preview (the preview document of an asset suffix, asset_preview_desc).
#define SNK_EDITOR_API_VERSION 0x0001'0001u

namespace edt
{
	// What a game_editor module (#588) gets: the editor's services, and the registries, jobs and problems only through
	// functions that keep what they register in the module's scope. Not handed out: dialog_service (its callbacks would
	// outlive the module; a wrapper comes with the first module that needs a dialog). Unloading the module undoes all
	// of it, so the editor is as it was
	// before the load (and a reload registers again without clashing with the old entries).
	class SNK_EDITOR_API editor_module_context
	{
	public:
		editor_module_context(editor_context& editor, registration_scope& scope);
		editor_module_context(const editor_module_context&) = delete;
		editor_module_context& operator=(const editor_module_context&) = delete;

		void add_panel(panel_desc desc);
		void add_menu(std::string name);
		void add_action(action_desc desc);
		void add_action(cmd::command_desc command, std::optional<action_ui> ui);
		void add_inspector(std::string type, inspector_renderer renderer);
		void add_asset_kind(asset_kind_desc desc);
		// The document that previews the assets of a suffix (editor API 1.1, #589). Its make_preview runs on the editor's main
		// thread when the preview opens; the document it makes must run its tasks on tasks() (not on jobs(), which the module
		// does not own), and its close() and destructor run at unload after shutdown_editor(): they must not rely on
		// anything the module's shutdown_editor() released.
		void add_asset_preview(asset_preview_desc desc);

		template<class... Args, class F>
		void subscribe(signal<Args...>& event, F&& handler)
		{
			m_scope.keep(event.subscribe(std::function<void(Args...)>(std::forward<F>(handler))));
		}

		// The owner of the jobs and task continuations the module starts: they are stopped when it unloads, and none
		// runs after shutdown_editor.
		const ds::lifetime& tasks() const { return m_scope.tasks(); }
		ds::task_system& task_system() const { return m_editor.tasks(); }

		// Jobs of the module: bound to tasks(), so unloading stops them. The job_service is read-only for a module
		// (job.list, job_info), starting goes through here.
		template<class F>
		job_service::job_id start_job(ds::lane where, ds::task_desc desc, F&& work,
			std::function<void(ds::task_result<std::invoke_result_t<F&, ds::task_context&>>&)> on_done)
		{
			return m_editor.jobs().start(where, std::move(desc), m_scope.tasks(), std::forward<F>(work), std::move(on_done));
		}
		template<class F>
		job_service::job_id start_job_with_result(ds::lane where, ds::task_desc desc, F&& work,
			std::function<void(ds::task_result<boost::json::value>&)> on_done = {})
		{
			return m_editor.jobs().start_with_result(where, std::move(desc), m_scope.tasks(), std::forward<F>(work), std::move(on_done));
		}

		// The problems of `source`, cleared when the module unloads.
		void set_problems(std::string source, std::vector<problem> items);

		res::resource_system& resources() const { return m_editor.resources(); }
		rnd::render_system& render() const { return m_editor.render(); }
		gui::gui_system& gui() const { return m_editor.gui(); }
		editor_services& services() const { return m_editor.services(); }
		document_manager& documents() { return m_editor.documents(); }
		editor_events& events() { return m_editor.events(); }
		selection_service& selection() { return m_editor.selection(); }
		const job_service& jobs() const { return m_editor.jobs(); }
		const problem_list& problems() const { return m_editor.problems(); }

	private:
		editor_context& m_editor;
		registration_scope& m_scope;
	};

	// The base of a game_editor module. The host puts the editor_context in the storage it hands over; the module only
	// writes register_editor. Declare it with SNK_DECLARE_EDITOR_MODULE. The host always calls shutdown_services before
	// it destroys the module, also when shutdown_editor throws: destroying the module first would run its queued
	// callbacks over members that are already gone. The module's DLL must not touch edt:: from a static initializer.
	// Still not tracked by the context: services() (the engine's own registries, which a module reaches through its
	// game layer anyway) and the raw signals of events() (use editor_module_context::subscribe: a connection made
	// by hand must be dropped in shutdown_editor).
	class SNK_EDITOR_API editor_extension : public app::modules::module_interface
	{
	public:
		editor_extension();
		~editor_extension() override;
		// Explicit: dllexport on MSVC would otherwise generate copies of the move-only members.
		editor_extension(const editor_extension&) = delete;
		editor_extension& operator=(const editor_extension&) = delete;

		virtual void register_editor(editor_module_context& context) = 0;
		// Called after the module's tasks are stopped (none runs or delivers from here on) and before what it registered
		// is removed; the module's own state goes here. A task of the module must honour its stop token.
		virtual void shutdown_editor() {}

		// The editor module: it registers into the editor, so it loads after it and goes before it.
		std::vector<std::string_view> depends_on() const override { return { "editor" }; }

		void register_services(ds::app_data_storage& data) final;
		void initialize_services(ds::app_data_storage& data) final;
		void shutdown_services(ds::app_data_storage& data) final;

	private:
		editor_context* m_editor = nullptr;
		std::unique_ptr<registration_scope> m_scope;
		std::unique_ptr<editor_module_context> m_context;
	};

	// Empty when the host can use a module that reports `module_api_version` (the function a module exports as
	// snk_editor_api_version; null when it has none): the major equals SNK_EDITOR_API_VERSION's and the minor is not
	// above it. Otherwise why not. The host checks this before it creates the module.
	[[nodiscard]] SNK_EDITOR_API std::string check_editor_api_version(std::uint32_t (*module_api_version)());
}

// The four module functions (SNK_DECLARE_MODULE) plus snk_editor_api_version.
#define SNK_DECLARE_EDITOR_MODULE_PREFIXED(prefix, Type) \
	SNK_DECLARE_MODULE_PREFIXED(prefix, Type) \
	extern "C" SNK_MODULE_EXPORT std::uint32_t prefix##editor_api_version() \
	{ \
		return SNK_EDITOR_API_VERSION; \
	}

#define SNK_DECLARE_EDITOR_MODULE(Type) SNK_DECLARE_EDITOR_MODULE_PREFIXED(snk_, Type)

// The module_entry of an editor module declared with SNK_DECLARE_EDITOR_MODULE_PREFIXED(prefix, ...): the five functions.
#define SNK_EDITOR_MODULE_ENTRY(prefix) \
	::app::modules::module_entry{ &prefix##module_api_version, &prefix##module_build_info, &prefix##create_module, \
		&prefix##destroy_module, &prefix##editor_api_version }
