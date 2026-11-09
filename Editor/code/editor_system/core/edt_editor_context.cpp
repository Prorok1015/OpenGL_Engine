#include <snk/editor/edt_editor_context.h>
#include "edt_service_commands.h"
#include <snk/resource/res_system.h>
#include <snk/common/engine_assert.h>
#include <utility>

edt::editor_context::editor_context(res::resource_system& resources, rnd::render_system& render,
	gui::gui_system& gui, editor_services& services, ds::task_system& tasks, cmd::command_registry& commands,
	document& scene, preview_factory make_preview)
	: m_resources(resources)
	, m_render(render)
	, m_gui(gui)
	, m_tasks(tasks)
	, m_services(services)
	, m_documents(scene, std::move(make_preview), m_events)
	, m_jobs(tasks, &m_events)
	, m_actions(commands)
{
	ASSERT_MSG(services.assembler() && services.descs(), "editor_context needs the desc system and the ecs assembler in the editor services");
	adopt(register_job_commands(m_actions.commands(), m_jobs));
	adopt(register_dialog_commands(m_actions.commands(), m_dialogs));
	adopt(register_problem_commands(m_actions.commands(), m_problems));
	m_job_failures = report_failed_jobs(m_events, m_jobs, m_dialogs);
}

edt::editor_context::~editor_context() = default;

void edt::editor_context::adopt(std::vector<registration> added)
{
	for (auto& r : added) m_service_commands.push_back(std::move(r));
}
