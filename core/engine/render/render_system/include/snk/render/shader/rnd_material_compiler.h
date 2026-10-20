#pragma once
#include <memory>
#include <snk/common/common.h>
#include <snk/common/snk_api.h>
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/render/shader/rnd_shader_config_cache.h>
#include <snk/render/rnd_material_desc.h>

namespace rnd
{
	class texture_manager;

	// Inputs the render-side compiler needs that are not part of the (POD) scene material itself.
	// The world matrix is not one of them: it travels in the draw call and reaches the shader as ObjectData.
	struct material_compile_context
	{
		texture_manager*  textures = nullptr;
		bool              has_skinning = false;
	};

	// Turns a POD material_desc into a render-ready rnd::shader_config (samplers resolved,
	// per-draw uniforms/defines/constants baked).
	struct material_compiler_interface
	{
		virtual ~material_compiler_interface() = default;
		virtual rnd::shader_config compile(const material_desc& mat, const material_compile_context& ctx) = 0;
	};

	class default_material_compiler : public material_compiler_interface
	{
	public:
		rnd::shader_config compile(const material_desc& mat, const material_compile_context& ctx) override;
	};

	// Compiles `mat` once for reuse across frames: the pass variants are interned into `variants`. The result is `complete` only
	// when every sampler texture was resolved; a texture still loading leaves it to be rebuilt next frame.
	SNK_API std::shared_ptr<compiled_material> build_compiled_material(
		material_compiler_interface& compiler, shader_config_cache& variants, texture_manager& textures,
		std::shared_ptr<const material_desc> mat, bool skinned);
}
