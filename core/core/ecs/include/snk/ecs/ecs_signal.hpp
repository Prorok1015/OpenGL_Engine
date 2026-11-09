#pragma once
#include <snk/common/engine_assert.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

// RAII subscriptions (WORLD_BUS.md 3.4), the core twin of edt::signal. One thread owns a signal: the one that made
// it (the main thread for the world bus); subscribe, emit and disconnect assert they run there.
namespace ecs {

namespace detail {

template <class... Args>
struct signal_slot
{
	std::uint64_t id = 0;
	bool alive = true;
	std::function<void(Args...)> fn;
};

template <class... Args>
struct signal_state
{
	std::vector<std::shared_ptr<signal_slot<Args...>>> slots;
	std::uint64_t next_id = 1;
	std::thread::id owner = std::this_thread::get_id();

	bool on_owner_thread() const { return std::this_thread::get_id() == owner; }
};

} // namespace detail

// Move-only. Unsubscribes in the destructor; harmless when the signal is already gone.
class connection
{
public:
	connection() = default;
	explicit connection(std::function<void()> disconnect)
		: m_disconnect(std::move(disconnect))
	{
	}
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
		if (auto fn = std::exchange(m_disconnect, nullptr)) fn();
	}

private:
	std::function<void()> m_disconnect;
};

// Slots run in subscription order. A slot removed during an emit is not called afterwards; one added during an
// emit first runs on the next emit. The list is copied for the length of one emit only.
template <class... Args>
class signal
{
public:
	[[nodiscard]] connection subscribe(std::function<void(Args...)> fn)
	{
		ASSERT_MSG(m_state->on_owner_thread(), "ecs::signal::subscribe: owner thread only");
		auto entry = std::make_shared<detail::signal_slot<Args...>>();
		entry->id = m_state->next_id++;
		entry->fn = std::move(fn);
		m_state->slots.push_back(entry);
		return connection([weak = std::weak_ptr<detail::signal_state<Args...>>(m_state), id = entry->id]() {
			const auto state = weak.lock();
			if (!state) return;
			ASSERT_MSG(state->on_owner_thread(), "ecs::connection::disconnect: owner thread of the signal only");
			auto& slots = state->slots;
			const auto it = std::ranges::find_if(slots, [id](const auto& s) { return s->id == id; });
			if (it == slots.end()) return;
			(*it)->alive = false;
			slots.erase(it);
		});
	}

	void emit(Args... args) const
	{
		ASSERT_MSG(m_state->on_owner_thread(), "ecs::signal::emit: owner thread only");
		const auto snapshot = m_state->slots;
		for (const auto& s : snapshot) {
			if (s->alive) s->fn(args...);
		}
	}

	std::size_t subscriber_count() const { return m_state->slots.size(); }

private:
	std::shared_ptr<detail::signal_state<Args...>> m_state = std::make_shared<detail::signal_state<Args...>>();
};

} // namespace ecs
