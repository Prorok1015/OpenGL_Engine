#include "shader/rnd_material_compiler.h"
#include "texture/rnd_texture_manager.h"
#include "engine_assert.h"

rnd::shader_config rnd::default_material_compiler::compile(const scn::material_desc& mat, const material_compile_context& ctx)
{
	ASSERT_MSG(ctx.textures, "material_compile_context.textures must be set");

	// --- runtime_data: resolve samplers + per-draw uniforms (was scn::material_desc::get_shader_desc) ---
	rnd::shader_config::runtime_data rdata;
	rdata.samplers.resize(mat.samplers_textures.size());
	for (int idx = 0; idx < mat.samplers_textures.size(); ++idx)
	{
		if (mat.samplers_textures[idx].is_valid()) {
			rdata.samplers[idx] = ctx.textures->require_texture(mat.samplers_textures[idx]);
		} else {
			rdata.samplers[idx] = nullptr;
		}
	}

	if (ctx.world_matrix) {
		rdata.uniforms["uWorldMeshMatr"] = *ctx.world_matrix;
	}

	rdata.uniforms["diffuseColor"] = mat.albedo;
	rdata.uniforms["emissiveColor"] = mat.emissive;
	rdata.uniforms["shininess"] = mat.shininess;

	// --- constant_data: program tags + defines (was scn::material_desc::cdata) ---
	rnd::shader_config::constant_data cdata;
	cdata.program = rnd::shader_config::shader_program_data::build();
	cdata.program.set_vertex_shader(mat.vertex_shader);
	cdata.program.set_fragment_shader(mat.fragment_shader);
	cdata.program.set_geometry_shader(mat.geometry_shader);
	cdata.defines = mat.defines;

	// --- former extractor mutations, now driven by the compile context ---
	if (ctx.directional_light_count > 0) {
		cdata.constants["DIRECTION_LIGHT_COUNT"] = std::to_string(ctx.directional_light_count);
	}

	if (ctx.has_skinning) {
		cdata.defines.push_back("USE_ANIMATION");
	}

	return rnd::shader_config{ cdata, rdata };
}
