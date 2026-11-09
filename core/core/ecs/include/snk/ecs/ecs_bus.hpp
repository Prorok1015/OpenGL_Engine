#pragma once
#include <snk/ecs/ecs_entt_abi.h>
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_signal.hpp>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/snk_api.h>
#include <entt/entt.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The event bus of one world (WORLD_BUS.md 3). A system sees it as ordinary ctx resources of its registry:
//   ecs::event_writer<T>&, const ecs::event_reader<T>&
// Events wait in a queue until world_bus::deliver(), which the world calls once per frame right after the builtin
// `update` phase. A delivery replaces every reader's window and then calls the subscribers. Between worlds use the
// set's mailbox (ecs_channel.hpp); ecs::event<T> (entities only) is a different thing.
namespace ecs {

inline constexpr size_t default_event_limit = 1024;

// queue: Play, Step and Pause (the world calls deliver() when it runs a frame).
// discard: Edit. Emits and posts are dropped without a warning, windows are empty, deliver() does nothing.
enum class bus_mode : uint8_t
{
	queue,
	discard,
};

namespace detail {

struct bus_state
{
	std::string world;
	bus_mode mode = bus_mode::queue;
	size_t limit = default_event_limit;
	uint64_t deliveries = 0;
	bool delivering = false;
	// Set by the world between its delivery and the end of its frame: an emit then waits a frame.
	bool after_delivery = false;
	std::thread::id owner = std::this_thread::get_id();

	bool on_owner_thread() const { return std::this_thread::get_id() == owner; }
};

class bus_slot
{
public:
	virtual ~bus_slot() = default;
	virtual entt::id_type hash() const = 0;
	// Moves the queue into the window, numbered by seq.
	virtual void fill_window() = 0;
	// Taken for every type before any subscriber runs: a subscription made during a delivery starts with the next.
	virtual void freeze_subscribers() = 0;
	virtual void notify_subscribers() = 0;
	virtual size_t queued() const = 0;
	virtual size_t window_size() const = 0;
	virtual void clear() = 0;
};

template <class T> class bus_slot_impl;
template <class T> struct bus_type_impl;

} // namespace detail

// Unbound (the type is not registered in the bus_registry of the world): emit asserts, like outbox::send.
template <class T>
class event_writer
{
public:
	void emit(T payload);
	bool bound() const { return m_slot != nullptr; }

private:
	friend struct detail::bus_type_impl<T>;
	detail::bus_slot_impl<T>* m_slot = nullptr;
};

// The window: what the last delivery brought. Only the next delivery replaces it (Pause and Step keep it).
template <class T>
class event_reader
{
public:
	std::span<const envelope<T>> events() const;
	// Number of the delivery that filled the window, 0 before the first. A system that runs while no delivery
	// happens (run_in_edit in Pause) sees the same window every frame and skips it by this number.
	uint64_t delivery() const;
	bool bound() const { return m_slot != nullptr; }

private:
	friend struct detail::bus_type_impl<T>;
	const detail::bus_slot_impl<T>* m_slot = nullptr;
};

namespace detail {

template <class T>
class bus_slot_impl final : public bus_slot
{
public:
	explicit bus_slot_impl(const bus_state* state) : m_state(state) {}

	entt::id_type hash() const override { return entt::type_hash<T>::value(); }

	void push_from_world(T payload)
	{
		ASSERT_MSG(m_state->on_owner_thread(), "event_writer<{}>::emit of world '{}': main thread only", entt::type_name<T>::value(), m_state->world);
		if (m_state->mode == bus_mode::discard) return;
#if SNK_ASSERTS_ENABLED
		if (m_state->after_delivery && !m_state->delivering && !m_late_warned) {
			egLOG_WARN("scene", "world '{}': {} emitted after the delivery point waits a frame for the next delivery (WORLD_BUS 3.2)",
					   m_state->world, entt::type_name<T>::value());
			m_late_warned = true;
		}
#endif
		if (queued() >= m_state->limit) {
			++m_dropped;
			return;
		}
		auto& queue = m_state->delivering ? m_from_subscribers : m_from_systems;
		queue.push_back(std::move(payload));
	}

	// External posts come first in a delivery, so on overflow the latest queued event of the world gives way.
	void post(T payload)
	{
		ASSERT_MSG(m_state->on_owner_thread(), "world_bus::post<{}> to world '{}': main thread only", entt::type_name<T>::value(), m_state->world);
		if (m_state->mode == bus_mode::discard) return;
		if (m_external.size() >= m_state->limit) {
			++m_dropped;
			return;
		}
		if (queued() >= m_state->limit) {
			auto& victim = m_from_systems.empty() ? m_from_subscribers : m_from_systems;
			victim.pop_back();
			++m_dropped;
		}
		m_external.push_back(std::move(payload));
	}

	// Delivery order: external posts, then what subscribers sent during the previous delivery, then the systems.
	void fill_window() override
	{
		m_window.clear();
		m_window.reserve(queued());
		uint32_t seq = 0;
		// The limit may have been lowered after the events were queued.
		auto take = [&](std::vector<T>& queue, std::string_view from) {
			for (auto& payload : queue) {
				if (m_window.size() >= m_state->limit) {
					++m_dropped;
					continue;
				}
				m_window.push_back(envelope<T>{ std::string{ from }, address{ m_state->world }, seq++, std::move(payload) });
			}
			queue.clear();
		};
		take(m_external, {});
		take(m_from_subscribers, m_state->world);
		take(m_from_systems, m_state->world);
		m_window_delivery = m_state->deliveries;
		report_dropped();
	}

	void freeze_subscribers() override
	{
		if (m_subscribers.subscriber_count() > 0) m_frozen = m_subscribers.freeze();
	}

	void notify_subscribers() override
	{
		if (!m_frozen) return;
		const auto frozen = std::move(*m_frozen);
		m_frozen.reset();
		for (const auto& e : m_window) frozen.emit(e);
	}

	size_t queued() const override { return m_external.size() + m_from_subscribers.size() + m_from_systems.size(); }
	size_t window_size() const override { return m_window.size(); }

	void clear() override
	{
		m_external.clear();
		m_from_subscribers.clear();
		m_from_systems.clear();
		m_window.clear();
		m_window_delivery = 0;
		m_dropped = 0;
		m_reported_dropped = 0;
	}

	[[nodiscard]] connection subscribe(std::function<void(const envelope<T>&)> fn) { return m_subscribers.subscribe(std::move(fn)); }

	std::span<const envelope<T>> window() const { return m_window; }
	uint64_t window_delivery() const { return m_window_delivery; }

private:
	// A world that overflows every frame by the same count warns once, not every frame.
	void report_dropped()
	{
		if (m_dropped > 0 && m_dropped != m_reported_dropped) {
			egLOG_WARN("scene", "world '{}': {} events of {} dropped, more than {} per delivery", m_state->world, m_dropped, entt::type_name<T>::value(), m_state->limit);
		}
		m_reported_dropped = m_dropped;
		m_dropped = 0;
	}

	const bus_state* m_state = nullptr;
	std::vector<T> m_external;
	std::vector<T> m_from_subscribers;
	std::vector<T> m_from_systems;
	std::vector<envelope<T>> m_window;
	uint64_t m_window_delivery = 0;
	size_t m_dropped = 0;
	size_t m_reported_dropped = 0;
	// Kept in every build type: a module and the engine may be built with and without asserts.
	bool m_late_warned = false;
	signal<const envelope<T>&> m_subscribers;
	std::optional<typename signal<const envelope<T>&>::frozen> m_frozen;
};

class bus_type
{
public:
	virtual ~bus_type() = default;
	virtual entt::id_type hash() const = 0;
	virtual std::string_view name() const = 0;
	// JSON Schema of the payload for MCP (#605); empty when the type has none.
	virtual std::string_view schema() const = 0;
	virtual std::unique_ptr<bus_slot> make_slot(const bus_state* state) const = 0;
	// Puts the writer and reader of this type into the registry's ctx, bound to the slot.
	virtual void bind(entt::registry& registry, bus_slot& slot) const = 0;
};

template <class T>
struct bus_type_impl final : bus_type
{
	explicit bus_type_impl(std::string schema_text) : m_schema(std::move(schema_text)) {}

	entt::id_type hash() const override { return entt::type_hash<T>::value(); }
	std::string_view name() const override { return entt::type_name<T>::value(); }
	std::string_view schema() const override { return m_schema; }

	std::unique_ptr<bus_slot> make_slot(const bus_state* state) const override { return std::make_unique<bus_slot_impl<T>>(state); }

	void bind(entt::registry& registry, bus_slot& slot) const override
	{
		auto& typed = static_cast<bus_slot_impl<T>&>(slot);
		registry.ctx().template emplace<event_writer<T>>().m_slot = &typed;
		registry.ctx().template emplace<event_reader<T>>().m_slot = &typed;
	}

private:
	std::string m_schema;
};

} // namespace detail

template <class T>
void event_writer<T>::emit(T payload)
{
	ASSERT_MSG(m_slot, "event_writer<{}>: event type is not registered in the bus_registry of the world", entt::type_name<T>::value());
	if (!m_slot) return;
	m_slot->push_from_world(std::move(payload));
}

template <class T>
std::span<const envelope<T>> event_reader<T>::events() const
{
	return m_slot ? m_slot->window() : std::span<const envelope<T>>{};
}

template <class T>
uint64_t event_reader<T>::delivery() const
{
	return m_slot ? m_slot->window_delivery() : 0;
}

// The event types a world can carry. The engine registers its own in the base registry, a module in its layer's.
// Registration order is the order types are delivered in.
class bus_registry
{
public:
	bus_registry() = default;
	// A layer's registry (MODULE_LAYER.md): the parent's types as they are now, at their indices, then this one's.
	// The parent outlives this registry.
	explicit bus_registry(const bus_registry* parent)
		: m_parent(parent)
		, m_parent_size(parent ? parent->size() : 0)
	{
	}

	// False when the type is already registered (here or in the parent). `schema` is the payload's JSON Schema,
	// shown by MCP (#605); without it MCP shows only the envelope.
	template <class T>
	bool add(std::string schema = {})
	{
		static_assert(std::copy_constructible<T>, "a bus event type must be copyable");
		if (index_of<T>()) return false;
		m_types.push_back(std::make_unique<detail::bus_type_impl<T>>(std::move(schema)));
		return true;
	}

	template <class T>
	std::optional<size_t> index_of() const
	{
		const entt::id_type hash = entt::type_hash<T>::value();
		for (size_t i = 0; i < size(); ++i) {
			if (type(i).hash() == hash) return i;
		}
		return std::nullopt;
	}

	template <class T>
	bool contains() const { return index_of<T>().has_value(); }

	size_t size() const { return m_parent_size + m_types.size(); }
	const detail::bus_type& type(size_t index) const
	{
		return index < m_parent_size ? m_parent->type(index) : *m_types[index - m_parent_size];
	}

private:
	const bus_registry* m_parent = nullptr;
	size_t m_parent_size = 0;
	std::vector<std::unique_ptr<detail::bus_type>> m_types;
};

// One per world, kept in its registry's ctx. Main thread only.
class SNK_API world_bus
{
public:
	world_bus();
	~world_bus();
	world_bus(world_bus&&) noexcept;
	world_bus& operator=(world_bus&&) noexcept;
	world_bus(const world_bus&) = delete;
	world_bus& operator=(const world_bus&) = delete;

	// Gives the registry a writer and a reader of every type of `types` not bound yet; a second call with a registry
	// that grew binds only the new types. `types` must keep the same order (the same registry or a layer over it):
	// otherwise nothing more is bound, an error is logged and the result is false.
	bool bind(const bus_registry& types, entt::registry& registry, std::string_view world);

	// Switching to discard drops the queue and the windows (reader delivery() reads 0 again).
	void set_mode(bus_mode mode);
	bus_mode mode() const;

	// Events of one type a delivery takes, 1..UINT32_MAX (seq is 32 bits); more are dropped with a warning when the
	// dropped count changes. 0 is refused with an error. Default default_event_limit.
	void set_event_limit(size_t limit);
	size_t event_limit() const;

	const std::string& world() const;
	// Types bound so far.
	size_t bound_types() const;
	// The world says its delivery point is behind it this frame (true) or ahead (false). An emit while true is a
	// late sender: in a build with asserts it is warned about once per type.
	void set_after_delivery(bool after);
	uint64_t deliveries() const;
	bool delivering() const;
	// Events waiting for the next delivery, all types.
	size_t queued() const;

	// An event from outside the world (editor, game shell, a test); it goes first in the next delivery and its
	// envelope's `from` is empty. The type must be bound.
	template <class T>
	void post(T payload)
	{
		auto* slot = find<T>();
		ASSERT_MSG(slot, "world_bus::post<{}>: event type is not bound to world '{}'", entt::type_name<T>::value(), world());
		if (slot) slot->post(std::move(payload));
	}

	// Called during each delivery, per event of T, in subscription order. What a subscriber emits goes to the next
	// delivery. The type must be bound.
	template <class T>
	[[nodiscard]] connection subscribe(std::function<void(const envelope<T>&)> fn)
	{
		auto* slot = find<T>();
		ASSERT_MSG(slot, "world_bus::subscribe<{}>: event type is not bound to world '{}'", entt::type_name<T>::value(), world());
		return slot ? slot->subscribe(std::move(fn)) : connection{};
	}

	// Fills every window (types in registration order), then calls the subscribers type by type: those subscribed
	// when the delivery began. Does nothing in discard mode.
	void deliver();

	// Drops the queue and the windows (code reload); returns how many events were dropped. Not from a subscriber.
	size_t clear();

private:
	detail::bus_state& state() const;

	template <class T>
	detail::bus_slot_impl<T>* find() const
	{
		const entt::id_type hash = entt::type_hash<T>::value();
		for (const auto& slot : m_slots) {
			if (slot->hash() == hash) return static_cast<detail::bus_slot_impl<T>*>(slot.get());
		}
		return nullptr;
	}

	std::unique_ptr<detail::bus_state> m_state;
	std::vector<std::unique_ptr<detail::bus_slot>> m_slots;
};

} // namespace ecs
