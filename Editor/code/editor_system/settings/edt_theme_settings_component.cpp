#include "edt_theme_settings_component.h"
#include "edt_editor_settings_panel.h"
#include "edt_theme_settings.h"
#include "../controller/edt_editor_layer.h"

#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/gui/gui_gui_service_init.h>

edt::theme_settings_component::theme_settings_component(ports from)
	: m_ports(from)
{
	m_model = std::make_unique<theme_settings_model>(theme_settings_model::ports{ .resources = m_ports.resources,
		.descs = m_ports.descs, .themes = m_ports.themes,
		.theme_set_by_config = engine::gui::theme_set_by_config(), .list_files = {} });
	m_panel = std::make_shared<editor_settings_panel>(*m_model);
	m_model->start();
	m_ports.layer.register_implicit("edt/theme_settings", [this] {
		m_model->poll();
		return true;
	});
}

edt::theme_settings_component::~theme_settings_component() = default;

void edt::theme_settings_component::install(std::vector<registration>& out)
{
	out.push_back(m_ports.context.panels().add({ .id = m_panel->get_id(), .title = m_panel->get_title(),
		.slot = dock_slot::floating, .open_by_default = false, .panel = m_panel }));
	out.push_back(m_ports.context.actions().add({ .id = "editor.settings.open", .label = "Editor Settings...",
		.menu_path = "Edit", .separator_before = true,
		.execute = no_args([this] {
			m_ports.context.panels().set_open(m_panel->get_id(), true);
			m_panel->focus();
		}),
		.expose_as_tool = false, .category = cmd::agent_category::not_for_agents }));
}
