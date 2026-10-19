#pragma once
#include <snk/driver/rnd_bind_group.h>

#include <cstddef>
#include <cstdint>
#include <glm/mat4x4.hpp>

// What the engine's shaders read from bind groups. The numbers are the (set, slot) of SNK_UBO/SNK_SSBO/SNK_TEX in
// core/res/shaders/snk_common.glsl and the shaders that include it; the backend's binding table turns them into
// the numbers of its API.
namespace rnd::bindings
{
	// Set 0, frame and view: uniform blocks.
	inline constexpr std::uint32_t matrices_slot = 0;
	inline constexpr std::uint32_t lights_slot = 1;

	// Set 1, inputs of a pass: textures. The composition reads two, the skybox one.
	inline constexpr std::uint32_t accum_slot = 0;
	inline constexpr std::uint32_t reveal_slot = 1;
	inline constexpr std::uint32_t sky_slot = 0;

	// Set 3, object: one uniform block moved per draw by a dynamic offset, and two storage buffers.
	inline constexpr std::uint32_t object_slot = 0;
	inline constexpr std::uint32_t bones_slot = 0;
	inline constexpr std::uint32_t skin_slot = 1;
}

namespace rnd
{
	// std140 block ObjectData of snk_common.glsl.
	struct object_data
	{
		glm::mat4 world{ 1.0f };
		std::uint32_t bone_offset = 0;
		std::uint32_t pad0[3]{};
	};

	static_assert(offsetof(object_data, world) == 0);
	static_assert(offsetof(object_data, bone_offset) == 64);
	static_assert(sizeof(object_data) == 80);

	// Set 0: Matrices and GlobalLights, read by vertex and fragment stages.
	inline driver::bind_group_layout_desc frame_layout_desc()
	{
		driver::bind_group_layout_desc desc;
		desc.entries.push_back({ driver::binding_type::ubo, bindings::matrices_slot, driver::shader_stage::all, false });
		desc.entries.push_back({ driver::binding_type::ubo, bindings::lights_slot, driver::shader_stage::all, false });
		return desc;
	}

	// Set 1 of the composition pass: the transparent accumulation and revealage targets.
	inline driver::bind_group_layout_desc composition_layout_desc()
	{
		driver::bind_group_layout_desc desc;
		desc.entries.push_back({ driver::binding_type::texture, bindings::accum_slot, driver::shader_stage::fragment, false });
		desc.entries.push_back({ driver::binding_type::texture, bindings::reveal_slot, driver::shader_stage::fragment, false });
		return desc;
	}

	// Set 1 of the skybox pass: the cubemap.
	inline driver::bind_group_layout_desc sky_layout_desc()
	{
		driver::bind_group_layout_desc desc;
		desc.entries.push_back({ driver::binding_type::texture, bindings::sky_slot, driver::shader_stage::fragment, false });
		return desc;
	}

	// Set 3: ObjectData (the offset of the draw's element is dynamic), the frame's bone matrices and the skin's index buffer.
	inline driver::bind_group_layout_desc object_layout_desc()
	{
		driver::bind_group_layout_desc desc;
		desc.entries.push_back({ driver::binding_type::ubo, bindings::object_slot, driver::shader_stage::vertex, true });
		desc.entries.push_back({ driver::binding_type::ssbo, bindings::bones_slot, driver::shader_stage::vertex, false });
		desc.entries.push_back({ driver::binding_type::ssbo, bindings::skin_slot, driver::shader_stage::vertex, false });
		return desc;
	}
}
