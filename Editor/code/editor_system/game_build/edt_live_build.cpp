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
	ASSERT_MSG(m_deps.start_build && m_deps.is_building && m_deps.stop_background && m_deps.reload,
		"live_build needs start_build, is_building, stop_background and reload");
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
	if (m_ours && m_deps.is_building()) m_deps.stop_background();
	m_ours = false;
	m_dirty = false;
	m_force = false;
	m_last_change.reset();
	m_built.reset();
	m_reload_due = false;
	m_last_modules.clear();
	// Outcomes of a build started before are stale now; results of reloads requested before are dropped.
	++m_attempt;
	m_inbox->clear();
	m_rounds.clear();
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
	mark_rounds_stale();
	if (!m_ours) {
		m_status.state = live_state::pending;
		m_status.message.clear();
	}
}

void edt::live_build::tick()
{
	if (!m_enabled) return;
	const bool ours_ended = take_outcomes();
	const bool building = m_deps.is_building();
	const bool build_ended = ours_ended || (m_was_building && !building);
	m_was_building = building;
	if (m_ours && !building) {
		// Cancelled from outside (job.cancel): job_service gives no on_done, so no outcome comes.
		m_ours = false;
		m_dirty = false;
		m_force = false;
		m_status.state = live_state::idle;
		m_status.message = "The live build was cancelled";
		m_status.job = 0;
	}
	if (m_dirty && (m_force || !m_last_change || m_deps.now() - *m_last_change >= k_debounce) && may_start(build_ended)) {
		try_start();
	}
	if (m_reload_due) refresh_quiet_ms();
	// Never while a build runs: it may be writing the module file, and game_reloader refuses then anyway.
	if (m_reload_due && !m_dirty && !m_ours && !building && quiet_enough()) {
		reload_built();
	}
}

bool edt::live_build::may_start(bool build_ended)
{
	// A build that ended in this frame may have queued its reload (#435) for the UI stage: let that run first.
	if (build_ended) return false;
	if (m_deps.reload_pending && m_deps.reload_pending()) {
		m_status.state = live_state::pending;
		m_status.message = "waiting for the module reload";
		return false;
	}
	return true;
}

void edt::live_build::refresh_quiet_ms()
{
	const auto now = m_deps.now();
	if (m_quiet_read && now - *m_quiet_read < k_quiet_refresh) return;
	m_quiet_read = now;
	m_quiet_ms = m_deps.settings().live_quiet_ms;
}

void edt::live_build::on_manual_build_succeeded()
{
	if (!m_enabled || m_ours) return;
	m_status.errors = 0;
	m_status.warnings = 0;
	if (m_status.state == live_state::errors) {
		m_status.state = live_state::idle;
		m_status.message = "manual build succeeded";
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
	// Queued: tick() starts it once the reload ran.
	if (!may_start(false)) return std::nullopt;
	return try_start();
}

edt::reload_now_result edt::live_build::reload_now()
{
	const auto refused = [](std::string why) { return reload_now_result{ reload_now_kind::refused, std::move(why) }; };
	if (!m_enabled) return refused("Live mode is off");
	take_outcomes();
	if (m_ours || m_deps.is_building()) return refused("A build is running");
	if (!m_rounds.empty()) return refused("A reload is still waiting for its result");
	std::vector<std::string> modules = m_last_modules;
	if (modules.empty() && m_deps.modules) modules = m_deps.modules();
	if (modules.empty()) return refused("There is no game module to reload");
	// Taken over by this request; the quiet period no longer matters.
	m_reload_due = false;
	if (!request_reloads(modules)) {
		if (m_status.state == live_state::settling) {
			m_status.state = live_state::reloaded;
			m_status.message = "up to date";
		}
		return { reload_now_kind::up_to_date, "up to date" };
	}
	return { reload_now_kind::requested, {} };
}

bool edt::live_build::take_outcomes()
{
	if (m_inbox->empty()) return false;
	inbox items;
	items.swap(*m_inbox);
	bool handled = false;
	for (const auto& item : items) {
		// A request restarted by a newer one reports `restarted` under its own attempt.
		if (item.attempt != m_attempt || !m_ours) continue;
		handle(item.outcome);
		handled = true;
	}
	return handled;
}

void edt::live_build::handle(const build_outcome& outcome)
{
	m_ours = false;
	if (outcome.kind == build_outcome::preempted) {
		// The person's build goes first; these sources are still unbuilt, so live mode builds again after it.
		m_dirty = true;
		m_status.state = live_state::pending;
		m_status.message = std::string(k_waiting_message);
		return;
	}
	if (outcome.not_started) {
		// It waited for a build to stop and was then refused (not a cancel): the changes stay to be built, and the
		// next tick's start reports the reason again or gets through.
		m_dirty = true;
		m_status.state = live_state::errors;
		m_status.message = outcome.message;
		warn_once(outcome.message);
		return;
	}
	m_status.job = outcome.job;
	m_status.last_build_end = m_deps.now();
	switch (outcome.kind) {
	case build_outcome::preempted:
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
		m_last_modules.clear();
		for (const auto& built : outcome.modules) m_last_modules.push_back(built.module);
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
		m_status.message = std::string(k_waiting_message);
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
			m_status.message = std::string(k_waiting_message);
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
	m_quiet_read = m_deps.now();
	m_built.reset();
	m_reload_due = false;
	mark_rounds_stale();
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

void edt::live_build::reload_built()
{
	ASSERT_MSG(m_built.has_value(), "reload_built without a successful live build");
	m_reload_due = false;
	if (m_built->empty()) {
		m_status.state = live_state::idle;
		m_status.message = "The live build produced no module file";
		return;
	}
	std::vector<std::string> modules;
	for (const auto& built : *m_built) modules.push_back(built.module);
	if (!request_reloads(modules)) {
		// A build that changed nothing (ninja had no work): the running copies are that build already.
		m_status.state = live_state::reloaded;
		m_status.message = "up to date";
	}
}

bool edt::live_build::request_reloads(const std::vector<std::string>& modules)
{
	std::vector<std::string> wanted;
	for (const auto& module : modules) {
		if (!m_deps.is_loaded_current || !m_deps.is_loaded_current(module)) wanted.push_back(module);
	}
	if (wanted.empty()) return false;
	mark_rounds_stale();
	reload_round round;
	round.id = ++m_round_id;
	round.waiting = wanted.size();
	m_rounds.push_back(std::move(round));
	m_status.state = live_state::reloading;
	m_status.message.clear();
	const std::uint64_t id = m_round_id;
	for (const auto& module : wanted) {
		m_deps.reload(module, [this, alive = std::weak_ptr<int>(m_alive), id, module](const core::reload_report& report) {
			if (!alive.expired()) on_reload_result(id, module, report);
		});
	}
	return true;
}

void edt::live_build::on_reload_result(std::uint64_t round, const std::string& module, const core::reload_report& report)
{
	const auto it = std::find_if(m_rounds.begin(), m_rounds.end(), [round](const reload_round& item) { return item.id == round; });
	// A round dropped by set_enabled(false).
	if (it == m_rounds.end()) return;
	if (report.status != core::reload_status::reloaded) {
		problem item;
		item.severity = report.status == core::reload_status::refused ? problem_severity::warning : problem_severity::error;
		switch (report.status) {
		case core::reload_status::refused:
			item.message = std::format("The game module '{}' was not reloaded: {}. The running version is kept.", module, report.message);
			break;
		case core::reload_status::rolled_back:
			item.message = std::format("The game module '{}' was rolled back to the previous build: {}", module, report.message);
			break;
		default:
			item.message = std::format("The game module '{}' is not loaded: {}", module, report.message);
			break;
		}
		egLOG_WARN("editor/build", "live reload: {}", item.message);
		it->failures.push_back(std::move(item));
	}
	if (--it->waiting > 0) return;

	reload_round done = std::move(*it);
	m_rounds.erase(it);
	const bool failed = !done.failures.empty();
	const std::string first = failed ? done.failures.front().message : std::string{};
	if (done.id > m_finished_round) {
		m_finished_round = done.id;
		if (failed) {
			m_problems.set(k_problem_source, std::move(done.failures));
		} else {
			m_problems.clear(k_problem_source);
		}
	}
	if (!failed) m_status.last_reload = m_deps.now();
	if (done.stale || m_status.state != live_state::reloading) return;
	m_status.state = failed ? live_state::reload_failed : live_state::reloaded;
	m_status.message = first;
}

void edt::live_build::mark_rounds_stale()
{
	for (auto& round : m_rounds) round.stale = true;
}

void edt::live_build::warn_once(const std::string& message)
{
	if (message == m_warned) return;
	m_warned = message;
	egLOG_WARN("editor/build", "the live build did not start: {}", message);
}
