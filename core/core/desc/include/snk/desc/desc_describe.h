#pragma once
#include <snk/common/common.h>
#include <snk/desc/desc_hints.hpp>
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_schema.h>
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <boost/describe.hpp>
#include <boost/json/value.hpp>
#include <boost/mp11/algorithm.hpp>

// Schema generation from Boost.Describe. This header pulls in Describe and mp11, so include it only where a
// described type is declared or registered, not from widely-included headers.
namespace desc
{
	// Optional per-enum specialization:
	//   template<> struct desc::enum_names<pass_queue> {
	//       static constexpr std::string_view name = "pass_queue";
	//       static constexpr std::array<std::pair<pass_queue, std::string_view>, 1> overrides{{ { pass_queue::OPAQUE, "opaque" } }};
	//       static constexpr pass_queue fallback = pass_queue::OPAQUE;
	//       static constexpr pass_queue null_value = pass_queue::NONE; // read from / written as JSON null (write_null_if_empty)
	//   };
	// Every member is optional; enumerators without an override are written under their Describe name.
	template<typename E>
	struct enum_names;

	template<typename E>
	concept has_enum_names = requires { sizeof(enum_names<E>); };

	template<typename E>
	std::string_view enum_item_name(E value)
	{
		if constexpr (has_enum_names<E>) {
			if constexpr (requires { enum_names<E>::overrides; }) {
				for (const auto& entry : enum_names<E>::overrides) {
					if (entry.first == value) {
						return entry.second;
					}
				}
			}
		}
		return boost::describe::enum_to_string(value, "");
	}

	template<typename E>
	std::string enum_type_name()
	{
		if constexpr (has_enum_names<E>) {
			if constexpr (requires { enum_names<E>::name; }) {
				return std::string{ enum_names<E>::name };
			}
		}
		return {};
	}

	// `name` wins over enum_names<E>::name.
	template<typename E>
	enum_schema make_enum_schema(std::string_view name = {})
	{
		enum_schema schema;
		schema.name = name.empty() ? enum_type_name<E>() : std::string{ name };
		if constexpr (has_enum_names<E>) {
			if constexpr (requires { enum_names<E>::fallback; }) {
				schema.fallback = static_cast<std::int64_t>(enum_names<E>::fallback);
			}
			if constexpr (requires { enum_names<E>::null_value; }) {
				schema.null_value = static_cast<std::int64_t>(enum_names<E>::null_value);
			}
		}
		boost::mp11::mp_for_each<boost::describe::describe_enumerators<E>>([&](auto enumerator) {
			schema.items.push_back(enum_item{ std::string{ enum_item_name<E>(enumerator.value) }, static_cast<std::int64_t>(enumerator.value) });
		});
		return schema;
	}

	// Calls visit(name, member_pointer) for every non-static data member Describe lists for T (any access,
	// declaration order, bases not included).
	template<typename T, typename Visitor>
	void for_each_data_member(Visitor&& visit)
	{
		namespace bd = boost::describe;
		boost::mp11::mp_for_each<bd::describe_members<T, bd::mod_any_access>>([&](auto descriptor) {
			using descriptor_type = decltype(descriptor);
			if constexpr ((descriptor_type::modifiers & (bd::mod_static | bd::mod_function)) == 0) {
				visit(std::string_view{ descriptor_type::name }, descriptor_type::pointer);
			}
			(void)descriptor;
		});
	}

	template<typename T>
	json::value value_to_json(const T& value);

	inline std::string member_key(const field_attr_data* attr, std::string_view member)
	{
		if (attr && !attr->key.empty()) {
			return std::string{ attr->key };
		}
		return "/" + std::string{ member };
	}

	// Object holding every described member of `value` under its schema key.
	template<typename S>
	json::value struct_to_json(const S& value)
	{
		json::value out(json::object{});
		const auto& table = field_attrs<S>;
		for_each_data_member<S>([&](std::string_view member, auto pointer) {
			json_pointer_set(out, member_key(table.find(member), member), value_to_json(value.*pointer));
		});
		return out;
	}

	// The per-kind converter behind schema defaults; #129 builds serialize on top of it.
	template<typename T>
	json::value value_to_json(const T& value)
	{
		constexpr field_kind kind = kind_of<T>;
		if constexpr (kind == field_kind::boolean) {
			return json::value(static_cast<bool>(value));
		} else if constexpr (kind == field_kind::i32 || kind == field_kind::i64) {
			return json::value(static_cast<std::int64_t>(value));
		} else if constexpr (kind == field_kind::u32 || kind == field_kind::u64) {
			return json::value(static_cast<std::uint64_t>(value));
		} else if constexpr (kind == field_kind::f32 || kind == field_kind::f64) {
			return json::value(static_cast<double>(value));
		} else if constexpr (kind == field_kind::vec2 || kind == field_kind::vec3 || kind == field_kind::vec4) {
			json::array components;
			components.reserve(static_cast<std::size_t>(T::length()));
			for (int i = 0; i < T::length(); ++i) {
				components.push_back(static_cast<double>(value[i]));
			}
			return json::value(std::move(components));
		} else if constexpr (kind == field_kind::quat) {
			json::array components;
			components.push_back(static_cast<double>(value.x));
			components.push_back(static_cast<double>(value.y));
			components.push_back(static_cast<double>(value.z));
			components.push_back(static_cast<double>(value.w));
			return json::value(std::move(components));
		} else if constexpr (kind == field_kind::string) {
			return json::value(std::string_view{ value });
		} else if constexpr (kind == field_kind::res_tag) {
			return json::value(value.view());
		} else if constexpr (kind == field_kind::enumeration) {
			return json::value(enum_item_name<T>(value));
		} else if constexpr (kind == field_kind::array) {
			json::array elements;
			elements.reserve(std::size(value));
			for (const auto& element : value) {
				elements.push_back(value_to_json(element));
			}
			return json::value(std::move(elements));
		} else if constexpr (kind == field_kind::structure) {
			return struct_to_json(value);
		} else {
			return json::value();
		}
	}

	// True when every field_attrs<T> entry names a member Describe lists for T. schema_generator<T>::make
	// static_asserts it, so a typo or a renamed member fails the build instead of being ignored.
	template<typename T>
	constexpr bool attrs_match_members()
	{
		namespace bd = boost::describe;
		bool all_found = true;
		for (const auto& item : field_attrs<T>.items) {
			bool found = false;
			boost::mp11::mp_for_each<bd::describe_members<T, bd::mod_any_access>>([&](auto descriptor) {
				using descriptor_type = decltype(descriptor);
				if (std::string_view{ descriptor_type::name } == item.data().member) {
					found = true;
				}
			});
			all_found = all_found && found;
		}
		return all_found;
	}

	// Schema of a type with BOOST_DESCRIBE_CLASS (members) and, optionally, a field_attrs<T> specialization.
	// T must be default constructible: the defaults are the converted members of T{}.
	template<typename T>
	struct schema_generator
	{
		static type_schema make(std::string_view name, int version, schema_kind kind = schema_kind::desc)
		{
			static_assert(attrs_match_members<T>(), "field_attrs<T> names a member that BOOST_DESCRIBE_CLASS does not list");

			type_schema schema;
			schema.name = std::string{ name };
			schema.id = schema_id(name);
			schema.version = version;
			schema.kind = kind;
			schema.is_component = kind == schema_kind::component;

			const T defaults{};
			const auto& table = field_attrs<T>;
			for_each_data_member<T>([&](std::string_view member, auto pointer) {
				using member_type = std::remove_cvref_t<decltype(defaults.*pointer)>;
				const field_attr_data* attr = table.find(member);

				field_schema field;
				field.name = std::string{ member };
				field.key = member_key(attr, member);
				field.kind = kind_of<member_type>;
				field.element = element_kind_of<member_type>();
				const bool converts = !attr || !attr->has_kind || attr->kind == field.kind;
				if (attr) {
					if (attr->has_kind) {
						field.kind = attr->kind;
					}
					if (attr->has_range) {
						field.range = attr->range;
					}
					field.ref_type = std::string{ attr->ref };
					field.write = attr->write;
					for (std::size_t i = 0; i < attr->alias_count; ++i) {
						field.read_aliases.emplace_back(attr->aliases[i]);
					}
					field.optional = attr->optional;
					field.editor_hidden = attr->hidden;
					field.read_only = attr->read_only;
					field.color = attr->color;
					field.degrees = attr->degrees;
					field.tooltip = std::string{ attr->tooltip };
					field.filter = std::string{ attr->filter };
				}
				if constexpr (std::is_enum_v<member_type>) {
					if (field.ref_type.empty()) {
						field.ref_type = enum_type_name<member_type>();
					}
				}
				if (converts) {
					field.default_value = value_to_json(defaults.*pointer);
				}
				schema.fields.push_back(std::move(field));
			});
			return schema;
		}

		// Schemas of the enum members that have a name (field_attr::ref or enum_names<E>::name).
		static std::vector<enum_schema> enums()
		{
			std::vector<enum_schema> result;
			const T defaults{};
			const auto& table = field_attrs<T>;
			for_each_data_member<T>([&](std::string_view member, auto pointer) {
				using member_type = std::remove_cvref_t<decltype(defaults.*pointer)>;
				if constexpr (std::is_enum_v<member_type>) {
					const field_attr_data* attr = table.find(member);
					enum_schema schema = make_enum_schema<member_type>(attr ? attr->ref : std::string_view{});
					if (!schema.name.empty()) {
						result.push_back(std::move(schema));
					}
				}
			});
			return result;
		}
	};
}
