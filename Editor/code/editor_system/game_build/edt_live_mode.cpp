#include "edt_live_mode.h"
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_code.h>
#include <snk/resource/res_system.h>
#include <imgui.h>
#include <boost/json/parse.hpp>
#include <boost/json/string.hpp>
#include <exception>
#include <format>
#include <utility>
#include <version>

namespace json = boost::json;

namespace {
	constexpr const char* k_status_popup = "##live_status_popup";

	constexpr const char* k_status_schema = R"({
		"type": "object",
		"properties": {
			"enabled": { "type": "boolean" },
			"state": { "type": "string", "enum": ["off", "idle", "pending", "building", "errors", "settling", "reloading",
				"reloaded", "reload_failed"] },
			"errors": { "type": "integer", "minimum": 0 },
			"warnings": { "type": "integer", "minimum": 0 },
			"configuration": { "type": "string" },
			"job": { "type": "integer", "minimum": 0 },
			"last_build_end_seconds_ago": { "type": "number", "minimum": 0 },
			"last_reload_seconds_ago": { "type": "number", "minimum": 0 },
			"message": { "type": "string" }
		},
		"required": ["enabled", "state", "errors", "warnings", "configuration", "job", "message"],
		"additionalProperties": false
	})";

	constexpr const char* k_rebuild_schema = R"({
		"type": "object",
		"properties": { "started": { "type": "boolean" } },
		"required": ["started"],
		"additionalProperties": false
	})";

	constexpr const char* k_reload_schema = R"({
		"type": "object",
		"properties": {
			"status": { "type": "string", "enum": ["requested", "up_to_date"] },
			"message": { "type": "string" }
		},
		"required": ["status", "message"],
		"additionalProperties": false
	})";

	bool is_code(const res::tag& tag)
	{
		return tag.is_valid() && prj::is_code_file(tag.relative());
	}

	ImVec4 color_of(edt::live_state state)
	{
		switch (state) {
		case edt::live_state::building:
		case edt::live_state::pending:
		case edt::live_state::reloading: return { 0.45f, 0.75f, 1.f, 1.f };
		case edt::live_state::settling: return { 0.9f, 0.8f, 0.35f, 1.f };
		case edt::live_state::reloaded: return { 0.45f, 0.85f, 0.45f, 1.f };
		case edt::live_state::errors:
		case edt::live_state::reload_failed: return { 1.f, 0.4f, 0.35f, 1.f };
		case edt::live_state::off:
		case edt::live_state::idle: break;
		}
		return ImGui::GetStyleColorVec4(ImGuiCol_Text);
	}

	std::string local_clock(std::chrono::system_clock::time_point when)
	{
		const auto seconds = std::chrono::floor<std::chrono::seconds>(when);
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
		try {
			const std::chrono::zoned_time local{ std::chrono::current_zone(), seconds };
			const auto of_day = std::chrono::hh_mm_ss(local.get_local_time() - std::chrono::floor<std::chrono::days>(local.get_local_time()));
			return std::format("{:02}:{:02}:{:02}", of_day.hours().count(), of_day.minutes().count(), of_day.seconds().count());
		} catch (const std::exception&) {
			// No time zone database: UTC below.
		}
#endif
		const auto of_day = std::chrono::hh_mm_ss(seconds - std::chrono::floor<std::chrono::days>(seconds));
		return std::format("{:02}:{:02}:{:02}", of_day.hours().count(), of_day.minutes().count(), of_day.seconds().count());
	}

	edt::live_mode_options with_frame_reload(edt::live_mode_options options, const std::shared_ptr<int>& alive)
	{
		if (!options.next_frame || !options.reload_module) return options;
		options.build.reload = [next_frame = options.next_frame, reload = options.reload_module,
			current = options.build.is_loaded_current, alive = std::weak_ptr<int>(alive)](const std::string& module,
			std::function<void(const core::reload_report&)> done) {
			next_frame(std::string(edt::live_mode::k_reload_frame_id) + module,
				[reload, current, alive, module, done = std::move(done)] {
					if (alive.expired()) return;
					// #435 may have taken the same file earlier in this frame: a second swap would only cost a level rebuild.
					if (current && current(module)) {
						done(core::reload_report{ core::reload_status::reloaded, "up to date" });
						return;
					}
					done(reload(module));
				});
		};
		return options;
	}

	double seconds_between(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
	{
		const double seconds = std::chrono::duration<double>(to - from).count();
		return seconds < 0.0 ? 0.0 : seconds;
	}
}

std::vector<edt::source_change> edt::source_changes_of(std::span<const res::file_change> changes)
{
	std::vector<source_change> out;
	out.reserve(changes.size());
	for (const res::file_change& change : changes) {
		const bool relevant = !change.is_directory
			&& (is_code(change.tag) || (change.kind == res::change_kind::renamed && is_code(change.old_tag)));
		out.push_back({ change.tag.string(), relevant });
	}
	return out;
}

std::string edt::live_status_text(const live_status& status, std::string_view reload_clock)
{
	switch (status.state) {
	case live_state::off: return "Live: off";
	case live_state::idle: return "Live: idle";
	case live_state::pending: return "Live: pending";
	case live_state::building: return "Live: building...";
	case live_state::errors:
		return status.errors > 0 ? std::format("Live: errors ({})", status.errors) : std::string("Live: errors");
	case live_state::settling: return "Live: settling";
	case live_state::reloading: return "Live: reloading";
	case live_state::reloaded:
		return reload_clock.empty() ? std::string("Live: reloaded") : std::format("Live: reloaded {}", reload_clock);
	case live_state::reload_failed: return "Live: reload failed";
	}
	return "Live: ?";
}

json::object edt::live_status_json(bool enabled, const live_status& status, std::chrono::steady_clock::time_point now)
{
	json::object out{
		{ "enabled", enabled },
		{ "state", json::string(to_string(status.state)) },
		{ "errors", status.errors < 0 ? 0 : status.errors },
		{ "warnings", status.warnings < 0 ? 0 : status.warnings },
		{ "configuration", json::string(status.configuration) },
		{ "job", status.job },
		{ "message", json::string(status.message) } };
	if (status.last_build_end) out["last_build_end_seconds_ago"] = seconds_between(*status.last_build_end, now);
	if (status.last_reload) out["last_reload_seconds_ago"] = seconds_between(*status.last_reload, now);
	return out;
}

edt::live_mode::live_mode(problem_list& problems, action_registry& actions, live_mode_options options)
	: m_options(with_frame_reload(std::move(options), m_alive))
	, m_now(m_options.build.now ? m_options.build.now
		: std::function<std::chrono::steady_clock::time_point()>([] { return std::chrono::steady_clock::now(); }))
	, m_build(problems, m_options.build)
{
	register_commands(actions);
}

edt::live_mode::~live_mode()
{
	if (m_watched) m_watched->unsubscribe_changes(this);
}

bool edt::live_mode::watch(res::resource_system& resources)
{
	if (m_watched) return true;
	if (resources.mount_roots(prj::k_code_protocol).empty()) return false;
	m_watched = &resources;
	resources.subscribe_changes(prj::k_code_protocol, this, [this](std::span<const res::file_change> changes) { on_changes(changes); });
	return true;
}

std::string edt::live_mode::unavailable_reason() const
{
	return m_options.unavailable_reason ? m_options.unavailable_reason() : std::string{};
}

std::optional<std::string> edt::live_mode::set_enabled(bool enabled, bool store)
{
	if (enabled && !m_build.enabled()) {
		if (std::string reason = unavailable_reason(); !reason.empty()) return reason;
	}
	if (store && m_options.store_enabled) {
		if (auto problem = m_options.store_enabled(enabled)) {
			egLOG_WARN("editor/build", "live mode is {} for this session only: {}", enabled ? "on" : "off", *problem);
		}
	}
	if (enabled == m_build.enabled()) return std::nullopt;
	m_build.set_enabled(enabled);
	egLOG_INFO("editor/build", "live mode is {}", enabled ? "on" : "off");
	if (m_options.on_toggled) m_options.on_toggled();
	return std::nullopt;
}

void edt::live_mode::follow_settings(const build_settings& settings)
{
	if (settings.live_enabled == m_build.enabled()) return;
	if (auto refused = set_enabled(settings.live_enabled, false)) {
		egLOG_WARN("editor/build", "live mode stays off: {}", *refused);
	}
}

void edt::live_mode::on_changes(std::span<const res::file_change> changes)
{
	if (!m_build.enabled()) return;
	const std::vector<source_change> mapped = source_changes_of(changes);
	m_build.on_changes(mapped);
}

void edt::live_mode::tick()
{
	m_build.tick();
}

const std::string& edt::live_mode::reload_clock()
{
	const auto& last = m_build.status().last_reload;
	if (!last) {
		m_clock_of.reset();
		m_clock_text.clear();
	} else if (m_clock_of != last) {
		m_clock_of = last;
		const auto ago = m_now() - *last;
		m_clock_text = local_clock(std::chrono::system_clock::now()
			- std::chrono::duration_cast<std::chrono::system_clock::duration>(ago));
	}
	return m_clock_text;
}

void edt::live_mode::draw_status()
{
	// Off: no widget, no spacing, no popup: the menu bar is exactly what it is without live mode.
	if (!m_build.enabled()) return;
	const live_status& status = m_build.status();
	const std::string label = live_status_text(status, reload_clock()) + "###live_status";
	const ImGuiStyle& style = ImGui::GetStyle();
	const float width = ImGui::CalcTextSize(label.c_str(), nullptr, true).x;
	const float right = ImGui::GetWindowWidth() - width - style.ItemSpacing.x * 2.f;
	if (right > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(right);

	ImGui::PushStyleColor(ImGuiCol_Text, color_of(status.state));
	const bool clicked = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(width, 0.f));
	ImGui::PopStyleColor();
	if (clicked) ImGui::OpenPopup(k_status_popup);
	if (!status.message.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", status.message.c_str());

	if (ImGui::BeginPopup(k_status_popup)) {
		if (status.errors > 0 || status.warnings > 0) {
			ImGui::TextDisabled("Last build: %d errors, %d warnings", status.errors, status.warnings);
		}
		if (!status.message.empty()) {
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.f);
			ImGui::TextUnformatted(status.message.c_str());
			ImGui::PopTextWrapPos();
		}
		if (status.errors > 0 || status.warnings > 0 || !status.message.empty()) ImGui::Separator();
		if (ImGui::MenuItem("Turn off")) set_enabled(false, true);
		if (m_options.open_problems && ImGui::MenuItem("Open Problems")) m_options.open_problems();
		if (m_options.open_settings && ImGui::MenuItem("Build Settings...")) m_options.open_settings();
		ImGui::EndPopup();
	}
}

void edt::live_mode::register_commands(action_registry& actions)
{
	action_desc toggle;
	toggle.id = k_toggle_id;
	toggle.label = "Live Mode";
	toggle.menu_path = "Build";
	toggle.disabled_reason = [this] { return unavailable_reason(); };
	toggle.checked = [this] { return m_build.enabled(); };
	// Turning it off always works, even when the reason to be unavailable came up while it was on.
	toggle.enabled = [this] { return m_build.enabled() || unavailable_reason().empty(); };
	toggle.execute = [this](const json::value&) {
		if (auto refused = set_enabled(!m_build.enabled(), true)) return action_result::failure(cmd::codes::disabled, *refused);
		return action_result::success();
	};
	toggle.expose_as_tool = false;
	if (auto added = actions.add(std::move(toggle)); added.active()) m_registrations.push_back(std::move(added));

	const auto live_on = [this] { return m_build.enabled(); };

	cmd::command_desc status;
	status.id = "live.status";
	status.summary = "Live mode's state (shown while the person has live mode on): idle, pending, building, errors, settling, "
		"reloading, reloaded or reload_failed, the last live build's error and warning counts, its configuration and job, "
		"seconds since the last build ended and since the last reload, and the last message";
	status.args_schema = cmd::no_args_schema();
	status.result_schema = json::parse(k_status_schema);
	status.mutates = false;
	status.agent_when_enabled = true;
	status.enabled = live_on;
	status.invoke = [this](const cmd::call&) {
		return cmd::result::success(live_status_json(m_build.enabled(), m_build.status(), m_now()));
	};
	if (auto added = actions.add(std::move(status), std::nullopt); added.active()) m_registrations.push_back(std::move(added));

	cmd::command_desc rebuild;
	rebuild.id = "live.rebuild_now";
	rebuild.summary = "Live mode: build the game module now, without waiting for the debounce (it waits behind a build the "
		"person started). Compile errors go to problems.list; follow the result with live.status";
	rebuild.args_schema = cmd::no_args_schema();
	rebuild.result_schema = json::parse(k_rebuild_schema);
	rebuild.mutates = true;
	rebuild.agent_when_enabled = true;
	rebuild.enabled = live_on;
	rebuild.invoke = [this](const cmd::call&) {
		if (auto problem = m_build.rebuild_now()) return cmd::result::failure(cmd::codes::failed, *problem);
		return cmd::result::success(json::object{ { "started", m_build.status().state == live_state::building } });
	};
	if (auto added = actions.add(std::move(rebuild), std::nullopt); added.active()) m_registrations.push_back(std::move(added));

	cmd::command_desc reload;
	reload.id = "live.reload_now";
	reload.summary = "Live mode: reload now, without waiting for the quiet period, each game module whose build differs from "
		"the running copy. status requested: the reload runs on the next editor frame, its result shows in live.status "
		"(reloaded or reload_failed); up_to_date: nothing to reload. Fails while a build runs or reloads still wait";
	reload.args_schema = cmd::no_args_schema();
	reload.result_schema = json::parse(k_reload_schema);
	reload.mutates = true;
	reload.agent_when_enabled = true;
	reload.enabled = live_on;
	reload.invoke = [this](const cmd::call&) {
		const reload_now_result outcome = m_build.reload_now();
		switch (outcome.kind) {
		case reload_now_kind::requested:
			return cmd::result::success(json::object{ { "status", "requested" }, { "message", json::string(outcome.message.empty()
				? std::string_view("the reload runs on the next editor frame; see live.status") : std::string_view(outcome.message)) } });
		case reload_now_kind::up_to_date:
			return cmd::result::success(json::object{ { "status", "up_to_date" }, { "message", json::string(outcome.message) } });
		case reload_now_kind::refused:
			break;
		}
		return cmd::result::failure(cmd::codes::failed, outcome.message);
	};
	if (auto added = actions.add(std::move(reload), std::nullopt); added.active()) m_registrations.push_back(std::move(added));
}
