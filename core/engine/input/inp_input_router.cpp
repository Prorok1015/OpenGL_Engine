#include <snk/input/inp_input_router.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <string_view>
#include <vector>

namespace
{
	bool contains(const glm::ivec4& rect, glm::vec2 pos)
	{
		return pos.x >= rect.x && pos.x <= rect.z && pos.y >= rect.y && pos.y <= rect.w;
	}
}

inp::input_router::input_router(std::shared_ptr<const ecs_input_manager> source)
	: m_source(std::move(source))
{
	ASSERT_MSG(m_source, "input_router needs the input source");
}

void inp::input_router::bind(input_target target, const glm::ivec4& rect)
{
	m_bindings.push_back(binding{ std::move(target), rect });
}

void inp::input_router::clear_bindings()
{
	m_bindings.clear();
}

void inp::input_router::forget_set(std::string_view set)
{
	std::erase_if(m_bindings, [set](const binding& b) { return b.target.set == set; });
	if (const auto it = m_holds.find(set); it != m_holds.end()) {
		m_holds.erase(it);
	}
	if (const auto it = m_stacks.find(set); it != m_stacks.end()) {
		m_stacks.erase(it);
	}
	if (const auto it = m_injected.find(set); it != m_injected.end()) {
		m_injected.erase(it);
	}
}

void inp::input_router::inject(const input_target& target, std::string action, action_edge edge)
{
	if (target.context.empty()) {
		egLOG_WARN("input", "inject '{}' into set '{}' names no world, dropped", action, target.set);
		return;
	}
	m_injected[target.set].push_back(queued_injection{ target.context, std::move(action), edge });
}

std::vector<inp::input_router::queued_injection> inp::input_router::take_injected(std::string_view set)
{
	const auto it = m_injected.find(set);
	if (it == m_injected.end()) return {};
	std::vector<queued_injection> taken = std::move(it->second);
	m_injected.erase(it);
	return taken;
}

inp::input_stack& inp::input_router::stack(std::string_view set)
{
	const auto it = m_stacks.find(set);
	return it != m_stacks.end() ? it->second : m_stacks.emplace(std::string{ set }, input_stack{}).first->second;
}

inp::input_state inp::input_router::neutral(const inp::input_state& raw)
{
	inp::input_state state;
	state.mouse_pos = raw.mouse_pos;
	state.mouse_prev = raw.mouse_pos;
	return state;
}

inp::routed_input inp::input_router::route(std::string_view set, std::string_view context) const
{
	const inp::input_state& raw = m_source->raw();

	const input_target* winner = nullptr;
	const glm::ivec4* rect = nullptr;
	for (auto it = m_bindings.rbegin(); it != m_bindings.rend(); ++it) {
		if (contains(it->rect, raw.mouse_pos)) {
			winner = &it->target;
			rect = &it->rect;
			break;
		}
	}
	if (!winner && m_default) {
		winner = &*m_default;
	}

	if (!winner || !winner->matches(set, context)) {
		return { neutral(raw), false };
	}

	routed_input out{ raw, true };
	out.state.mouse_dir = glm::vec2{ 0.f };
	if (rect) {
		const glm::vec2 half_size = glm::vec2{ rect->z - rect->x, rect->w - rect->y } * 0.5f;
		if (half_size.x > 0.f && half_size.y > 0.f) {
			out.state.mouse_dir = raw.mouse_dir / half_size;
		}
	}
	return out;
}
