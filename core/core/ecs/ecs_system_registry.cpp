#include <snk/ecs/ecs_system_registry.hpp>
#include <algorithm>
#include <utility>

ecs::system_registry::system_registry() = default;

ecs::system_registry::system_registry(const system_registry* parent)
	: m_parent(parent)
	, m_phases(parent ? &parent->m_phases : nullptr)
{
}

ecs::system_registry::~system_registry() = default;
ecs::system_registry::system_registry(const system_registry&) = default;
ecs::system_registry::system_registry(system_registry&&) = default;
ecs::system_registry& ecs::system_registry::operator=(const system_registry&) = default;
ecs::system_registry& ecs::system_registry::operator=(system_registry&&) = default;

bool ecs::system_registry::add(std::string_view name, system_desc desc, system_run_fn run, std::function<system_instance()> make,
	std::vector<access_entry> access)
{
	std::string key{ name };
	if (m_parent && m_parent->find(key)) {
		return false;
	}
	system_entry entry{ key, std::move(desc), run, std::move(make), std::move(access) };
	return m_entries.emplace(std::move(key), std::move(entry)).second;
}

bool ecs::system_registry::remove(const std::string& name)
{
	return m_entries.erase(name) != 0;
}

std::size_t ecs::system_registry::size() const noexcept
{
	return m_entries.size();
}

const ecs::system_entry* ecs::system_registry::find(const std::string& name) const
{
	auto it = m_entries.find(name);
	if (it != m_entries.end()) {
		return &it->second;
	}
	return m_parent ? m_parent->find(name) : nullptr;
}

const ecs::system_registry* ecs::system_registry::parent() const noexcept
{
	return m_parent;
}

std::vector<std::string> ecs::system_registry::names() const
{
	std::vector<std::string> out = m_parent ? m_parent->names() : std::vector<std::string>{};
	out.reserve(out.size() + m_entries.size());
	for (const auto& entry : m_entries) {
		out.push_back(entry.first);
	}
	std::sort(out.begin(), out.end());
	return out;
}

ecs::phase_table& ecs::system_registry::phases() noexcept
{
	return m_phases;
}

const ecs::phase_table& ecs::system_registry::phases() const noexcept
{
	return m_phases;
}
