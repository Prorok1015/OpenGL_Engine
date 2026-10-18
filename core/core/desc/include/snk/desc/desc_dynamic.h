#pragma once
#include <snk/common/snk_api.h>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_schema_ops.h>
#include <functional>
#include <memory>
#include <string_view>
#include <boost/json/value.hpp>

namespace desc
{
	// A desc whose type exists only as a type_schema (wasm/JSON): the values live in a blob laid out by the schema.
	class SNK_API dynamic_desc : public desc_base
	{
	public:
		explicit dynamic_desc(std::shared_ptr<const schema_binding> binding);

		void copy_to(desc_base& other) const override;
		void deserialize(desc_system& system, const json::object& object) override;
		void serialize(json::object& object) const override;

		const type_schema& schema() const { return *m_binding->schema; }
		const schema_binding& binding() const { return *m_binding; }

		// Field by its key; null for an unknown key.
		json::value get(std::string_view key) const;
		// False for an unknown key or a value that does not fit the field.
		bool set(std::string_view key, const json::value& value);

	private:
		std::shared_ptr<const schema_binding> m_binding;
		mutable dyn_blob m_blob;
	};

	// Builds the factory desc_system keeps for a schema-only type. Empty when the schema cannot be bound (its
	// structure fields name schemas that are not in `resolve`, or its layout is missing).
	SNK_API std::function<std::shared_ptr<desc_base>()> make_dynamic_factory(const type_schema& schema, const schema_registry& resolve);
}
