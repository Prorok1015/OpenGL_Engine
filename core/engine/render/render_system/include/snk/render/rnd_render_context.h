#pragma once
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_texture_interface.h>
#include <snk/render/rnd_bindings.h>
#include <snk/render/rnd_gpu_blocks.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/shader/rnd_shader_config_cache.h>
#include <snk/render/texture/rnd_render_targets.h>
#include <snk/render/texture/rnd_texture_id.h>
#include <snk/resource/res_tag.h>

#include <cstdint>
#include <span>

namespace rnd
{
	// What a render pass sees of the renderer. The texture, shader and geometry managers stay behind it: a pass asks
	// for a texture, a program or a vertex array and gets the object, it cannot reach the cache that keeps them.
	// The renderer implements it for the length of one render_frame; a test can implement it to run a pass alone.
	class render_context
	{
	public:
		virtual ~render_context() = default;

		// The packets extraction made for the frame; none when it made none.
		virtual std::span<render_packet_t> packets() = 0;

		// Textures by tag. A render target is made on the first call and again when its size changes.
		virtual driver::texture_interface* find_texture(const res::tag& tag) const = 0;
		virtual driver::texture_interface* ensure_render_target(const res::tag& tag, glm::ivec2 size,
			driver::texture_header::TYPE format, driver::texture_usage usage, const render_targets::sampling& params = {}) = 0;

		// The texture an id names now (a material's map, the sky); null while it is still loading or was removed.
		virtual driver::texture_handle texture(texture_id id) const = 0;

		// The vertex array of a geometry; null while it is still loading.
		virtual driver::vertex_array_interface* geometry(const res::tag& tag) = 0;

		// Shader variants and the program of one (compiled on first use; null when it does not compile).
		virtual shader_config_cache& variants() = 0;
		virtual driver::shader_interface* program(variant_id variant) = 0;
		virtual const set_layouts& layouts() const = 0;

		// Set 0: the camera and the lights of the packet being drawn; bind_frame inside a render pass, before its draws.
		virtual void write_view(const global_params& view) = 0;
		virtual void write_lights(const lights_params& lights) = 0;
		virtual void bind_frame() = 0;

		// The material instance a draw (or the skybox) names.
		virtual const compiled_material& material(material_instance_id id) const = 0;
		// Set 2: the bind group of a material instance (its params and maps).
		virtual void bind_material(material_instance_id id) = 0;

		// Set 3: the frame's objects and bone matrices go to the GPU once, before the passes that draw them.
		virtual void upload_objects(std::span<const object_data> objects, std::span<const glm::mat4> bones) = 0;
		// Binds element `index` of the uploaded objects; a skinned draw names its skin.
		virtual void bind_object(std::uint32_t index, const res::tag& skin) = 0;
	};

	// Binds what a draw of `material` reads from set 2.
	inline void configure_pass(render_context& context, material_instance_id material)
	{
		context.bind_material(material);
	}
}
