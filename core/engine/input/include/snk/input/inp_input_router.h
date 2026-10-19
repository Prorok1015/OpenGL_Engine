#pragma once
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/input/inp_events.hpp>
#include <snk/common/snk_api.h>
#include <glm/glm.hpp>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace inp
{
	// Names the contexts a binding feeds. An empty `context` means every context of the set.
	struct input_target
	{
		std::string set;
		std::string context;

		bool matches(std::string_view set_name, std::string_view context_name) const {
			return set == set_name && (context.empty() || context == context_name);
		}
		bool operator==(const input_target&) const = default;
	};

	struct routed_input
	{
		inp::input_state state;
		// Mirrors `inp::input_focus::active` of the context.
		bool focused = false;
	};

	// Decides which context sees the raw input. Panels (or the default target) bind a screen rect to
	// a target; the context whose binding is under the cursor gets the full state, every other
	// context a neutral one. The router knows contexts only by name, so it needs no scene module.
	class SNK_API input_router
	{
	public:
		explicit input_router(std::shared_ptr<const ecs_input_manager> source);

		// Rect is {x0, y0, x1, y1} in framebuffer pixels, bounds included. Panels bind every frame;
		// bindings live until clear_bindings(). Overlapping rects: the one bound last wins.
		void bind(input_target target, const glm::ivec4& rect);
		void clear_bindings();
		std::size_t binding_count() const { return m_bindings.size(); }

		// Used while no explicit binding is under the cursor, with the whole backbuffer as its
		// rect (no size, so the normalized mouse delta stays zero). Without the editor this is the
		// set "level". The editor resets it: there ownership follows the panels.
		void set_default_target(std::optional<input_target> target) { m_default = std::move(target); }
		const std::optional<input_target>& default_target() const { return m_default; }

		routed_input route(std::string_view set, std::string_view context) const;

		// Buttons released, deltas zero, the cursor where it last was.
		static inp::input_state neutral(const inp::input_state& raw);

	private:
		struct binding
		{
			input_target target;
			glm::ivec4 rect;
		};

		std::shared_ptr<const ecs_input_manager> m_source;
		std::vector<binding> m_bindings;
		std::optional<input_target> m_default;
	};
}
