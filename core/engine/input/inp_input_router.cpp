#include <snk/input/inp_input_router.h>
#include <snk/common/engine_assert.h>

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
