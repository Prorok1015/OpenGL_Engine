#pragma once
#include <snk/common/snk_api.h>
#include <snk/desc/desc_schema_ops.h>
#include <boost/json/object.hpp>
#include <entt/core/type_info.hpp>
#include <entt/entity/registry.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scn
{
	// What a snapshot of a live world does with a registered type (HOT_RELOAD_PLAY.md section 4.2-4.3).
	enum class component_category : std::uint8_t
	{
		data, // saved through its schema; without one it is reported as lost
		derived, // never saved: the engine rebuilds it from the data after a restore
		editor_only, // never saved: the editor puts it back itself
		ctx, // a ctx resource of the world, saved through its schema
	};

	struct component_entry;

	namespace detail
	{
		template<typename T>
		bool contains(const entt::registry& reg, entt::entity entity)
		{
			return reg.all_of<T>(entity);
		}

		template<typename T>
		bool contains_ctx(const entt::registry& reg, entt::entity)
		{
			return reg.ctx().contains<T>();
		}

		template<typename T>
		bool write_value(const desc::schema_binding& schema, const T& value, json::object& out)
		{
			desc::serialize(schema, &value, out);
			return true;
		}

		template<typename T>
		bool write(const desc::schema_binding& schema, const entt::registry& reg, entt::entity entity, json::object& out)
		{
			if (!reg.all_of<T>(entity)) {
				return false;
			}
			if constexpr (std::is_empty_v<T>) {
				return write_value(schema, T{}, out);
			} else {
				return write_value(schema, reg.get<T>(entity), out);
			}
		}

		template<typename T>
		bool write_ctx(const desc::schema_binding& schema, const entt::registry& reg, entt::entity, json::object& out)
		{
			const T* value = reg.ctx().find<T>();
			return value && write_value(schema, *value, out);
		}

		// The value is complete before it is placed: an on_construct observer never sees a default-built component.
		template<typename T>
		void read(const desc::schema_binding& schema, entt::registry& reg, entt::entity entity, const json::object& in, const desc::read_context& ctx)
		{
			T value{};
			desc::deserialize(schema, &value, in, ctx);
			reg.emplace_or_replace<T>(entity, std::move(value));
		}

		template<typename T>
		void read_ctx(const desc::schema_binding& schema, entt::registry& reg, entt::entity, const json::object& in, const desc::read_context& ctx)
		{
			T value{};
			desc::deserialize(schema, &value, in, ctx);
			reg.ctx().insert_or_assign(std::move(value));
		}
	}

	struct component_entry
	{
		using contains_fn = bool (*)(const entt::registry&, entt::entity);
		using write_fn = bool (*)(const desc::schema_binding&, const entt::registry&, entt::entity, json::object&);
		using read_fn = void (*)(const desc::schema_binding&, entt::registry&, entt::entity, const json::object&, const desc::read_context&);

		std::string name;
		entt::id_type id = 0;
		component_category category = component_category::data;
		// Null: the type has no schema and is not saved.
		std::shared_ptr<const desc::schema_binding> schema;
		// The entity is ignored for a ctx resource.
		contains_fn contains = nullptr;
		write_fn write = nullptr;
		read_fn read = nullptr;
		const std::type_info* object_type = nullptr;

		bool saved() const { return schema && (category == component_category::data || category == component_category::ctx); }
	};

	// The runtime components and ctx resources a snapshot knows, by stable name, in registration order. A layer over
	// `parent` (MODULE_LAYER.md) sees the parent's entries first and cannot reuse their names or types. This is the
	// schema side of the names in ecs::component_registry; main thread, between frames.
	class SNK_API component_catalog
	{
	public:
		component_catalog() = default;
		explicit component_catalog(const component_catalog* parent)
			: m_parent(parent)
		{
		}
		component_catalog(const component_catalog&) = delete;
		component_catalog& operator=(const component_catalog&) = delete;

		// `schema` must be bound to T (desc::bind_described<T>); null registers T as a type without a schema.
		// Each returns nullptr (logged, nothing added) when the name or the type is taken here or in the parent.
		template<typename T>
		const component_entry* add(std::string name, std::shared_ptr<const desc::schema_binding> schema)
		{
			return add_typed<T>(std::move(name), component_category::data, std::move(schema));
		}

		template<typename T>
		const component_entry* add_derived(std::string name)
		{
			return add_typed<T>(std::move(name), component_category::derived, nullptr);
		}

		template<typename T>
		const component_entry* add_editor_only(std::string name)
		{
			return add_typed<T>(std::move(name), component_category::editor_only, nullptr);
		}

		template<typename T>
		const component_entry* add_ctx(std::string name, std::shared_ptr<const desc::schema_binding> schema)
		{
			static_assert(!std::is_empty_v<T>, "an empty ctx resource carries nothing to save");
			component_entry entry = make_entry<T>(std::move(name), component_category::ctx, std::move(schema));
			entry.contains = &detail::contains_ctx<T>;
			entry.write = &detail::write_ctx<T>;
			entry.read = &detail::read_ctx<T>;
			return add_entry(std::move(entry));
		}

		// Removes one of this layer's entries; false for an unknown name or one of the parent's.
		bool remove(std::string_view name);

		const component_entry* find(std::string_view name) const;
		const component_entry* find(entt::id_type id) const;

		// The parent's entries, then this layer's, each in registration order.
		template<typename F>
		void for_each(F&& visit) const
		{
			if (m_parent) {
				m_parent->for_each(visit);
			}
			for (const auto& entry : m_entries) {
				if (!entry.name.empty()) {
					visit(entry);
				}
			}
		}

		// This layer's own entries, without the parent's.
		std::size_t size() const { return m_by_name.size(); }

	private:
		template<typename T>
		component_entry make_entry(std::string name, component_category category, std::shared_ptr<const desc::schema_binding> schema)
		{
			component_entry entry;
			entry.name = std::move(name);
			entry.id = entt::type_hash<T>::value();
			entry.category = category;
			entry.schema = std::move(schema);
			entry.object_type = &typeid(T);
			return entry;
		}

		template<typename T>
		const component_entry* add_typed(std::string name, component_category category, std::shared_ptr<const desc::schema_binding> schema)
		{
			component_entry entry = make_entry<T>(std::move(name), category, std::move(schema));
			entry.contains = &detail::contains<T>;
			entry.write = &detail::write<T>;
			entry.read = &detail::read<T>;
			return add_entry(std::move(entry));
		}

		const component_entry* add_entry(component_entry entry);

		const component_catalog* m_parent = nullptr;
		// A removed entry stays as a tombstone (empty name): the pointers handed out keep pointing at stable storage.
		std::deque<component_entry> m_entries;
		std::unordered_map<std::string, const component_entry*> m_by_name;
		std::unordered_map<entt::id_type, const component_entry*> m_by_id;
	};

	// A storage of `reg` holding at least one component whose type `catalog` does not know. entt's own entity
	// storage and storages left empty (a type no entity has any more) are not listed. `type` is entt's type name.
	struct unregistered_storage
	{
		entt::id_type id = 0;
		std::string type;
	};

	SNK_API std::vector<unregistered_storage> unregistered_storages(const entt::registry& reg, const component_catalog& catalog);

	// Every runtime component of the engine (HOT_RELOAD_PLAY.md section 4.3): data types with their schema, derived
	// ones marked as such. Registered once in the base catalog (scn::scene_init).
	SNK_API void register_engine_components(component_catalog& catalog);
}
