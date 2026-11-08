#pragma once
#include <snk/desc/desc_describe_ops.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_schema_ops.h>
#include <snk/ecs/ecs_channel.hpp>
#include <boost/describe/class.hpp>
#include <boost/json/array.hpp>
#include <boost/json/value.hpp>
#include <entt/entity/entity.hpp>
#include <glm/mat4x4.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>

// What a schema of a runtime component needs beyond the desc kinds (HOT_RELOAD_PLAY.md section 4.2): entt::entity
// members, glm::mat4 members and ecs::entity_ref. Include it wherever a component is described: the schema of a type
// is generated in every translation unit that uses it, and an entity member read without this header would be taken
// for an enumeration.
//
// A component is described from outside (BOOST_DESCRIBE_STRUCT next to the schema, in the component's namespace):
// the component itself gets no new member, base or virtual function, so its layout and behaviour do not change.

namespace desc
{
	// Snapshots carry an entity as its 32-bit raw id. A runtime (blob) schema stores field_kind::entity as a u32 too, but
	// snapshots never go through a blob: only C++ components are captured, through these traits.
	static_assert(sizeof(entt::entity) == sizeof(std::uint32_t), "a snapshot writes an entity as its 32-bit raw id");

	template<>
	struct entity_traits<entt::entity>
	{
		static std::optional<std::uint32_t> to_raw(entt::entity entity)
		{
			if (entity == entt::null) {
				return std::nullopt;
			}
			return static_cast<std::uint32_t>(entt::to_integral(entity));
		}

		static entt::entity from_raw(std::uint32_t raw) { return static_cast<entt::entity>(raw); }
		static entt::entity empty() { return entt::null; }
	};

	// Sixteen numbers, column by column (glm's own order), so the value survives a round trip bit for bit when it is
	// read back as float.
	template<>
	struct json_codec<glm::mat4>
	{
		static json::value to_json(const glm::mat4& matrix)
		{
			json::array numbers;
			numbers.reserve(16);
			for (int column = 0; column < 4; ++column) {
				for (int row = 0; row < 4; ++row) {
					numbers.push_back(static_cast<double>(matrix[column][row]));
				}
			}
			return json::value(std::move(numbers));
		}

		static bool from_json(glm::mat4& matrix, const json::value& in)
		{
			if (!in.is_array() || in.as_array().size() != 16) {
				return false;
			}
			glm::mat4 parsed{};
			for (std::size_t i = 0; i < 16; ++i) {
				double number = 0.0;
				if (!json_to_double(in.as_array()[i], number)) {
					return false;
				}
				parsed[static_cast<int>(i / 4)][static_cast<int>(i % 4)] = static_cast<float>(number);
			}
			matrix = parsed;
			return true;
		}
	};
}

namespace ecs
{
	BOOST_DESCRIBE_STRUCT(entity_ref, (), (context, path))
}
