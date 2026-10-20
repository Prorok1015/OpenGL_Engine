#pragma once
#include <snk/common/snk_api.h>
#include <snk/resource/res_tag.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <boost/json/value.hpp>
#include <glm/fwd.hpp>

namespace json = boost::json;

// Runtime description of a desc type (ADR-04, docs/roadmap/design/DESC_SCHEMA.md). The same structures
// come from Boost.Describe (desc_describe.h) for C++ types and from parse_type_schema for wasm/JSON types.
namespace desc
{
	// std::hash is not stable across platforms/STLs; type ids end up in saved data.
	constexpr std::uint64_t schema_id(std::string_view name) noexcept
	{
		std::uint64_t h = 14695981039346656037ull;
		for (unsigned char c : name) {
			h ^= c;
			h *= 1099511628211ull;
		}
		return h;
	}

	// Vectors are arrays of numbers, quat is [x, y, z, w], node_ref is a 16-char hex string.
	enum class field_kind : std::uint8_t
	{
		boolean, i32, u32, i64, u64, f32, f64,
		vec2, vec3, vec4, quat, color,
		string, enumeration, res_tag, desc_ref, node_ref, entity,
		structure, array, custom,
	};

	// always: written as is. if_valid: skipped when empty (null, "", []). if_not_default: skipped when equal to the
	// default. null_if_empty: written as is, but an empty value (a null-mapped enumerator, see enum_schema::null_value)
	// becomes an explicit JSON null (and a null reads back as the empty value). never: a read-only key (a legacy key
	// that is still read but never written back); the member is still copied and compared, but as it is not in the
	// serialized JSON it is not diffed or overridden, and a copy between different types (through JSON) skips it.
	enum class write_policy : std::uint8_t { always, if_valid, if_not_default, null_if_empty, never };

	enum class schema_kind : std::uint8_t { desc, component, structure };

	SNK_API std::string_view to_string(field_kind kind);
	SNK_API std::string_view to_string(write_policy policy);
	SNK_API std::string_view to_string(schema_kind kind);
	SNK_API std::optional<field_kind> field_kind_from_string(std::string_view text);
	SNK_API std::optional<write_policy> write_policy_from_string(std::string_view text);
	SNK_API std::optional<schema_kind> schema_kind_from_string(std::string_view text);

	struct field_range
	{
		double min = 0.0;
		double max = 0.0;
		double step = 0.0;
		bool operator==(const field_range&) const = default;
	};

	struct blob_layout
	{
		std::uint32_t size = 0;
		std::uint32_t alignment = 1;
		bool operator==(const blob_layout&) const = default;
	};

	struct field_schema
	{
		std::string name;
		std::string key;
		field_kind kind = field_kind::boolean;
		field_kind element = field_kind::boolean; // meaningful for field_kind::array only
		std::string ref_type; // enum_schema / type_schema name for enumeration, structure, desc_ref
		json::value default_value;
		std::optional<field_range> range;
		write_policy write = write_policy::always;
		std::vector<std::string> read_aliases;
		bool optional = false;
		bool editor_hidden = false;
		bool read_only = false;
		bool color = false;
		bool degrees = false;
		std::string tooltip;
		std::string filter;
		std::uint32_t offset = 0; // in the blob; runtime schemas only, not part of equality
	};

	struct type_schema
	{
		std::string name;
		std::uint64_t id = 0;
		int version = 1;
		schema_kind kind = schema_kind::desc;
		bool is_component = false;
		bool is_document = false;
		std::vector<field_schema> fields;
		blob_layout layout; // runtime schemas only, not part of equality

		const field_schema* find_field(std::string_view key) const
		{
			for (const auto& field : fields) {
				if (field.key == key) {
					return &field;
				}
			}
			return nullptr;
		}
	};

	// Equality compares what the schema says about the data; offsets and the blob layout are storage details.
	inline bool operator==(const field_schema& a, const field_schema& b)
	{
		return a.name == b.name && a.key == b.key && a.kind == b.kind && a.element == b.element
			&& a.ref_type == b.ref_type && a.default_value == b.default_value && a.range == b.range
			&& a.write == b.write && a.read_aliases == b.read_aliases && a.optional == b.optional
			&& a.editor_hidden == b.editor_hidden && a.read_only == b.read_only && a.color == b.color
			&& a.degrees == b.degrees && a.tooltip == b.tooltip && a.filter == b.filter;
	}

	inline bool operator==(const type_schema& a, const type_schema& b)
	{
		return a.name == b.name && a.id == b.id && a.version == b.version && a.kind == b.kind
			&& a.is_component == b.is_component && a.is_document == b.is_document && a.fields == b.fields;
	}

	struct enum_item
	{
		std::string name;
		std::int64_t value = 0;
		bool operator==(const enum_item&) const = default;
	};

	struct enum_schema
	{
		std::string name;
		std::vector<enum_item> items;
		std::optional<std::int64_t> fallback; // value for an unknown string
		std::optional<std::int64_t> null_value; // the enumerator that stands for a JSON null: read from null, written as null by write_policy::null_if_empty
		bool operator==(const enum_schema&) const = default;

		const enum_item* find(std::string_view text) const
		{
			for (const auto& item : items) {
				if (item.name == text) {
					return &item;
				}
			}
			return nullptr;
		}

		const enum_item* find_value(std::int64_t value) const
		{
			for (const auto& item : items) {
				if (item.value == value) {
					return &item;
				}
			}
			return nullptr;
		}
	};

	class SNK_API schema_registry
	{
	public:
		// Both add functions return nullptr and keep the registry unchanged when the name (or the id) is taken.
		const type_schema* add(type_schema schema);
		bool remove(std::string_view name);
		// Keeps a stored schema in step with current_version(name); false if there is no such schema.
		bool set_version(std::string_view name, int version);
		const type_schema* find(std::string_view name) const;
		const type_schema* find(std::uint64_t id) const;
		bool contains(std::string_view name) const;

		const enum_schema* add_enum(enum_schema schema);
		bool remove_enum(std::string_view name);
		const enum_schema* find_enum(std::string_view name) const;

		std::size_t size() const { return m_types.size(); }
		std::vector<std::string> type_names() const;

	private:
		// node-based maps: returned pointers stay valid until the entry is removed
		std::unordered_map<std::string, type_schema> m_types;
		std::unordered_map<std::uint64_t, std::string> m_names_by_id;
		std::unordered_map<std::string, enum_schema> m_enums;
	};

	// Size and alignment of one field in a runtime blob (C layout, no pointers): string, res_tag,
	// desc_ref, array and custom are a u32 index into the handle table; structure is resolved by
	// compute_layout from the nested schema.
	SNK_API blob_layout layout_of(field_kind kind);

	// Fills type_schema::layout and field_schema::offset in declaration order. Throws
	// std::runtime_error for a structure field whose schema is not in `resolve`.
	SNK_API void compute_layout(type_schema& schema, const schema_registry* resolve = nullptr);

	// {"name": "mymod.health", "version": 1, "kind": "component",
	//  "fields": [{"name": "hp", "key": "/hp", "kind": "f32", "default": 100, "range": [0, 1000]}]}
	// Field keys not listed in the design table are rejected as typos. Defaults are normalized to the
	// representation the C++ generator produces (floats as double, unsigned as uint64). Computes the layout.
	// Throws std::runtime_error with the offending path on malformed input.
	SNK_API type_schema parse_type_schema(const json::object& object, const schema_registry* resolve = nullptr);

	// {"name": "pass_queue", "items": {"opaque": 0, "transparent": 1}, "fallback": 0}
	SNK_API enum_schema parse_enum_schema(const json::object& object);

	template<typename T>
	concept is_described = requires { boost_public_member_descriptor_fn(static_cast<T**>(nullptr)); };

	// Defined in desc_describe.h. Registering a described type through desc_system needs that header.
	template<typename T>
	struct schema_generator;

	template<typename T>
	concept has_schema_generator = requires { sizeof(schema_generator<T>); };

	namespace detail
	{
		template<typename T>
		struct sequence_traits
		{
			static constexpr bool value = false;
		};

		template<typename U, typename A>
		struct sequence_traits<std::vector<U, A>>
		{
			static constexpr bool value = true;
			using element = U;
		};

		template<typename U, std::size_t N>
		struct sequence_traits<std::array<U, N>>
		{
			static constexpr bool value = true;
			using element = U;
		};

		template<typename T>
		constexpr field_kind deduce_kind() noexcept
		{
			if constexpr (std::is_same_v<T, bool>) {
				return field_kind::boolean;
			} else if constexpr (std::is_integral_v<T>) {
				if constexpr (sizeof(T) == 4) {
					return std::is_signed_v<T> ? field_kind::i32 : field_kind::u32;
				} else if constexpr (sizeof(T) == 8) {
					return std::is_signed_v<T> ? field_kind::i64 : field_kind::u64;
				} else {
					return field_kind::custom;
				}
			} else if constexpr (std::is_same_v<T, float>) {
				return field_kind::f32;
			} else if constexpr (std::is_same_v<T, double>) {
				return field_kind::f64;
			} else if constexpr (std::is_enum_v<T>) {
				return field_kind::enumeration;
			} else if constexpr (std::is_same_v<T, glm::vec2>) {
				return field_kind::vec2;
			} else if constexpr (std::is_same_v<T, glm::vec3>) {
				return field_kind::vec3;
			} else if constexpr (std::is_same_v<T, glm::vec4>) {
				return field_kind::vec4;
			} else if constexpr (std::is_same_v<T, glm::quat>) {
				return field_kind::quat;
			} else if constexpr (std::is_same_v<T, std::string>) {
				return field_kind::string;
			} else if constexpr (std::is_same_v<T, res::tag>) {
				return field_kind::res_tag;
			} else if constexpr (sequence_traits<T>::value) {
				return field_kind::array;
			} else if constexpr (is_described<T>) {
				return field_kind::structure;
			} else {
				return field_kind::custom;
			}
		}
	}

	// Kind a C++ member type maps to. Types it does not know (e.g. ds::fixed_vector, res_handle) are
	// custom; a field_attrs entry can override the kind (desc_hints.hpp).
	template<typename T>
	inline constexpr field_kind kind_of = detail::deduce_kind<std::remove_cvref_t<T>>();

	template<typename T>
	constexpr field_kind element_kind_of() noexcept
	{
		using traits = detail::sequence_traits<std::remove_cvref_t<T>>;
		if constexpr (traits::value) {
			return kind_of<typename traits::element>;
		} else {
			return field_kind::boolean;
		}
	}
}
