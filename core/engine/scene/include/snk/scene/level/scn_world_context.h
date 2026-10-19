#pragma once
#include <snk/ecs/ecs_fixed_stepper.hpp>
#include <snk/ecs/ecs_invoker.hpp>
#include <snk/ecs/ecs_system_plan.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/common/snk_api.h>
#include <entt/entity/registry.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scn
{
	// One simulation unit: a single world with its own registry, system list, execution plan,
	// fixed stepper and run mode. The owner (scn::level for now) decides when its phases run.
	class SNK_API world_context
	{
	public:
		world_context(uint32_t id, std::string_view name);
		~world_context() = default;
		world_context(const world_context&) = delete;
		world_context& operator=(const world_context&) = delete;
		world_context(world_context&&) = delete;
		world_context& operator=(world_context&&) = delete;

		entt::registry& state() { return m_state; }
		entt::registry const& state() const { return m_state; }
		uint32_t get_id() const { return m_id; }
		const std::string_view get_name() const { return m_name; }

		// Reseeds the context's `ecs::rng` from the base seed and its id.
		void seed(uint64_t base_seed);

		// An empty list still gets a plan; a context whose list was never set has none.
		void set_systems(std::vector<std::string> systems) { m_systems = std::move(systems); }
		const std::optional<std::vector<std::string>>& systems() const { return m_systems; }

		// Builds the plan from the system list (drops it when no list was set). Call
		// resolve_slots afterwards.
		void rebuild_plan(const ecs::system_registry& registry);
		// Resolves the plan's phases against the owner's phase order.
		void resolve_slots(const std::vector<ecs::phase_id>& phase_order);
		// Null when the context has no systems registered.
		const ecs::system_plan* plan() const { return m_plan ? &*m_plan : nullptr; }

		bool has_phase_work(size_t order_index) const;
		void run_phase(size_t order_index, ecs::run_mode mode, const ecs::runtime_context& ctx) const;
		void apply_commands() { apply_commands(m_state); }
		static void apply_commands(entt::registry& state);

		// The owner keeps these identical across its contexts.
		void sync_time(const ecs::fixed_stepper& stepper, ecs::run_mode mode)
		{
			m_stepper = stepper;
			m_mode = mode;
		}
		const ecs::fixed_stepper& stepper() const { return m_stepper; }
		ecs::run_mode mode() const { return m_mode; }

	private:
		uint32_t m_id;
		std::string m_name;
		entt::registry m_state;

		std::optional<std::vector<std::string>> m_systems;
		std::optional<ecs::system_plan> m_plan;
		// m_plan's phase_plan for every position of the owner's phase order (null where absent).
		std::vector<const ecs::phase_plan*> m_slots;

		ecs::fixed_stepper m_stepper;
		ecs::run_mode m_mode = ecs::run_mode::play;
	};
} // namespace scn
