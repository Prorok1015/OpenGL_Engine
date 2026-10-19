#include <snk/scene/level/scn_set_channel.h>

scn::set_channel::set_channel(std::string set_name, std::shared_ptr<ecs::channel_registry> types)
	: m_name(std::move(set_name))
	, m_types(types ? std::move(types) : std::make_shared<ecs::channel_registry>())
	, m_external_writer(&m_board, ecs::blackboard_data::external_writer)
{
}

scn::set_channel::~set_channel() = default;

void scn::set_channel::attach(uint32_t id, std::string_view name, entt::registry& registry)
{
	registry.ctx().insert_or_assign(ecs::blackboard_reader{ &m_board });
	registry.ctx().insert_or_assign(ecs::blackboard_writer{ &m_board, static_cast<int64_t>(id) });
	for (size_t i = 0; i < m_types->size(); ++i) {
		m_types->type(i).bind(registry, name, &m_limit);
	}
}

void scn::set_channel::ensure_slots()
{
	while (m_slots.size() < m_types->size()) {
		m_slots.push_back(m_types->type(m_slots.size()).make_slot());
	}
}

void scn::set_channel::bind_missing(std::span<const ecs::channel_participant> contexts)
{
	// a type registered after the context was created
	for (const auto& c : contexts) {
		for (size_t i = 0; i < m_types->size(); ++i) {
			m_types->type(i).bind(*c.registry, c.name, &m_limit);
		}
	}
}

void scn::set_channel::begin_frame(std::span<const ecs::channel_participant> contexts)
{
	ensure_slots();
	bind_missing(contexts);
	m_board.commit();
	for (auto& slot : m_slots) {
		slot->exchange(contexts, m_limit, m_name);
	}
	++m_exchanges;
}

void scn::set_channel::skip_frame(std::span<const ecs::channel_participant> contexts)
{
	ensure_slots();
	bind_missing(contexts);
	for (auto& slot : m_slots) {
		slot->consume(contexts);
	}
}

void scn::set_channel::clear()
{
	m_board.clear();
	for (auto& slot : m_slots) {
		slot->clear();
	}
}
