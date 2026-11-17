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

	// Which signal of a component's storage an observer hears.
	enum class observed : std::uint8_t
	{
		construct,
		update,
		destroy,
	};

	// One observer slot of a world, kept in its ctx while the world lives. entt's thunk for it and the control block
	// its `fn` points to are made where observe<T> is instantiated, which is a module's binary for a module's observer:
	// that is safe only because a layer's worlds go before the layer and the module (context_manager::destroy_layer
	// asserts it).
	struct observer_slot
	{
		std::weak_ptr<std::function<void(ecs::bus_context&, entt::entity)>> fn;
		std::weak_ptr<int> muted;
		std::string set;
		std::string world;
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

		// Called on `what` of T in each matching world (entt's on_construct / on_update / on_destroy). It is connected to a
		// world when the world is created, so it sees the entities a level or Play spawns; one observed after a world
		// exists is connected to it at that world's next update. Like a bus subscriber it hears nothing while the world's
		// set is in Edit (its bus discards), so a level load or an editor edit does not reach it. The registry is
		// read-only to `fn`: it changes the world through bus_context::changer()/spawner(), applied after the current
		// phase. Silent while mute_observers() holds; the returned connection removes it.
		template <class T>
		[[nodiscard]] ecs::connection observe(event_filter filter, observed what, std::function<void(ecs::bus_context&, entt::entity)> fn)
		{
			auto callback = std::make_shared<std::function<void(ecs::bus_context&, entt::entity)>>(std::move(fn));
			attach_fn attach = [what, weak = std::weak_ptr(callback), muted = mute_token()](ecs::world_bus& bus,
				entt::registry& registry, std::string_view set) {
				std::shared_ptr<observer_slot> slot = keep_slot(registry, observer_slot{ weak, muted, std::string{ set }, std::string{ bus.world() } });
				switch (what) {
				case observed::construct: registry.on_construct<T>().template connect<&notify_observer>(*slot); break;
				case observed::update: registry.on_update<T>().template connect<&notify_observer>(*slot); break;
				case observed::destroy: registry.on_destroy<T>().template connect<&notify_observer>(*slot); break;
				}
				return observer_connection(std::move(slot));
			};
			return add(std::move(filter), std::move(attach), std::move(callback));
		}

		// While the returned guard lives, no observer of this list is called (a restore puts back entities that already
		// existed: HOT_RELOAD_PLAY.md 6, step 4). Guards nest.
		class mute_guard
		{
		public:
			mute_guard() = default;
			explicit mute_guard(std::weak_ptr<int> muted)
				: m_muted(std::move(muted))
			{
				if (const auto depth = m_muted.lock()) ++*depth;
			}
			~mute_guard() { release(); }
			mute_guard(const mute_guard&) = delete;
			mute_guard& operator=(const mute_guard&) = delete;
			mute_guard(mute_guard&& other) noexcept
				: m_muted(std::exchange(other.m_muted, {}))
			{
			}
			mute_guard& operator=(mute_guard&& other) noexcept
			{
				if (this != &other) {
					release();
					m_muted = std::exchange(other.m_muted, {});
				}
				return *this;
			}
			// Unmutes now (once); a default or moved-from guard holds nothing.
			void release()
			{
				if (const auto depth = std::exchange(m_muted, {}).lock()) --*depth;
			}

		private:
			std::weak_ptr<int> m_muted;
		};
		[[nodiscard]] mute_guard mute_observers() { return mute_guard{ mute_token() }; }
		bool observers_muted() const;

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
			// Depth of mute_observers(); last, so the fields above keep their place (module API 3.4).
			int muted = 0;
		};

		ecs::connection add(event_filter filter, attach_fn attach, std::shared_ptr<void> callback);

		// The world's ctx owns the slot: entt keeps a pointer to it until the registry goes, and is never asked to
		// release it (a world's storages may already be gone when its ctx is destroyed). So the slots of a world only
		// grow: one per observer connected to it, a removed one silent, until the world goes.
		static std::shared_ptr<observer_slot> keep_slot(entt::registry& registry, observer_slot slot);
		// What every observer slot calls: checks the mute, the world's run mode and the callback, then calls it.
		static void notify_observer(observer_slot& slot, entt::registry& registry, entt::entity entity);
		// Disconnecting silences the slot; it stays connected and does nothing.
		static ecs::connection observer_connection(std::shared_ptr<observer_slot> slot);

		// Out of line, so a module never reads the layout of `state`.
		std::weak_ptr<int> mute_token() const;

		std::shared_ptr<state> m_state;
	};
}
