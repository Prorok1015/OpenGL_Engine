#pragma once
#include <snk/ecs/ecs_entt_abi.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/snk_api.h>
#include <entt/entt.hpp>
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The data channel of a context set (WORLD_MESSAGING 3): a blackboard (last value per key) and a
// mailbox (one-off messages). A system sees them as ordinary ctx resources of its own registry:
//   const ecs::blackboard_reader&, ecs::blackboard_writer&, const ecs::inbox<T>&, ecs::outbox<T>&
// Everything written in frame N becomes visible to every context of the set in frame N+1: the set
// exchanges the buffers once, at the start of its frame (scn::set_channel), so the result does not
// depend on the order the contexts or phases run in. No frame-allocator memory is used here.
namespace ecs {

// A node of another context of the same set. The receiver resolves it in its own registry.
struct entity_ref
{
	std::string context;
	std::vector<uint64_t> path;
	bool operator==(const entity_ref&) const = default;
};

// An empty context name addresses every other context of the set (not the sender).
struct address
{
	std::string context;
	bool operator==(const address&) const = default;
};

// A typed blackboard key. The type is checked (ASSERT) when a key is accessed as another type
// than the one it holds. Declare keys as constants: `inline constexpr ecs::bb_key<int> hp{ "hud.health" };`
template <class T>
struct bb_key
{
	std::string_view name;
};

struct bb_entry
{
	entt::id_type type = 0;
	std::string_view type_name;
	std::shared_ptr<const void> value; // immutable: a reader's pointer stays valid until the key is replaced
	int64_t writer = 0;
	bool conflict_warned = false;
};

// Committed (visible) and pending (written this frame) values of the blackboard, both sorted by key.
class SNK_API blackboard_data
{
public:
	using map_type = std::map<std::string, bb_entry, std::less<>>;

	// External writers (editor, game shell) lose against every context.
	static constexpr int64_t external_writer = -1;

	// Several writers of a key in one frame: the greater writer id wins, a warning is logged.
	void write(std::string_view name, entt::id_type type, std::string_view type_name,
			   std::shared_ptr<const void> value, int64_t writer);
	const void* find(std::string_view name, entt::id_type type, std::string_view type_name) const;

	// Pending values replace the committed ones; the pending map is emptied.
	void commit();
	void clear();

	const map_type& committed() const { return m_committed; }
	const map_type& pending() const { return m_pending; }

private:
	map_type m_committed;
	map_type m_pending;
};

// Frame N-1 as it was committed; does not change during the frame.
class blackboard_reader
{
public:
	blackboard_reader() = default;
	explicit blackboard_reader(const blackboard_data* data) : m_data(data) {}

	template <class T>
	const T* find(bb_key<T> key) const
	{
		if (!m_data) return nullptr;
		return static_cast<const T*>(m_data->find(key.name, entt::type_hash<T>::value(), entt::type_name<T>::value()));
	}

	template <class T>
	T get_or(bb_key<T> key, T fallback) const
	{
		const T* value = find(key);
		return value ? *value : std::move(fallback);
	}

private:
	const blackboard_data* m_data = nullptr;
};

// Writes into the back buffer; the value is visible from the next frame.
class blackboard_writer
{
public:
	blackboard_writer() = default;
	blackboard_writer(blackboard_data* data, int64_t writer) : m_data(data), m_writer(writer) {}

	template <class T>
	void set(bb_key<T> key, T value)
	{
		ASSERT_MSG(m_data, "blackboard_writer is not bound to a set");
		if (!m_data) return;
		m_data->write(key.name, entt::type_hash<T>::value(), entt::type_name<T>::value(),
					  std::make_shared<const T>(std::move(value)), m_writer);
	}

private:
	blackboard_data* m_data = nullptr;
	int64_t m_writer = 0;
};

template <class T>
struct envelope
{
	std::string from; // sender context name; empty for an external post
	address to;
	uint32_t seq = 0; // order of sending inside one sender and one frame
	T payload;
};

namespace detail {
template <class T> class channel_slot_impl;
template <class T> struct channel_type_impl;
}

// Messages sent during a frame; handed over at the next exchange. Holds at most the set's
// per-type per-frame limit, the rest is dropped with a warning (guards against ping-pong).
template <class T>
class outbox
{
public:
	void send(address to, T payload)
	{
		ASSERT_MSG(m_limit, "outbox<{}>: message type is not registered in the channel_registry of the set",
				   entt::type_name<T>::value());
		if (!m_limit) return;
		if (m_pending.size() >= *m_limit) {
			if (!m_overflow_warned) {
				egLOG_WARN("scene", "outbox<{}> of '{}' is full ({} messages per frame), dropping", entt::type_name<T>::value(), m_from, *m_limit);
				m_overflow_warned = true;
			}
			return;
		}
		m_pending.push_back(envelope<T>{ m_from, std::move(to), m_next_seq++, std::move(payload) });
	}

	std::span<const envelope<T>> pending() const { return m_pending; }
	bool bound() const { return m_limit != nullptr; }

private:
	friend class detail::channel_slot_impl<T>;
	friend struct detail::channel_type_impl<T>;

	std::string m_from;
	const size_t* m_limit = nullptr;
	std::vector<envelope<T>> m_pending;
	uint32_t m_next_seq = 0;
	bool m_overflow_warned = false;
};

// Messages delivered at the start of this frame; valid for all its phases and fixed steps.
template <class T>
class inbox
{
public:
	std::span<const envelope<T>> messages() const { return m_messages; }

private:
	friend class detail::channel_slot_impl<T>;
	std::vector<envelope<T>> m_messages;
};

// A context taking part in an exchange (in ascending id order).
struct channel_participant
{
	uint32_t id = 0;
	std::string_view name;
	entt::registry* registry = nullptr;
};

namespace detail {

// Per set and message type: what is not owned by a context (external posts) plus the exchange.
class channel_slot
{
public:
	virtual ~channel_slot() = default;
	// Delivery order: external posts, then contexts by ascending id, then by seq.
	virtual void exchange(std::span<const channel_participant> contexts, size_t limit, std::string_view set_name) = 0;
	// No exchange this frame: the inboxes are consumed, nothing is delivered.
	virtual void consume(std::span<const channel_participant> contexts) = 0;
	virtual void clear() = 0;
};

class channel_type
{
public:
	virtual ~channel_type() = default;
	virtual entt::id_type hash() const = 0;
	virtual std::string_view name() const = 0;
	// Creates the context's inbox and outbox when it has none yet.
	virtual void bind(entt::registry& registry, std::string_view context_name, const size_t* limit) const = 0;
	virtual std::unique_ptr<channel_slot> make_slot() const = 0;
};

template <class T>
class channel_slot_impl final : public channel_slot
{
public:
	void post(address to, T payload, size_t limit, std::string_view set_name)
	{
		if (m_external.size() >= limit) {
			egLOG_WARN("scene", "set '{}': external posts of {} exceed {} per frame, dropping", set_name, entt::type_name<T>::value(), limit);
			return;
		}
		m_external.push_back(envelope<T>{ std::string{}, std::move(to), m_next_seq++, std::move(payload) });
	}

	void exchange(std::span<const channel_participant> contexts, size_t limit, std::string_view set_name) override
	{
		std::vector<inbox<T>*> inboxes;
		std::vector<outbox<T>*> outboxes;
		inboxes.reserve(contexts.size());
		outboxes.reserve(contexts.size());
		size_t incoming = m_external.size();
		for (const auto& c : contexts) {
			auto* in = c.registry->ctx().template find<inbox<T>>();
			auto* out = c.registry->ctx().template find<outbox<T>>();
			if (in) in->m_messages.clear();
			if (out) incoming += out->m_pending.size();
			inboxes.push_back(in);
			outboxes.push_back(out);
		}
		for (auto* in : inboxes) {
			if (in) in->m_messages.reserve(std::min(incoming, limit));
		}

		size_t delivered = 0;
		size_t over_limit = 0;
		auto deliver = [&](envelope<T>&& msg) {
			if (delivered >= limit) {
				++over_limit;
				return;
			}
			if (msg.to.context.empty()) {
				++delivered;
				for (size_t i = 0; i < inboxes.size(); ++i) {
					// a context that wants its own message addresses itself by name
					if (inboxes[i] && !(contexts[i].name == msg.from && !msg.from.empty())) inboxes[i]->m_messages.push_back(msg);
				}
				return;
			}
			const auto it = std::ranges::find_if(contexts, [&](const channel_participant& c) { return c.name == msg.to.context; });
			if (it == contexts.end()) {
				egLOG_WARN("scene", "set '{}': {} from '{}' dropped, no context '{}'", set_name, entt::type_name<T>::value(), msg.from, msg.to.context);
				return;
			}
			++delivered;
			if (auto* in = inboxes[static_cast<size_t>(it - contexts.begin())]) in->m_messages.push_back(std::move(msg));
		};

		for (auto& msg : m_external) deliver(std::move(msg));
		m_external.clear();
		m_next_seq = 0;
		for (auto* out : outboxes) {
			if (!out) continue;
			for (auto& msg : out->m_pending) deliver(std::move(msg));
			out->m_pending.clear();
			out->m_next_seq = 0;
			out->m_overflow_warned = false;
		}
		if (over_limit > 0) {
			egLOG_WARN("scene", "set '{}': {} {} dropped, more than {} per frame", set_name, over_limit, entt::type_name<T>::value(), limit);
		}
	}

	void consume(std::span<const channel_participant> contexts) override
	{
		for (const auto& c : contexts) {
			if (auto* in = c.registry->ctx().template find<inbox<T>>()) in->m_messages.clear();
		}
	}

	void clear() override
	{
		m_external.clear();
		m_next_seq = 0;
	}

private:
	std::vector<envelope<T>> m_external;
	uint32_t m_next_seq = 0;
};

template <class T>
struct channel_type_impl final : channel_type
{
	entt::id_type hash() const override { return entt::type_hash<T>::value(); }
	std::string_view name() const override { return entt::type_name<T>::value(); }

	void bind(entt::registry& registry, std::string_view context_name, const size_t* limit) const override
	{
		registry.ctx().template emplace<inbox<T>>();
		auto& out = registry.ctx().template emplace<outbox<T>>();
		if (out.bound()) return;
		out.m_from = std::string{ context_name };
		out.m_limit = limit;
	}

	std::unique_ptr<channel_slot> make_slot() const override { return std::make_unique<channel_slot_impl<T>>(); }
};

} // namespace detail

// The message types a set can carry. Modules register theirs once at init; sets share one registry.
// Registration order is the order types are exchanged in.
class channel_registry
{
public:
	channel_registry() = default;
	// A layer's registry (MODULE_LAYER.md): the parent's types as they are now, at their indices, then this one's.
	// Types the parent gets later are not seen here, so the indices of the sets built on it never move. The parent
	// outlives this registry.
	explicit channel_registry(const channel_registry* parent)
		: m_parent(parent)
		, m_parent_size(parent ? parent->size() : 0)
	{
	}

	// False when the type is already registered (here or in the parent). A broadcast copies the message, so T must
	// be copyable.
	template <class T>
	bool add()
	{
		static_assert(std::copy_constructible<T>, "a channel message type must be copyable (broadcast)");
		if (index_of<T>()) return false;
		m_types.push_back(std::make_unique<detail::channel_type_impl<T>>());
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
	const detail::channel_type& type(size_t index) const
	{
		return index < m_parent_size ? m_parent->type(index) : *m_types[index - m_parent_size];
	}

private:
	const channel_registry* m_parent = nullptr;
	size_t m_parent_size = 0;
	std::vector<std::unique_ptr<detail::channel_type>> m_types;
};

} // namespace ecs
