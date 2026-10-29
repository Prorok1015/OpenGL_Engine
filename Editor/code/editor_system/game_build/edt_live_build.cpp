#include "edt_live_build.h"
#include <snk/common/logger/engine_log.h>
#include <snk/common/engine_assert.h>
#include <boost/json/object.hpp>
#include <algorithm>
#include <format>

std::string_view edt::to_string(live_state state)
{
	switch (state) {
	case live_state::off: return "off";
	case live_state::idle: return "idle";
	case live_state::pending: return "pending";
	case live_state::building: return "building";
	case live_state::errors: return "errors";
	case live_state::settling: return "settling";
	case live_state::reloading: return "reloading";
	case live_state::reloaded: return "reloaded";
	case live_state::reload_failed: return "reload_failed";
	}
	return "?";
}

edt::live_build::live_build(problem_list& problems, live_build_deps deps)
	: m_problems(problems)
	, m_deps(std::move(deps))
{
	ASSERT_MSG(m_deps.start_build && m_deps.is_building && m_deps.cancel_build && m_deps.reload,
		"live_build needs start_build, is_building, cancel_build and reload");
	if (!m_deps.now) m_deps.now = [] { return std::chrono::steady_clock::now(); };
	if (!m_deps.settings) m_deps.settings = [] { return build_settings{}; };
	if (!m_deps.make_request) {
		m_deps.make_request = [](const build_settings& settings) { return make_live_build_request(settings, game_builder_options{}); };
	}
}

edt::live_build::~live_build() = default;

void edt::live_build::set_enabled(bool enabled)
{
	if (enabled == m_enabled) return;
	m_enabled = enabled;
	if (enabled) {
		m_status = live_status{};
		m_status.state = live_state::idle;
		return;
	}
	if (m_ours && m_deps.is_building()) m_deps.cancel_build();
	m_ours = false;
	m_dirty = false;
	m_force = false;
	m_last_change.reset();
	m_built.reset();
	m_reload_due = false;
	// Outcomes of a build started before are stale now.
	++m_attempt;
	m_inbox->clear();
	m_warned.clear();
	m_problems.clear(k_problem_source);
	m_status = live_status{};
}

void edt::live_build::on_changes(std::span<const source_change> changes)
{
	if (!m_enabled) return;
	if (std::none_of(changes.begin(), changes.end(), [](const source_change& change) { return change.relevant; })) return;
	m_last_change = m_deps.now();
	++m_change_gen;
	m_dirty = true;
	m_built.reset();
	m_reload_due = false;
	if (!m_ours) {
		m_status.state = live_state::pending;
		m_status.message.clear();
	}
}

void edt::live_build::tick()
{
	if (!m_enabled) return;
	take_outcomes();
	if (m_ours && !m_deps.is_building()) {
		// Cancelled from outside (job.cancel): job_service gives no on_done, so no outcome comes.
		m_ours = false;
		m_dirty = false;
		m_force = false;
		m_status.state = live_state::idle;
		m_status.message = "The live build was cancelled";
		m_status.job = 0;
	}
	if (m_dirty && (m_force || !m_last_change || m_deps.now() - *m_last_change >= k_debounce)) {
		try_start();
	}
	// Never while a build runs: it may be writing the module file, and game_reloader refuses then anyway.
	if (m_reload_due && !m_dirty && !m_ours && !m_deps.is_building() && quiet_enough()) {
		reload_built();
	}
}

std::optional<std::string> edt::live_build::rebuild_now()
{
	if (!m_enabled) return "Live mode is off";
	take_outcomes();
	m_dirty = true;
	m_force = true;
	m_built.reset();
	m_reload_due = false;
	return try_start();
}

std::optional<std::string> edt::live_build::reload_now()
{
	if (!m_enabled) return "Live mode is off";
	take_outcomes();
	if (m_ours || m_deps.is_building()) return "A build is running";
	if (!m_built) return "There is no successful live build to reload, or the sources changed after it started";
	return reload_built();
}

void edt::live_build::take_outcomes()
{
	if (m_inbox->empty()) return;
	inbox items;
	items.swap(*m_inbox);
	for (const auto& item : items) {
		// A request restarted by a newer one reports `restarted` under its own attempt.
		if (item.attempt != m_attempt || !m_ours) continue;
		handle(item.outcome);
	}
}

void edt::live_build::handle(const build_outcome& outcome)
{
	m_ours = false;
	m_status.job = outcome.job;
	m_status.last_build_end = m_deps.now();
	switch (outcome.kind) {
	case build_outcome::restarted:
		m_status.state = m_dirty ? live_state::pending : live_state::idle;
		return;
	case build_outcome::failed:
		m_status.errors = outcome.errors;
		m_status.warnings = outcome.warnings;
		m_status.message = outcome.message;
		m_status.state = m_dirty ? live_state::pending : live_state::errors;
		return;
	case build_outcome::succeeded:
		m_status.errors = outcome.errors;
		m_status.warnings = outcome.warnings;
		m_status.message.clear();
		if (m_change_gen != m_build_gen) {
			// Built from sources that changed meanwhile: the next build is pending.
			m_status.state = live_state::pending;
			return;
		}
		m_built = outcome.modules;
		m_reload_due = true;
		m_status.state = live_state::settling;
		return;
	}
}

std::optional<std::string> edt::live_build::try_start()
{
	if (m_deps.is_building() && !m_ours) {
		m_status.state = live_state::pending;
		m_status.message = "Waiting for the running build";
		return std::nullopt;
	}
	const build_settings settings = m_deps.settings();
	build_request request = m_deps.make_request(settings);
	const std::uint64_t attempt = m_attempt + 1;
	request.on_finished = [box = std::weak_ptr<inbox>(m_inbox), attempt](const build_outcome& outcome) {
		if (const auto alive = box.lock()) alive->push_back({ attempt, outcome });
	};
	const std::string configuration = request.configuration;
	const action_result result = m_deps.start_build(std::move(request));
	if (!result.ok) {
		if (result.code == "busy") {
			m_status.state = live_state::pending;
			m_status.message = "Waiting for the running build";
			return std::nullopt;
		}
		// No retry until the next change: the reason (no project, no presets) does not go away by itself.
		m_dirty = false;
		m_force = false;
		m_status.state = live_state::errors;
		m_status.message = result.error;
		warn_once(result.error);
		return result.error;
	}
	m_warned.clear();
	m_attempt = attempt;
	m_dirty = false;
	m_force = false;
	m_ours = true;
	m_build_gen = m_change_gen;
	m_quiet_ms = settings.live_quiet_ms;
	m_built.reset();
	m_reload_due = false;
	m_status.state = live_state::building;
	m_status.configuration = configuration;
	m_status.message.clear();
	m_status.job = 0;
	if (const auto* object = result.value.if_object()) {
		if (const auto* job = object->if_contains("job"); job && job->is_number()) m_status.job = job->to_number<std::uint64_t>();
	}
	return std::nullopt;
}

bool edt::live_build::quiet_enough() const
{
	return !m_last_change || m_deps.now() - *m_last_change >= std::chrono::milliseconds(m_quiet_ms);
}

std::optional<std::string> edt::live_build::reload_built()
{
	ASSERT_MSG(m_built.has_value(), "reload_built without a successful live build");
	m_reload_due = false;
	if (m_built->empty()) {
		m_status.state = live_state::idle;
		m_status.message = "The live build produced no module file";
		return m_status.message;
	}
	m_status.state = live_state::reloading;
	std::vector<problem> failures;
	for (const auto& built : *m_built) {
		const core::reload_report report = m_deps.reload(built.module);
		if (report.status == core::reload_status::reloaded) continue;
		problem item;
		item.severity = report.status == core::reload_status::refused ? problem_severity::warning : problem_severity::error;
		switch (report.status) {
		case core::reload_status::refused:
			item.message = std::format("The game module '{}' was not reloaded: {}. The running version is kept.", built.module, report.message);
			break;
		case core::reload_status::rolled_back:
			item.message = std::format("The game module '{}' was rolled back to the previous build: {}", built.module, report.message);
			break;
		default:
			item.message = std::format("The game module '{}' is not loaded: {}", built.module, report.message);
			break;
		}
		egLOG_WARN("editor/build", "live reload: {}", item.message);
		failures.push_back(std::move(item));
	}
	if (failures.empty()) {
		m_problems.clear(k_problem_source);
		m_status.state = live_state::reloaded;
		m_status.last_reload = m_deps.now();
		m_status.message.clear();
		return std::nullopt;
	}
	m_status.message = failures.front().message;
	m_status.state = live_state::reload_failed;
	m_problems.set(k_problem_source, std::move(failures));
	return m_status.message;
}

void edt::live_build::warn_once(const std::string& message)
{
	if (message == m_warned) return;
	m_warned = message;
	egLOG_WARN("editor/build", "the live build did not start: {}", message);
}
