#include <snk/editor/edt_editor_context.h>

edt::editor_context::editor_context(desc::desc_system& descs, res::resource_system& resources, rnd::render_system& render,
	gui::gui_system& gui, scn::ecs_assembler& assembler, ecs::system_factory& systems,
	scn::context_manager& contexts)
	: m_descs(descs)
	, m_resources(resources)
	, m_render(render)
	, m_gui(gui)
	, m_assembler(assembler)
	, m_systems(systems)
	, m_contexts(contexts)
{
}

edt::editor_context::~editor_context() = default;
