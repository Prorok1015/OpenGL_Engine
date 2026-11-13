#include <snk/input/inp_actions.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/engine_assert.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <algorithm>
#include <iterator>

bool inp::action_map::add(std::string name, std::vector<action_binding> bindings)
{
	if (name.empty() || std::ranges::any_of(m_actions, [&](const action& a) { return a.name == name; })) return false;
	m_actions.push_back(action{ std::move(name), std::move(bindings) });
	return true;
}

std::span<const inp::action> inp::action_map::actions() const
{
	return m_actions;
}

std::size_t inp::action_map::size() const
{
	return m_actions.size();
}

inp::action_id inp::action_map::id(std::string_view name) const
{
	const auto it = std::ranges::find_if(m_actions, [&](const action& a) { return a.name == name; });
	if (it != m_actions.end()) return static_cast<action_id>(std::distance(m_actions.begin(), it));
	ASSERT_MSG(!ds::task_system::on_worker(), "action_map::id: an unknown name from a task; look actions up at init");
	if (m_warned.emplace(name).second) {
		egLOG_WARN("input", "no input action '{}' in res/input/actions.desc", name);
	}
	return invalid_action;
}

const inp::action* inp::action_map::find(action_id id) const
{
	return id < m_actions.size() ? &m_actions[id] : nullptr;
}

namespace
{
	constexpr std::string_view k_gamepad_button_names[] = {
		"A", "B", "X", "Y", "LB", "RB", "BACK", "START", "GUIDE", "LSTICK", "RSTICK", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT" };
	static_assert(std::size(k_gamepad_button_names) == core::inp::gamepad_button_count, "a name for every gamepad button");

	constexpr std::string_view k_gamepad_axis_names[] = {
		"LeftX-", "LeftX+", "LeftY-", "LeftY+", "RightX-", "RightX+", "RightY-", "RightY+", "LeftTrigger", "RightTrigger" };
	static_assert(std::size(k_gamepad_axis_names) == static_cast<std::size_t>(core::inp::GAMEPAD_AXES::COUNT), "a name for every directed axis");
}

std::optional<inp::GAMEPAD_BUTTONS> inp::gamepad_button_from_name(std::string_view name)
{
	const auto it = std::ranges::find(k_gamepad_button_names, name);
	if (it == std::end(k_gamepad_button_names)) return std::nullopt;
	return static_cast<GAMEPAD_BUTTONS>(std::distance(std::begin(k_gamepad_button_names), it));
}

std::string_view inp::gamepad_button_name(GAMEPAD_BUTTONS button)
{
	const auto index = static_cast<std::size_t>(button);
	return index < std::size(k_gamepad_button_names) ? k_gamepad_button_names[index] : std::string_view{};
}

std::optional<inp::GAMEPAD_AXES> inp::gamepad_axis_from_name(std::string_view name)
{
	const auto it = std::ranges::find(k_gamepad_axis_names, name);
	if (it == std::end(k_gamepad_axis_names)) return std::nullopt;
	return static_cast<GAMEPAD_AXES>(std::distance(std::begin(k_gamepad_axis_names), it));
}

std::string_view inp::gamepad_axis_name(GAMEPAD_AXES axis)
{
	const auto index = static_cast<std::size_t>(axis);
	return index < std::size(k_gamepad_axis_names) ? k_gamepad_axis_names[index] : std::string_view{};
}

inp::GAMEPAD_AXIS_IDS inp::gamepad_axis_id(GAMEPAD_AXES axis)
{
	switch (axis) {
	case GAMEPAD_AXES::LEFT_X_NEG: case GAMEPAD_AXES::LEFT_X_POS: return GAMEPAD_AXIS_IDS::LEFT_X;
	case GAMEPAD_AXES::LEFT_Y_NEG: case GAMEPAD_AXES::LEFT_Y_POS: return GAMEPAD_AXIS_IDS::LEFT_Y;
	case GAMEPAD_AXES::RIGHT_X_NEG: case GAMEPAD_AXES::RIGHT_X_POS: return GAMEPAD_AXIS_IDS::RIGHT_X;
	case GAMEPAD_AXES::RIGHT_Y_NEG: case GAMEPAD_AXES::RIGHT_Y_POS: return GAMEPAD_AXIS_IDS::RIGHT_Y;
	case GAMEPAD_AXES::LEFT_TRIGGER: return GAMEPAD_AXIS_IDS::LEFT_TRIGGER;
	case GAMEPAD_AXES::RIGHT_TRIGGER: return GAMEPAD_AXIS_IDS::RIGHT_TRIGGER;
	case GAMEPAD_AXES::COUNT: break;
	}
	return GAMEPAD_AXIS_IDS::COUNT;
}

int inp::gamepad_axis_direction(GAMEPAD_AXES axis)
{
	switch (axis) {
	case GAMEPAD_AXES::LEFT_X_NEG: case GAMEPAD_AXES::LEFT_Y_NEG: case GAMEPAD_AXES::RIGHT_X_NEG: case GAMEPAD_AXES::RIGHT_Y_NEG: return -1;
	default: return 1;
	}
}
