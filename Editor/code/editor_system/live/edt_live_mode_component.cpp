#include "edt_live_mode_component.h"
#include "edt_agent_policy.h"
#include "edt_build_settings.h"
#include "edt_build_settings_panel.h"
#include "edt_editor_layer.h"
#include "edt_game_builder.h"
#include "edt_game_reloader.h"
#include "edt_live_consent.h"
#include "edt_problems_panel.h"
#include "edt_settings.h"
#include <snk/common/logger/engine_log.h>
#include <snk/core/core_project_modules.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/project/prj_code.h>
#include <snk/project/prj_project.h>
#include <snk/resource/res_system.h>
#include <imgui.h>
#include <filesystem>
#include <utility>

edt::live_mode_component::live_mode_component(ports from)
	: m_ports(std::move(from))
{
}

edt::live_mode_component::~live_mode_component() = default;

void edt::live_mode_component::install()
{
	const std::optional<std::filesystem::path> snk_dir = m_ports.project != nullptr
		? std::optional<std::filesystem::path>{ m_ports.project->snk_dir() } : std::nullopt;
	// No key, no consent: live mode is neither read on nor stored for a project whose path cannot be made canonical.
	const std::optional<std::string> project_key = snk_dir ? canonical_project_settings_key(snk_dir->parent_path())
		: std::nullopt;
	const bool has_code = !m_ports.resources.mount_roots(prj::k_code_protocol).empty();

	live_mode_options live;
	live.build.settings = m_ports.builder.options().settings;
	live.build.make_request = [this](const build_settings& settings) {
		return make_live_build_request(settings, m_ports.builder.options());
	};
	live.build.start_build = [this](build_request request) { return m_ports.builder.start(std::move(request)); };
	live.build.is_building = [this] { return m_ports.builder.is_building(); };
	live.build.stop_background = [this] { m_ports.builder.stop_background(); };
	// The host appears in the storage after init, like for game_reloader: looked up when used.
	live.build.is_loaded_current = [this](const std::string& module) {
		core::project_module_host* host = m_ports.module_host();
		return host != nullptr && host->is_current(module);
	};
	live.build.modules = [this] {
		return m_ports.project != nullptr ? project_module_names(*m_ports.project) : std::vector<std::string>{};
	};
	live.build.is_editor_module = [this](const std::string& module) {
		core::project_module_host* host = m_ports.module_host();
		return host != nullptr && host->is_editor_module(module);
	};
	live.build.reload_pending = [this] {
		return m_ports.layer.has_implicit_prefix(game_reloader::k_reload_frame_id) || m_ports.layer.has_implicit_prefix(live_mode::k_reload_frame_id);
	};
	live.events = &m_ports.context.events();
	live.next_frame = [this](const std::string& id, std::function<void()> fn) {
		m_ports.layer.register_implicit(id, [fn = std::move(fn)] {
			fn();
			return false;
		});
	};
	live.reload_module = [this](const std::string& module, std::function<void(const core::reload_report&)> done) {
		m_ports.reloader.reload_quiet_when_ready(module, std::move(done));
	};
	live.unavailable_reason = [this, has_code] {
		if (std::string reason = m_ports.builder.disabled_reason(); !reason.empty()) return reason;
		return has_code ? std::string{} : std::string{ "The project has no code folder (code://)" };
	};
	if (project_key) {
		live.store_enabled = [this, key = *project_key](bool enabled) {
			return store_live_consent(m_ports.resources, key, enabled);
		};
	}
	live.trusted = [this] {
		const auto trust = m_ports.trust.lock();
		return trust && trust->trusted();
	};
	live.ask_trust = [this](std::function<void()> then) { ask_trust(std::move(then)); };
	live.on_toggled = [this] {
		if (m_ports.tools_changed) m_ports.tools_changed();
	};
	live.open_problems = [this] {
		m_ports.context.panels().set_open(m_ports.problems.get_id(), true);
		ImGui::SetWindowFocus(m_ports.problems.get_title().c_str());
	};
	live.open_settings = [this] {
		m_ports.context.panels().set_open(m_ports.build_settings.get_id(), true);
		m_ports.build_settings.focus();
	};
	m_live = std::make_unique<live_mode>(m_ports.context.problems(), m_ports.context.actions(), std::move(live));
	m_live->watch(m_ports.resources);
	m_ports.build_settings.set_live_unavailable_reason([this] { return m_live->unavailable_reason(); });
	m_ports.build_settings.set_live_switch([this] { return m_live->enabled(); }, [this](bool enabled) {
		const auto apply = [this, enabled] {
			if (auto refused = m_live->set_enabled(enabled, true)) {
				egLOG_WARN("editor/build", "live mode stays off: {}", *refused);
			}
		};
		if (enabled && !m_live->enabled() && !m_live->trusted()) {
			ask_trust(apply);
			return;
		}
		apply();
	});
	m_ports.layer.set_menu_bar_status([this] { m_live->draw_status(); });

	apply_live_consent();
}

void edt::live_mode_component::ask_trust(std::function<void()> on_trusted)
{
	const auto answered = [this, on_trusted = std::move(on_trusted)](trust_answer answer) {
		const auto trust = m_ports.trust.lock();
		if (answer == trust_answer::trusted) {
			// Always: the host retries every module the gate refused, those of a stored "Don't Trust" too.
			load_waiting_modules([this, on_trusted] {
				apply_live_consent();
				if (on_trusted) on_trusted();
			});
			return;
		}
		m_followups.drop();
		if (trust && trust->waiting()) {
			egLOG_WARN("editor/trust", "the project runs without its modules ({}): you did not trust it",
				module_list(trust->waiting_modules()));
		}
		if (m_ports.builder.is_building()) {
			m_ports.builder.cancel();
			egLOG_WARN("editor/trust", "the running build was cancelled: {}", k_untrusted_project_error);
		}
		if (trust && trust->allowed_any()) {
			// A loaded library cannot be taken back safely while its objects live in the editor.
			m_ports.context.dialogs().message(message_level::info, "The project's modules that are already loaded keep "
				"running until you restart the editor; from then on they are not loaded.");
		}
		if (m_live && m_live->enabled()) {
			// Not stored: the person's switch comes back once they trust the project again.
			static_cast<void>(m_live->set_enabled(false, false));
			egLOG_WARN("editor/trust", "live mode is off: {}", k_untrusted_project_error);
		}
	};
	edt::ask_project_trust(m_ports.context.dialogs(), m_ports.trust, answered);
}

void edt::live_mode_component::load_waiting_modules(std::function<void()> then)
{
	// Kept here, not in the callback: register_implicit ignores the id while an earlier load still waits.
	m_followups.queue(std::move(then));
	// Not inside the dialog's answer, nor under a level load, a migration or Play: the game layer changes with it.
	m_ports.layer.register_implicit("edt/project_trust/load", [this, logged = std::make_shared<std::string>()] {
		std::string reason;
		if (auto migrating = m_ports.migration_blocker()) {
			reason = *migrating;
		} else if (auto loading = m_ports.load_in_progress()) {
			reason = *loading;
		} else if (m_ports.play_active()) {
			reason = "Play runs";
		}
		if (!reason.empty()) {
			if (*logged != reason) {
				*logged = reason;
				egLOG_INFO("editor/trust", "the project's modules load once this ends: {}", reason);
			}
			return true;
		}
		if (const auto trust = m_ports.trust.lock()) trust->clear_waiting();
		core::project_module_host* host = m_ports.module_host();
		const core::project_modules_report report = host != nullptr ? host->load_waiting() : core::project_modules_report{};
		if (!report.loaded.empty()) {
			egLOG_INFO("editor/trust", "trusted modules loaded: {}", module_list(report.loaded));
		}
		if (!report.refused.empty() || !report.untrusted.empty() || !report.missing_required.empty()) {
			egLOG_WARN("editor/trust", "modules not loaded after trust: refused {}; untrusted {}; missing {}",
				module_list(report.refused), module_list(report.untrusted), module_list(report.missing_required));
		}
		m_followups.loaded();
		// A frame later: a build or live mode started now would run beside the layer change of this load.
		m_ports.layer.register_implicit("edt/project_trust/after", [this] {
			const auto trust = m_ports.trust.lock();
			m_followups.run_ready(trust && trust->trusted());
			return false;
		});
		return false;
	});
}

void edt::live_mode_component::apply_live_consent()
{
	if (!m_live || m_live->enabled() || m_ports.project == nullptr) return;
	const std::filesystem::path snk_dir = m_ports.project->snk_dir();
	const std::optional<std::string> project_key = canonical_project_settings_key(snk_dir.parent_path());
	if (!project_key) return;
	if (live_mode_at_open(load_build_settings(snk_dir), read_setting(m_ports.resources, k_live_mode_settings), *project_key)) {
		if (auto refused = m_live->set_enabled(true, false)) {
			egLOG_WARN("editor/build", "live mode was on for this project but stays off: {}", *refused);
		}
	}
}

void edt::live_mode_component::tick()
{
	if (m_live) m_live->tick();
}

void edt::live_mode_component::draw_status() const
{
	if (m_live) m_live->draw_status();
}
