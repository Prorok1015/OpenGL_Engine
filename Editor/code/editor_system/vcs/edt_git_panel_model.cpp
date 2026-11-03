#include "edt_git_panel_model.h"
#include <snk/common/logger/engine_log.h>
#include <boost/json.hpp>
#include <algorithm>
#include <format>
#include <utility>

namespace {
	namespace json = boost::json;

	std::string string_of(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_string() ? std::string(value->as_string()) : std::string{};
	}

	std::optional<std::int64_t> number_of(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		if (!value || !value->is_number()) return std::nullopt;
		return value->to_number<std::int64_t>();
	}

	char status_char(const json::object& object, std::string_view key)
	{
		const std::string text = string_of(object, key);
		return text.empty() ? '.' : text.front();
	}

	bool starts_with(std::string_view text, std::string_view prefix)
	{
		return text.substr(0, prefix.size()) == prefix;
	}
}

std::vector<edt::git_diff_line> edt::diff_lines(std::string_view text)
{
	using kind = git_diff_line::kind;
	std::vector<git_diff_line> out;
	bool in_header = false;
	std::size_t begin = 0;
	while (begin < text.size()) {
		std::size_t end = text.find('\n', begin);
		if (end == std::string_view::npos) end = text.size();
		std::string_view line = text.substr(begin, end - begin);
		begin = end + 1;
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

		kind type = kind::context;
		if (starts_with(line, "diff --git ")) {
			in_header = true;
			type = kind::meta;
		} else if (starts_with(line, "@@")) {
			in_header = false;
			type = kind::hunk;
		} else if (in_header) {
			type = kind::meta;
		} else if (starts_with(line, "+")) {
			type = kind::added;
		} else if (starts_with(line, "-")) {
			type = kind::removed;
		} else if (starts_with(line, "\\")) {
			type = kind::note;
		}
		out.push_back({ type, std::string(line) });
	}
	return out;
}

edt::git_panel_model::git_panel_model(editor_events& events, ports ports)
	: m_events(events), m_ports(std::move(ports))
{
	m_finished = m_events.job_finished.subscribe(
		[this](std::uint64_t id, job_status status, const std::string& error, const std::optional<json::value>& result) {
			finished(id, status, error, result);
		});
}

void edt::git_panel_model::set_visible(bool visible)
{
	if (visible == m_visible) return;
	m_visible = visible;
	if (visible && m_stale) refresh();
}

void edt::git_panel_model::tick()
{
	if (m_refresh_at && m_visible && m_ports.now() >= *m_refresh_at) {
		m_refresh_at.reset();
		refresh();
	}
}

void edt::git_panel_model::files_changed()
{
	m_stale = true;
	if (m_visible) m_refresh_at = m_ports.now() + k_change_delay;
}

void edt::git_panel_model::refresh()
{
	m_stale = false;
	m_read = true;
	m_refresh_at.reset();
	const cmd::result info = m_ports.invoke("vcs.info", json::object{});
	const json::value* available = info.ok && info.value.is_object() ? info.value.as_object().if_contains("available") : nullptr;
	m_available = available && available->is_bool() && available->as_bool();
	if (!m_available) {
		m_reason = info.ok && info.value.is_object() ? string_of(info.value.as_object(), "reason") : info.error;
		if (m_reason.empty()) m_reason = "git is not available";
		forget();
		return;
	}
	m_reason.clear();
	// A newer read replaces what an older one would bring; the diff follows the status (reselect).
	m_status_job = start("vcs.status", json::object{}, job_kind::status, "Status");
	m_branches_job = start("vcs.branches", json::object{}, job_kind::branches, "Branches");
}

void edt::git_panel_model::forget()
{
	m_status_job.reset();
	m_branches_job.reset();
	m_diff_job.reset();
	m_head.clear();
	m_detached = false;
	m_upstream.clear();
	m_ahead.reset();
	m_behind.reset();
	m_staged.clear();
	m_unstaged.clear();
	m_untracked.clear();
	m_branches.clear();
	m_selected.clear();
	m_diff.clear();
	m_diff_note.clear();
	m_diff_truncated = false;
}

void edt::git_panel_model::reselect()
{
	if (m_selected.empty()) return;
	const auto in = [this](const std::vector<git_change>& changes) {
		return std::any_of(changes.begin(), changes.end(), [this](const git_change& c) { return c.path == m_selected; });
	};
	const bool staged = in(m_staged);
	const bool unstaged = in(m_unstaged);
	const std::string path = m_selected;
	if (in(m_untracked) || (staged && unstaged)) {
		select(path, staged && unstaged ? m_selected_staged : false);
	} else if (staged || unstaged) {
		select(path, staged);
	} else {
		m_selected.clear();
		m_diff.clear();
		m_diff_note.clear();
		m_diff_truncated = false;
		m_diff_job.reset();
	}
}

bool edt::git_panel_model::can_commit() const
{
	return m_busy.empty() && !m_staged.empty() && m_message.find_first_not_of(" \t\r\n") != std::string::npos;
}

void edt::git_panel_model::select(const std::string& path, bool staged)
{
	m_selected = path;
	m_selected_staged = staged;
	m_diff.clear();
	m_diff_truncated = false;
	m_diff_note.clear();
	m_diff_job.reset();
	const bool untracked = std::any_of(m_untracked.begin(), m_untracked.end(), [&](const git_change& c) { return c.path == path; });
	if (untracked) {
		m_diff_note = "New file (not tracked yet): no diff.";
		return;
	}
	m_diff_job = start("vcs.diff", json::object{ { "path", path }, { "staged", staged } }, job_kind::diff, "Diff");
}

void edt::git_panel_model::set_staged(const std::string& path, bool staged)
{
	start_write(staged ? "vcs.stage" : "vcs.unstage", json::object{ { "paths", json::array{ json::string(path) } } },
		staged ? "Stage" : "Unstage");
}

void edt::git_panel_model::commit()
{
	if (!can_commit()) return;
	start_write("vcs.commit", json::object{ { "message", m_message } }, "Commit");
}

void edt::git_panel_model::fetch()
{
	start_write("vcs.fetch", json::object{}, "Fetch");
}

void edt::git_panel_model::pull()
{
	start_write("vcs.pull", json::object{}, "Pull");
}

void edt::git_panel_model::push()
{
	if (m_detached || m_head.empty()) {
		fail("Push: not on a branch");
		return;
	}
	const std::size_t slash = m_upstream.find('/');
	json::object args{ { "branch", m_head } };
	if (slash != std::string::npos && slash > 0) {
		args["remote"] = m_upstream.substr(0, slash);
	} else {
		args["remote"] = "origin";
		args["set_upstream"] = true;
	}
	start_write("vcs.push", std::move(args), "Push");
}

void edt::git_panel_model::checkout(const git_branch& branch)
{
	std::string ref = branch.name;
	if (branch.remote) {
		const std::size_t slash = ref.find('/');
		if (slash != std::string::npos) ref = ref.substr(slash + 1);
	}
	start_write("vcs.checkout", json::object{ { "ref", ref } }, "Checkout");
}

void edt::git_panel_model::create_branch(const std::string& name)
{
	start_write("vcs.branch.create", json::object{ { "name", name } }, "New branch");
}

void edt::git_panel_model::cancel()
{
	if (m_busy_job && m_ports.cancel) m_ports.cancel(*m_busy_job);
}

std::optional<std::uint64_t> edt::git_panel_model::start(std::string_view id, json::value args, job_kind kind, std::string label)
{
	const cmd::result started = m_ports.invoke(id, std::move(args));
	if (!started.ok) {
		fail(std::format("{}: {}", label, started.error));
		return std::nullopt;
	}
	const json::value* job = started.value.is_object() ? started.value.as_object().if_contains("job") : nullptr;
	if (!job || !job->is_number()) {
		fail(std::format("{}: no job was started", label));
		return std::nullopt;
	}
	const auto job_id = job->to_number<std::uint64_t>();
	m_jobs[job_id] = pending{ kind, std::move(label) };
	return job_id;
}

void edt::git_panel_model::start_write(std::string_view id, json::value args, std::string label)
{
	if (!m_busy.empty()) {
		fail(std::format("{}: wait until {} has finished", label, m_busy));
		return;
	}
	const std::string busy = label;
	if (const auto job = start(id, std::move(args), job_kind::write, std::move(label))) {
		m_busy = busy;
		m_busy_job = job;
	}
}

void edt::git_panel_model::finished(std::uint64_t id, job_status status, const std::string& error,
	const std::optional<json::value>& result)
{
	const auto it = m_jobs.find(id);
	if (it == m_jobs.end()) return;
	const pending job = std::move(it->second);
	m_jobs.erase(it);
	const bool current = (job.kind == job_kind::status && m_status_job == id) || (job.kind == job_kind::branches && m_branches_job == id)
		|| (job.kind == job_kind::diff && m_diff_job == id) || job.kind == job_kind::write;
	if (job.kind == job_kind::status && m_status_job == id) m_status_job.reset();
	if (job.kind == job_kind::branches && m_branches_job == id) m_branches_job.reset();
	if (job.kind == job_kind::diff && m_diff_job == id) m_diff_job.reset();
	if (job.kind == job_kind::write) {
		m_busy.clear();
		m_busy_job.reset();
	}

	bool succeeded = false;
	if (status == job_status::failed) {
		fail(std::format("{}: {}", job.label, error));
	} else if (status == job_status::done && result && result->is_object()) {
		const json::object& value = result->as_object();
		if (const json::value* git_error = value.if_contains("error"); git_error && git_error->is_object()) {
			const std::string message = string_of(git_error->as_object(), "message");
			fail(std::format("{}: {}", job.label, message.empty() ? string_of(git_error->as_object(), "code") : message));
		} else {
			succeeded = true;
			if (current && job.kind == job_kind::status) apply_status(value);
			if (current && job.kind == job_kind::branches) apply_branches(value);
			if (current && job.kind == job_kind::diff) apply_diff(value);
		}
	}
	if (job.kind != job_kind::write) return;
	++m_writes_finished;
	if (succeeded && job.label == "Commit") m_message.clear();
	// A cancel is also what the editor's shutdown sends (job_service::cancel_all_and_wait): nothing is asked then.
	if (status == job_status::cancelled) {
		m_stale = true;
		return;
	}
	// What a write changed shows at once, whatever it ended with (a pull that stopped half way changed things too);
	// hidden, it shows when the panel is opened again.
	if (m_visible) {
		refresh();
	} else {
		m_stale = true;
	}
}

void edt::git_panel_model::apply_status(const json::object& result)
{
	m_staged.clear();
	m_unstaged.clear();
	m_untracked.clear();
	if (const json::value* branch = result.if_contains("branch"); branch && branch->is_object()) {
		const json::object& b = branch->as_object();
		m_head = string_of(b, "head");
		m_detached = b.contains("detached") && b.at("detached").is_bool() && b.at("detached").as_bool();
		m_upstream = string_of(b, "upstream");
		m_ahead = number_of(b, "ahead");
		m_behind = number_of(b, "behind");
	}
	const json::value* entries = result.if_contains("entries");
	if (!entries || !entries->is_array()) {
		reselect();
		return;
	}
	for (const json::value& item : entries->as_array()) {
		if (!item.is_object()) continue;
		const json::object& entry = item.as_object();
		git_change change{ string_of(entry, "path"), string_of(entry, "orig_path"), status_char(entry, "index"), status_char(entry, "worktree") };
		if (change.index == '?' || change.worktree == '?') {
			m_untracked.push_back(std::move(change));
			continue;
		}
		if (change.index != '.' && change.index != '!') m_staged.push_back(change);
		if (change.worktree != '.' && change.worktree != '!') m_unstaged.push_back(std::move(change));
	}
	reselect();
}

void edt::git_panel_model::apply_branches(const json::object& result)
{
	m_branches.clear();
	const json::value* list = result.if_contains("branches");
	if (!list || !list->is_array()) return;
	for (const json::value& item : list->as_array()) {
		if (!item.is_object()) continue;
		const json::object& ref = item.as_object();
		git_branch branch{ string_of(ref, "name") };
		// origin/HEAD only names the remote's default branch: nothing to switch to.
		if (branch.name.ends_with("/HEAD")) continue;
		branch.remote = ref.contains("remote") && ref.at("remote").is_bool() && ref.at("remote").as_bool();
		branch.head = ref.contains("head") && ref.at("head").is_bool() && ref.at("head").as_bool();
		m_branches.push_back(std::move(branch));
	}
}

void edt::git_panel_model::apply_diff(const json::object& result)
{
	m_diff = diff_lines(string_of(result, "text"));
	m_diff_truncated = result.contains("truncated") && result.at("truncated").is_bool() && result.at("truncated").as_bool();
}

void edt::git_panel_model::fail(std::string text)
{
	egLOG_WARN("vcs", "{}", text);
	m_error = std::move(text);
}
