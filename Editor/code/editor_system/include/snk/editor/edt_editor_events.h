#pragma once
#include "edt_document.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace edt
{
	// What is selected in the active document. Nodes are addressed by stable ids, never by pointers into
	// the desc, so the value stays meaningful across edits (see selection_service).
	struct selection
	{
		document_id document = 0;
		std::string world;
		// Node ids from the world root. A node inside a prefab instance: the instance's path, then its ids.
		std::vector<std::uint64_t> path;
		// For such a node: the ids below the instance (the tail of `path`); edits become override records.
		std::vector<std::uint64_t> target;
		bool readonly = false;

		bool empty() const { return path.empty(); }
	};

	// Where a job (job_service) is. `running` also covers queued and finished-but-not-delivered.
	enum class job_status : std::uint8_t { running, done, failed, cancelled };

	inline const char* job_status_name(job_status status)
	{
		switch (status) {
		case job_status::running: return "running";
		case job_status::done: return "done";
		case job_status::failed: return "failed";
		case job_status::cancelled: return "cancelled";
		}
		return "running";
	}

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

		// Made by signal::subscribe; `disconnect` runs once, on disconnect() or destruction.
		explicit connection(std::function<void()> disconnect)
			: m_disconnect(std::move(disconnect))
		{
		}

	private:
		std::function<void()> m_disconnect;
	};

	// Main thread only: there is no locking, and slots run on the emitting thread.
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

	// Editor-wide notifications, reached through editor_context::events(). Producers: selection_service
	// (selection_changed), document_manager (document_opened/closed/activated), scene_document::commit (document_changed),
	// world_controller wiring (active_world_changed), job_service (job_started, job_progress, job_finished),
	// problem_list (problems_changed).
	class editor_events
	{
	public:
		signal<document_id> document_opened;
		signal<document_id> document_closed;
		signal<document_id> document_activated;
		signal<document_id, std::uint64_t> document_changed; // (document, generation)
		signal<document_id> active_world_changed;
		signal<const selection&> selection_changed;
		signal<> level_reloaded;
		// Per job, in this order: job_started once, job_progress whenever the fraction or the note changed (at most
		// once per job_service::pump(), i.e. per frame), job_finished once. A job cancelled before it ran finishes
		// with `cancelled` and no progress.
		signal<std::uint64_t, const std::string&> job_started; // (id, label)
		signal<std::uint64_t, float, const std::string&> job_progress; // (id, fraction in [0, 1], note)
		signal<std::uint64_t, job_status, const std::string&> job_finished; // (id, done | failed | cancelled, error text)
		// The problems of one source were replaced or cleared (problem_list).
		signal<const std::string&> problems_changed; // (source)
	};
}
