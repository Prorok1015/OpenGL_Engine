#include "edt_play_component.h"
#include "edt_bus_commands.h"
#include "edt_editor_layer.h"
#include "edt_editor_mode_commands.h"
#include "edt_game_view_panel.h"
#include "edt_inspector_panel.h"
#include "edt_input_target.h"
#include "edt_module_swap_gate.h"
#include "edt_play_commands.h"
#include "edt_play_preflight.h"
#include "edt_problems_panel.h"
#include "edt_runtime_commands.h"
#include "edt_scene_document.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_viewport_panel.h"
#include "edt_world_controller.h"
#include <snk/core/core_project_modules.h>
#include <snk/ecs/ecs_system.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/project/prj_project.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/gui/gui_theme.h>
#include <snk/gui/gui_ui_font.h>
#include <imgui.h>
#include <format>
#include <utility>

edt::play_component::play_component(ports from)
	: m_ports(std::move(from))
{
	play_session_options options;
	options.input = m_ports.input_router;
	// user:// is the project's test profile; Play gets a fresh one of its own every start.
	if (const auto user_roots = m_ports.resources.mount_roots(res::tag::user); !user_roots.empty() && m_ports.project != nullptr) {
		options.profile_resources = &m_ports.resources;
		options.finish_user_writes = m_ports.finish_user_writes;
		options.play_profile = m_ports.project->play_profile_dir();
		options.editor_profile = user_roots;
	}
	if (m_ports.project != nullptr) options.project_root = m_ports.project->root;
	options.report_full_snapshot = m_ports.report_full_snapshot;
	// The problems list, not a dialog: live mode reloads on every build, and a dialog each time would be in the way.
	// Never from inside the swap: the host refuses a revert while a reload runs, so it waits for the next UI frame.
	options.revert_module = [this](const std::string& module) {
		m_ports.layer.register_implicit("edt/revert_game_module", [this, module] {
			if (!m_play) return false;
			core::project_module_host* host = m_ports.module_host();
			m_play->run_revert(
				[&] {
					return host ? host->revert(module, m_ports.swap_gate.hooks())
						: core::reload_report{ core::reload_status::refused, "the editor did not load the project's modules" };
				},
				[&] {
					if (host) host->forget_revert(module);
				});
			return false;
		});
	};
	options.notify = [this](const std::string& message) {
		if (message.empty()) {
			m_ports.context.problems().clear("hot_reload");
			return;
		}
		problem item;
		item.severity = problem_severity::warning;
		item.message = message;
		item.notes = { "details: .cache/hot_reload/last_play_snapshot.json" };
		m_ports.context.problems().set("hot_reload", { std::move(item) });
	};
	options.restore_selection = [this](const selection& value) { return m_ports.hierarchy.select_path(value.path, value.target); };
	// The document changes once these land: Play would be built from the old one and the new one land under it.
	options.start_blocker = m_ports.start_blocker;
	options.each_frame = [this](const std::string& id, std::function<bool()> poll) { m_ports.layer.register_implicit(id, std::move(poll)); };
	options.preflight = [this] {
		play_preflight_services services;
		const ecs::system_factory* factory = m_ports.services.sfactory();
		services.systems = factory ? &factory->registry() : nullptr;
		services.descs = m_ports.services.descs();
		services.resources = m_ports.services.resources();
		services.assembler = m_ports.services.assembler();
		auto found = run_play_preflight(m_ports.level, services);
		m_ports.preflight.publish(m_ports.context.problems(), found);
		return found;
	};
	options.forget_runtime_entities = [this] {
		m_ports.game_view.forget_runtime_entities();
		m_ports.viewport.forget_runtime_entities();
	};
	m_play = std::make_unique<play_session>(m_ports.level, m_ports.services, m_ports.worlds, m_ports.context.selection(), m_ports.context.events(), std::move(options));
	m_ports.game_view.set_notice([this] {
		return m_play && m_play->reloading() ? std::format("Reloading... ({} resources)", m_play->reloading_resources()) : std::string{};
	});
	m_ports.swap_gate.set_play_hooks([this] { if (m_play) m_play->before_module_swap(); }, [this](const core::reload_report& report) {
		if (m_play) m_play->after_module_swap(report);
		if (m_modes) m_modes->after_module_swap();
	});

	const auto locked = [this] { return m_ports.level.edits_locked(); };
	m_ports.hierarchy.set_edits_locked(locked);
	m_ports.inspector.set_edits_locked(locked);
	m_ports.viewport.set_edits_locked(locked);
}

edt::play_component::~play_component() = default;

void edt::play_component::install(std::vector<registration>& out)
{
	// The Play menu and the agent's play.* (US-63-C, #491). The lambdas check m_play: shutdown() lets go of it before the
	// registrations of editor_system go.
	for (auto& command : register_play_commands(m_ports.context.actions(),
			{ .play = [this] { return m_play.get(); }, .person_is_editing = m_ports.person_is_editing,
				.dialogs = &m_ports.context.dialogs(), .show_problems = [this] {
					m_ports.context.panels().set_open(m_ports.problems.get_id(), true);
					ImGui::SetWindowFocus(m_ports.problems.get_title().c_str());
				} })) {
		out.push_back(std::move(command));
	}
	const auto play_set = [this]() -> scn::context_set* {
		const auto manager = m_ports.services.ctx_manager.lock();
		return manager ? manager->find_set(k_play_set) : nullptr;
	};
	// runtime.query / runtime.get (#552): the live entities of Play, read-only.
	for (auto& command : register_runtime_commands(m_ports.context.actions().commands(),
			{ .play = [this] { return m_play.get(); }, .play_set = play_set })) {
		out.push_back(std::move(command));
	}
	// world.events / input.actions (#605): the bus journal and the actions of Play, read-only.
	for (auto& command : register_bus_commands(m_ports.context.actions().commands(),
			{ .play = [this] { return m_play.get(); }, .play_set = play_set,
				.input = [this]() -> const inp::input_router* { return m_ports.input_router; } })) {
		out.push_back(std::move(command));
	}
	// app.advance / app.resume step the editor's whole frame loop: an agent gets them only during Play.
	if (m_ports.frame_commands != nullptr && m_ports.frame_control != nullptr) {
		play_frame_commands frame_commands = register_play_frame_commands(*m_ports.frame_commands,
			m_ports.context.actions().commands(), *m_ports.frame_control, [this] { return m_play && m_play->active(); },
			m_ports.context.events());
		out.push_back(std::move(frame_commands.commands.advance));
		out.push_back(std::move(frame_commands.commands.resume));
		m_play_hold_release = std::move(frame_commands.release_on_stop);
	}

	// Game mode (#294): F11 / View > Game Mode / editor.mode.toggle. The layer draws the Game view full-window instead
	// of the dockspace; the docked panel keeps its open state, so editor mode is drawn exactly as without it.
	// Headless (set by init before this): no layout, so a Stop leaves the mode alone.
	m_modes = std::make_unique<editor_mode_controller>(*m_play, m_ports.context.events(), !m_ports.context.actions().commands().headless());
	for (auto& command : register_editor_mode_commands(m_ports.context.actions(), { .modes = [this] { return m_modes.get(); },
			.play = [this] { return m_play.get(); }, .person_is_editing = m_ports.person_is_editing,
			.agent_mode_of = m_ports.agent_mode_of })) {
		out.push_back(std::move(command));
	}
	m_ports.layer.set_game_layout([this] { return m_modes && m_modes->mode() == editor_mode::game; },
		[this] {
			m_ports.viewport.suspend_camera();
			m_ports.game_view.render_fullscreen();
		},
		is_game_mode_shortcut,
		[this] { m_ports.game_view.bind_fullscreen_input(); });
	m_mode_changed = m_ports.context.events().editor_mode_changed.subscribe([this](editor_mode mode) {
		if (mode == editor_mode::editor) m_ports.viewport.resume_camera();
	});
}

void edt::play_component::shutdown(bool context_alive)
{
	m_mode_changed.disconnect();
	m_modes.reset();
	if (!m_play) return;
	// Stop brings the level back and reports through the context: only while the context is there.
	if (context_alive) {
		m_play.reset();
	} else {
		m_play->abandon();
	}
}

void edt::play_component::draw_status() const
{
	// Stopped: nothing at all, so the menu bar is what it is without Play.
	if (!m_play || !m_play->active()) return;
	const bool paused = m_play->state() == play_state::paused;
	const gui::theme_semantic_colors& colors = m_ports.context.colors();
	const char* text = paused ? "Play: paused" : "Play: playing";
	const ImVec4& color = paused ? colors.warning : colors.success;
	if (gui::ui_font_has_icons()) {
		ImGui::TextColored(color, "%s %s", paused ? gui::icon::pause : gui::icon::play, text);
	} else {
		ImGui::TextColored(color, "%s", text);
	}
}
