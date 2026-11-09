#pragma once
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_bus_context.hpp>
#include <snk/ecs/ecs_signal.hpp>
#include <snk/common/snk_api.h>
#include <entt/entity/registry.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scn
{
	// Which worlds a subscription hears. An empty field matches any: {} is every world of every set it covers. The Play
	// twin repeats the level's world names, so name the set together with the world.
	struct event_filter
	{
		std::string set{};
		std::string world{};

		bool matches(std::string_view set_name, std::string_view world_name) const
		{
			return (set.empty() || set == set_name) && (world.empty() || world == world_name);
		}
	};

	// Bus subscriptions that outlive the worlds they hear (WORLD_BUS.md 3.4): one list per layer (and one of the
	// base's). The sets of that layer connect every matching world at its next update, before the delivery, so a world
	// created later (level reload, Play start) hears them too and a subscription made during a delivery fires from
	// the next one. The connections live in each world's ctx and go with it; dropping a subscription silences it in
	// every world at once. Main thread only.
	class SNK_API event_subscriptions
	{
	public:
		using attach_fn = std::function<ecs::connection(ecs::world_bus&, entt::registry&, std::string_view set)>;

		event_subscriptions();
		~event_subscriptions();
		event_subscriptions(const event_subscriptions&) = delete;
		event_subscriptions& operator=(const event_subscriptions&) = delete;

		// Called per event of T in each matching world, in subscription order, during its delivery. The returned
		// connection removes the subscription; a world whose bus does not know T never calls it.
		template <class T>
		[[nodiscard]] ecs::connection subscribe(event_filter filter, std::function<void(ecs::bus_context&, const ecs::envelope<T>&)> fn)
		{
			auto callback = std::make_shared<std::function<void(ecs::bus_context&, const ecs::envelope<T>&)>>(std::move(fn));
			attach_fn attach = [weak = std::weak_ptr(callback)](ecs::world_bus& bus, entt::registry& registry, std::string_view set) {
				if (!bus.template knows<T>()) return ecs::connection{};
				return bus.template subscribe<T>([weak, &registry, set = std::string{ set }, world = std::string{ bus.world() }](const ecs::envelope<T>& e) {
					const auto target = weak.lock();
					if (!target) return;
					ecs::bus_context context{ set, world, registry };
					(*target)(context, e);
				});
			};
			return add(std::move(filter), std::move(attach), std::move(callback));
		}

		// Brings the world up to date: connects every matching subscription it is not connected to (also one skipped
		// before because the world's bus did not know its type yet) and drops the connections of removed ones. Does
		// nothing when neither the list nor the world's bound types changed since the last call.
		void attach(std::string_view set, std::string_view world, entt::registry& registry, ecs::world_bus& bus);

		size_t size() const;
		// Subscriptions connected in the world (tests: removed ones do not pile up).
		static size_t connected_in(const entt::registry& registry);

	private:
		struct entry
		{
			uint64_t id = 0;
			event_filter filter;
			attach_fn attach;
			std::shared_ptr<void> callback;
		};
		struct state
		{
			std::vector<entry> entries;
			uint64_t next_id = 1;
			// Moves on every add and remove.
			uint64_t generation = 0;
		};

		ecs::connection add(event_filter filter, attach_fn attach, std::shared_ptr<void> callback);

		std::shared_ptr<state> m_state;
	};
}
