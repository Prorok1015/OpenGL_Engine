#pragma once
#include <snk/common/snk_api.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <glm/glm.hpp>

namespace rnd
{
	// The std140 block MaterialParams of set 2, slot 0 (core/res/shaders/phong_model_lib.frag). The material
	// compiler turns a material into these bytes; the bind group of the material points at a buffer holding them.
	struct material_params
	{
		glm::vec4 diffuse_color{ 1.0f };
		glm::vec4 emissive_color{ 0.0f };
		float shininess = 32.0f;
		float pad0[3]{};	// the block is rounded up to a vec4
	};

	static_assert(offsetof(material_params, diffuse_color) == 0);
	static_assert(offsetof(material_params, emissive_color) == 16);
	static_assert(offsetof(material_params, shininess) == 32);
	static_assert(sizeof(material_params) == 48);
	static_assert(alignof(material_params) <= 16);

	enum class param_type : std::uint8_t { f32, vec2, vec3, vec4 };

	// std140 base alignment and size in bytes. A vec3 aligns like a vec4 but is 12 bytes long, so a float can follow it.
	constexpr std::uint32_t param_alignment(param_type type) noexcept
	{
		constexpr std::uint32_t alignments[] = { 4, 8, 16, 16 };
		return alignments[static_cast<std::size_t>(type)];
	}

	constexpr std::uint32_t param_size(param_type type) noexcept
	{
		constexpr std::uint32_t sizes[] = { 4, 8, 12, 16 };
		return sizes[static_cast<std::size_t>(type)];
	}

	// One member of the block, listed by hand: there is no reflection of the GLSL source yet (#123).
	struct param_decl
	{
		std::string_view name;
		param_type type = param_type::f32;
		std::uint32_t offset = 0;
	};

	// True when the members sit where std140 puts them: each at a multiple of its alignment, inside the block,
	// none on another, and the block a multiple of 16 bytes.
	constexpr bool is_std140_schema(std::span<const param_decl> schema, std::size_t block_size) noexcept
	{
		if (block_size == 0 || block_size % 16 != 0) {
			return false;
		}
		for (std::size_t i = 0; i < schema.size(); ++i) {
			const param_decl& a = schema[i];
			if (a.offset % param_alignment(a.type) != 0 || a.offset + param_size(a.type) > block_size) {
				return false;
			}
			for (std::size_t j = i + 1; j < schema.size(); ++j) {
				const param_decl& b = schema[j];
				const bool apart = a.offset + param_size(a.type) <= b.offset || b.offset + param_size(b.type) <= a.offset;
				if (!apart || a.name == b.name) {
					return false;
				}
			}
		}
		return true;
	}

	// The params of the phong shaders (scene, z_prepass, mix_opaque_trans_scene): what MaterialParams holds.
	inline constexpr param_decl phong_params[] = {
		{ "diffuseColor", param_type::vec4, offsetof(material_params, diffuse_color) },
		{ "emissiveColor", param_type::vec4, offsetof(material_params, emissive_color) },
		{ "shininess", param_type::f32, offsetof(material_params, shininess) },
	};

	static_assert(is_std140_schema(phong_params, sizeof(material_params)));

	using param_value = std::variant<float, glm::vec2, glm::vec3, glm::vec4>;

	struct named_param
	{
		std::string_view name;
		param_value value;
	};

	struct packed_params
	{
		std::vector<std::byte> bytes;
		std::string error;	// empty when every member of the schema got a value of its type

		bool ok() const noexcept { return error.empty(); }
	};

	// Writes `values` into a zeroed block of `block_size` bytes at the offsets the schema gives. A schema member
	// without a value, a value of another type or a value no member asks for is an error: the block would not
	// hold what the shader reads.
	SNK_API packed_params pack_params(std::span<const param_decl> schema, std::size_t block_size, std::span<const named_param> values);
}
