#pragma once
#include "ecs_system_registry.hpp"
#include "snk_api.h"
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ecs {

// One enabled system of a world, ready to run. Holds the stateful instance alive.
struct system_node
{
	std::string name;
	bool run_in_edit = false;
	system_run_fn run = nullptr;
	system_instance instance;

	void operator()(const runtime_context& ctx) const { run(instance.self, ctx); }
};

struct phase_plan
{
	phase_id id;
	std::vector<system_node> nodes; // execution order
};

enum class diagnostic_kind : uint8_t {
	unknown_system,             // name in the world's list is not registered
	duplicate_system,           // the same system listed twice in one world
	unknown_phase,              // the system's game phase was never added
	unknown_constraint_target,  // after/before names a system that is not registered at all
	wrong_phase_constraint,     // after/before contradicts the phase order
	cycle,                      // after/before cycle inside one phase
	access_conflict,            // debug validation: unordered conflicting access, see ecs_system_access.hpp
};

struct plan_diagnostic
{
	diagnostic_kind kind;
	std::string message;

	// Errors drop a system or an ordering constraint; the rest only warn.
	[[nodiscard]] bool is_error() const noexcept
	{
		return kind != diagnostic_kind::unknown_constraint_target && kind != diagnostic_kind::access_conflict;
	}
};

// The execution plan of one world, built once when the world loads or its system list changes:
// a flat vector of phases, each with its systems already in order.
struct SNK_API system_plan
{
	std::vector<phase_plan> phases; // same order as phase_table::ordered(), empty phases included
	std::vector<plan_diagnostic> diagnostics;

	[[nodiscard]] size_t system_count() const noexcept;
	[[nodiscard]] bool has_errors() const noexcept;
	[[nodiscard]] bool has_diagnostic(diagnostic_kind kind) const noexcept;
	[[nodiscard]] const phase_plan* find_phase(const phase_id& id) const noexcept;
	// System names of one phase in execution order (empty for an unknown phase).
	[[nodiscard]] std::vector<std::string> names_in(const phase_id& id) const;
};

struct plan_options
{
#ifdef NDEBUG
	bool validate_access = false;
#else
	bool validate_access = true;
#endif
};

// Builds the plan for the systems named in `names` (world_desc.systems order).
//  - Order inside a phase is a topological sort of the after/before constraints; ties are broken
//    stably by position in `names`, so a plain list keeps its meaning.
//  - A constraint naming a registered system that this world does not run is ignored; one naming
//    an unregistered system is a warning.
//  - Problems never throw or abort: they land in `diagnostics` (and the log). A cycle falls back
//    to list order for the systems involved.
[[nodiscard]] SNK_API system_plan build_system_plan(
	const system_registry& registry,
	std::span<const std::string> names,
	plan_options options = {});

} // namespace ecs
