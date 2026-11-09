#include <snk/scene/scn_event_subscriptions.h>
#include <snk/common/engine_assert.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <algorithm>
#include <map>

namespace {
	// Kept in a world's ctx: its connection per subscription id, and what it was last brought up to date with.
	// Move-only on purpose: entt's ctx copies a type it believes copyable, and a map of connections only looks so.
	struct world_subscriptions
	{
		world_subscriptions() = default;
		world_subscriptions(const world_subscriptions&) = delete;
		world_subscriptions& operator=(const world_subscriptions&) = delete;
		world_subscriptions(world_subscriptions&&) noexcept = default;
		world_subscriptions& operator=(world_subscriptions&&) noexcept = default;

		uint64_t generation = 0;
		size_t bound_types = 0;
		bool synced = false;
		std::map<uint64_t, ecs::connection> connections;
	};

	// The observer slots of a world, append-only: entt holds pointers to them until the registry goes.
	struct world_observer_slots
	{
		world_observer_slots() = default;
		world_observer_slots(const world_observer_slots&) = delete;
		world_observer_slots& operator=(const world_observer_slots&) = delete;
		world_observer_slots(world_observer_slots&&) noexcept = default;
		world_observer_slots& operator=(world_observer_slots&&) noexcept = default;

		std::vector<std::shared_ptr<scn::observer_slot>> slots;
	};
}

scn::event_subscriptions::event_subscriptions() : m_state(std::make_shared<state>()) {}
scn::event_subscriptions::~event_subscriptions() = default;

ecs::connection scn::event_subscriptions::add(event_filter filter, attach_fn attach, std::shared_ptr<void> callback)
{
	ASSERT_MSG(!ds::task_system::on_worker(), "event_subscriptions::subscribe: main thread only");
	const uint64_t id = m_state->next_id++;
	m_state->entries.push_back(entry{ id, std::move(filter), std::move(attach), std::move(callback) });
	++m_state->generation;
	return ecs::connection([weak = std::weak_ptr<state>(m_state), id] {
		const auto st = weak.lock();
		if (!st) return;
		if (std::erase_if(st->entries, [id](const entry& e) { return e.id == id; }) > 0) ++st->generation;
	});
}

void scn::event_subscriptions::attach(std::string_view set, std::string_view world, entt::registry& registry, ecs::world_bus& bus)
{
	auto& links = registry.ctx().emplace<world_subscriptions>();
	if (links.synced && links.generation == m_state->generation && links.bound_types == bus.bound_types()) return;

	std::erase_if(links.connections, [this](const auto& link) {
		return std::ranges::none_of(m_state->entries, [&](const entry& e) { return e.id == link.first; });
	});
	// attach_fn adds a world_bus slot, never an entry: iterating by index stays valid.
	for (size_t i = 0; i < m_state->entries.size(); ++i) {
		const entry& e = m_state->entries[i];
		if (links.connections.contains(e.id) || !e.filter.matches(set, world)) continue;
		if (auto c = e.attach(bus, registry, set); c.connected()) links.connections.emplace(e.id, std::move(c));
	}
	links.generation = m_state->generation;
	// knows<T>() may have bound new types during the loop.
	links.bound_types = bus.bound_types();
	links.synced = true;
}

std::shared_ptr<scn::observer_slot> scn::event_subscriptions::keep_slot(entt::registry& registry, observer_slot slot)
{
	auto& kept = registry.ctx().emplace<world_observer_slots>();
	return kept.slots.emplace_back(std::make_shared<observer_slot>(std::move(slot)));
}

void scn::event_subscriptions::notify_observer(observer_slot& slot, entt::registry& registry, entt::entity entity)
{
	if (const auto muted = slot.muted.lock(); !muted || *muted > 0) return;
	const auto fn = slot.fn.lock();
	if (!fn) return;
	ecs::bus_context context{ slot.set, slot.world, registry };
	(*fn)(context, entity);
}

ecs::connection scn::event_subscriptions::observer_connection(std::shared_ptr<observer_slot> slot)
{
	return ecs::connection([slot = std::move(slot)] { slot->fn.reset(); });
}

std::weak_ptr<int> scn::event_subscriptions::mute_token() const
{
	return std::shared_ptr<int>(m_state, &m_state->muted);
}

bool scn::event_subscriptions::observers_muted() const
{
	return m_state->muted > 0;
}

size_t scn::event_subscriptions::size() const
{
	return m_state->entries.size();
}

size_t scn::event_subscriptions::connected_in(const entt::registry& registry)
{
	const auto* links = registry.ctx().find<world_subscriptions>();
	return links ? links->connections.size() : 0;
}
