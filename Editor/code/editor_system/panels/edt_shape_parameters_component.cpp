#include "edt_shape_parameters_component.h"
#include "edt_document_cast.h"
#include "edt_inspector_panel.h"
#include "edt_prefab_document.h"
#include "edt_scene_document.h"
#include "edt_shape_parameters_section.h"
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/model_codec/res_tag.h>
#include <snk/resource/res_system.h>
#include <format>
#include <optional>

namespace json = boost::json;

edt::shape_parameters_component::shape_parameters_component(ports from)
	: m_ports(std::move(from))
{
	shape_parameters_section::ports section;
	section.read_poly = [this](const std::string& poly_path) {
		const desc::desc_system* descs = m_ports.services.descs();
		if (!descs || !m_ports.desc_layout) return desc_file_read{};
		return read_desc_file(m_ports.resources, *descs, m_ports.desc_layout(), res::tag::make(poly_path));
	};
	section.invoke = [this](std::string_view command, const json::value& args) { return m_ports.context.actions().invoke(command, args); };
	section.facts = [this](std::string_view geometry, const std::string& poly_path) {
		mesh::rebuild_facts facts;
		facts.poly = poly_path;
		facts.level_open = !m_ports.state.world_descs.empty();
		facts.level_dirty = m_ports.state.is_dirty();
		for (auto& world : m_ports.state.world_descs) facts.level_nodes += mesh::count_shape_nodes(world.get_root(), geometry);
		if (const prefab_document* prefab = prefab_of(m_ports.context.documents()); prefab && prefab->working()) {
			facts.prefab_open = true;
			facts.prefab_dirty = prefab->is_dirty();
			facts.prefab_nodes = mesh::count_shape_nodes(prefab->working()->get_root(), geometry);
		}
		return facts;
	};
	section.confirm = [this](std::string title, std::string text, std::function<void()> proceed) {
		m_ports.context.dialogs().choose(std::move(title), std::move(text), { "Rebuild", "Cancel" },
			[proceed = std::move(proceed)](std::optional<std::size_t> choice) {
				if (choice == std::optional<std::size_t>{ 0 }) proceed();
			});
	};
	section.retarget_prefab = [this](std::string_view geometry, const json::object& range) {
		prefab_document* prefab = prefab_of(m_ports.context.documents());
		if (!prefab || !prefab->working()) return;
		prefab->edit([&](scn::prefab_desc& working) { return mesh::retarget_shape_nodes(working.get_root(), geometry, range) > 0; });
	};
	section.report_error = [this](std::string text) {
		egLOG_WARN("editor/shape", "{}", text);
		m_ports.context.dialogs().message(message_level::error, std::move(text));
	};
	section.locked = [this] { return m_ports.state.edits_locked(); };
	m_section = std::make_unique<shape_parameters_section>(std::move(section));

	m_ports.inspector.set_component_section([this](const std::string& type, const scn::prefab_desc::prefab_comp_node& comp, const gui::theme_semantic_colors& colors) {
		if (type == "mesh_node_desc") m_section->draw(comp, colors);
	});
}

edt::shape_parameters_component::~shape_parameters_component()
{
	m_ports.inspector.set_component_section(nullptr);
}
