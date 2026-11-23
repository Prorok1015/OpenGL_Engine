#include <snk/render/shader/rnd_material_compiler.h>
#include <snk/render/shader/rnd_material_params.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>

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
	};
	packed_params packed = pack_params(phong_params, sizeof(material_params), values);
	ASSERT_MSG(packed.ok(), "the phong params schema does not match what the material compiler gives: {}", packed.error);
	rdata.material_params = std::move(packed.bytes);

	// --- constant_data: the program and defines of the shader desc, with the material's defines ---
	// An empty program is not drawn (no stage loads). build_compiled_material rebuilds while the shader is still loading.
	rnd::shader_config::constant_data cdata;
	if (const auto shader = mat.shader.get()) {
		cdata = shader->config(mat.defines).cdata;
	} else {
		cdata.defines = mat.defines;
		if (!mat.shader.is_valid()) {
			egLOG_WARN("render/material", "'{}' has no shader, it is not drawn", mat.get_tag().view());
		} else if (mat.shader.has_error()) {
			egLOG_ERROR("render/material", "the shader '{}' of '{}' did not load, the material is not drawn: {}",
				mat.shader.get_tag().view(), mat.get_tag().view(), mat.shader.error_text());
		}
	}

	// --- former extractor mutations, now driven by the compile context ---
	if (ctx.has_skinning) {
		cdata.defines.push_back("USE_ANIMATION");
	}

	return rnd::shader_config{ cdata, rdata };
}

std::shared_ptr<rnd::compiled_material> rnd::build_compiled_material(
	material_compiler_interface& compiler, shader_config_cache& variants, texture_manager& textures,
	std::shared_ptr<const material_desc> mat, bool skinned)
{
	ASSERT_MSG(mat, "build_compiled_material needs a loaded material");
	auto out = std::make_shared<compiled_material>();
	out->source = mat;
	out->skinned = skinned;
	out->texture_generation = textures.generation();

	rnd::shader_config cfg = compiler.compile(*mat, material_compile_context{ &textures, skinned });
	out->base = variants.intern(cfg.cdata);
	out->opaque = variants.intern(cfg.cdata, pass_define_opaque);
	out->runtime = std::move(cfg.rdata);

	// A failed shader is final: rebuilding would only log the same error every frame.
	if (mat->shader.is_valid() && !mat->shader.is_ready() && !mat->shader.has_error()) {
		out->complete = false;
	}
	for (std::size_t idx = 0; idx < mat->samplers_textures.size(); ++idx) {
		if (mat->samplers_textures[idx].is_valid() && !textures.is_ready(out->runtime.samplers[idx])) {
			out->complete = false;
		}
	}
	return out;
}
