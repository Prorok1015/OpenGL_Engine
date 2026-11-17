#pragma once
#include <snk/driver/rnd_buffer_layout.h>
#include <glm/glm.hpp>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <vector>

// #1100: one vertex layout for the whole geometry of an imported model, the union of the attributes of the meshes it
// holds. A mesh without one of them gets its default here, so nothing a model file holds can make a mesh's vertices
// disagree with the layout. For the model import (core/model_import, #1108) only: not part of the module API.
namespace scn::import_layout
{
	struct vertex_attributes
	{
		bool normal = false;
		bool uv = false;
		bool tangents = false; // tangent and bitangent
		bool color = false;    // no importer reads vertex colors yet; the default keeps such a mesh from going black

		vertex_attributes& operator|=(const vertex_attributes& other) noexcept
		{
			normal = normal || other.normal;
			uv = uv || other.uv;
			tangents = tangents || other.tangents;
			color = color || other.color;
			return *this;
		}
	};

	// A zero normal is NaN after normalize() in the shader.
	inline const glm::vec3 default_normal{ 0.f, 0.f, 1.f };
	inline const glm::vec2 default_uv{ 0.f, 0.f };
	inline const glm::vec4 default_color{ 1.f, 1.f, 1.f, 1.f };

	struct tangent_frame
	{
		glm::vec3 tangent;
		glm::vec3 bitangent;
	};

	// A mesh without tangents is one without uvs, which is normal: any unit tangent perpendicular to the vertex normal,
	// bitangent = cross(normal, tangent). For default_normal it is (1, 0, 0) and (0, 1, 0).
	inline tangent_frame default_tangents(const glm::vec3& normal)
	{
		const float length = glm::length(normal);
		const glm::vec3 n = length > 1e-6f ? normal / length : default_normal;
		const glm::vec3 reference = std::abs(n.y) < 0.9f ? glm::vec3{ 0.f, 1.f, 0.f } : glm::vec3{ 1.f, 0.f, 0.f };
		const glm::vec3 tangent = glm::normalize(glm::cross(reference, n));
		return { tangent, glm::cross(n, tangent) };
	}

	inline rnd::driver::BufferLayout layout_of(const vertex_attributes& attributes)
	{
		using rnd::driver::SHADER_DATA_TYPE;
		std::vector<rnd::driver::BufferElement> elements;
		elements.push_back({ SHADER_DATA_TYPE::VEC3_F, "position" });
		if (attributes.normal) elements.push_back({ SHADER_DATA_TYPE::VEC3_F, "normal" });
		if (attributes.uv) elements.push_back({ SHADER_DATA_TYPE::VEC2_F, "uv" });
		if (attributes.tangents) {
			elements.push_back({ SHADER_DATA_TYPE::VEC3_F, "tangent" });
			elements.push_back({ SHADER_DATA_TYPE::VEC3_F, "bitangent" });
		}
		if (attributes.color) elements.push_back({ SHADER_DATA_TYPE::VEC4_F, "color" });
		return rnd::driver::BufferLayout{ elements };
	}

	// The attributes of a layout made by layout_of.
	inline vertex_attributes attributes_of(const rnd::driver::BufferLayout& layout)
	{
		vertex_attributes out;
		for (const auto& element : layout) {
			if (element.Name == "normal") out.normal = true;
			else if (element.Name == "uv") out.uv = true;
			else if (element.Name == "tangent") out.tangents = true;
			else if (element.Name == "color") out.color = true;
		}
		return out;
	}

	// One vertex of a mesh: what the mesh has; an empty one takes its default when the layout has the attribute.
	struct vertex_values
	{
		glm::vec3 position{ 0.f };
		std::optional<glm::vec3> normal;
		std::optional<glm::vec2> uv;
		std::optional<glm::vec3> tangent;
		std::optional<glm::vec3> bitangent;
		std::optional<glm::vec4> color;
	};

	namespace detail
	{
		template<class T>
		void append(std::vector<std::byte>& out, const T& value)
		{
			const std::size_t begin = out.size();
			out.resize(begin + sizeof(T));
			std::memcpy(out.data() + begin, &value, sizeof(T));
		}
	}

	// Appends exactly layout_of(layout).get_stride() bytes, in the order of layout_of. A value the layout has no place for
	// is not written.
	inline void append_vertex(std::vector<std::byte>& out, const vertex_attributes& layout, const vertex_values& vertex)
	{
		detail::append(out, vertex.position);
		if (layout.normal) detail::append(out, vertex.normal.value_or(default_normal));
		if (layout.uv) detail::append(out, vertex.uv.value_or(default_uv));
		if (layout.tangents) {
			const tangent_frame fallback = vertex.tangent && vertex.bitangent
				? tangent_frame{ *vertex.tangent, *vertex.bitangent }
				: default_tangents(vertex.normal.value_or(default_normal));
			detail::append(out, vertex.tangent.value_or(fallback.tangent));
			detail::append(out, vertex.bitangent.value_or(fallback.bitangent));
		}
		if (layout.color) detail::append(out, vertex.color.value_or(default_color));
	}
}
