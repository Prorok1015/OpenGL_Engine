#include <snk/render/shader/rnd_material_compiler.h>
#include <snk/render/shader/rnd_material_params.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

	return rnd::shader_config{ cdata, rdata };
}

namespace
{
	bool defines_name(const std::vector<std::string>& defines, std::string_view name)
	{
		return std::ranges::any_of(defines, [name](const std::string& define) {
			return define == name || (define.size() > name.size() && define.starts_with(name) && define[name.size()] == ' ');
		});
	}

	// Here and not in a compiler, so a module's own compiler keeps the test too (#1439). A cutoff of 0 discards nothing, and a
	// shader that splits by alpha itself has no use for it: neither gets a variant of its own.
	void add_alpha_test(rnd::shader_config::constant_data& cdata, const rnd::material_desc& mat)
	{
		if (mat.alpha_cutoff <= 0.0f || defines_name(cdata.defines, rnd::k_alpha_split_define) || defines_name(cdata.defines, rnd::k_alpha_test_define)) {
			return;
		}
		cdata.defines.emplace_back(rnd::k_alpha_test_define);
	}
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
	add_alpha_test(cfg.cdata, *mat);
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
