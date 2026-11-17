#pragma once
#include <snk/ecs/ecs_fixed_stepper.hpp>
#include <snk/ecs/ecs_invoker.hpp>
#include <snk/ecs/ecs_system_plan.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/scene/level/scn_input_stack_desc.h>
#include <snk/scene/scn_time.h>
#include <snk/common/snk_api.h>
#include <entt/entity/registry.hpp>
#include <entt/meta/context.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace scn
{
	// Where a context came from; a reload of the level touches only `level` contexts.
	class component_catalog;

	enum class context_kind : uint8_t { level, runtime, preview, play };

	// One simulation unit: a single world with its own registry, system list, execution plan,
	// fixed stepper and run mode. The owning context_set decides when its phases run.
	class SNK_API world_context
	{
	public:
		world_context(uint32_t id, std::string_view name);
		~world_context() = default;
		world_context(const world_context&) = delete;
		world_context& operator=(const world_context&) = delete;
		world_context(world_context&&) = delete;
		world_context& operator=(world_context&&) = delete;

		entt::registry& state();
		entt::registry const& state() const;
		uint32_t get_id() const;
		const std::string_view get_name() const;

		context_kind kind() const;
		void set_kind(context_kind kind);
		// Whether the render extractor takes this context's cameras.
		bool extract() const;
		void set_extract(bool extract);

		// Input stacking order (not inp::input_layer, the window processor stack): the higher layer gets the mouse first (UI = 1, the rest 0).
		int input_layer() const;
		void set_input_layer(int layer);

		// Where the world stands among the worlds of its set: the order of the level file, worlds made while it runs after
		// all of them. Never reused (a world id is), so it orders equal input layers (INPUT_STACK.md sec. 3.1).
		uint32_t desc_index() const;
		void set_desc_index(uint32_t index);
		// The level's defaults for the base input layer; none when the file has no `input_stack` for the world.
		const std::optional<input_stack_desc>& input_stack() const;
		void set_input_stack(std::optional<input_stack_desc> stack);

		// Reseeds the context's `ecs::rng` from the base seed and its id.
		void seed(uint64_t base_seed);

		// An empty list still gets a plan; a context whose list was never set has none.
		void set_systems(std::vector<std::string> systems);
		const std::optional<std::vector<std::string>>& systems() const;

		// Builds the plan from the system list (drops it when no list was set). Call
		// resolve_slots afterwards.
		void rebuild_plan(const ecs::system_registry& registry);
		// Resolves the plan's phases against the owner's phase order.
		void resolve_slots(const std::vector<ecs::phase_id>& phase_order);
		// Null when the context has no systems registered.
		const ecs::system_plan* plan() const;

		bool has_phase_work(size_t order_index) const;
		void run_phase(size_t order_index, ecs::run_mode mode, const ecs::runtime_context& ctx) const;
		void apply_commands();
		// Moves the sandbox entities into `state`; component types are looked up in `layer_meta` (a module layer's
		// meta context, MODULE_LAYER.md), then in the engine's.
		static void apply_commands(entt::registry& state, const entt::meta_ctx* layer_meta = nullptr);
		// The same, and the pools of the components without a C++ type `catalog` knows are moved too (#606), their
		// entity fields mapped as the sandbox entities are.
		static void apply_commands(entt::registry& state, const entt::meta_ctx* layer_meta, const component_catalog* catalog);

		// The meta context of the layer the context belongs to; null: the engine's only.
		void set_meta(const entt::meta_ctx* meta);
		// The catalog of the layer the context belongs to (its components without a C++ type); null: none.
		void set_catalog(const component_catalog* catalog);

		// The context owns its time. begin_frame advances the stepper by the mode's rules and
		// writes delta_time and update_alpha into the registry ctx; the fixed steps it allows are
		// then played one by one through write_fixed_time().
		void begin_frame(std::chrono::duration<float> dt);
		int fixed_steps() const;
		void write_fixed_time();
		// Forgets the leftover fraction of a step (a fresh load); alpha and world_time go back to 0.
		void reset_time();
		// Puts the world's time back where a snapshot left it: world_time and the fixed step not yet taken.
		void restore_time(double world_time, double fixed_accumulator);

		// The owner keeps stepper and mode identical across the contexts of a set (lockstep).
		void set_mode(ecs::run_mode mode);
		void set_stepper(const ecs::fixed_stepper& stepper);
		const ecs::fixed_stepper& stepper() const;
		ecs::run_mode mode() const;
		// Factor on the frame delta in Edit and Play; set (and validated) only by the owning context_set.
		float time_scale() const;

	private:
		friend class context_set;
		void set_time_scale(float scale);

		uint32_t m_id;
		std::string m_name;
		context_kind m_kind = context_kind::runtime;
		uint32_t m_desc_index = 0;

		const entt::meta_ctx* m_meta = nullptr;
		const component_catalog* m_catalog = nullptr;
		entt::registry m_state;

		bool m_extract = true;
		int m_input_layer = 0;
		std::optional<input_stack_desc> m_input_stack;

		std::optional<std::vector<std::string>> m_systems;
		std::optional<ecs::system_plan> m_plan;
		// m_plan's phase_plan for every position of the owner's phase order (null where absent).
		std::vector<const ecs::phase_plan*> m_slots;

		ecs::fixed_stepper m_stepper;
		ecs::run_mode m_mode = ecs::run_mode::play;
		float m_time_scale = 1.0f;
		int m_fixed_steps = 0;
	};
} // namespace scn
