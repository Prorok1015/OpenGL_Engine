#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_service_commands.h>
#include <snk/editor/edt_state_commands.h>
#include <snk/resource/res_system.h>
#include "edt_scene_document.h"
#include <utility>

edt::editor_context::editor_context(desc::desc_system& descs, res::resource_system& resources, rnd::render_system& render,
	gui::gui_system& gui, scn::ecs_assembler& assembler, ecs::system_factory& systems,
	scn::context_manager& contexts, ds::task_system& tasks, cmd::command_registry& commands,
	scene_document& scene)
	: m_descs(descs)
	, m_resources(resources)
	, m_render(render)
	, m_gui(gui)
	, m_assembler(assembler)
	, m_systems(systems)
	, m_contexts(contexts)
	, m_documents(scene, m_events, resources, descs)
	, m_jobs(tasks, &m_events)
	, m_actions(commands)
{
	auto add_all = [this](std::vector<registration> added) {
		for (auto& r : added) m_service_commands.push_back(std::move(r));
	};
	add_all(register_job_commands(m_actions.commands(), m_jobs));
	add_all(register_dialog_commands(m_actions.commands(), m_dialogs));
	add_all(register_state_commands(m_actions.commands(), state_command_sources{
		m_documents, m_selection, m_descs, m_assembler, m_asset_kinds,
		[] { return res::resource_system::get_resource_layers(); } }));
	m_job_failures = report_failed_jobs(m_events, m_jobs, m_dialogs);
}

edt::editor_context::~editor_context() = default;
