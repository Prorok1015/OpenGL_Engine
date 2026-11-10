#pragma once
#include "edt_api.h"
#include "edt_action_registry.h"
#include "edt_asset_kind_registry.h"
#include "edt_dialog_service.h"
#include "edt_document_manager.h"
#include "edt_editor_events.h"
#include "edt_editor_services.h"
#include "edt_inspector_registry.h"
#include "edt_job_service.h"
#include "edt_panel_registry.h"
#include "edt_problem_list.h"
#include "edt_registration_scope.h"
#include "edt_selection_service.h"
#include <string>
#include <vector>

namespace res { class resource_system; }
namespace rnd { class render_system; }
namespace gui { class gui_system; }
namespace ds { class task_system; }
namespace cmd { class command_registry; }

namespace edt
{
	// Facade over the engine services and the editor's own services. Created by editor_system::init and
	// registered in ds::app_data_storage (data.require<edt::editor_context>()). Engine services are
	// references and are not owned; `commands` is the process-wide command registry that actions() adds the
	// editor's part to (and that the engine registered log.read into). The job.*, dialog.* and problems.list
	// commands are registered here and live as long as the context; adopt() lets the editor hand it more
	// registrations (the commands that read the scene) to live and die with it. A job that failed (threw) is logged and shown as an error
	// message dialog; the destructor waits for the running jobs, so build the context after what they use. The scene document belongs to the editor (its controllers are built
	// on it before the context exists); the context's document_manager refers to it and owns the previews, which the
	// asset kinds of asset_kinds() make (asset_kind_desc::make_preview).
	// The engine services the editor works through (context manager, assembler, system factory) are the editor's
	// editor_services, which the context refers to: they live as long as the editor, so the context must not be used
	// after the editor_system is gone. Game data (descs, levels, prefabs) goes through services() at the moment of use,
	// since the game layer can change; resources() is the base resource system, for the editor's own assets.
	class SNK_EDITOR_API editor_context
	{
	public:
		editor_context(res::resource_system& resources, rnd::render_system& render,
			gui::gui_system& gui, editor_services& services, ds::task_system& tasks, cmd::command_registry& commands,
			document& scene);
		~editor_context();
		editor_context(const editor_context&) = delete;
		editor_context& operator=(const editor_context&) = delete;

		res::resource_system& resources() const { return m_resources; }
		rnd::render_system& render() const { return m_render; }
		gui::gui_system& gui() const { return m_gui; }
		ds::task_system& tasks() const { return m_tasks; }
		editor_services& services() const { return m_services; }

		document_manager& documents() { return m_documents; }
		const document_manager& documents() const { return m_documents; }
		editor_events& events() { return m_events; }
		const editor_events& events() const { return m_events; }
		selection_service& selection() { return m_selection; }
		const selection_service& selection() const { return m_selection; }
		dialog_service& dialogs() { return m_dialogs; }
		job_service& jobs() { return m_jobs; }
		problem_list& problems() { return m_problems; }
		const problem_list& problems() const { return m_problems; }
		panel_registry& panels() { return m_panels; }
		action_registry& actions() { return m_actions; }
		inspector_registry& inspectors() { return m_inspectors; }
		asset_kind_registry& asset_kinds() { return m_asset_kinds; }

		// The registrations die with the context, before the services their commands call.
		void adopt(std::vector<registration> added);

		// The scopes of the game_editor modules that are loaded (editor_extension keeps its own here): the host asks before
		// it unloads one.
		void track_module_scope(const registration_scope& scope);
		void untrack_module_scope(const registration_scope& scope);
		// Empty when the modules can be unloaded now; otherwise why not: one of their task continuations is running. Only
		// those are counted: a reload started from a module's panel draw, action or inspector callback is not seen (the
		// registries do not count their callbacks), so such code must not ask for a reload.
		std::string module_unload_refusal() const;

	private:
		res::resource_system& m_resources;
		rnd::render_system& m_render;
		gui::gui_system& m_gui;
		ds::task_system& m_tasks;
		editor_services& m_services;
		editor_events m_events;
		document_manager m_documents;
		selection_service m_selection{ m_events };
		dialog_service m_dialogs;
		job_service m_jobs;
		problem_list m_problems{ &m_events };
		panel_registry m_panels;
		action_registry m_actions;
		inspector_registry m_inspectors;
		asset_kind_registry m_asset_kinds;
		// After the services they call and the registry they live in: destroyed first.
		std::vector<registration> m_service_commands;
		std::vector<const registration_scope*> m_module_scopes;
		// Shows a failed job to the person; uses m_jobs and m_dialogs, so it goes first.
		connection m_job_failures;
	};
}
