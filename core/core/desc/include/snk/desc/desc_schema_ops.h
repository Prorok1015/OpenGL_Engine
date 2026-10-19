#pragma once
#include <snk/common/snk_api.h>
#include <snk/desc/desc_schema.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

// Generic operations over a type_schema (docs/roadmap/design/DESC_SCHEMA.md §2.3). A schema_binding pairs a
// schema with per-field ops that know where a field lives in an object: a member pointer for a C++ type
// (desc_describe_ops.h) or an offset in a runtime blob (bind_runtime). Everything below works on bindings and
// never learns where the schema came from.
namespace desc
{
	struct schema_binding;

	// obj is the address of the whole object (a T* for C++ bindings, a dyn_ref* for runtime ones).
	struct field_ops
	{
		using to_json_fn = void (*)(const field_ops& ops, const void* obj, json::value& out);
		using from_json_fn = bool (*)(const field_ops& ops, void* obj, const json::value& in);
		using equal_fn = bool (*)(const field_ops& ops, const void* a, const void* b);
		using assign_fn = void (*)(const field_ops& ops, void* dst, const void* src);

		to_json_fn to_json = nullptr; // leaves `out` null when the field has nothing to write (custom without a codec)
		from_json_fn from_json = nullptr; // false: the value does not fit the field, the field is left as it was
		equal_fn equal = nullptr;
		assign_fn assign = nullptr;

		field_kind kind = field_kind::custom;
		field_kind element = field_kind::boolean;
		std::uint32_t offset = 0; // runtime bindings
		std::shared_ptr<const enum_schema> enumeration; // enumeration fields (and array elements of enumerations)
		std::shared_ptr<const schema_binding> nested; // structure fields
		std::shared_ptr<const field_ops> element_ops; // C++ array fields
	};

	struct schema_binding
	{
		std::shared_ptr<const type_schema> schema;
		std::vector<field_ops> ops; // parallel to schema->fields
		const std::type_info* object_type = nullptr; // what `obj` points to; two bindings copy member-wise only if it matches
	};

	// A runtime object: bytes laid out by compute_layout plus the table behind string/res_tag/desc_ref/array/custom
	// fields (the blob holds slot+1 as u32, 0 meaning "never set").
	struct dyn_blob
	{
		std::vector<std::byte> bytes;
		std::vector<json::value> handles;
	};

	// Address of a (possibly nested) struct inside a blob.
	struct dyn_ref
	{
		dyn_blob* blob = nullptr;
		std::uint32_t base = 0;
	};

	struct json_change
	{
		std::string pointer;
		json::value value;
	};

	enum class override_status : std::uint8_t { valid, stale, bad_value };

	// `resolve` supplies enum and nested struct schemas by name. Throws std::runtime_error when a structure
	// field's schema is missing. The schema needs its layout (parse_type_schema / compute_layout).
	SNK_API std::shared_ptr<const schema_binding> bind_runtime(type_schema schema, const schema_registry& resolve);

	// A zeroed blob of schema.layout.size bytes with every field set to its default.
	SNK_API dyn_blob make_blob(const schema_binding& binding);

	// Writes every field under its key in `out`, in schema order, honoring the field's write policy. Keys already in
	// `out` stay.
	SNK_API void serialize(const schema_binding& binding, const void* obj, json::object& out);

	// A key (or alias) missing from `in` leaves the field untouched; a value that does not fit logs a warning and
	// leaves the field as it was; keys the schema does not know are ignored (debug log).
	SNK_API void deserialize(const schema_binding& binding, void* obj, const json::object& in);

	// Same schema and object type: member-wise. Otherwise serialize(src) -> deserialize(dst), so only keys both
	// sides know cross over, with a warning.
	SNK_API void copy(const schema_binding& src_binding, const void* src, const schema_binding& dst_binding, void* dst);

	SNK_API bool equal(const schema_binding& binding, const void* a, const void* b);

	// Records for the fields of `schema` whose value differs between `base` and `edited` (numbers within a float
	// tolerance). array and custom fields are compared and recorded whole, structure fields field by field when
	// `resolve` knows the nested schema and key by key otherwise. A field missing from `edited` is not a change.
	SNK_API std::vector<json_change> diff(const type_schema& schema, const json::value& base, const json::value& edited, const schema_registry* resolve = nullptr);

	// The pointer must name a field (or an alias of one), or lead into a structure or array field, and the value
	// must fit the field. An unknown pointer logs a "stale override" warning; the record is never removed.
	SNK_API override_status validate_override(const type_schema& schema, const json_change& record, const schema_registry* resolve = nullptr);

	// Relative tolerance for float32 values (float32 holds ~7 digits); shared by diff and the editor's override edits.
	inline constexpr double float_relative_tolerance = 1e-6;

	// Number-aware JSON comparison; tolerance is relative: |a - b| <= tolerance * max(|a|, |b|).
	SNK_API bool json_equal(const json::value& a, const json::value& b, double tolerance = 0.0);

	// True when `value` can be stored in a field of this kind (enumeration: a name or a number the enum knows).
	SNK_API bool json_fits(field_kind kind, field_kind element, const enum_schema* enumeration, const json::value& value);

	// Enum <-> string through the schema. An unknown name takes the schema's fallback; without one it is nullopt.
	SNK_API std::optional<std::int64_t> enum_from_json(const enum_schema& schema, const json::value& value);
	SNK_API json::value enum_to_json(const enum_schema& schema, std::int64_t value);

	// Any JSON number as a double; an integer or an integral double that fits 64 bits.
	SNK_API bool json_to_double(const json::value& value, double& out);
	SNK_API bool json_to_int64(const json::value& value, std::int64_t& out);
	SNK_API bool json_to_uint64(const json::value& value, std::uint64_t& out);

	// Skipped by write_policy::if_valid: null, empty string, empty array.
	SNK_API bool is_valid_value(const json::value& value);

	// Supplies a JSON codec to custom-kind fields of a C++ type:
	//   template<> struct desc::json_codec<ds::fixed_vector<res::tag, 4>> {
	//       static json::value to_json(const ds::fixed_vector<res::tag, 4>&);
	//       static bool from_json(ds::fixed_vector<res::tag, 4>&, const json::value&);
	//   };
	template<typename T>
	struct json_codec;

	template<typename T>
	concept has_json_codec = requires { sizeof(json_codec<T>); };
}
