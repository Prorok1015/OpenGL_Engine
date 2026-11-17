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

	// Set 2, material: the MaterialParams block and the maps of the phong shaders (SNK_TEX(2, n)).
	inline constexpr std::uint32_t material_params_slot = 0;
	inline constexpr std::uint32_t diffuse_slot = 0;
	inline constexpr std::uint32_t specular_slot = 1;
	inline constexpr std::uint32_t normal_slot = 2;
	inline constexpr std::uint32_t material_texture_count = 3;

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

	// Set 2: MaterialParams and the three maps. A map the material does not have is a placeholder texture.
	inline driver::bind_group_layout_desc material_layout_desc()
	{
		driver::bind_group_layout_desc desc;
		desc.entries.push_back({ driver::binding_type::ubo, bindings::material_params_slot, driver::shader_stage::fragment, false });
		desc.entries.push_back({ driver::binding_type::texture, bindings::diffuse_slot, driver::shader_stage::fragment, false });
		desc.entries.push_back({ driver::binding_type::texture, bindings::specular_slot, driver::shader_stage::fragment, false });
		desc.entries.push_back({ driver::binding_type::texture, bindings::normal_slot, driver::shader_stage::fragment, false });
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

namespace rnd
{
	// The bind group layouts of the engine's sets, made once. A pass names the ones its shaders read in the
	// pipelines it builds (pipeline_desc::set_layouts), which is what the validation checks a draw against.
	struct set_layouts
	{
		driver::bind_group_layout_handle frame;
		driver::bind_group_layout_handle composition;
		driver::bind_group_layout_handle sky;
		driver::bind_group_layout_handle material;
		driver::bind_group_layout_handle object;

		using list = ds::fixed_vector<driver::bind_group_layout_handle, driver::max_bind_sets>;

		// Shaders of the scene: set 0, the material of set 2 and the per-draw set 3.
		list scene() const { return { frame, {}, material, object }; }
		// The debug lines read the camera only.
		list frame_only() const { return { frame }; }
		list skybox() const { return { frame, sky }; }
		list composite() const { return { {}, composition }; }
	};
}
