#pragma once
// desc_system.h (included below, desc references call get_field_desc) uses these without including them.
#include <algorithm>
#include <cstring>
#include <format>
#include <sstream>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_describe.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_reference.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <vector>
#include <boost/json/value.hpp>

// field_ops and schema_binding for C++ types described with Boost.Describe. Heavy like desc_describe.h: include it
// only where a described type is declared or registered.
namespace desc
{
	// field_kind::desc_ref for a res::res_handle<T> member (set the kind with field_attr::kind, the handle itself is a
	// custom type to the deduction). Reads what desc_system::get_field_desc<T> reads: a string tag, a {guid, path}
	// reference (res::reference_from_json, #758), or an inline object with "__type" that is stored once under a
	// deterministic memory:// tag of the owner and required from there.
	// An empty string clears the handle (so does a null on a field with write_null_if_empty). Written as the tag of the
	// handle, an empty string while it is not set (so write_if_valid() leaves the key out). A handle whose tag is one of
	// those memory copies (desc::is_override_tag) is written as the inline object again (the loaded desc's own serialize plus
	// "__type"): the copy lives in the memory store of this process only, and a saved tag would not resolve in the next
	// one. A value that is neither a reference nor an object with "__type", a read without a desc_system and owner in the
	// context, or an inline object of an owner without a tag, does not fit: the field keeps its handle. Only the
	// handle itself is a desc_ref: the elements of a vector of handles are custom fields.
	template<typename T>
	struct desc_ref_traits<res::res_handle<T>>
	{
		static bool from_json(res::res_handle<T>& handle, const json::value& in, const read_context& ctx)
		{
			if (in.is_string() && in.as_string().empty()) {
				handle = res::res_handle<T>{};
				return true;
			}
			bool inline_desc = false;
			if (in.is_object()) {
				const json::value* type = in.as_object().if_contains("__type");
				inline_desc = type && type->is_string();
			}
			if (!inline_desc && !res::reference_from_json(in)) {
				return false;
			}
			if (!ctx.system || !ctx.owner) {
				egLOG_WARN("desc", "a desc reference needs the desc_system and the owner desc to be read, keeping the current one");
				return false;
			}
			if (inline_desc && ctx.owner->get_tag().view().empty()) {
				egLOG_WARN("desc", "an inline desc needs an owner with a tag to name its memory copy, keeping the current reference");
				return false;
			}
			handle = ctx.system->get_field_desc<T>(*ctx.owner, in, ctx.field_pointer);
			return true;
		}

		// Main thread only: the inline memory copy is read with get_sync (it waits for the load).
		static json::value to_json(const res::res_handle<T>& handle)
		{
			// The resource system finds the desc adapter through T::base_type.
			static_assert(requires { typename T::base_type; }, "a desc_ref target must declare 'using base_type = desc::desc_base;'");
			if (!handle.is_valid()) {
				return json::value(std::string_view{});
			}
			const std::string_view tag = handle.get_tag().view();
			// A failed handle keeps its tag; get_sync would log an error on every save.
			if (desc::is_override_tag(tag) && !handle.has_error()) { // see desc_system::make_mem_tag_deterministic
				if (const auto instance = handle.get_sync()) {
					json::object inline_desc;
					instance->serialize(inline_desc);
					if (!instance->get_type().empty()) {
						inline_desc["__type"] = std::string{ instance->get_type() };
					}
					return json::value(std::move(inline_desc));
				}
			}
			return json::value(tag);
		}
	};

	namespace detail
	{
		template<typename M>
		field_ops make_value_context(const field_attr_data* attr);

		// The codec's read, with the context when the codec takes one.
		template<typename M>
		bool codec_from_json(M& value, const json::value& in, const read_context& ctx)
		{
			if constexpr (has_contextual_json_codec<M>) {
				return json_codec<M>::from_json(value, in, ctx);
			} else {
				(void)ctx;
				return json_codec<M>::from_json(value, in);
			}
		}

		template<typename M>
		std::shared_ptr<const schema_binding> bind_nested(std::string_view name);

		template<typename M>
		struct value_ops
		{
			static void to_json(const field_ops& ops, const M& value, json::value& out)
			{
				constexpr field_kind natural = kind_of<M>;
				if constexpr (has_desc_ref_traits<M>) {
					if (ops.kind == field_kind::desc_ref) {
						out = desc_ref_traits<M>::to_json(value);
						return;
					}
				}
				if constexpr (has_json_codec<M>) {
					if (ops.kind == field_kind::custom) {
						out = json_codec<M>::to_json(value);
						return;
					}
				}
				if constexpr (natural == field_kind::entity) {
					const auto raw = entity_traits<M>::to_raw(value);
					out = raw ? json::value(*raw) : json::value(nullptr);
				} else if constexpr (natural == field_kind::enumeration) {
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

			static bool from_json(const field_ops& ops, M& value, const json::value& in, const read_context& ctx)
			{
				constexpr field_kind natural = kind_of<M>;
				if constexpr (has_desc_ref_traits<M>) {
					if (ops.kind == field_kind::desc_ref) {
						if (in.is_null() && ops.accepts_null) {
							value = M{};
							return true;
						}
						return desc_ref_traits<M>::from_json(value, in, ctx);
					}
				}
				if constexpr (has_json_codec<M>) {
					if (ops.kind == field_kind::custom) {
						return codec_from_json<M>(value, in, ctx);
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
					if (in.is_null() && ops.accepts_null) {
						value = std::string{};
						return true;
					}
					if (!in.is_string()) {
						return false;
					}
					value = std::string{ in.as_string() };
					return true;
				} else if constexpr (natural == field_kind::res_tag) {
					if (in.is_null() && ops.accepts_null) {
						value = res::tag{};
						return true;
					}
					const auto reference = reference_tag(in, ctx);
					if (!reference) {
						return false;
					}
					value = *reference;
					if (value.is_valid() && value.protocol().empty()) {
						// Kept as written, so a save does not change the file; nothing can load it (#631).
						::res::report_load_warning(::res::diagnostic_kind::schema, ctx.field_pointer, ::res::no_protocol_reason(value));
					}
					const std::string_view owner_protocol = ctx.owner ? ctx.owner->get_tag().protocol() : std::string_view{};
					if (std::string why = ::res::reference_problem(value, owner_protocol); !why.empty()) {
						// Emptied, so whatever loads the field later (a renderer) is never handed the tag (#697).
						::res::report_load_warning(::res::diagnostic_kind::reference, ctx.field_pointer, std::move(why));
						value = res::tag{};
					}
					return true;
				} else if constexpr (natural == field_kind::entity) {
					if (in.is_null()) {
						value = entity_traits<M>::empty();
						return true;
					}
					std::uint64_t raw = 0;
					if (!json_to_uint64(in, raw) || raw > std::numeric_limits<std::uint32_t>::max()) {
						return false;
					}
					if (!ctx.entities) {
						egLOG_WARN("desc", "an entity reference is read only when a snapshot is restored, keeping the current one");
						return false;
					}
					const auto restored = ctx.entities->find(static_cast<std::uint32_t>(raw));
					value = restored ? entity_traits<M>::from_raw(*restored) : entity_traits<M>::empty();
					return true;
				} else if constexpr (natural == field_kind::enumeration) {
					if (in.is_null() && ops.accepts_null && ops.enumeration && ops.enumeration->null_value) {
						value = static_cast<M>(*ops.enumeration->null_value);
						return true;
					}
					const auto converted = ops.enumeration ? enum_from_json(*ops.enumeration, in) : std::nullopt;
					if (!converted) {
						return false;
					}
					value = static_cast<M>(*converted);
					return true;
				} else if constexpr (natural == field_kind::array) {
					using element_type = typename detail::sequence_traits<M>::element;
					if (in.is_null() && ops.accepts_null) {
						value = M{};
						return true;
					}
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
						if (!value_ops<element_type>::from_json(*ops.element_ops, element, item, ctx)) {
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
					deserialize(*ops.nested, &value, in.as_object(), ctx);
					return true;
				} else {
					if constexpr (has_json_codec<M>) {
						return codec_from_json<M>(value, in, ctx);
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

		// An optional is its value or nothing (field_schema::optional): nothing is not written, a JSON null reads as nothing,
		// the value goes through the ops of its own kind, which are the field's ops.
		template<typename T>
			requires (!has_json_codec<std::optional<T>>)
		struct value_ops<std::optional<T>>
		{
			static_assert(std::is_default_constructible_v<T>, "a described std::optional<T> needs a default constructible T");

			static void to_json(const field_ops& ops, const std::optional<T>& value, json::value& out)
			{
				if (!value) {
					out = json::value();
					return;
				}
				value_ops<T>::to_json(ops, *value, out);
			}

			static bool from_json(const field_ops& ops, std::optional<T>& value, const json::value& in, const read_context& ctx)
			{
				if (in.is_null()) {
					value.reset();
					return true;
				}
				T parsed = value ? *value : T{};
				if (!value_ops<T>::from_json(ops, parsed, in, ctx)) {
					return false;
				}
				value = std::move(parsed);
				return true;
			}

			static bool equal(const field_ops& ops, const std::optional<T>& a, const std::optional<T>& b)
			{
				if (a.has_value() != b.has_value()) {
					return false;
				}
				return !a || value_ops<T>::equal(ops, *a, *b);
			}

			static void assign(std::optional<T>& dst, const std::optional<T>& src)
			{
				if constexpr (std::is_copy_assignable_v<T> && std::is_copy_constructible_v<T>) {
					dst = src;
				}
			}
		};

		// A map described as a field_kind::map (no json_codec of its own): an object, a value per key by the ops of the
		// element. Written by key, so an unordered_map has one text. Read by key too, like every object that overrides a
		// parent's (json_deep_merge in the editor's documents and override records): the keys of the file are put into
		// what the map holds, and a null value removes the key. A key the member initializer puts there comes back at every
		// load: only a null in the file removes it. A value that does not fit leaves the field as it was.
		template<typename M>
			requires (detail::string_map_traits<M>::value && kind_of<M> == field_kind::map)
		struct value_ops<M>
		{
			using mapped_type = typename detail::string_map_traits<M>::mapped_type;
			static_assert(!detail::optional_traits<mapped_type>::value,
				"a map of std::optional<T> cannot be described: an empty value is written as null, which a read takes for a removed key");

			static void to_json(const field_ops& ops, const M& value, json::value& out)
			{
				std::vector<const typename M::value_type*> entries;
				entries.reserve(value.size());
				for (const auto& entry : value) {
					entries.push_back(&entry);
				}
				std::ranges::sort(entries, {}, [](const typename M::value_type* entry) { return entry->first; });
				json::object members;
				for (const auto* entry : entries) {
					json::value converted;
					value_ops<mapped_type>::to_json(*ops.element_ops, entry->second, converted);
					members.emplace(entry->first, std::move(converted));
				}
				out = json::value(std::move(members));
			}

			static bool from_json(const field_ops& ops, M& value, const json::value& in, const read_context& ctx)
			{
				if (in.is_null() && ops.accepts_null) {
					value.clear();
					return true;
				}
				if (!in.is_object()) {
					return false;
				}
				M parsed = value;
				for (const auto& member : in.as_object()) {
					if (member.key().empty()) {
						return false;
					}
					const std::string key{ member.key() };
					if (member.value().is_null()) {
						parsed.erase(key);
						continue;
					}
					const auto existing = parsed.find(key);
					mapped_type element = existing != parsed.end() ? existing->second : mapped_type{};
					if (!value_ops<mapped_type>::from_json(*ops.element_ops, element, member.value(), ctx)) {
						return false;
					}
					parsed.insert_or_assign(key, std::move(element));
				}
				value = std::move(parsed);
				return true;
			}

			static bool equal(const field_ops& ops, const M& a, const M& b)
			{
				if (a.size() != b.size()) {
					return false;
				}
				for (const auto& entry : a) {
					const auto other = b.find(entry.first);
					if (other == b.end() || !value_ops<mapped_type>::equal(*ops.element_ops, entry.second, other->second)) {
						return false;
					}
				}
				return true;
			}

			static void assign(M& dst, const M& src)
			{
				dst = src;
			}
		};

		template<typename M>
		field_ops make_value_context(const field_attr_data* attr)
		{
			if constexpr (detail::optional_traits<M>::value) {
				return make_value_context<typename detail::optional_traits<M>::value_type>(attr);
			} else {
				field_ops ops;
				ops.kind = kind_of<M>;
				ops.element = element_kind_of<M>();
				if constexpr (kind_of<M> == field_kind::enumeration) {
					ops.enumeration = std::make_shared<const enum_schema>(make_enum_schema<M>(attr ? attr->ref : std::string_view{}));
				} else if constexpr (kind_of<M> == field_kind::array) {
					ops.element_ops = std::make_shared<const field_ops>(make_value_context<typename detail::sequence_traits<M>::element>(nullptr));
				} else if constexpr (kind_of<M> == field_kind::map) {
					ops.element_ops = std::make_shared<const field_ops>(make_value_context<typename detail::string_map_traits<M>::mapped_type>(nullptr));
				} else if constexpr (kind_of<M> == field_kind::structure) {
					ops.nested = bind_nested<M>(attr && !attr->ref.empty() ? attr->ref : std::string_view{ "struct" });
				}
				return ops;
			}
		}

		template<typename T, auto Pointer>
		struct member_ops
		{
			using member_type = std::remove_cvref_t<decltype(std::declval<T&>().*Pointer)>;

			static void to_json(const field_ops& ops, const void* obj, json::value& out)
			{
				value_ops<member_type>::to_json(ops, static_cast<const T*>(obj)->*Pointer, out);
			}

			static bool from_json(const field_ops& ops, void* obj, const json::value& in, const read_context& ctx)
			{
				return value_ops<member_type>::from_json(ops, static_cast<T*>(obj)->*Pointer, in, ctx);
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
				ops.accepts_null = binding->schema->fields[index].write == write_policy::null_if_empty;
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
