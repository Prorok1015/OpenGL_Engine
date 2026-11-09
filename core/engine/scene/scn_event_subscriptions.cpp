#include <snk/scene/scn_event_subscriptions.h>
#include <snk/common/engine_assert.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <algorithm>

namespace {
	// Kept in a world's ctx: what it has been offered and the connections that make it heard.
	struct world_subscriptions
	{
		uint64_t offered_up_to = 0;
		std::vector<ecs::connection> connections;
	};
}

scn::event_subscriptions::event_subscriptions() : m_state(std::make_shared<state>()) {}
scn::event_subscriptions::~event_subscriptions() = default;

ecs::connection scn::event_subscriptions::add(event_filter filter, attach_fn attach, std::shared_ptr<void> callback)
{
	ASSERT_MSG(!ds::task_system::on_worker(), "event_subscriptions::subscribe: main thread only");
	const uint64_t id = m_state->next_id++;
	m_state->entries.push_back(entry{ id, std::move(filter), std::move(attach), std::move(callback) });
	return ecs::connection([weak = std::weak_ptr<state>(m_state), id] {
		const auto st = weak.lock();
		if (!st) return;
		std::erase_if(st->entries, [id](const entry& e) { return e.id == id; });
	});
}

void scn::event_subscriptions::attach(std::string_view set, std::string_view world, entt::registry& registry, ecs::world_bus& bus)
{
	auto& links = registry.ctx().emplace<world_subscriptions>();
	if (links.offered_up_to + 1 >= m_state->next_id) return;
	// Each attach may add a subscription (a world_bus slot), never an entry: iterating by index stays valid.
	for (size_t i = 0; i < m_state->entries.size(); ++i) {
		const entry& e = m_state->entries[i];
		if (e.id <= links.offered_up_to || !e.filter.matches(set, world)) continue;
		if (auto c = e.attach(bus, registry, set); c.connected()) links.connections.push_back(std::move(c));
	}
	links.offered_up_to = m_state->next_id - 1;
}

size_t scn::event_subscriptions::size() const
{
	return m_state->entries.size();
}
