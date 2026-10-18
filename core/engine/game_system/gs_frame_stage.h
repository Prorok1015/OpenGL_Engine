#pragma once
#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "ds/ds_store.hpp"
#include "snk_api.h"

namespace rnd { struct frame_context; }

namespace gs
{
	// Per-frame data threaded to every frame stage (EPIC-49 US-49-8). `render` is the single
	// frame_context shared by the extract and render stages within one frame.
	struct frame_step_context
	{
		ds::app_data_storage& storage;
		std::chrono::duration<float> dt;
		rnd::frame_context& render;
	};

	// The fixed, ordered frame stages (mirror of the ECS phases, ecs_phase.hpp). Built-ins cannot be
	// reordered or removed; a module adds its own named stage before/after one of them.
	enum class builtin_stage : uint8_t {
		begin,       // frame allocator reset; nothing may run before it
		input,       // window events
		update,      // level / ECS update
		post_update, // end-of-update bookkeeping (input deltas)
		extract,     // render data extraction
		render,      // 3D passes
		ui,          // host UI (editor ImGui)
		present,     // swap buffers
	};

	inline constexpr std::size_t builtin_stage_count = 8;

	[[nodiscard]] constexpr std::string_view to_string(builtin_stage s) noexcept
	{
		switch (s) {
		case builtin_stage::begin:       return "begin";
		case builtin_stage::input:       return "input";
		case builtin_stage::update:      return "update";
		case builtin_stage::post_update: return "post_update";
		case builtin_stage::extract:     return "extract";
		case builtin_stage::render:      return "render";
		case builtin_stage::ui:          return "ui";
		case builtin_stage::present:     return "present";
		}
		return "?";
	}

	[[nodiscard]] SNK_API std::optional<builtin_stage> parse_builtin_stage(std::string_view name) noexcept;

	enum class stage_side : uint8_t { before, after };

	// Ordered registry of per-frame stages. Execution order: for every built-in in canonical order,
	// the stages added before it (in registration order), the built-in's body, the stages added
	// after it (in registration order).
	class SNK_API frame_stage_registry
	{
	public:
		using stage_fn = std::function<void(frame_step_context&)>;

		// Gives a built-in stage its body. False (logged) if it already has one.
		bool set_body(builtin_stage stage, stage_fn fn);

		// False (nothing added, logged) when the name is empty, is a built-in name or is taken, or
		// when the stage is asked to run before `begin`.
		bool add_stage(std::string name, builtin_stage anchor, stage_side side, stage_fn fn);

		void run(frame_step_context& ctx);

		// Stage names in execution order, built-ins included (introspection for tests).
		[[nodiscard]] std::vector<std::string> stage_names() const;

	private:
		struct extra_stage { std::string name; builtin_stage anchor; stage_side side; stage_fn fn; };

		std::array<stage_fn, builtin_stage_count> m_bodies;
		std::vector<extra_stage> m_extra;
	};

	// Bodies of the built-in stages common to every frame loop.
	SNK_API void register_core_frame_stages(frame_stage_registry& registry);
}
