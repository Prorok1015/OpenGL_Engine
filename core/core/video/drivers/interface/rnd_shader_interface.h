#pragma once
#include <glm/glm.hpp>
#include <string_view>
#include <variant>

namespace rnd::driver
{
	using uniform_data = std::variant<glm::mat4, glm::vec4, glm::vec3, glm::vec2, int, float>;

	class shader_interface
	{
	public:
		virtual ~shader_interface() {}

		virtual void uniform(std::string_view name, const uniform_data& val) = 0;
		virtual void use() = 0;
	};
}