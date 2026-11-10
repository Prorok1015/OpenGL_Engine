#include "edt_desc_problems.h"
#include <charconv>
#include <format>
#include <utility>

namespace {
	// The count at the front of a collector's "N more diagnostics"; 1 when the text has none.
	std::size_t dropped_count(const std::string& reason)
	{
		std::size_t count = 0;
		const auto [end, error] = std::from_chars(reason.data(), reason.data() + reason.size(), count);
		return error == std::errc{} && end != reason.data() && count > 0 ? count : 1;
	}
}

edt::problem edt::desc_problem_publisher::to_problem(const res::diagnostic& item)
{
	problem out;
	out.severity = item.severity == res::diagnostic_severity::error ? problem_severity::error : problem_severity::warning;
	if (!item.resource.empty()) {
		const res::tag tag{ item.resource };
		out.file = tag.protocol() == res::tag::default_protocol() ? std::string(tag.relative()) : item.resource;
	}
	out.code = item.kind;
	out.message = item.reason;
	if (!item.json_path.empty()) out.notes.push_back(item.json_path);
	return out;
}

void edt::desc_problem_publisher::report(problem_list& problems, const std::string& scope, const std::vector<res::diagnostic>& items,
	const res::tag& copy, const res::tag& file)
{
	scope_problems now;
	for (const res::diagnostic& item : items) {
		if (item.kind == res::diagnostic_kind::truncated) {
			now.dropped += dropped_count(item.reason);
			continue;
		}
		if (copy.is_valid() && file.is_valid() && item.resource == copy.view()) {
			res::diagnostic renamed = item;
			renamed.resource = file.string();
			now.items.push_back(to_problem(renamed));
		} else {
			now.items.push_back(to_problem(item));
		}
	}
	if (now.items.empty() && now.dropped == 0) {
		clear(problems, scope);
		return;
	}
	m_scopes.insert_or_assign(scope, std::move(now));
	publish(problems);
}

void edt::desc_problem_publisher::clear(problem_list& problems, std::string_view scope)
{
	const auto found = m_scopes.find(scope);
	if (found == m_scopes.end()) return;
	m_scopes.erase(found);
	publish(problems);
}

void edt::desc_problem_publisher::publish(problem_list& problems)
{
	std::vector<problem> all;
	std::size_t dropped = 0;
	for (const auto& [scope, set] : m_scopes) {
		all.insert(all.end(), set.items.begin(), set.items.end());
		dropped += set.dropped;
	}
	if (dropped > 0) {
		problem more;
		more.severity = problem_severity::warning;
		more.code = res::diagnostic_kind::truncated;
		more.message = std::format("{} more diagnostics", dropped);
		all.push_back(std::move(more));
	}
	if (all.empty()) {
		if (m_published) problems.clear(source);
		m_published = false;
		return;
	}
	problems.set(source, std::move(all));
	m_published = true;
}
