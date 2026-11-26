#include <snk/render/shader/rnd_material_compiler.h>
#include <snk/render/shader/rnd_material_params.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>

#include <algorithm>
#include <string>
#include <utility>

rnd::material_shader_state rnd::read_material_shader(const material_desc& mat)
{
	material_shader_state state;
	state.desc = mat.shader.get();
	state.failed = !state.desc && mat.shader.has_error();
	return state;
}

rnd::shader_config rnd::default_material_compiler::compile(const material_desc& mat, const material_compile_context& ctx)
{
	ASSERT_MSG(ctx.textures, "material_compile_context.textures must be set");

	// --- runtime_data: resolve the samplers, pack the MaterialParams block ---
	rnd::shader_config::runtime_data rdata;
	rdata.samplers.resize(mat.samplers_textures.size());
	for (std::size_t idx = 0; idx < mat.samplers_textures.size(); ++idx)
	{
		if (mat.samplers_textures[idx].is_valid()) {
			rdata.samplers[idx] = ctx.textures->require(mat.samplers_textures[idx]);
		} else {
			rdata.samplers[idx] = texture_id{};
		}
	}

	const named_param values[] = {
		{ "diffuseColor", mat.albedo },
		{ "emissiveColor", mat.emissive },
		{ "shininess", mat.shininess },
		{ "alphaCutoff", mat.alpha_cutoff },
	};
	packed_params packed = pack_params(phong_params, sizeof(material_params), values);
	ASSERT_MSG(packed.ok(), "the phong params schema does not match what the material compiler gives: {}", packed.error);
	rdata.material_params = std::move(packed.bytes);

	// --- constant_data: the program and defines of the shader desc, with the material's defines ---
	// An empty program is not drawn (no stage loads). build_compiled_material rebuilds while the shader is still loading.
	rnd::shader_config::constant_data cdata;
	const material_shader_state shader = ctx.shader ? *ctx.shader : read_material_shader(mat);
	if (shader.desc) {
		cdata = shader.desc->config(mat.defines).cdata;
	} else {
		cdata.defines = mat.defines;
		// A shader that failed to load is reported by material_instance_cache, once per material (#1273).
		if (!mat.shader.is_valid()) {
			egLOG_WARN("render/material", "'{}' has no shader, it is not drawn", mat.get_tag().view());
		}
	}

	// --- former extractor mutations, now driven by the compile context ---
	if (ctx.has_skinning) {
		cdata.defines.push_back("USE_ANIMATION");
	}
	// A cutoff of 0 discards nothing, so the test is left out of the program altogether (#1439).
	if (mat.alpha_cutoff > 0.0f && std::ranges::find(cdata.defines, std::string_view(k_alpha_test_define)) == cdata.defines.end()) {
		cdata.defines.push_back(std::string(k_alpha_test_define));
	}

	return rnd::shader_config{ cdata, rdata };
}

std::shared_ptr<rnd::compiled_material> rnd::build_compiled_material(
	material_compiler_interface& compiler, shader_config_cache& variants, texture_manager& textures,
	std::shared_ptr<const material_desc> mat, bool skinned)
{
	ASSERT_MSG(mat, "build_compiled_material needs a loaded material");
	const material_shader_state shader = read_material_shader(*mat);
	return build_compiled_material(compiler, variants, textures, std::move(mat), skinned, shader);
}

std::shared_ptr<rnd::compiled_material> rnd::build_compiled_material(
	material_compiler_interface& compiler, shader_config_cache& variants, texture_manager& textures,
	std::shared_ptr<const material_desc> mat, bool skinned, const material_shader_state& shader)
{
	ASSERT_MSG(mat, "build_compiled_material needs a loaded material");
	auto out = std::make_shared<compiled_material>();
	out->source = mat;
	out->skinned = skinned;
	out->texture_generation = textures.generation();

	rnd::shader_config cfg = compiler.compile(*mat, material_compile_context{ &textures, skinned, shader });
	out->base = variants.intern(cfg.cdata);
	out->opaque = variants.intern(cfg.cdata, pass_define_opaque);
	out->runtime = std::move(cfg.rdata);

	// A failed shader is final: rebuilding would only log the same error every frame.
	if (mat->shader.is_valid() && !shader.desc && !shader.failed) {
		out->complete = false;
	}
	for (std::size_t idx = 0; idx < mat->samplers_textures.size(); ++idx) {
		if (mat->samplers_textures[idx].is_valid() && !textures.is_ready(out->runtime.samplers[idx])) {
			out->complete = false;
		}
	}
	return out;
}
