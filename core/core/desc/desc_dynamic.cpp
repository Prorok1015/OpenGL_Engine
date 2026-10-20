#include <snk/desc/desc_dynamic.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_json_ops.h>
#include <exception>
#include <utility>

namespace desc
{
	dynamic_desc::dynamic_desc(std::shared_ptr<const schema_binding> binding)
		: m_binding(std::move(binding))
		, m_blob(make_blob(*m_binding))
	{
	}

	void dynamic_desc::copy_to(desc_base& other) const
	{
		if (auto* typed = dynamic_cast<dynamic_desc*>(&other)) {
			dyn_ref from{ &m_blob, 0 };
			dyn_ref to{ &typed->m_blob, 0 };
			copy(*m_binding, &from, *typed->m_binding, &to);
			return;
		}
		desc_base::copy_to(other);
	}

	void dynamic_desc::deserialize(desc_system& system, const json::object& object)
	{
		dyn_ref ref{ &m_blob, 0 };
		desc::deserialize(*m_binding, &ref, object, read_context{ &system, this });
	}

	void dynamic_desc::serialize(json::object& object) const
	{
		dyn_ref ref{ &m_blob, 0 };
		desc::serialize(*m_binding, &ref, object);
	}

	json::value dynamic_desc::get(std::string_view key) const
	{
		const auto& fields = m_binding->schema->fields;
		for (std::size_t i = 0; i < fields.size(); ++i) {
			if (fields[i].key == key) {
				dyn_ref ref{ &m_blob, 0 };
				json::value value;
				m_binding->ops[i].to_json(m_binding->ops[i], &ref, value);
				return value;
			}
		}
		return json::value();
	}

	bool dynamic_desc::set(std::string_view key, const json::value& value)
	{
		const auto& fields = m_binding->schema->fields;
		for (std::size_t i = 0; i < fields.size(); ++i) {
			if (fields[i].key == key) {
				dyn_ref ref{ &m_blob, 0 };
				return m_binding->ops[i].from_json(m_binding->ops[i], &ref, value, read_context{});
			}
		}
		return false;
	}

	std::function<std::shared_ptr<desc_base>()> make_dynamic_factory(const type_schema& schema, const schema_registry& resolve)
	{
		try {
			type_schema bound = schema;
			if (bound.layout.size == 0 && !bound.fields.empty()) {
				compute_layout(bound, &resolve);
			}
			std::shared_ptr<const schema_binding> binding = bind_runtime(std::move(bound), resolve);
			return [binding]() -> std::shared_ptr<desc_base> { return std::make_shared<dynamic_desc>(binding); };
		} catch (const std::exception& e) {
			egLOG_ERROR("desc", "no factory for schema '{}': {}", schema.name, e.what());
			return {};
		}
	}
}
