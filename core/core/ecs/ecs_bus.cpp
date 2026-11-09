#include <snk/ecs/ecs_bus.hpp>

ecs::world_bus::world_bus() : m_state(std::make_unique<detail::bus_state>()) {}
ecs::world_bus::~world_bus() = default;
ecs::world_bus::world_bus(world_bus&&) noexcept = default;
ecs::world_bus& ecs::world_bus::operator=(world_bus&&) noexcept = default;

void ecs::world_bus::bind(const bus_registry& types, entt::registry& registry, std::string_view world)
{
	ASSERT_MSG(m_slots.empty() || m_state->world == world, "world_bus of '{}' is bound again as '{}'", m_state->world, world);
	m_state->world = std::string{ world };
	for (size_t i = m_slots.size(); i < types.size(); ++i) {
		const auto& type = types.type(i);
		auto slot = type.make_slot(m_state.get());
		type.bind(registry, *slot);
		m_slots.push_back(std::move(slot));
	}
}

void ecs::world_bus::set_mode(bus_mode mode)
{
	ASSERT_MSG(!m_state->delivering, "world_bus::set_mode during a delivery of world '{}'", m_state->world);
	if (m_state->mode == mode) return;
	m_state->mode = mode;
	if (mode == bus_mode::discard) clear();
}

ecs::bus_mode ecs::world_bus::mode() const { return m_state->mode; }

void ecs::world_bus::set_event_limit(size_t limit) { m_state->limit = limit; }
size_t ecs::world_bus::event_limit() const { return m_state->limit; }

const std::string& ecs::world_bus::world() const { return m_state->world; }
uint64_t ecs::world_bus::deliveries() const { return m_state->deliveries; }
bool ecs::world_bus::delivering() const { return m_state->delivering; }

size_t ecs::world_bus::queued() const
{
	size_t total = 0;
	for (const auto& slot : m_slots) total += slot->queued();
	return total;
}

void ecs::world_bus::deliver()
{
	ASSERT_MSG(!m_state->delivering, "world_bus::deliver of world '{}' re-entered from a subscriber", m_state->world);
	if (m_state->delivering || m_state->mode == bus_mode::discard) return;

	++m_state->deliveries;
	// Every window is filled before any subscriber runs, so a subscriber of one type reads the others' new windows.
	for (const auto& slot : m_slots) slot->fill_window();

	m_state->delivering = true;
	struct delivering_reset
	{
		detail::bus_state& state;
		~delivering_reset() { state.delivering = false; }
	} reset{ *m_state };
	for (const auto& slot : m_slots) slot->notify_subscribers();
}

size_t ecs::world_bus::clear()
{
	ASSERT_MSG(!m_state->delivering, "world_bus::clear during a delivery of world '{}'", m_state->world);
	size_t dropped = 0;
	for (const auto& slot : m_slots) {
		dropped += slot->queued() + slot->window_size();
		slot->clear();
	}
	return dropped;
}
