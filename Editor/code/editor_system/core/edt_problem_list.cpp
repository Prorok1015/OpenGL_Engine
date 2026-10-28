#include <snk/editor/edt_problem_list.h>
#include <snk/editor/edt_editor_events.h>
#include <algorithm>

const char* edt::problem_severity_name(problem_severity severity)
{
	switch (severity) {
	case problem_severity::error: return "error";
	case problem_severity::warning: return "warning";
	case problem_severity::note: return "note";
	}
	return "error";
}

std::optional<edt::problem_severity> edt::problem_severity_from(std::string_view name)
{
	if (name == "error") return problem_severity::error;
	if (name == "warning") return problem_severity::warning;
	if (name == "note") return problem_severity::note;
	return std::nullopt;
}

edt::problem_list::problem_list(editor_events* events)
	: m_events(events)
{
}

void edt::problem_list::set(std::string_view source, std::vector<problem> items)
{
	auto it = std::find_if(m_sources.begin(), m_sources.end(), [source](const auto& s) { return s.first == source; });
	if (it == m_sources.end()) {
		m_sources.emplace_back(std::string(source), std::move(items));
		changed(m_sources.back().first);
		return;
	}
	it->second = std::move(items);
	changed(it->first);
}

void edt::problem_list::clear(std::string_view source)
{
	auto it = std::find_if(m_sources.begin(), m_sources.end(), [source](const auto& s) { return s.first == source; });
	if (it == m_sources.end()) return;
	std::string name = std::move(it->first);
	m_sources.erase(it);
	changed(std::move(name));
}

std::vector<edt::sourced_problem> edt::problem_list::list() const
{
	std::vector<sourced_problem> out;
	out.reserve(size());
	for (const auto& [source, items] : m_sources) {
		for (const auto& item : items) out.push_back({ source, item });
	}
	return out;
}

std::vector<std::string> edt::problem_list::sources() const
{
	std::vector<std::string> out;
	out.reserve(m_sources.size());
	for (const auto& entry : m_sources) out.push_back(entry.first);
	return out;
}

std::size_t edt::problem_list::count(problem_severity severity) const
{
	std::size_t n = 0;
	for (const auto& entry : m_sources) {
		n += static_cast<std::size_t>(std::count_if(entry.second.begin(), entry.second.end(),
			[severity](const problem& p) { return p.severity == severity; }));
	}
	return n;
}

std::size_t edt::problem_list::size() const
{
	std::size_t n = 0;
	for (const auto& entry : m_sources) n += entry.second.size();
	return n;
}

void edt::problem_list::changed(std::string source)
{
	++m_generation;
	if (m_events) m_events->problems_changed.emit(source);
}
