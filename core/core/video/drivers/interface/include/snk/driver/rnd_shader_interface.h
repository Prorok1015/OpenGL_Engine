#pragma once
#include <snk/common/shader_serial.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <string_view>
#include <variant>

namespace rnd::driver
{
	using uniform_data = std::variant<glm::mat4, glm::vec4, glm::vec3, glm::vec2, glm::ivec4, int, float>;

	class shader_interface
	{
	public:
		virtual ~shader_interface() {}

		// Names this shader for as long as the process runs. Unlike its address it is never handed out again,
		// so a pipeline cannot take a new shader for an old one that lived at the same place.
		std::uint64_t serial() const noexcept { return m_serial; }

		// Sets a uniform of the program the bound pipeline uses; the program is selected by bind_pipeline.
		virtual void uniform(std::string_view name, const uniform_data& val) = 0;

	private:
		const std::uint64_t m_serial = next_shader_serial();
	};
}