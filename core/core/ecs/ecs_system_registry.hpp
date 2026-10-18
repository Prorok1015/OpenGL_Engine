#pragma once
#include "ecs_phase.hpp"
#include "ecs_system_access.hpp"
#include "ecs_system_interface.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ecs {

struct runtime_context;

// Execution mode of a context (ARCHITECTURE_REVIEW 9.5).
//   edit  - only systems flagged run_in_edit (transforms, camera matrices), gameplay does not tick
//   play  - every system
//   pause - only run_in_edit systems, time is frozen
//   step  - one full frame (one fixed_update + one pass of the other phases), then back to pause
enum class run_mode : uint8_t { edit, play, pause, step };

[[nodiscard]] constexpr bool mode_runs_system(run_mode mode, bool run_in_edit) noexcept
{
	return mode == run_mode::play || mode == run_mode::step || run_in_edit;
}

// Everything the author of a system decides in code at registration. Which systems a world runs
// stays data (world_desc.systems); the phase and the ordering constraints are code.
// `after` / `before` name other registered systems and only order systems of the same phase;
// a system in an earlier phase is already guaranteed to run first.
struct system_desc
{
	phase_id phase = ecs::phase::update;
	std::vector<std::string> after;
	std::vector<std::string> before;
	bool run_in_edit = false;
};

// What a world gets for one enabled system: the shared instance of a stateful system (or null),
// an opaque self pointer for the run function.
struct system_instance
{
	std::shared_ptr<system_interface> owner;
	void* self = nullptr;
};

using system_run_fn = void (*)(void* self, const runtime_context& ctx);

struct system_entry
{
	std::string name;
	system_desc desc;
	system_run_fn run = nullptr;
	std::function<system_instance()> make;
	std::vector<access_entry> access; // deduced from the argument types, for validation only
};

// Name -> system registration plus the phase list of the context. Pure data: plans are built
// from it (ecs_system_plan.hpp).
class system_registry
{
public:
	// False when the name is taken.
	bool add(system_entry entry)
	{
		const std::string key = entry.name;
		return m_entries.emplace(key, std::move(entry)).second;
	}

	[[nodiscard]] const system_entry* find(const std::string& name) const
	{
		auto it = m_entries.find(name);
		return it == m_entries.end() ? nullptr : &it->second;
	}

	[[nodiscard]] phase_table& phases() noexcept { return m_phases; }
	[[nodiscard]] const phase_table& phases() const noexcept { return m_phases; }

private:
	std::unordered_map<std::string, system_entry> m_entries;
	phase_table m_phases;
};

} // namespace ecs
