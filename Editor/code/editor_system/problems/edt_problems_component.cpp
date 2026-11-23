#include "edt_problems_component.h"
#include "edt_asset_component.h"
#include "edt_asset_problems.h"
#include "edt_input_actions_view.h"
#include "edt_instance_problems.h"
#include "edt_problems_panel.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/res_system.h>
#include <imgui.h>
#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <utility>

namespace
{
	constexpr const char* k_instances_scope = "instances";
}

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
			std::string label = action.label;
			out.emplace_back(std::move(label), [this, item, action = std::move(action), layers = assets->database().layers()] {
				const fix_question ask = [this](const problem_action& asked, std::function<void(bool)> answer) {
					dialog_service& dialogs = m_ports.context.dialogs();
					if (!dialogs.can_show()) {
						return false;
					}
					dialogs.choose(asked.label, asked.confirm, { asked.label, "Cancel" },
						[answer = std::move(answer)](std::optional<std::size_t> picked) { answer(picked && *picked == 0); });
					return true;
				};
				// The answer may come after a project switch: the fix is found again only in the project it was asked for.
				auto again = [this, item, label = action.label, layers, alive = std::weak_ptr<int>(m_alive)]() -> std::optional<problem_action> {
					if (alive.expired()) {
						return std::nullopt;
					}
					asset_component* now = m_ports.assets();
					if (!now || now->database().layers() != layers) {
						return std::nullopt;
					}
					for (problem_action& found : now->problem_actions(item)) {
						if (found.label == label) {
							return std::move(found);
						}
					}
					return std::nullopt;
				};
				run_person_fix(action, ask, std::move(again));
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
	refresh_instances();
}

void edt::problems_component::refresh_instances()
{
	const scene_document& state = m_ports.state;
	std::vector<instance_world> worlds;
	level_worlds located;
	located.level = state.level_tag.string();
	located.names = { located.level };
	for (std::size_t i = 0; i < state.world_descs.size(); ++i) {
		const res::tag file = state.world_tag(i);
		const std::string file_name = file.is_valid() ? std::string(file.view()) : located.level;
		worlds.push_back({ &state.world_descs[i], file_name, file.is_valid() ? std::string{} : std::format("/worlds/{}", i) });
		located.worlds.push_back({ i < state.world_names.size() ? state.world_names[i] : std::string{}, &state.world_descs[i].get_root(), file_name });
	}
	const std::vector<res::diagnostic> items = instance_diagnostics(worlds);
	if (items.empty()) {
		m_desc.clear(m_ports.context.problems(), k_instances_scope);
		return;
	}
	m_desc.report(m_ports.context.problems(), k_instances_scope, items, {}, {},
		[&located](const res::diagnostic& item) { return locate_desc(located, item.resource, item.json_path); });
}

void edt::problems_component::follow_input_actions(const gs::input_actions& actions)
{
	if (actions.revision() == m_input_revision) return;
	m_input_revision = actions.revision();
	auto problems = input_actions_view::conflict_problems(actions.conflicts());
	if (problems.empty()) m_ports.context.problems().clear("input");
	else m_ports.context.problems().set("input", std::move(problems));
}

void edt::problems_component::follow_shaders()
{
	m_shader.follow(m_ports.context.problems(), m_ports.context.render());
}

void edt::problems_component::go_to_problem(const std::string& source, const problem& item)
{
	if (source == shader_problem_publisher::source) {
		// The file opens in the app the system gives it, at its top: going to the line is #1291.
		if (item.file.empty() || !m_ports.open_file) return;
		const res::tag file{ item.file };
		const std::optional<res::disk_file> on_disk = m_ports.context.resources().disk_file_of(file);
		if (!on_disk) {
			egLOG_INFO("editor", "'{}' is not a file on the disk", item.file);
			return;
		}
		if (const std::string error = m_ports.open_file(on_disk->file); !error.empty()) {
			egLOG_WARN("editor", "cannot open '{}': {}", on_disk->file.string(), error);
		}
		return;
	}
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
