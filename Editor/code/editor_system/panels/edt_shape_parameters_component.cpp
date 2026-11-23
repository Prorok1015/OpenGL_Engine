#include "edt_shape_parameters_component.h"
#include "edt_document_cast.h"
#include "edt_inspector_panel.h"
#include "edt_prefab_document.h"
#include "edt_prefab_shape.h"
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
	const auto shown_prefab = [this]() -> prefab_document* { return m_ports.shown_prefab ? m_ports.shown_prefab() : nullptr; };
	section.read_poly = [this](const std::string& poly_path) {
		const desc::desc_system* descs = m_ports.services.descs();
		if (!descs || !m_ports.desc_layout) return desc_file_read{};
		return read_desc_file(m_ports.resources, *descs, m_ports.desc_layout(), res::tag::make(poly_path));
	};
	section.facts = [this](std::string_view geometry, const std::string& poly_path, const json::object& range) {
		mesh::rebuild_facts facts;
		facts.poly = poly_path;
		facts.level_open = !m_ports.state.world_descs.empty();
		facts.level_dirty = m_ports.state.is_dirty();
		facts.level_agent_changes = m_ports.state.has_agent_changes();
		for (auto& world : m_ports.state.world_descs) facts.level_nodes += mesh::count_stale_shape_nodes(world.get_root(), geometry, range);
		if (const prefab_document* prefab = shown_prefab(); prefab && prefab->working()) {
			facts.prefab_open = true;
			facts.prefab_dirty = prefab->is_dirty();
			facts.prefab_blocker = prefab_rebuild_blocker(*prefab);
			facts.prefab_nodes = mesh::count_stale_shape_nodes(prefab->working()->get_root(), geometry, range);
		}
		return facts;
	};
	section.confirm = [this](std::string title, std::string text, std::function<void()> proceed) {
		m_ports.context.dialogs().choose(std::move(title), std::move(text), { "Rebuild", "Cancel" },
			[proceed = std::move(proceed)](std::optional<std::size_t> choice) {
				if (choice == std::optional<std::size_t>{ 0 }) proceed();
			});
	};
	section.rebuild = [this](const json::object& arguments, std::string_view geometry) {
		return rebuild_shape(m_ports.context.actions(), shown_prefab(), arguments, geometry);
	};
	section.report_error = [this](std::string text) {
		egLOG_WARN("editor/shape", "{}", text);
		m_ports.context.dialogs().message(message_level::error, std::move(text));
	};
	section.locked = [this] {
		if (m_ports.state.edits_locked()) return true;
		const prefab_document* prefab = shown_prefab();
		return prefab && prefab->edits_locked();
	};
	m_section = std::make_unique<shape_parameters_section>(std::move(section));

	m_ports.inspector.set_component_section([this](const std::string& type, const scn::prefab_desc::prefab_comp_node& comp, const gui::theme_semantic_colors& colors) {
		if (type == "mesh_node_desc") m_section->draw(comp, colors);
	});
}

edt::shape_parameters_component::~shape_parameters_component()
{
	m_ports.inspector.set_component_section(nullptr);
}
