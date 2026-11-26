#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <snk/common/common.h>
#include <snk/common/snk_api.h>
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/render/shader/rnd_shader_config_cache.h>
#include <snk/render/rnd_material_desc.h>

namespace rnd
{
	// Added by build_compiled_material (whatever the compiler) to a material whose alpha_cutoff is above 0: scene.frag and
	// z_prepass.frag discard below it (#1439).
	inline constexpr std::string_view k_alpha_test_define = "ALPHA_TEST";
	// Defined by a shader that splits by alpha itself (mix_opaque_trans_scene: below 1 to OIT, the rest opaque): such a
	// program gets no ALPHA_TEST, and z_prepass discards below 1 for it, as its opaque pass does.
	inline constexpr std::string_view k_alpha_split_define = "ALPHA_SPLIT";

	class texture_manager;

	// The shader desc of a material read once: compile and the completeness of its result must see the same state, or a
	// shader that finishes loading between two reads leaves an empty program that is never rebuilt.
	struct material_shader_state
	{
		std::shared_ptr<const shader_desc> desc;
		bool failed = false;
	};

	SNK_API material_shader_state read_material_shader(const material_desc& mat);

	// Inputs the render-side compiler needs that are not part of the (POD) scene material itself.
	// The world matrix is not one of them: it travels in the draw call and reaches the shader as ObjectData.
	struct material_compile_context
	{
		texture_manager*  textures = nullptr;
		bool              has_skinning = false;
		// What build_compiled_material read; nullopt: compile reads the material's shader itself.
		std::optional<material_shader_state> shader;
	};

	// Turns a POD material_desc into a render-ready rnd::shader_config (samplers resolved,
	// per-draw uniforms/defines/constants baked).
	// A module fills this vtable (render_system::set_material_compiler) or calls it (get_material_compiler) (#1298): raised
	// with any change of its bases or virtual functions (check_vtable_pins.py).
	inline constexpr std::uint32_t material_compiler_interface_abi_revision = 1;

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
	// when every sampler texture was resolved and the shader desc has loaded; a texture or the shader still loading leaves it
	// to be rebuilt next frame. A shader that failed to load is complete with an empty program: it is not drawn, not retried.
	SNK_API std::shared_ptr<compiled_material> build_compiled_material(
		material_compiler_interface& compiler, shader_config_cache& variants, texture_manager& textures,
		std::shared_ptr<const material_desc> mat, bool skinned);
	// The same with the shader state the caller read: material_instance_cache compiles with the shader it keeps, which
	// after a reload is newer than mat->shader.
	SNK_API std::shared_ptr<compiled_material> build_compiled_material(
		material_compiler_interface& compiler, shader_config_cache& variants, texture_manager& textures,
		std::shared_ptr<const material_desc> mat, bool skinned, const material_shader_state& shader);
}
