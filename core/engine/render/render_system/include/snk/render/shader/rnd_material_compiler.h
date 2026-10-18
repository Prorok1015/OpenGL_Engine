#pragma once
#include <snk/common/common.h>
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/scene/scn_material_desc.h>

namespace rnd
{
	class texture_manager;

	// Inputs the render-side compiler needs that are not part of the (POD) scene material itself.
	// `world_matrix` is borrowed (nullptr when the entity has no world transform — e.g. a skybox
	// without one), mirroring the old get_shader_desc handle.all_of<world_transform>() check.
	struct material_compile_context
	{
		const glm::mat4*  world_matrix = nullptr;
		texture_manager*  textures = nullptr;
		bool              has_skinning = false;
	};

	// Turns a POD scn::material_desc into a render-ready rnd::shader_config (samplers resolved,
	// per-draw uniforms/defines/constants baked). Lives render-side so scn_material_desc.h stays
	// free of rnd_*/driver headers (EPIC-49 US-49-5).
	struct material_compiler_interface
	{
		virtual ~material_compiler_interface() = default;
		virtual rnd::shader_config compile(const scn::material_desc& mat, const material_compile_context& ctx) = 0;
	};

	class default_material_compiler : public material_compiler_interface
	{
	public:
		rnd::shader_config compile(const scn::material_desc& mat, const material_compile_context& ctx) override;
	};
}
