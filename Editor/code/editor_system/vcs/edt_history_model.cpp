#include "edt_history_model.h"
#include <snk/common/logger/engine_log.h>
#include <boost/json.hpp>
#include <format>
#include <utility>

namespace {
	namespace json = boost::json;

	std::string string_of(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_string() ? std::string(value->as_string()) : std::string{};
	}

	bool bool_of(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_bool() && value->as_bool();
	}

	std::optional<edt::vcs::history_ref::kind> ref_kind_of(std::string_view text)
	{
		using kind = edt::vcs::history_ref::kind;
		if (text == "head") return kind::head;
		if (text == "branch") return kind::branch;
		if (text == "remote") return kind::remote;
		if (text == "tag") return kind::tag;
		return std::nullopt;
	}
}

std::optional<edt::vcs::history_commit> edt::vcs::history_commit_of(const json::object& value)
{
	history_commit commit;
	commit.hash = string_of(value, "hash");
	if (commit.hash.empty()) return std::nullopt;
	if (const json::value* parents = value.if_contains("parents"); parents && parents->is_array()) {
		for (const json::value& parent : parents->as_array()) {
			if (parent.is_string()) commit.parents.emplace_back(parent.as_string());
		}
	}
	commit.author = string_of(value, "author");
	commit.email = string_of(value, "email");
	commit.subject = string_of(value, "subject");
	if (const json::value* time = value.if_contains("time"); time && time->is_number()) {
		commit.time = time->to_number<std::int64_t>();
	}
	if (const json::value* refs = value.if_contains("refs"); refs && refs->is_array()) {
		for (const json::value& item : refs->as_array()) {
			if (!item.is_object()) continue;
			const auto type = ref_kind_of(string_of(item.as_object(), "kind"));
			std::string name = string_of(item.as_object(), "name");
			if (!type || name.empty()) continue;
			commit.refs.push_back({ std::move(name), *type });
		}
	}
	return commit;
}

std::vector<edt::vcs::history_file> edt::vcs::history_files_of(const json::object& result)
{
	std::vector<history_file> files;
	const json::value* list = result.if_contains("files");
	if (!list || !list->is_array()) return files;
	for (const json::value& item : list->as_array()) {
		if (!item.is_object()) continue;
		const json::object& entry = item.as_object();
		history_file file{ string_of(entry, "path"), string_of(entry, "orig_path") };
		if (file.path.empty()) continue;
		if (const std::string status = string_of(entry, "status"); !status.empty()) file.status = status.front();
		files.push_back(std::move(file));
	}
	return files;
}

edt::vcs::history_model::history_model(editor_events& events, ports ports)
	: m_events(events), m_ports(std::move(ports))
{
	m_finished = m_events.job_finished.subscribe(
		[this](std::uint64_t id, job_status status, const std::string& error, const std::optional<json::value>& result) {
			finished(id, status, error, result);
		});
}

edt::vcs::history_model::~history_model()
{
	if (!m_ports.cancel) return;
	for (const auto& entry : m_jobs) m_ports.cancel(entry.first);
}

void edt::vcs::history_model::set_visible(bool visible)
{
	const bool shown_again = visible && !m_visible;
	m_visible = visible;
	// What it read before it was hidden may be old by now.
	if (shown_again || (m_visible && !m_loaded_once)) reload();
}

void edt::vcs::history_model::tick()
{
	if (!m_reload_at || m_ports.now() < *m_reload_at) return;
	reload();
}

void edt::vcs::history_model::reload()
{
	m_reload_at.reset();
	m_restarts = 0;
	m_error.clear();
	if (m_applied_filter != m_filter) clear_selection();
	m_applied_filter = m_filter;
	restart();
	// Hidden, it loads when it is shown again.
	m_loaded_once = m_visible;
	if (m_visible) start_page();
}

void edt::vcs::history_model::set_filter(history_filter filter)
{
	if (filter == m_filter) return;
	m_filter = std::move(filter);
	m_reload_at = m_ports.now() + k_filter_delay;
}

void edt::vcs::history_model::show_rows(std::size_t, std::size_t last)
{
	m_last_shown = last;
	if (!m_visible || m_log_job || m_at_end || m_log_failed || !m_loaded_once) return;
	if (last + k_prefetch_rows < m_commits.size()) return;
	start_page();
}

void edt::vcs::history_model::select(std::size_t row)
{
	if (row >= m_commits.size() || m_commits[row].hash == m_selected) return;
	clear_selection();
	m_error.clear();
	m_selected = m_commits[row].hash;
	if (m_ports.extended_reads) m_files_job = start("vcs.show_files", json::object{ { "commit", m_selected } }, job_kind::files);
}

void edt::vcs::history_model::clear_selection()
{
	forget_job(m_files_job);
	forget_job(m_diff_job);
	m_selected.clear();
	m_files.clear();
	m_selected_file.clear();
	m_diff_text.clear();
	m_diff_truncated = false;
	++m_diff_version;
}

void edt::vcs::history_model::select_file(const std::string& path)
{
	if (m_selected.empty() || !m_ports.extended_reads || path == m_selected_file) return;
	forget_job(m_diff_job);
	m_selected_file = path;
	m_diff_text.clear();
	m_diff_truncated = false;
	++m_diff_version;
	m_diff_job = start("vcs.diff", json::object{ { "commit", m_selected }, { "path", path } }, job_kind::diff);
}

std::optional<std::size_t> edt::vcs::history_model::selected_row() const
{
	const auto it = m_row_of.find(m_selected);
	if (it == m_row_of.end()) return std::nullopt;
	return it->second;
}

std::optional<std::uint64_t> edt::vcs::history_model::start(std::string_view id, json::value args, job_kind kind)
{
	const cmd::result started = m_ports.invoke(id, std::move(args));
	if (!started.ok) {
		fail(std::format("{}: {}", id, started.error));
		return std::nullopt;
	}
	const json::value* job = started.value.is_object() ? started.value.as_object().if_contains("job") : nullptr;
	if (!job || !job->is_number()) {
		fail(std::format("{}: no job was started", id));
		return std::nullopt;
	}
	const auto job_id = job->to_number<std::uint64_t>();
	m_jobs[job_id] = kind;
	return job_id;
}

void edt::vcs::history_model::start_page()
{
	json::object args{ { "limit", k_page_size } };
	if (!m_applied_filter.branch.empty()) {
		args["ref"] = m_applied_filter.branch;
	} else if (m_ports.extended_reads) {
		args["all"] = true;
	}
	if (m_ports.extended_reads && !m_applied_filter.author.empty()) args["author"] = m_applied_filter.author;
	if (m_ports.extended_reads && !m_applied_filter.text.empty()) args["grep"] = m_applied_filter.text;
	if (!m_cursor.empty()) args["cursor"] = m_cursor;
	m_log_job = start("vcs.log", std::move(args), job_kind::log);
	if (!m_log_job) m_log_failed = true;
}

void edt::vcs::history_model::restart()
{
	forget_job(m_log_job);
	m_commits.clear();
	m_row_of.clear();
	m_graph = commit_graph{};
	m_cursor.clear();
	m_at_end = false;
	m_log_failed = false;
}

void edt::vcs::history_model::finished(std::uint64_t id, job_status status, const std::string& error,
	const std::optional<json::value>& result)
{
	const auto it = m_jobs.find(id);
	if (it == m_jobs.end()) return;
	const job_kind kind = it->second;
	m_jobs.erase(it);
	std::optional<std::uint64_t>& current = kind == job_kind::log ? m_log_job : kind == job_kind::files ? m_files_job : m_diff_job;
	if (current != id) return;
	current.reset();
	const char* label = kind == job_kind::log ? "History" : kind == job_kind::files ? "Commit files" : "Commit diff";

	// A cancel is also what the editor's shutdown sends (job_service::cancel_all_and_wait).
	if (status == job_status::cancelled) return;
	if (status == job_status::failed || !result || !result->is_object()) {
		fail(std::format("{}: {}", label, error.empty() ? "no result" : error));
		if (kind == job_kind::log) m_log_failed = true;
		return;
	}
	const json::object& value = result->as_object();
	if (const json::value* git_error = value.if_contains("error"); git_error && git_error->is_object()) {
		const json::object& details = git_error->as_object();
		const std::string code = string_of(details, "code");
		if (kind == job_kind::log && code == "stale" && m_restarts < k_max_restarts) {
			++m_restarts;
			restart();
			start_page();
			return;
		}
		const std::string message = code == "stale" ? "the history kept changing while it was read; press Refresh" : string_of(details, "message");
		fail(std::format("{}: {}", label, message.empty() ? code : message));
		if (kind == job_kind::log) m_log_failed = true;
		return;
	}
	switch (kind) {
	case job_kind::log:
		apply_page(value);
		break;
	case job_kind::files:
		m_files = history_files_of(value);
		break;
	case job_kind::diff:
		m_diff_text = string_of(value, "text");
		m_diff_truncated = bool_of(value, "truncated");
		++m_diff_version;
		break;
	}
}

void edt::vcs::history_model::apply_page(const json::object& result)
{
	m_restarts = 0;
	std::vector<graph_commit> laid_out;
	if (const json::value* list = result.if_contains("commits"); list && list->is_array()) {
		laid_out.reserve(list->as_array().size());
		for (const json::value& item : list->as_array()) {
			if (!item.is_object()) continue;
			auto commit = history_commit_of(item.as_object());
			if (!commit || m_row_of.contains(commit->hash)) continue;
			m_row_of.emplace(commit->hash, m_commits.size());
			laid_out.push_back({ commit->hash, commit->parents });
			m_commits.push_back(std::move(*commit));
		}
	}
	m_graph.append(laid_out);
	m_cursor = string_of(result, "next_cursor");
	// A page that brought nothing new would be asked for again and again.
	if (m_cursor.empty() || laid_out.empty()) {
		m_cursor.clear();
		m_at_end = true;
		m_graph.finish();
		return;
	}
	// The rows on screen may still reach past what came (a tall window, a page shortened by the size limit).
	show_rows(0, m_last_shown);
}

void edt::vcs::history_model::forget_job(std::optional<std::uint64_t>& job)
{
	if (!job) return;
	if (m_ports.cancel) m_ports.cancel(*job);
	m_jobs.erase(*job);
	job.reset();
}

void edt::vcs::history_model::fail(std::string text)
{
	egLOG_WARN("vcs", "{}", text);
	m_error = std::move(text);
}
