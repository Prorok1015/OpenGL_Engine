#pragma once
#include <snk/ecs/ecs_phase.hpp>
#include <snk/ecs/ecs_system.h>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ecs {

// Owns a set of system and game-phase registrations in a system_factory and removes them, in
// reverse order, on destruction (the counterpart of scn::registration_scope for systems).
// Plans built while the scope was alive keep the systems until they are rebuilt
// (scn::context_set::rebuild_all_plans): rebuild them before the code of the systems goes away.
class system_scope
{
public:
	explicit system_scope(system_factory& factory)
		: m_factory(&factory)
	{
	}

	~system_scope() { release(); }

	system_scope(const system_scope&) = delete;
	system_scope& operator=(const system_scope&) = delete;

	system_scope(system_scope&& other) noexcept
		: m_factory(other.m_factory)
		, m_entries(std::move(other.m_entries))
	{
		other.m_entries.clear();
	}

	system_scope& operator=(system_scope&& other) noexcept
	{
		if (this != &other) {
			release();
			m_factory = other.m_factory;
			m_entries = std::move(other.m_entries);
			other.m_entries.clear();
		}
		return *this;
	}

	template <auto Candidate>
	void system(const std::string& name, system_desc desc = {})
	{
		m_factory->register_system<Candidate>(name, std::move(desc));
		m_entries.push_back({ entry_kind::system, name });
	}

	template <auto Candidate, typename T>
	requires std::is_base_of_v<ecs::system_interface, T>
	void system(const std::string& name, system_desc desc, std::shared_ptr<T> instance)
	{
		m_factory->register_system<Candidate, T>(name, std::move(desc), std::move(instance));
		m_entries.push_back({ entry_kind::system, name });
	}

	template <auto Candidate, typename T, class... ARGS>
	requires std::is_base_of_v<ecs::system_interface, T>
	void system(const std::string& name, system_desc desc, ARGS... args)
	{
		m_factory->register_system<Candidate, T, ARGS...>(name, std::move(desc), args...);
		m_entries.push_back({ entry_kind::system, name });
	}

	// False (nothing recorded) when the factory rejects the name.
	bool game_phase(std::string name, phase anchor, phase_side side)
	{
		if (!m_factory->add_game_phase(name, anchor, side)) {
			return false;
		}
		m_entries.push_back({ entry_kind::phase, std::move(name) });
		return true;
	}

	std::size_t size() const { return m_entries.size(); }

private:
	enum class entry_kind { system, phase };
	struct entry {
		entry_kind kind;
		std::string name;
	};

	void release()
	{
		for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
			if (it->kind == entry_kind::system) {
				m_factory->unregister_system(it->name);
			} else {
				m_factory->remove_game_phase(it->name);
			}
		}
		m_entries.clear();
	}

	system_factory* m_factory;
	std::vector<entry> m_entries;
};

} // namespace ecs
