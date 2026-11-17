#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/snk_api.h>

namespace app
{
	// Per-frame data threaded to every frame stage. Engine-level frame data (the render
	// frame_context) is a service in `storage`, not a member here.
	struct stage_context
	{
		ds::app_data_storage& storage;
		std::chrono::duration<float> dt;
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
		using stage_fn = std::function<void(stage_context&)>;
		using hook_fn = std::function<void(ds::app_data_storage&)>;

		// Gives a built-in stage its body. False (logged) if it already has one.
		bool set_body(builtin_stage stage, stage_fn fn);
		// Takes the body away again (a module that set it is being unloaded).
		void clear_body(builtin_stage stage);

		// False (nothing added, logged) when the name is empty, is a built-in name or is taken, or
		// when the stage is asked to run before `begin`.
		bool add_stage(std::string name, builtin_stage anchor, stage_side side, stage_fn fn);
		// False when there is no such added stage (built-ins cannot be removed).
		bool remove_stage(std::string_view name);

		void run(stage_context& ctx);

		// Stage names in execution order, built-ins included (introspection for tests).
		[[nodiscard]] std::vector<std::string> stage_names() const;

		// Run once by loop_service::init (registration order) / term (reverse order).
		void add_init_hook(hook_fn fn) { m_init_hooks.push_back(std::move(fn)); }
		void add_term_hook(hook_fn fn) { m_term_hooks.push_back(std::move(fn)); }
		void run_init_hooks(ds::app_data_storage& storage);
		void run_term_hooks(ds::app_data_storage& storage);

	private:
		struct extra_stage { std::string name; builtin_stage anchor; stage_side side; stage_fn fn; };

		std::array<stage_fn, builtin_stage_count> m_bodies;
		std::vector<extra_stage> m_extra;
		std::vector<hook_fn> m_init_hooks;
		std::vector<hook_fn> m_term_hooks;
	};

	// Body of the `begin` stage: the frame allocator reset every other stage relies on.
	SNK_API void register_core_frame_stages(frame_stage_registry& registry);
}
