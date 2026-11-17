#pragma once
#include <snk/ecs/ecs_phase.hpp>
#include <snk/ecs/ecs_system_access.hpp>
#include <snk/ecs/ecs_system_interface.hpp>
#include <snk/common/snk_api.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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
	std::vector<std::string> after{};
	std::vector<std::string> before{};
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

// Built by system_registry::add in the engine (#785). Module code may read one through find(): a field is appended at
// the end, never reordered.
struct system_entry
{
	std::string name;
	system_desc desc;
	system_run_fn run = nullptr;
	std::function<system_instance()> make;
	std::vector<access_entry> access; // deduced from the argument types, for validation only
};

// Name -> system registration plus the phase list of the context. Pure data: plans are built
// from it (ecs_system_plan.hpp). Every member is defined in the engine (#785): a module hands in the parts of an entry
// and never builds one, so system_entry can grow without a new layout of module code.
class SNK_API system_registry
{
public:
	system_registry();
	// A layer over `parent` (MODULE_LAYER.md): find() and the phase list fall through to it, a name
	// it has cannot be added here, and add/remove change only this layer.
	explicit system_registry(const system_registry* parent);
	~system_registry();
	system_registry(const system_registry&);
	system_registry(system_registry&&);
	system_registry& operator=(const system_registry&);
	system_registry& operator=(system_registry&&);

	// False when the name is taken (here or in the parent).
	bool add(std::string_view name, system_desc desc, system_run_fn run, std::function<system_instance()> make,
		std::vector<access_entry> access);

	// False when nothing is registered under the name. Not synchronised: remove between frames, with no task
	// running a plan. Plans built earlier still hold the system's run function and instances: rebuild them
	// (scn::context_set::rebuild_all_plans) before the code goes away.
	bool remove(const std::string& name);

	// This layer's own systems, without the parent's.
	[[nodiscard]] std::size_t size() const noexcept;

	[[nodiscard]] const system_entry* find(const std::string& name) const;

	[[nodiscard]] const system_registry* parent() const noexcept;

	// The names of every system visible from this layer (the parent's and this layer's own), in name order. A name is
	// never in both: add() refuses a name the parent has.
	[[nodiscard]] std::vector<std::string> names() const;

	[[nodiscard]] phase_table& phases() noexcept;
	[[nodiscard]] const phase_table& phases() const noexcept;

private:
	const system_registry* m_parent = nullptr;
	std::unordered_map<std::string, system_entry> m_entries;
	phase_table m_phases;
};

} // namespace ecs
