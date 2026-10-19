#pragma once
#include <snk/ecs/ecs_channel.hpp>
#include <snk/common/engine_assert.h>
#include <snk/common/snk_api.h>
#include <entt/entity/registry.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scn
{
	// The data channel of one context_set (WORLD_MESSAGING 3): blackboard and mailbox with a lag of
	// exactly one frame. A set never sees the channel of another set. The context_set drives it:
	// `begin_frame` once at the start of the set's frame, before the contexts' own begin_frame.
	// Persistent containers only (nothing here lives in the frame allocator).
	class SNK_API set_channel
	{
	public:
		// Messages of one type that can be sent (and delivered) in one frame; the rest is dropped
		// with a warning. Stops a ping-pong between contexts from growing without bound.
		static constexpr size_t default_message_limit = 1024;

		// `types` is shared by the sets of a manager; null makes the channel use a registry of its own.
		explicit set_channel(std::string set_name, std::shared_ptr<ecs::channel_registry> types = nullptr);
		~set_channel();
		set_channel(const set_channel&) = delete;
		set_channel& operator=(const set_channel&) = delete;
		set_channel(set_channel&&) = delete;
		set_channel& operator=(set_channel&&) = delete;

		ecs::channel_registry& types() { return *m_types; }
		const ecs::channel_registry& types() const { return *m_types; }

		void set_message_limit(size_t limit) { m_limit = limit; }
		size_t message_limit() const { return m_limit; }

		// Puts the endpoints into the context's registry ctx: blackboard_reader, blackboard_writer
		// (writer id = context id) and an inbox/outbox for every registered message type. Safe to
		// call again (types registered later are added).
		void attach(uint32_t id, std::string_view name, entt::registry& registry);

		// The exchange: the back blackboard becomes visible and the outboxes of the previous frame
		// are delivered to the inboxes, in the order external posts, contexts by ascending id,
		// then send order. `contexts` must be in ascending id order.
		void begin_frame(std::span<const ecs::channel_participant> contexts);

		// A frame without an exchange (Edit/Pause): nothing becomes visible and nothing is
		// delivered; the inboxes are emptied so a message is never seen twice.
		void skip_frame(std::span<const ecs::channel_participant> contexts);

		// Posts from code outside the phases (editor, shell). `from` is empty; delivered before
		// the messages of the contexts.
		template <class T>
		void post_external(ecs::address to, T payload)
		{
			const auto index = m_types->template index_of<T>();
			ASSERT_MSG(index, "set '{}': message type is not registered in the channel_registry", m_name);
			if (!index) return;
			slot_for<T>(*index).post(std::move(to), std::move(payload), m_limit, m_name);
		}

		// Writes of code outside the phases; they lose against any context writing the same key.
		ecs::blackboard_writer& external_blackboard() { return m_external_writer; }
		// The committed blackboard (state of the last exchange), sorted by key.
		const ecs::blackboard_data& blackboard() const { return m_board; }
		ecs::blackboard_reader reader() const { return ecs::blackboard_reader{ &m_board }; }

		// Exchanges done so far (a frozen frame is not one).
		uint64_t exchange_count() const { return m_exchanges; }

		// Forgets the blackboard and every undelivered external post (a level load, a set clear).
		// Endpoints in live registries keep their pending messages; contexts are normally gone.
		void clear();

	private:
		template <class T>
		ecs::detail::channel_slot_impl<T>& slot_for(size_t index)
		{
			ensure_slots();
			return static_cast<ecs::detail::channel_slot_impl<T>&>(*m_slots[index]);
		}

		void ensure_slots();
		void bind_missing(std::span<const ecs::channel_participant> contexts);

		std::string m_name;
		std::shared_ptr<ecs::channel_registry> m_types;
		size_t m_limit = default_message_limit;
		uint64_t m_exchanges = 0;
		ecs::blackboard_data m_board;
		ecs::blackboard_writer m_external_writer;
		// Indexed like the registry's types.
		std::vector<std::unique_ptr<ecs::detail::channel_slot>> m_slots;
	};
}
