#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_service_commands.h>
#include <snk/editor/edt_state_commands.h>
#include <snk/resource/res_system.h>
#include <snk/common/engine_assert.h>
#include "edt_scene_document.h"
#include <utility>

edt::editor_context::editor_context(res::resource_system& resources, rnd::render_system& render,
	gui::gui_system& gui, editor_services& services, ds::task_system& tasks, cmd::command_registry& commands,
	scene_document& scene)
	: m_resources(resources)
	, m_render(render)
	, m_gui(gui)
	, m_services(services)
	, m_documents(scene, services, m_events)
	, m_jobs(tasks, &m_events)
	, m_actions(commands)
{
	ASSERT_MSG(services.assembler() && services.descs(), "editor_context needs the desc system and the ecs assembler in the editor services");
	auto add_all = [this](std::vector<registration> added) {
		for (auto& r : added) m_service_commands.push_back(std::move(r));
	};
	add_all(register_job_commands(m_actions.commands(), m_jobs));
	add_all(register_dialog_commands(m_actions.commands(), m_dialogs));
	add_all(register_problem_commands(m_actions.commands(), m_problems));
	add_all(register_state_commands(m_actions.commands(), state_command_sources{
		m_documents, m_selection, m_services, m_asset_kinds,
		[this] { return m_resources.get_resource_layers(); } }));
	m_job_failures = report_failed_jobs(m_events, m_jobs, m_dialogs);
}

edt::editor_context::~editor_context() = default;
