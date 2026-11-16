#include "edt_startup_component.h"
#include "edt_begin_then_poll.h"
#include "edt_editor_layer.h"
#include "edt_editor_mode.h"
#include "edt_engine_version_prompt.h"
#include "edt_level_load_component.h"
#include "edt_path_text.h"
#include "edt_project_trust_service.h"
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/project/prj_project.h>
#include <snk/scene/level/scn_start_level.h>
#include <boost/json/object.hpp>
#include <imgui.h>
#include <format>
#include <optional>
#include <utility>

namespace
{
	// Level opened at startup, e.g. --set editor/startup_level levels/smoke_level.desc (automation, headless runs).
	CFG_VAR_DEF_STRING(cfg_startup_level, "editor/startup_level", "");
	// "game" (--game, #294): once the start level has loaded, the Game view fills the window and Play starts; "editor":
	// the panels. Anything else warns and is "editor".
	CFG_VAR_DEF_STRING(cfg_start_mode, "editor/start_mode", "editor");
	// --game=res://<level>: the level game mode starts on, instead of editor/startup_level. Only a res:// tag is taken.
	CFG_VAR_DEF_STRING(cfg_game_level, "editor/game_level", "");
}

edt::startup_component::startup_component(ports from)
	: m_ports(std::move(from))
{
}

edt::startup_component::~startup_component() = default;

void edt::startup_component::install_start_level()
{
	// A project held back for its engine version: nothing of it is mounted, and no start level is loaded either.
	const bool project_held = m_ports.held != nullptr;
	if (project_held) {
		prj::held_project& held = *m_ports.held;
		// Open Anyway, and Migrate... (#761): the same restart, the new editor opening the Project Migration window.
		const auto restart_on = [&context = m_ports.context, manifest = held.manifest_file](bool open_migration) {
			const auto outcome = context.actions().invoke("file.open_project", boost::json::object{
				{ "path", utf8_of(manifest) }, { "allow_engine_version_mismatch", true }, { "discard_unsaved", true },
				{ "open_migration", open_migration } });
			if (!outcome.ok) {
				context.dialogs().message(message_level::error, std::format("The project was not opened: {}.", outcome.error));
			}
		};
		ask_about_held_project(m_ports.context.dialogs(), held, m_ports.exit, [restart_on] { restart_on(false); },
			[restart_on] { restart_on(true); });
	}

	std::string startup_level = m_ports.start_level_name(*cfg_startup_level);
	const std::optional<editor_mode> start_mode = editor_mode_of(*cfg_start_mode);
	if (!start_mode) {
		egLOG_WARN("editor", "editor/start_mode '{}' is neither 'editor' nor 'game': the editor starts in editor mode", *cfg_start_mode);
	}
	const bool start_in_game = start_mode == editor_mode::game;
	if (start_in_game && !cfg_game_level->empty()) {
		if (const std::string problem = game_level_problem(*cfg_game_level); problem.empty()) {
			startup_level = *cfg_game_level;
		} else {
			// Never read as a file path; and no other level stands in for the one asked for.
			egLOG_ERROR("editor", "--game='{}' is not taken: {}; no level is loaded at startup", *cfg_game_level, problem);
			startup_level.clear();
		}
	}
	if (start_in_game && (project_held || startup_level.empty())) {
		egLOG_WARN("editor", "game mode needs a start level (--game=<level>, editor/startup_level or the project's start "
			"scene) and none is loaded: the editor starts in editor mode");
	}
	if (!project_held && !startup_level.empty()) {
		// On the first frame: the host loads the project's game modules after init (#428), and the level may use their types.
		const res::tag level = scn::start_level_tag(startup_level);
		const auto modes = [this]() -> editor_mode_controller* { return m_ports.modes ? m_ports.modes() : nullptr; };
		// #518: a neutral loading screen until the load ends; every end of it (loaded, failed, Esc in its modal) passes
		// through the poll below to finish_startup.
		if (editor_mode_controller* controller = modes(); start_in_game && controller) controller->begin_startup(std::string(level.view()));
		m_ports.layer.register_implicit("edt/load_level", begin_then_poll([this, level] { static_cast<void>(m_ports.level_load.begin(level)); },
			[this, start_in_game, modes] {
				bool loaded = false;
				if (m_ports.level_load.poll(&loaded)) {
					return true;
				}
				// Begun in a frame, the load ends a frame later, and closing its modal focuses the top panel. The
				// editor starts with no panel focused, as when the load began before the first frame (#437).
				ImGui::SetWindowFocus(nullptr);
				if (editor_mode_controller* controller = modes(); start_in_game && controller) {
					controller->finish_startup(loaded, static_cast<bool>(m_ports.stop_after_level_loaded));
				}
				return false;
			}));
	}
}

void edt::startup_component::install_trust_check()
{
	// #921: main loads the project's modules after init, so the first frame knows which wait for the person's answer.
	// Headless nobody is asked: they stay unloaded unless --trust-project was given.
	if (m_ports.held != nullptr) return;
	m_ports.layer.register_implicit("edt/project_trust", [this] {
		const auto trust = m_ports.trust.lock();
		if (!trust || !trust->waiting()) return false;
		if (m_ports.headless) {
			egLOG_WARN("editor/trust", "not loaded: {}: project modules are not trusted: pass --trust-project",
				module_list(trust->waiting_modules()));
		} else if (m_ports.ask_trust) {
			m_ports.ask_trust(nullptr);
		}
		return false;
	});
}
