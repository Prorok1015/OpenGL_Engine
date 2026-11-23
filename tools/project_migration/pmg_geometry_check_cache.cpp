#include <snk/project_migration/pmg_geometry_check_cache.h>
#include <snk/common/hash/sha256.h>
#include <span>
#include <utility>

prj::migration::geometry_check_cache::geometry_check_cache(std::size_t max_entries)
	: m_max_entries(max_entries)
{
}

std::string prj::migration::geometry_check_cache::problem(std::string_view path, std::string_view text,
	const std::function<std::string()>& check)
{
	key id{ std::string(path), text.size(), cmn::sha256_hex(std::as_bytes(std::span(text.data(), text.size()))) };
	{
		std::lock_guard lock(m_mutex);
		if (const auto found = m_problems.find(id); found != m_problems.end()) return found->second;
	}
	std::string answer = check();
	std::lock_guard lock(m_mutex);
	// One entry a path: the answer for an older text of it is no use any more.
	for (auto it = m_problems.lower_bound(key{ id.path, 0, {} }); it != m_problems.end() && it->first.path == id.path;) {
		it = m_problems.erase(it);
	}
	// A full cache keeps what it holds: a scan walks the project in the same order every time, so evicting would make the
	// files past the bound push out the ones before them and no scan would hit.
	if (m_problems.size() < m_max_entries) m_problems.emplace(std::move(id), answer);
	return answer;
}

void prj::migration::geometry_check_cache::clear()
{
	std::lock_guard lock(m_mutex);
	m_problems.clear();
}

std::size_t prj::migration::geometry_check_cache::size() const
{
	std::lock_guard lock(m_mutex);
	return m_problems.size();
}
