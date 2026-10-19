#pragma once
#include "edt_editor_events.h"

namespace desc { class desc_system; }
namespace res { class resource_system; }
namespace rnd { class render_system; }
namespace gui { class gui_system; }
namespace scn { class ecs_assembler; }
namespace ecs { class system_factory; }

namespace edt
{
	// Facade over the engine services and the editor's own services. Created by editor_system::init and
	// registered in ds::app_data_storage (data.require<edt::editor_context>()). Engine services are
	// references and are not owned. Registries, documents and selection join it in later steps.
	class editor_context
	{
	public:
		editor_context(desc::desc_system& descs, res::resource_system& resources, rnd::render_system& render,
			gui::gui_system& gui, scn::ecs_assembler& assembler, ecs::system_factory& systems);
		~editor_context();
		editor_context(const editor_context&) = delete;
		editor_context& operator=(const editor_context&) = delete;

		desc::desc_system& descs() const { return m_descs; }
		res::resource_system& resources() const { return m_resources; }
		rnd::render_system& render() const { return m_render; }
		gui::gui_system& gui() const { return m_gui; }
		scn::ecs_assembler& assembler() const { return m_assembler; }
		ecs::system_factory& systems() const { return m_systems; }

		editor_events& events() { return m_events; }
		const editor_events& events() const { return m_events; }

	private:
		desc::desc_system& m_descs;
		res::resource_system& m_resources;
		rnd::render_system& m_render;
		gui::gui_system& m_gui;
		scn::ecs_assembler& m_assembler;
		ecs::system_factory& m_systems;
		editor_events m_events;
	};
}
