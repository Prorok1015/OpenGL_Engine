#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "snk_api.h"

// ECS phases (ARCHITECTURE_REVIEW 9.5). The engine owns a fixed, ordered list of built-in phases;
// a game may add its own *named* phase anchored before/after a built-in one, but cannot reorder
// or remove the built-ins. A system's phase is a property of what the system does (transform
// propagation is always `transform`), so the system author sets it in code at registration.
namespace ecs {

enum class phase : uint8_t {
	input,
	pre_update,
	fixed_update, // runs N times per frame from the fixed-step accumulator (see ecs_fixed_stepper.hpp)
	update,
	post_update,
	transform,
	late_update,
};

inline constexpr size_t builtin_phase_count = 7;

inline constexpr std::array<phase, builtin_phase_count> builtin_phases = {
	phase::input, phase::pre_update, phase::fixed_update, phase::update,
	phase::post_update, phase::transform, phase::late_update,
};

[[nodiscard]] constexpr std::string_view to_string(phase p) noexcept
{
	switch (p) {
	case phase::input:        return "input";
	case phase::pre_update:   return "pre_update";
	case phase::fixed_update: return "fixed_update";
	case phase::update:       return "update";
	case phase::post_update:  return "post_update";
	case phase::transform:    return "transform";
	case phase::late_update:  return "late_update";
	}
	return "?";
}

[[nodiscard]] constexpr std::optional<phase> parse_phase(std::string_view name) noexcept
{
	for (phase p : builtin_phases) {
		if (to_string(p) == name) return p;
	}
	return std::nullopt;
}

// Identifies a phase: a built-in one or a game phase by name. Built-ins convert implicitly, so
// `{ .phase = ecs::phase::transform }` and `{ .phase = "my_game_phase" }` both work.
class phase_id
{
public:
	phase_id(phase p) : m_name(to_string(p)), m_builtin(p) {}
	phase_id(std::string_view name) : m_name(name), m_builtin(parse_phase(name)) {}
	phase_id(const char* name) : phase_id(std::string_view{ name }) {}

	[[nodiscard]] const std::string& name() const noexcept { return m_name; }
	[[nodiscard]] bool is_builtin() const noexcept { return m_builtin.has_value(); }
	[[nodiscard]] std::optional<phase> builtin() const noexcept { return m_builtin; }

	friend bool operator==(const phase_id& a, const phase_id& b) noexcept { return a.m_name == b.m_name; }

private:
	std::string m_name;
	std::optional<phase> m_builtin;
};

enum class phase_side : uint8_t { before, after };

struct game_phase
{
	std::string name;
	phase anchor = phase::update;
	phase_side side = phase_side::after;
};

// The ordered phase list of a context: built-ins plus the game's anchored phases.
// Game phases sharing an anchor and side keep their registration order. Game phases run once per
// frame; only the built-in `fixed_update` is stepped by the accumulator, even when a game phase
// is anchored next to it.
class SNK_API phase_table
{
public:
	// False (and nothing added) when the name is empty, is a built-in name or is already taken.
	bool add_game_phase(std::string name, phase anchor, phase_side side);

	[[nodiscard]] bool contains(const phase_id& id) const;
	[[nodiscard]] const std::vector<game_phase>& game_phases() const noexcept { return m_game; }

	// Execution order: before-anchored game phases, the built-in, after-anchored game phases.
	[[nodiscard]] std::vector<phase_id> ordered() const;

private:
	std::vector<game_phase> m_game;
};

} // namespace ecs
