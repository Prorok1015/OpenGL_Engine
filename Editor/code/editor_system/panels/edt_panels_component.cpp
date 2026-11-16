#include "edt_panels_component.h"
#include "edt_asset_browser_panel.h"
#include "edt_blackboard_panel.h"
#include "edt_build_settings_panel.h"
#include "edt_code_commands.h"
#include "edt_code_section.h"
#include "edt_console_panel.h"
#include "edt_game_view_panel.h"
#include "edt_git_panel.h"
#include "edt_input_actions_panel.h"
#include "edt_input_target.h"
#include "edt_inspector_panel.h"
#include "edt_override_edit.h"
#include "edt_problems_component.h"
#include "edt_problems_panel.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_ui_probe.h"
#include "edt_viewport_panel.h"
#include "edt_world_controller.h"
#include "edt_world_systems_panel.h"
#include <snk/config/cfg_variables.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/gui/gui_system.h>
#include <snk/platform/plt_shell_open.h>
#include <snk/project/prj_project.h>
#include <snk/render/rnd_capture_service.h>
#include <snk/render/texture/rnd_render_targets.h>
#include <boost/json/value.hpp>
#include <snk/render/rnd_render_system.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world.h>
#include <glm/vec3.hpp>
#include <format>
#include <span>
#include <utility>

namespace
{
	// Opens the Game view at startup (automation); otherwise it is toggled from the View menu.
	CFG_VAR_DEF_BOOL(cfg_show_game_view, "editor/show_game_view", false);
	// Logs the screen rectangle of the Asset Browser entries (Editor/UiProbe), so a headless smoke script finds an item by name.
	CFG_VAR_DEF_BOOL(cfg_ui_probe, "editor/ui_probe", false);
}

edt::panels_component::panels_component(ports from)
	: m_ports(std::move(from))
{
	m_hierarchy = std::make_shared<scene_hierarchy_panel>();
	m_inspector = std::make_shared<inspector_panel>();
	m_inspector->set_inspector_registry(&m_ports.context.inspectors());
	m_console = std::make_shared<console_panel>();
	m_asset_browser = std::make_shared<asset_browser_panel>();
	m_asset_browser->set_layers(m_ports.resources.get_resource_layers());
	if (cfg_ui_probe)
		m_asset_browser->set_ui_probe(std::make_shared<ui_probe>());
	if (const auto code_roots = m_ports.resources.mount_roots(prj::k_code_protocol); !code_roots.empty()) {
		code_actions actions{ m_ports.resources, code_tree_model{ code_roots.back() },
			[](const std::filesystem::path& file) { return platform::open_in_shell(file); } };
		m_asset_browser->set_code_section(std::make_shared<code_section>(m_ports.resources, std::move(actions)));
	}
	if (m_ports.capture_log) {
		auto console_weak = std::weak_ptr<console_panel>(m_console);
		m_log_sink = engine::add_log_sink([console_weak](std::string_view category, engine::log_level level, std::string_view message) {
			auto panel = console_weak.lock();
			if (!panel) return;
			edt::log_level edt_level = edt::log_level::info;
			if (level == engine::log_level::warn)  edt_level = edt::log_level::warning;
			if (level == engine::log_level::error) edt_level = edt::log_level::error;
			panel->add_log(edt_level, std::format("[{}] {}", category, message));
		});
	}

	m_inspector->set_desc_source([this] { return m_ports.services.descs(); });
	m_inspector->set_asset_source(m_ports.asset_catalog);
	{
		// One composed view of the instances for the inspector and the hierarchy; edits and pushes invalidate it.
		auto view_cache = std::make_shared<override_edit::view_cache>(m_ports.services, m_ports.state);
		m_inspector->set_view_cache(view_cache);
		m_hierarchy->set_view_cache(std::move(view_cache));
	}
	// "Changed by agent" (AI_INTEGRATION.md §3.4): nodes of the active world an agent's edits changed, not saved or accepted.
	const auto changed_by_agent = [this](std::uint64_t node_id) {
		return m_ports.state.has_active_world()
			&& m_ports.state.agent_changes().contains({ static_cast<std::size_t>(m_ports.state.active_world_idx), node_id });
	};
	m_hierarchy->set_agent_marks(changed_by_agent);
	m_inspector->set_agent_marks(changed_by_agent);

	m_viewport = std::make_shared<viewport_panel>(m_ports.rnd, m_ports.gui);
	m_viewport->set_registry_getter([this]() -> entt::registry* { return view_registry(m_ports.services, m_ports.state); });
	m_viewport->set_input_router(m_ports.input_router, [this] { return active_input_target(m_ports.services, m_ports.state); });
	m_viewport->set_on_asset_dropped([this](const std::string& res_rel_path) {
		if (!m_ports.state.editor_tag.is_valid()) return;
		m_ports.context.asset_kinds().drop_into_scene(m_ports.context, res::tag::make(res_rel_path));
	});
	wire_scene_callbacks();
}

edt::panels_component::~panels_component() = default;

void edt::panels_component::wire_scene_callbacks()
{
	m_hierarchy->set_on_world_changed([this](int idx) {
		// A refused switch leaves the old world active: resync the tab and the panels to it.
		if (!m_ports.worlds.switch_to_world(idx) && m_ports.worlds.on_world_switched)
			m_ports.worlds.on_world_switched(m_ports.state.active_world_idx);
	});
	m_hierarchy->set_on_create_world([this](const std::string& name) {
		m_ports.worlds.create_world(name);
	});
	m_hierarchy->set_on_create_node([this](const std::string& type_name) {
		m_ports.scene.create_entity(type_name);
	});
	m_hierarchy->set_on_delete_node([this](uint64_t node_id) {
		m_ports.scene.delete_entity(node_id);
	});
	m_hierarchy->set_on_delete_nodes([this](const std::vector<uint64_t>& node_ids) {
		m_ports.scene.delete_entities(node_ids);
	});
	m_hierarchy->set_on_rename_node([this](const std::string& /*old_name*/, const std::string& /*new_name*/) {
		m_ports.state.commit("Rename node");
	});
	m_hierarchy->set_on_duplicate_node([this](uint64_t node_id) {
		m_ports.scene.duplicate_entity(node_id);
	});
	m_hierarchy->set_on_duplicate_nodes([this](const std::vector<uint64_t>& node_ids) {
		m_ports.scene.duplicate_entities(node_ids);
	});
	m_hierarchy->set_on_reparent_node([this](uint64_t node_id, uint64_t new_parent_id, int insert_index) {
		m_ports.scene.reparent_entity(node_id, new_parent_id, insert_index);
	});

	m_viewport->set_on_transform_committed(
		[this](std::span<const uint64_t> path, glm::vec3 pos, glm::vec3 rot, glm::vec3 scale) {
			m_ports.scene.on_transform_committed(path, pos, rot, scale);
		});
}

void edt::panels_component::add(std::vector<registration>& out, const std::shared_ptr<panel_base>& panel, dock_slot slot, bool open_by_default)
{
	out.push_back(m_ports.context.panels().add({
		.id = panel->get_id(), .title = panel->get_title(), .slot = slot, .open_by_default = open_by_default, .panel = panel }));
}

void edt::panels_component::install(std::vector<registration>& out, const std::function<void()>& after_assets)
{
	add(out, m_hierarchy, dock_slot::left, true);
	add(out, m_inspector, dock_slot::right, true);
	add(out, m_viewport, dock_slot::center, true);
	m_game_view = std::make_shared<game_view_panel>(m_ports.rnd, m_ports.gui);
	// The views show the set "play" while Play runs (view_set), the level otherwise.
	m_game_view->set_registry_getter([this]() -> entt::registry* { return view_registry(m_ports.services, m_ports.state); });
	m_game_view->set_world_name_getter([this] {
		const std::string* name = m_ports.state.active_world_name();
		return name ? *name : std::string{};
	});
	m_game_view->set_all_registries([this](const game_view_panel::registry_visitor& visit) {
		if (scn::context_set* set = view_set(m_ports.services)) {
			set->for_each([&visit](scn::world_context& world) { visit(world.state()); });
		}
	});
	m_game_view->set_input_router(m_ports.input_router, [this] { return active_play_input_target(m_ports.services, m_ports.state); });
	add(out, m_game_view, dock_slot::center, cfg_show_game_view);
	// viewport.capture names the two views by what the panels show; the engine knows only these ids.
	if (m_ports.capture != nullptr) {
		rnd::capture_service& capture = *m_ports.capture;
		out.push_back(capture.add_target("scene", [this] {
			return m_ports.rnd.get_render_targets().current(viewport_panel::render_target_tag());
		}));
		out.push_back(capture.add_target("game", [this] {
			const auto& target = m_game_view->render_target();
			return target ? m_ports.rnd.get_render_targets().current(*target) : rnd::driver::texture_handle{};
		}));
	}
	add(out, m_console, dock_slot::bottom_right, true);
	const std::optional<std::filesystem::path> root = m_ports.project_root();
	m_problems_panel = std::make_shared<problems_panel>(m_ports.context.problems(), root ? *root : std::filesystem::path{});
	m_ports.problems.wire(*m_problems_panel);
	add(out, m_problems_panel, dock_slot::bottom_right, true);
	m_blackboard = std::make_shared<blackboard_panel>();
	m_blackboard->set_manager_getter([this] { return m_ports.services.ctx_manager.lock(); });
	add(out, m_blackboard, dock_slot::bottom_right, false);
	m_input_actions = std::make_shared<input_actions_panel>();
	m_input_actions->set_actions_getter(m_ports.input_actions);
	add(out, m_input_actions, dock_slot::bottom_right, false);
	add(out, m_asset_browser, dock_slot::bottom_left, true);
	after_assets();
	m_build_settings = std::make_shared<build_settings_panel>(m_ports.project_root, m_ports.default_configuration, m_ports.manual_build_running);
	m_build_settings->set_stop_background(m_ports.stop_live_build);
	add(out, m_build_settings, dock_slot::floating, false);
	// #456: through the vcs.* commands as the person's calls; closed by default, so it reads nothing until opened.
	git_panel_model::ports git_ports;
	git_ports.invoke = [this](std::string_view id, boost::json::value args) { return m_ports.context.actions().invoke(id, args); };
	git_ports.cancel = [this](std::uint64_t job) { return m_ports.context.jobs().cancel(job); };
	m_git = std::make_shared<git_panel>(m_ports.context.events(), std::move(git_ports), m_ports.context.dialogs(), m_ports.resources);
	add(out, m_git, dock_slot::right, false);
	world_systems_model::ports systems_ports;
	systems_ports.invoke = [this](std::string_view id, boost::json::value args) { return m_ports.context.actions().invoke(id, args); };
	systems_ports.playing = m_ports.play_active;
	m_world_systems = std::make_shared<world_systems_panel>(m_ports.context.events(), std::move(systems_ports));
	add(out, m_world_systems, dock_slot::right, false);
}
