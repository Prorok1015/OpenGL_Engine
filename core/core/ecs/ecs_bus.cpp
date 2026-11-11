#include <snk/ecs/ecs_bus.hpp>
#include <algorithm>
#include <format>
#include <limits>

ecs::detail::bus_journal_source::~bus_journal_source() = default;

ecs::bus_journal::bus_journal(size_t capacity) : m_capacity(std::max<size_t>(capacity, 1)) {}

void ecs::bus_journal::record(std::string_view type, uint64_t delivery, uint32_t seq, std::optional<std::string_view> from, std::string payload)
{
	if (payload.size() > max_payload_bytes) {
		payload = std::format(R"({{"truncated":true,"bytes":{}}})", payload.size());
	}
	if (m_entries.size() >= m_capacity) m_entries.pop_front();
	bus_journal_entry entry;
	entry.type = std::string{ type };
	entry.delivery = delivery;
	entry.seq = seq;
	if (from) entry.from = std::string{ *from };
	entry.payload = std::move(payload);
	m_entries.push_back(std::move(entry));
}

ecs::world_bus::world_bus() : m_state(std::make_unique<detail::bus_state>()) {}
ecs::world_bus::~world_bus() = default;
ecs::world_bus::world_bus(world_bus&&) noexcept = default;
ecs::world_bus& ecs::world_bus::operator=(world_bus&&) noexcept = default;

ecs::detail::bus_state& ecs::world_bus::state() const
{
	ASSERT_MSG(m_state, "world_bus used after it was moved from");
	return *m_state;
}

bool ecs::world_bus::bind(const bus_registry& types, entt::registry& registry, std::string_view world)
{
	auto& st = state();
	ASSERT_MSG(st.on_owner_thread(), "world_bus::bind of world '{}': main thread only", world);
	ASSERT_MSG(m_slots.empty() || st.world == world, "world_bus of '{}' is bound again as '{}'", st.world, world);
	const bool same_types = ENGINE_VERIFY(types.size() >= m_slots.size(), "world_bus of '{}': bound to {} event types, the registry now has {}", st.world, m_slots.size(), types.size());
	if (!same_types) return false;
	for (size_t i = 0; i < m_slots.size(); ++i) {
		const bool same = ENGINE_VERIFY(m_slots[i]->hash() == types.type(i).hash(), "world_bus of '{}': event type {} is {} in the registry, not the type bound before", st.world, i, types.type(i).name());
		if (!same) return false;
	}

	st.world = std::string{ world };
	m_types = &types;
	m_registry = &registry;
	for (size_t i = m_slots.size(); i < types.size(); ++i) {
		const auto& type = types.type(i);
		auto slot = type.make_slot(&st);
		type.bind(registry, *slot);
		m_slots.push_back(std::move(slot));
	}
	return true;
}

void ecs::world_bus::bind_new()
{
	if (m_types && m_registry && m_slots.size() < m_types->size()) bind(*m_types, *m_registry, state().world);
}

void ecs::world_bus::set_mode(bus_mode mode)
{
	auto& st = state();
	ASSERT_MSG(!st.delivering, "world_bus::set_mode during a delivery of world '{}'", st.world);
	if (st.delivering || st.mode == mode) return;
	st.mode = mode;
	if (mode == bus_mode::discard) clear();
}

ecs::bus_mode ecs::world_bus::mode() const { return state().mode; }

void ecs::world_bus::set_event_limit(size_t limit)
{
	auto& st = state();
	if (!ENGINE_VERIFY(limit > 0, "world_bus of '{}': an event limit of 0 is refused", st.world)) return;
	st.limit = std::min<size_t>(limit, std::numeric_limits<uint32_t>::max());
}

size_t ecs::world_bus::event_limit() const { return state().limit; }

const std::string& ecs::world_bus::world() const { return state().world; }
uint64_t ecs::world_bus::deliveries() const { return state().deliveries; }
size_t ecs::world_bus::bound_types() const { return m_slots.size(); }
void ecs::world_bus::set_after_delivery(bool after) { state().after_delivery = after; }
bool ecs::world_bus::delivering() const { return state().delivering; }

size_t ecs::world_bus::queued() const
{
	size_t total = 0;
	for (const auto& slot : m_slots) total += slot->queued();
	return total;
}

void ecs::world_bus::deliver()
{
	auto& st = state();
	ASSERT_MSG(st.on_owner_thread(), "world_bus::deliver of world '{}': main thread only", st.world);
	ASSERT_MSG(!st.delivering, "world_bus::deliver of world '{}' re-entered from a subscriber", st.world);
	if (st.delivering || st.mode == bus_mode::discard) return;

	++st.deliveries;
	// Every window is filled before any subscriber runs, so a subscriber of one type reads the others' new windows.
	for (const auto& slot : m_slots) slot->fill_window();
	if (st.journal) record_journal(*st.journal);
	for (const auto& slot : m_slots) slot->freeze_subscribers();

	st.delivering = true;
	struct delivering_reset
	{
		detail::bus_state& state;
		~delivering_reset() { state.delivering = false; }
	} reset{ st };
	for (const auto& slot : m_slots) slot->notify_subscribers();
}

size_t ecs::world_bus::clear()
{
	auto& st = state();
	ASSERT_MSG(st.on_owner_thread(), "world_bus::clear of world '{}': main thread only", st.world);
	ASSERT_MSG(!st.delivering, "world_bus::clear during a delivery of world '{}'", st.world);
	if (st.delivering) return 0;
	size_t dropped = 0;
	for (const auto& slot : m_slots) {
		dropped += slot->queued() + slot->window_size();
		slot->clear();
	}
	if (st.journal) st.journal->clear();
	return dropped;
}

void ecs::world_bus::enable_journal(size_t capacity)
{
	state().journal = std::make_unique<bus_journal>(capacity);
}

void ecs::world_bus::disable_journal()
{
	state().journal.reset();
}

const ecs::bus_journal* ecs::world_bus::journal() const
{
	return state().journal.get();
}

void ecs::world_bus::record_journal(bus_journal& journal) const
{
	const uint64_t delivery = state().deliveries;
	for (size_t i = 0; i < m_slots.size() && m_types && i < m_types->size(); ++i) {
		if (m_slots[i]->window_size() == 0) continue;
		const detail::bus_type& type = m_types->type(i);
		if (const auto* source = dynamic_cast<const detail::bus_journal_source*>(&type)) {
			try {
				source->record(*m_slots[i], delivery, journal);
			} catch (...) {
				egLOG_ERROR("scene", "world '{}': the journal could not record the events of {}", state().world, type.name());
			}
			continue;
		}
		// A module built before the journal: only where its events stand.
		for (size_t seq = 0; seq < m_slots[i]->window_size(); ++seq) {
			journal.record(type.name(), delivery, static_cast<uint32_t>(seq), std::nullopt, {});
		}
	}
}
