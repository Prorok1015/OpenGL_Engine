#pragma once
#include <snk/common/engine_assert.h>
#include <snk/desc/desc_describe.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/resource/res_tag.h>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <vector>

// field_ops and schema_binding for C++ types described with Boost.Describe. Heavy like desc_describe.h: include it
// only where a described type is declared or registered.
namespace desc
{
	namespace detail
	{
		template<typename M>
		field_ops make_value_context(const field_attr_data* attr);

		template<typename M>
		std::shared_ptr<const schema_binding> bind_nested(std::string_view name);

		template<typename M>
		struct value_ops
		{
			static void to_json(const field_ops& ops, const M& value, json::value& out)
			{
				constexpr field_kind natural = kind_of<M>;
				if constexpr (has_json_codec<M>) {
					if (ops.kind == field_kind::custom) {
						out = json_codec<M>::to_json(value);
						return;
					}
				}
				if constexpr (natural == field_kind::enumeration) {
					const std::string_view name = enum_item_name<M>(value);
					out = name.empty() ? json::value(static_cast<std::int64_t>(value)) : json::value(name);
				} else if constexpr (natural == field_kind::array) {
					json::array elements;
					elements.reserve(std::size(value));
					for (const auto& element : value) {
						json::value converted;
						value_ops<typename detail::sequence_traits<M>::element>::to_json(*ops.element_ops, element, converted);
						elements.push_back(std::move(converted));
					}
					out = json::value(std::move(elements));
				} else if constexpr (natural == field_kind::structure) {
					json::object nested;
					serialize(*ops.nested, &value, nested);
					out = json::value(std::move(nested));
				} else if constexpr (natural == field_kind::custom) {
					if constexpr (has_json_codec<M>) {
						out = json_codec<M>::to_json(value);
					} else {
						out = json::value();
					}
				} else {
					out = value_to_json(value);
				}
			}

			static bool from_json(const field_ops& ops, M& value, const json::value& in)
			{
				constexpr field_kind natural = kind_of<M>;
				if constexpr (has_json_codec<M>) {
					if (ops.kind == field_kind::custom) {
						return json_codec<M>::from_json(value, in);
					}
				}
				if constexpr (natural == field_kind::boolean) {
					if (!in.is_bool()) {
						return false;
					}
					value = in.as_bool();
					return true;
				} else if constexpr (natural == field_kind::i32 || natural == field_kind::i64) {
					std::int64_t number = 0;
					if (!json_to_int64(in, number) || number < std::numeric_limits<M>::min() || number > std::numeric_limits<M>::max()) {
						return false;
					}
					value = static_cast<M>(number);
					return true;
				} else if constexpr (natural == field_kind::u32 || natural == field_kind::u64) {
					std::uint64_t number = 0;
					if (!json_to_uint64(in, number) || number > std::numeric_limits<M>::max()) {
						return false;
					}
					value = static_cast<M>(number);
					return true;
				} else if constexpr (natural == field_kind::f32 || natural == field_kind::f64) {
					double number = 0.0;
					if (!json_to_double(in, number)) {
						return false;
					}
					value = static_cast<M>(number);
					return true;
				} else if constexpr (natural == field_kind::vec2 || natural == field_kind::vec3 || natural == field_kind::vec4) {
					if (!in.is_array() || in.as_array().size() != static_cast<std::size_t>(M::length())) {
						return false;
					}
					M parsed{};
					for (int i = 0; i < M::length(); ++i) {
						double number = 0.0;
						if (!json_to_double(in.as_array()[static_cast<std::size_t>(i)], number)) {
							return false;
						}
						parsed[i] = static_cast<typename M::value_type>(number);
					}
					value = parsed;
					return true;
				} else if constexpr (natural == field_kind::quat) {
					if (!in.is_array() || in.as_array().size() != 4) {
						return false;
					}
					double c[4] = {};
					for (std::size_t i = 0; i < 4; ++i) {
						if (!json_to_double(in.as_array()[i], c[i])) {
							return false;
						}
					}
					value.x = static_cast<float>(c[0]);
					value.y = static_cast<float>(c[1]);
					value.z = static_cast<float>(c[2]);
					value.w = static_cast<float>(c[3]);
					return true;
				} else if constexpr (natural == field_kind::string) {
					if (!in.is_string()) {
						return false;
					}
					value = std::string{ in.as_string() };
					return true;
				} else if constexpr (natural == field_kind::res_tag) {
					if (!in.is_string()) {
						return false;
					}
					value = in.as_string().empty() ? res::tag{} : res::tag{ std::string_view{ in.as_string() } };
					return true;
				} else if constexpr (natural == field_kind::enumeration) {
					const auto converted = ops.enumeration ? enum_from_json(*ops.enumeration, in) : std::nullopt;
					if (!converted) {
						return false;
					}
					value = static_cast<M>(*converted);
					return true;
				} else if constexpr (natural == field_kind::array) {
					using element_type = typename detail::sequence_traits<M>::element;
					if (!in.is_array()) {
						return false;
					}
					const auto& source = in.as_array();
					if constexpr (!std::is_same_v<M, std::vector<element_type>>) {
						if (source.size() != std::size(value)) {
							return false;
						}
					}
					std::vector<element_type> parsed;
					parsed.reserve(source.size());
					for (const auto& item : source) {
						element_type element{};
						if (!value_ops<element_type>::from_json(*ops.element_ops, element, item)) {
							return false;
						}
						parsed.push_back(std::move(element));
					}
					if constexpr (std::is_same_v<M, std::vector<element_type>>) {
						value = std::move(parsed);
					} else {
						for (std::size_t i = 0; i < parsed.size(); ++i) {
							value[i] = std::move(parsed[i]);
						}
					}
					return true;
				} else if constexpr (natural == field_kind::structure) {
					if (!in.is_object()) {
						return false;
					}
					deserialize(*ops.nested, &value, in.as_object());
					return true;
				} else {
					if constexpr (has_json_codec<M>) {
						return json_codec<M>::from_json(value, in);
					} else {
						return false;
					}
				}
			}

			static bool equal(const field_ops& ops, const M& a, const M& b)
			{
				constexpr field_kind natural = kind_of<M>;
				if constexpr (natural == field_kind::array) {
					if (std::size(a) != std::size(b)) {
						return false;
					}
					auto left = std::begin(a);
					auto right = std::begin(b);
					for (; left != std::end(a); ++left, ++right) {
						if (!value_ops<typename detail::sequence_traits<M>::element>::equal(*ops.element_ops, *left, *right)) {
							return false;
						}
					}
					return true;
				} else if constexpr (natural == field_kind::structure) {
					return desc::equal(*ops.nested, &a, &b);
				} else if constexpr (requires { a == b; }) {
					return a == b;
				} else if constexpr (has_json_codec<M>) {
					return json_equal(json_codec<M>::to_json(a), json_codec<M>::to_json(b));
				} else {
					return true;
				}
			}

			static void assign(M& dst, const M& src)
			{
				if constexpr (std::is_copy_assignable_v<M>) {
					dst = src;
				}
			}
		};

		template<typename M>
		field_ops make_value_context(const field_attr_data* attr)
		{
			field_ops ops;
			ops.kind = kind_of<M>;
			ops.element = element_kind_of<M>();
			if constexpr (std::is_enum_v<M>) {
				ops.enumeration = std::make_shared<const enum_schema>(make_enum_schema<M>(attr ? attr->ref : std::string_view{}));
			} else if constexpr (kind_of<M> == field_kind::array) {
				ops.element_ops = std::make_shared<const field_ops>(make_value_context<typename detail::sequence_traits<M>::element>(nullptr));
			} else if constexpr (kind_of<M> == field_kind::structure) {
				ops.nested = bind_nested<M>(attr && !attr->ref.empty() ? attr->ref : std::string_view{ "struct" });
			}
			return ops;
		}

		template<typename T, auto Pointer>
		struct member_ops
		{
			using member_type = std::remove_cvref_t<decltype(std::declval<T&>().*Pointer)>;

			static void to_json(const field_ops& ops, const void* obj, json::value& out)
			{
				value_ops<member_type>::to_json(ops, static_cast<const T*>(obj)->*Pointer, out);
			}

			static bool from_json(const field_ops& ops, void* obj, const json::value& in)
			{
				return value_ops<member_type>::from_json(ops, static_cast<T*>(obj)->*Pointer, in);
			}

			static bool equal(const field_ops& ops, const void* a, const void* b)
			{
				return value_ops<member_type>::equal(ops, static_cast<const T*>(a)->*Pointer, static_cast<const T*>(b)->*Pointer);
			}

			static void assign(const field_ops&, void* dst, const void* src)
			{
				value_ops<member_type>::assign(static_cast<T*>(dst)->*Pointer, static_cast<const T*>(src)->*Pointer);
			}

			static field_ops make(const field_attr_data* attr)
			{
				field_ops ops = make_value_context<member_type>(attr);
				ops.to_json = &to_json;
				ops.from_json = &from_json;
				ops.equal = &equal;
				ops.assign = &assign;
				return ops;
			}
		};
	}

	// Ops for the members of T in the order for_each_data_member (and so schema_generator<T>::make) lists them. The
	// schema must come from schema_generator<T> (possibly after a change that keeps its fields and keys).
	template<typename T>
	std::shared_ptr<const schema_binding> bind_described(std::shared_ptr<const type_schema> schema)
	{
		namespace bd = boost::describe;
		auto binding = std::make_shared<schema_binding>();
		binding->schema = std::move(schema);
		binding->object_type = &typeid(T);
		binding->ops.reserve(binding->schema->fields.size());

		const auto& table = field_attrs<T>;
		boost::mp11::mp_for_each<bd::describe_members<T, bd::mod_any_access>>([&](auto descriptor) {
			using descriptor_type = decltype(descriptor);
			if constexpr ((descriptor_type::modifiers & (bd::mod_static | bd::mod_function)) == 0) {
				const std::string_view member{ descriptor_type::name };
				const field_attr_data* attr = table.find(member);
				const std::size_t index = binding->ops.size();
				ASSERT_MSG(index < binding->schema->fields.size() && binding->schema->fields[index].key == member_key(attr, member),
					"The schema does not match the members of '{}' (field {})", binding->schema->name, member);
				field_ops ops = detail::member_ops<T, descriptor_type::pointer>::make(attr);
				ops.kind = binding->schema->fields[index].kind;
				binding->ops.push_back(std::move(ops));
			}
			(void)descriptor;
		});
		ASSERT_MSG(binding->ops.size() == binding->schema->fields.size(), "The schema of '{}' lists {} fields, the type has {}", binding->schema->name, binding->schema->fields.size(), binding->ops.size());
		return binding;
	}

	template<typename T>
	std::shared_ptr<const schema_binding> bind_described(std::string_view name, int version = 1, schema_kind kind = schema_kind::desc)
	{
		return bind_described<T>(std::make_shared<const type_schema>(schema_generator<T>::make(name, version, kind)));
	}

	namespace detail
	{
		template<typename M>
		std::shared_ptr<const schema_binding> bind_nested(std::string_view name)
		{
			return bind_described<M>(name, 1, schema_kind::structure);
		}
	}
}
