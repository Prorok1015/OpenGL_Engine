#include "edt_problems_component.h"
#include "edt_asset_component.h"
#include "edt_asset_problems.h"
#include "edt_input_actions_view.h"
#include "edt_problems_panel.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_services.h>
#include <imgui.h>
#include <algorithm>
#include <utility>

edt::problems_component::problems_component(ports from)
	: m_ports(std::move(from))
{
}

edt::problems_component::~problems_component() = default;

void edt::problems_component::install()
{
	m_connections.push_back(m_ports.context.events().document_changed.subscribe([this](document_id, std::uint64_t) { refresh_world(); }));
	m_connections.push_back(m_ports.context.events().active_world_changed.subscribe([this](document_id) { refresh_world(); }));
}

void edt::problems_component::wire(problems_panel& panel)
{
	panel.set_on_activate([this](const std::string& source, const problem& item) { go_to_problem(source, item); });
	// The idle gate of Edit > Undo (register_edit_actions): no dialog waiting, no gesture in progress.
	const auto problems_idle = [this] { return m_ports.context.dialogs().pending() == 0 && !ImGui::IsAnyItemActive(); };
	// The asset fixes are the person's own: no command, so no agent reaches them.
	panel.set_actions([this](const std::string& source, const problem& item) {
		std::vector<std::pair<std::string, std::function<void()>>> out;
		asset_component* assets = m_ports.assets();
		if (source != asset_problem_publisher::source || !assets) {
			return out;
		}
		for (problem_action& action : assets->problem_actions(item)) {
			out.emplace_back(action.label, [label = action.label, run = std::move(action.run)] {
				if (const asset_fix_result result = run(); !result.ok) {
					egLOG_WARN("assets", "{}: {}", label, result.error);
				}
			});
		}
		return out;
	}, problems_idle);
	panel.set_undo(
		[this]() -> std::optional<std::string> {
			asset_component* assets = m_ports.assets();
			return assets ? assets->next_undo_label() : std::nullopt;
		},
		[this] {
			asset_component* assets = m_ports.assets();
			if (!assets) {
				return;
			}
			if (const asset_fix_result result = assets->undo_last(); !result.ok) {
				egLOG_ERROR("assets", "{}", result.error);
			}
		},
		problems_idle);
}

void edt::problems_component::refresh_world()
{
	m_world.refresh(m_ports.context.problems(), m_ports.state, m_ports.services.level_set());
}

void edt::problems_component::follow_input_actions(const gs::input_actions& actions)
{
	if (actions.revision() == m_input_revision) return;
	m_input_revision = actions.revision();
	auto problems = input_actions_view::conflict_problems(actions.conflicts());
	if (problems.empty()) m_ports.context.problems().clear("input");
	else m_ports.context.problems().set("input", std::move(problems));
}

void edt::problems_component::go_to_problem(const std::string& source, const problem& item)
{
	if (source == desc_problem_publisher::source) {
		if (const auto target = m_desc.target_of(item)) go_to(*target);
		return;
	}
	if (source == play_preflight_publisher::source) {
		if (const auto target = m_play_preflight.target_of(item)) go_to(*target);
		return;
	}
	if (source == asset_problem_publisher::source) {
		asset_component* assets = m_ports.assets();
		const std::optional<std::string> file = assets ? assets->problem_file(item) : std::nullopt;
		if (!file) {
			return;
		}
		desc_target target;
		target.resource = *file;
		go_to(target);
		return;
	}
	if (source == world_problem_publisher::source) {
		const auto world = m_world.world_of(item);
		if (!world) return;
		desc_target target;
		target.level = m_ports.state.level_tag.string();
		target.world = *world;
		go_to(target);
	}
}

void edt::problems_component::go_to(const desc_target& target)
{
	scene_document& state = m_ports.state;
	if (!target.world.empty()) {
		if (target.level != state.level_tag.string()) {
			egLOG_INFO("editor", "open the level '{}' to go to the node", target.level);
			return;
		}
		if (m_ports.play_active()) {
			egLOG_INFO("editor", "stop Play to go to the node");
			return;
		}
		const auto found = std::ranges::find(state.world_names, target.world);
		const auto index = static_cast<std::size_t>(found - state.world_names.begin());
		if (found == state.world_names.end() || index >= state.world_descs.size()) {
			egLOG_INFO("editor", "the world '{}' is no longer in the level", target.world);
			return;
		}
		if (!target.path.empty()) {
			const auto address = scene_editor::resolve_node_path(state.world_descs[index].get_root().children, target.path);
			if (!address.owner || !address.target.empty()) {
				egLOG_INFO("editor", "the node of this problem is no longer in the world '{}'", target.world);
				return;
			}
		}
		if (state.active_world_idx != static_cast<int>(index)) {
			m_ports.worlds.select_world(static_cast<int>(index));
			// A world the level set does not build is refused: the selection must not point into an inactive world.
			if (state.active_world_idx != static_cast<int>(index)) {
				egLOG_INFO("editor", "the world '{}' could not be made active", target.world);
				return;
			}
		}
		if (target.path.empty()) return;
		selection value;
		value.document = state.id();
		value.world = target.world;
		value.path = target.path;
		m_ports.context.selection().select(std::move(value));
		return;
	}
	if (target.resource.empty()) return;
	if (!m_ports.open_asset(res::tag{ target.resource }, true)) {
		egLOG_INFO("editor", "nothing in the editor opens '{}'", target.resource);
	}
}
