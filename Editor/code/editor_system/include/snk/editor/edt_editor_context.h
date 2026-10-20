#pragma once
#include "edt_action_registry.h"
#include "edt_asset_kind_registry.h"
#include "edt_dialog_service.h"
#include "edt_document_manager.h"
#include "edt_editor_events.h"
#include "edt_inspector_registry.h"
#include "edt_job_service.h"
#include "edt_panel_registry.h"
#include "edt_selection_service.h"
#include <vector>

namespace desc { class desc_system; }
namespace res { class resource_system; }
namespace rnd { class render_system; }
namespace gui { class gui_system; }
namespace scn { class ecs_assembler; class context_manager; }
namespace ecs { class system_factory; }
namespace ds { class task_system; }
namespace cmd { class command_registry; }

namespace edt
{
	class scene_document;

	// Facade over the engine services and the editor's own services. Created by editor_system::init and
	// registered in ds::app_data_storage (data.require<edt::editor_context>()). Engine services are
	// references and are not owned; `commands` is the process-wide command registry that actions() adds the
	// editor's part to (and that the engine registered log.read into). The job.* and dialog.* commands are
	// registered here and live as long as the context. A job that failed (threw) is logged and shown as an error
	// message dialog; the destructor waits for the running jobs, so build the context after what they use. The scene document belongs to the editor (its controllers are built
	// on it before the context exists); the context's document_manager refers to it and owns the prefab preview.
	class editor_context
	{
	public:
		editor_context(desc::desc_system& descs, res::resource_system& resources, rnd::render_system& render,
			gui::gui_system& gui, scn::ecs_assembler& assembler, ecs::system_factory& systems,
			scn::context_manager& contexts, ds::task_system& tasks, cmd::command_registry& commands,
			scene_document& scene);
		~editor_context();
		editor_context(const editor_context&) = delete;
		editor_context& operator=(const editor_context&) = delete;

		desc::desc_system& descs() const { return m_descs; }
		res::resource_system& resources() const { return m_resources; }
		rnd::render_system& render() const { return m_render; }
		gui::gui_system& gui() const { return m_gui; }
		scn::ecs_assembler& assembler() const { return m_assembler; }
		ecs::system_factory& systems() const { return m_systems; }
		scn::context_manager& contexts() const { return m_contexts; }

		document_manager& documents() { return m_documents; }
		const document_manager& documents() const { return m_documents; }
		editor_events& events() { return m_events; }
		const editor_events& events() const { return m_events; }
		selection_service& selection() { return m_selection; }
		const selection_service& selection() const { return m_selection; }
		dialog_service& dialogs() { return m_dialogs; }
		job_service& jobs() { return m_jobs; }
		panel_registry& panels() { return m_panels; }
		action_registry& actions() { return m_actions; }
		inspector_registry& inspectors() { return m_inspectors; }
		asset_kind_registry& asset_kinds() { return m_asset_kinds; }

	private:
		desc::desc_system& m_descs;
		res::resource_system& m_resources;
		rnd::render_system& m_render;
		gui::gui_system& m_gui;
		scn::ecs_assembler& m_assembler;
		ecs::system_factory& m_systems;
		scn::context_manager& m_contexts;
		editor_events m_events;
		document_manager m_documents;
		selection_service m_selection{ m_events };
		dialog_service m_dialogs;
		job_service m_jobs;
		panel_registry m_panels;
		action_registry m_actions;
		inspector_registry m_inspectors;
		asset_kind_registry m_asset_kinds;
		// After the services they call and the registry they live in: destroyed first.
		std::vector<registration> m_service_commands;
		// Shows a failed job to the person; uses m_jobs and m_dialogs, so it goes first.
		connection m_job_failures;
	};
}
