#include <snk/render/shader/rnd_material_params.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <type_traits>
#include <glm/gtc/type_ptr.hpp>

namespace {
	using namespace rnd;

	param_type type_of(const param_value& value)
	{
		switch (value.index()) {
		case 0: return param_type::f32;
		case 1: return param_type::vec2;
		case 2: return param_type::vec3;
		default: return param_type::vec4;
		}
	}

	const char* type_name(param_type type)
	{
		switch (type) {
		case param_type::f32: return "float";
		case param_type::vec2: return "vec2";
		case param_type::vec3: return "vec3";
		case param_type::vec4: return "vec4";
		}
		return "?";
	}

	void write_value(std::byte* at, const param_value& value)
	{
		std::visit([at](const auto& v) {
			using value_type = std::decay_t<decltype(v)>;
			if constexpr (std::is_same_v<value_type, float>) {
				std::memcpy(at, &v, sizeof(float));
			} else {
				std::memcpy(at, glm::value_ptr(v), sizeof(value_type));
			}
		}, value);
	}
}

rnd::packed_params rnd::pack_params(std::span<const param_decl> schema, std::size_t block_size, std::span<const named_param> values)
{
	packed_params result;
	if (!is_std140_schema(schema, block_size)) {
		result.error = std::format("the params schema does not fit a std140 block of {} bytes\n", block_size);
		return result;
	}
	result.bytes.assign(block_size, std::byte{ 0 });

	for (const param_decl& decl : schema) {
		const auto found = std::find_if(values.begin(), values.end(), [&](const named_param& v) { return v.name == decl.name; });
		if (found == values.end()) {
			result.error += std::format("no value for '{}'\n", decl.name);
		} else if (type_of(found->value) != decl.type) {
			result.error += std::format("'{}' is a {} in the schema, the value is a {}\n", decl.name, type_name(decl.type), type_name(type_of(found->value)));
		} else {
			write_value(result.bytes.data() + decl.offset, found->value);
		}
	}
	for (const named_param& value : values) {
		const bool declared = std::any_of(schema.begin(), schema.end(), [&](const param_decl& d) { return d.name == value.name; });
		if (!declared) {
			result.error += std::format("'{}' is not in the schema\n", value.name);
		}
	}
	return result;
}
