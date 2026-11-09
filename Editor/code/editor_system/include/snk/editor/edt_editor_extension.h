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
#include <utility>

// The version of the editor API a game_editor module is built against (major << 16 | minor), exported by the module as
// snk_editor_api_version (SNK_DECLARE_EDITOR_MODULE) and checked by the host before it creates the module: the major
// must match, the module's minor must not exceed the host's. Raise the major for a change that breaks a module's
// build or binary, the minor for an addition.
#define SNK_EDITOR_API_VERSION 0x0001'0000u

namespace edt
{
	// What a game_editor module (#588) gets: the editor's services, and the registries only through functions that keep
	// what they register in the module's scope. Unloading the module undoes all of it, so the editor is as it was
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

		template<class... Args, class F>
		void subscribe(signal<Args...>& event, F&& handler)
		{
			m_scope.keep(event.subscribe(std::function<void(Args...)>(std::forward<F>(handler))));
		}

		// The owner of the jobs and task continuations the module starts: they are stopped when it unloads, and none
		// runs after shutdown_editor.
		const ds::lifetime& tasks() const { return m_scope.tasks(); }

		res::resource_system& resources() const { return m_editor.resources(); }
		rnd::render_system& render() const { return m_editor.render(); }
		gui::gui_system& gui() const { return m_editor.gui(); }
		editor_services& services() const { return m_editor.services(); }
		document_manager& documents() { return m_editor.documents(); }
		editor_events& events() { return m_editor.events(); }
		selection_service& selection() { return m_editor.selection(); }
		dialog_service& dialogs() { return m_editor.dialogs(); }
		job_service& jobs() { return m_editor.jobs(); }
		problem_list& problems() { return m_editor.problems(); }

	private:
		editor_context& m_editor;
		registration_scope& m_scope;
	};

	// The base of a game_editor module. The host puts the editor_context in the storage it hands over; the module only
	// writes register_editor. Declare it with SNK_DECLARE_EDITOR_MODULE.
	class SNK_EDITOR_API editor_extension : public app::modules::module_interface
	{
	public:
		~editor_extension() override;

		virtual void register_editor(editor_module_context& context) = 0;
		// Before everything the module registered is removed; the module's own state goes here.
		virtual void shutdown_editor() {}

		void register_services(ds::app_data_storage& data) final;
		void initialize_services(ds::app_data_storage& data) final;
		void shutdown_services(ds::app_data_storage& data) final;

	private:
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
