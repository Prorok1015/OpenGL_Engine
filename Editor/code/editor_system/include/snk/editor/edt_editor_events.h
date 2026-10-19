#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace edt
{
	// Placeholder until the document model (#137) introduces a real type.
	using document_id = std::uint64_t;

	template<class... Args> class signal;

	namespace detail
	{
		template<class... Args>
		struct slot
		{
			std::uint64_t id = 0;
			bool alive = true;
			std::function<void(Args...)> fn;
		};

		template<class... Args>
		struct signal_state
		{
			std::vector<std::shared_ptr<slot<Args...>>> slots;
			std::uint64_t next_id = 1;
		};
	}

	// Move-only RAII subscription. Unsubscribes in the destructor; stays harmless if the signal dies first.
	class connection
	{
	public:
		connection() = default;
		connection(const connection&) = delete;
		connection& operator=(const connection&) = delete;
		connection(connection&& other) noexcept
			: m_disconnect(std::exchange(other.m_disconnect, nullptr))
		{
		}
		connection& operator=(connection&& other) noexcept
		{
			if (this != &other) {
				disconnect();
				m_disconnect = std::exchange(other.m_disconnect, nullptr);
			}
			return *this;
		}
		~connection() { disconnect(); }

		bool connected() const { return m_disconnect != nullptr; }

		void disconnect()
		{
			if (auto fn = std::exchange(m_disconnect, nullptr)) {
				fn();
			}
		}

	private:
		template<class... Args> friend class signal;
		explicit connection(std::function<void()> disconnect)
			: m_disconnect(std::move(disconnect))
		{
		}

		std::function<void()> m_disconnect;
	};

	// Slots run in subscription order. Subscribing or unsubscribing from inside a slot is safe: a slot
	// removed during an emit is not called afterwards, one added during an emit first runs on the next emit.
	template<class... Args>
	class signal
	{
	public:
		[[nodiscard]] connection subscribe(std::function<void(Args...)> fn)
		{
			auto entry = std::make_shared<detail::slot<Args...>>();
			entry->id = m_state->next_id++;
			entry->fn = std::move(fn);
			m_state->slots.push_back(entry);
			return connection([weak = std::weak_ptr<detail::signal_state<Args...>>(m_state), id = entry->id]() {
				if (auto state = weak.lock()) {
					auto& slots = state->slots;
					auto it = std::find_if(slots.begin(), slots.end(), [id](const auto& s) { return s->id == id; });
					if (it != slots.end()) {
						(*it)->alive = false;
						slots.erase(it);
					}
				}
			});
		}

		void emit(Args... args) const
		{
			// A snapshot keeps iteration valid when a slot (un)subscribes.
			const auto snapshot = m_state->slots;
			for (const auto& s : snapshot) {
				if (s->alive) {
					s->fn(args...);
				}
			}
		}

		std::size_t subscriber_count() const { return m_state->slots.size(); }

	private:
		std::shared_ptr<detail::signal_state<Args...>> m_state = std::make_shared<detail::signal_state<Args...>>();
	};

	// Editor-wide notifications, reached through editor_context::events(). Nothing emits them yet: the
	// producers arrive with selection_service and the document model.
	class editor_events
	{
	public:
		signal<document_id> document_opened;
		signal<document_id> document_closed;
		signal<document_id> document_activated;
		signal<document_id, std::uint64_t> document_changed; // (document, generation)
		signal<document_id> active_world_changed;
		signal<> selection_changed;
		signal<> level_reloaded;
	};
}
