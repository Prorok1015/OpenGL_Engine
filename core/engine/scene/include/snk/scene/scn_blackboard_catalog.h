#pragma once
#include <snk/common/snk_api.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/scene/scn_component_catalog.h>
#include <boost/json/object.hpp>
#include <entt/core/type_info.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scn
{
	// A blackboard value type a snapshot of a set can save (HOT_RELOAD_PLAY.md 4.4).
	struct blackboard_type
	{
		using write_fn = void (*)(const desc::schema_binding&, const void* value, json::object& out);
		// `fits` false: a value of the input did not fit its field, which kept its default.
		using read_fn = std::shared_ptr<const void> (*)(const desc::schema_binding&, const json::object& in, const desc::read_context&, bool& fits);

		std::string name;
		entt::id_type id = 0;
		// entt's name of the type, as the blackboard checks a read against it.
		std::string_view type_name;
		std::shared_ptr<const desc::schema_binding> schema;
		write_fn write = nullptr;
		read_fn read = nullptr;
	};

	namespace detail
	{
		template<typename T>
		std::shared_ptr<const void> read_blackboard(const desc::schema_binding& schema, const json::object& in, const desc::read_context& ctx, bool& fits)
		{
			T value{};
			desc::deserialize(schema, &value, in, ctx);
			fits = detail::fits(schema, in);
			return std::make_shared<const T>(std::move(value));
		}
	}

	// The blackboard keeps its values type-erased, so a snapshot saves a value only when its type is registered here
	// with a schema. Registering is optional: the value of a type that is not is reported as lost (unsaved). One per
	// layer; the entries of a module point into its binary and go with its layer. Main thread, between frames.
	class SNK_API blackboard_catalog
	{
	public:
		blackboard_catalog() = default;
		blackboard_catalog(const blackboard_catalog&) = delete;
		blackboard_catalog& operator=(const blackboard_catalog&) = delete;

		// `schema` must be bound to T (desc::bind_described<T>). nullptr (logged, nothing added) when the name or the
		// type is taken, or without a schema.
		template<typename T>
		const blackboard_type* add(std::string name, std::shared_ptr<const desc::schema_binding> schema)
		{
			blackboard_type entry;
			entry.name = std::move(name);
			entry.id = entt::type_hash<T>::value();
			entry.type_name = entt::type_name<T>::value();
			entry.schema = std::move(schema);
			entry.write = &write_value;
			entry.read = &detail::read_blackboard<T>;
			return add_entry(std::move(entry));
		}

		const blackboard_type* find(std::string_view name) const;
		const blackboard_type* find(entt::id_type id) const;
		void clear();
		std::size_t size() const { return m_entries.size(); }

	private:
		static void write_value(const desc::schema_binding& schema, const void* value, json::object& out);
		const blackboard_type* add_entry(blackboard_type entry);

		std::vector<std::unique_ptr<blackboard_type>> m_entries;
		std::unordered_map<std::string, const blackboard_type*> m_by_name;
		std::unordered_map<entt::id_type, const blackboard_type*> m_by_id;
	};
}
