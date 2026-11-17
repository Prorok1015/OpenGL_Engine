#pragma once
#include <snk/ecs/ecs_phase.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/common/logger/engine_log.h>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ecs {

// Owns a set of system and game-phase registrations in a system_factory and removes them, in
// reverse order, on destruction (the counterpart of scn::registration_scope for systems).
// Teardown contract: destroy the scope between frames, with no task running a plan, then rebuild the
// plans (scn::context_set::rebuild_all_plans) before the code of the systems goes away: plans hold
// the run functions and system instances. The module loader does both for a game module (#220 PR-6).
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

	// Each returns false (nothing recorded, so the scope never removes someone else's entry) when the
	// factory rejects the name.
	template <auto Candidate>
	bool system(const std::string& name, system_desc desc = {})
	{
		return record(entry_kind::system, name, [&] { return m_factory->register_system<Candidate>(name, std::move(desc)); });
	}

	template <auto Candidate, typename T>
	requires std::is_base_of_v<ecs::system_interface, T>
	bool system(const std::string& name, system_desc desc, std::shared_ptr<T> instance)
	{
		return record(entry_kind::system, name, [&] { return m_factory->register_system<Candidate, T>(name, std::move(desc), std::move(instance)); });
	}

	template <auto Candidate, typename T, class... ARGS>
	requires std::is_base_of_v<ecs::system_interface, T>
	bool system(const std::string& name, system_desc desc, ARGS... args)
	{
		return record(entry_kind::system, name, [&] { return m_factory->register_system<Candidate, T, ARGS...>(name, std::move(desc), args...); });
	}

	bool game_phase(std::string name, phase anchor, phase_side side)
	{
		return record(entry_kind::phase, name, [&] { return m_factory->add_game_phase(name, anchor, side); });
	}

	[[nodiscard]] std::size_t size() const { return m_entries.size(); }

private:
	enum class entry_kind { system, phase };
	struct entry {
		entry_kind kind;
		std::string name;
	};

	// The entry is built and the storage reserved before the registration, so recording cannot throw
	// after the factory has accepted it.
	template <class Fn>
	bool record(entry_kind kind, const std::string& name, Fn&& registration)
	{
		entry recorded{ kind, name };
		m_entries.reserve(m_entries.size() + 1);
		if (!registration()) {
			return false;
		}
		m_entries.push_back(std::move(recorded));
		return true;
	}

	void release()
	{
		for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
			const bool removed = it->kind == entry_kind::system ? m_factory->unregister_system(it->name)
																: m_factory->remove_game_phase(it->name);
			if (!removed) {
				egLOG_WARN("ecs", "system_scope: '{}' was already removed from the factory", it->name);
			}
		}
		m_entries.clear();
	}

	system_factory* m_factory;
	std::vector<entry> m_entries;
};

} // namespace ecs
