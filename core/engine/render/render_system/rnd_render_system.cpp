#include <snk/render/rnd_render_system.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/render/rnd_material_groups.h>
#include <snk/render/rnd_per_draw_data.h>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include "geom/rnd_geom_manager.h"
#include "shader/rnd_shader_manager.h"
#include "texture/rnd_texture_2d_desc.h"
#include "texture/rnd_texture_cubemap_desc.h"
#include <snk/common/eng_profiler.h>
#include "rnd_gpu_profiler.h"

namespace {
	// The renderer as the passes of one frame see it: every call goes to the manager that owns the thing, the
	// passes never get the managers themselves.
	class frame_render_context final : public rnd::render_context
	{
	public:
		frame_render_context(rnd::frame_context& frame, rnd::texture_manager& textures, rnd::ShaderManager& shaders,
			rnd::geom_manager& geometry, rnd::per_draw_data& draw_data, rnd::material_groups& materials, const rnd::skinning_manager* skins)
			: m_frame(frame), m_textures(textures), m_shaders(shaders), m_geometry(geometry), m_draw_data(draw_data), m_materials(materials), m_skins(skins)
		{}

		std::span<rnd::render_packet_t> packets() override
		{
			if (!m_frame.data.has_value<std::pmr::vector<rnd::render_packet_t>>()) {
				return {};
			}
			return m_frame.data.require<std::pmr::vector<rnd::render_packet_t>>();
		}

		rnd::driver::texture_interface* find_texture(const res::tag& tag) const override { return m_textures.find(tag); }

		rnd::driver::texture_interface* ensure_render_target(const res::tag& tag, glm::ivec2 size,
			rnd::driver::texture_header::TYPE format, rnd::driver::texture_usage usage, const rnd::render_targets::sampling& params) override
		{
			return rnd::render_targets::ensure(m_textures, tag, size, format, usage, params);
		}

		rnd::driver::vertex_array_interface* geometry(const res::tag& tag) override { return m_geometry.require_geometry(tag); }

		rnd::shader_config_cache& variants() override { return m_shaders.variants(); }
		rnd::driver::shader_interface* program(rnd::variant_id variant) override { return m_shaders.program(variant); }
		const rnd::set_layouts& layouts() const override { return m_shaders.layouts(); }

		void write_view(const rnd::global_params& view) override { m_shaders.update_global_uniform(view); }
		void write_lights(const rnd::lights_params& lights) override { m_shaders.update_global_sun(lights); }
		void bind_frame() override { m_shaders.bind_frame(); }

		void bind_material(const rnd::compiled_material& material) override { m_materials.bind(material); }

		void upload_objects(std::span<const rnd::object_data> objects, std::span<const glm::mat4> bones) override { m_draw_data.upload(objects, bones); }
		void bind_object(std::uint32_t index, const res::tag& skin) override { m_draw_data.bind_object(index, skin, m_skins); }

	private:
		rnd::frame_context& m_frame;
		rnd::texture_manager& m_textures;
		rnd::ShaderManager& m_shaders;
		rnd::geom_manager& m_geometry;
		rnd::per_draw_data& m_draw_data;
		rnd::material_groups& m_materials;
		const rnd::skinning_manager* m_skins;
	};
}

void rnd::render_system::begin_frame(frame_context& context)
{
	context.textures = m_textures.get();
	context.variants = &m_shaders->variants();
	context.material_compiler = m_material_compiler.get();
}

rnd::render_system::render_system(std::unique_ptr<rnd::driver::driver_interface> driver, desc::desc_system& d, res::resource_system& res_sys)
	: drv(std::move(driver))
	, m_textures(std::make_unique<rnd::texture_manager>(drv.get(), d, res_sys))
	, m_shaders(std::make_unique<rnd::ShaderManager>(drv.get(), res_sys))
	, m_geometry(std::make_unique<rnd::geom_manager>(drv.get(), d, res_sys))
	, m_draw_data(std::make_unique<rnd::per_draw_data>(*drv))
	, m_materials(std::make_unique<rnd::material_groups>(*drv))
{
	auto& texture_manager = *m_textures;
	texture_manager.register_loader("texture_2d_desc", [](driver::driver_interface* drv, const rnd::texture_desc& desc) -> std::unique_ptr<rnd::driver::texture_interface> {
		const rnd::texture_2d_desc& tex_2d_desc = static_cast<const rnd::texture_2d_desc&>(desc);
		auto picture_handle = tex_2d_desc.get_picture_handle();
		if (!picture_handle.is_ready()) {
			// Still loading in the background: the texture manager asks again next frame.
			if (picture_handle.has_error()) {
				egLOG_WARN("render/texture", "Failed to load texture: picture resource of {0} failed to load", tex_2d_desc.get_tag().view());
			}
			return nullptr;
		}
		auto picture = picture_handle.get();
		rnd::driver::texture_header header = desc.get_header();
		header.data.extent.width = picture->size().x;
		header.data.extent.height = picture->size().y;
		header.data.initial_data = picture->data();
		switch (picture->channels()) {
			case 1: {
				if (header.data.format != driver::texture_header::TYPE::R8) {
					header.data.format = driver::texture_header::TYPE::R8;
				}
			}break;
			case 3: {
				if (header.data.format != driver::texture_header::TYPE::RGB8) {
					header.data.format = driver::texture_header::TYPE::RGB8;
				}
			}break;
			case 4: {
				if (header.data.format != driver::texture_header::TYPE::RGBA8) {
					header.data.format = driver::texture_header::TYPE::RGBA8;
				}
			}break;
			default:
				break;
		}
		auto texture = drv->create_texture(header);
		return texture;

	});

	rnd::get_gpu_profiler().init(*drv.get());

	texture_manager.register_loader("texture_cubemap_desc", [](driver::driver_interface* drv, const rnd::texture_desc& desc) -> std::unique_ptr<rnd::driver::texture_interface> {
		const auto& tex_cubmap_desc = static_cast<const rnd::texture_cubemap_desc&>(desc);

		if (tex_cubmap_desc.get_source_type() == rnd::texture_cubemap_desc::SOURCE_TYPE::SINGLE_FILE) {
			egLOG_WARN("render/texture", "Cubemap texture loading from single file is not implemented yet (tag: {0})", desc.get_tag().view());
			return nullptr;
		}

		// The faces load in the background: come back next frame instead of caching a texture with holes.
		const auto& awaited = tex_cubmap_desc.get_faces();
		for (const auto* face : { &awaited.right, &awaited.left, &awaited.top, &awaited.bottom, &awaited.front, &awaited.back }) {
			if (face->is_valid() && !face->is_ready() && !face->has_error()) {
				return nullptr;
			}
		}

		rnd::driver::texture_header header;

		auto load_face_data = [] (res::res_handle<res::picture_resource> res) 
			-> std::tuple<std::vector<unsigned char>, rnd::driver::texture_header::TYPE, uint32_t, uint32_t> 
		{
			rnd::driver::texture_header::TYPE format;
			switch (res->channels()) {
				case 1: format = rnd::driver::texture_header::TYPE::R8; break;
				case 3: format = rnd::driver::texture_header::TYPE::RGB8; break;
				case 4: format = rnd::driver::texture_header::TYPE::RGBA8; break;
				default:
					throw std::runtime_error("Unsupported number of channels in cubemap texture");
			}

			std::vector<unsigned char> data(res->size().x * res->size().y * res->channels());
			std::copy(res->data(), res->data() + data.size(), data.begin());

			return std::make_tuple(std::move(data), format, res->size().x, res->size().y);
		};

		const auto& faces = tex_cubmap_desc.get_faces();
		auto [right_data, right_format, right_width, right_height] = load_face_data(faces.right);
		header.data.extent.width = right_width;
		header.data.extent.height = right_height;
		header.data.format = right_format;
		header.data.extent.faces = 6;  // Important: 6 faces for cubemap

		header.wrap = rnd::driver::texture_header::WRAPPING::CLAMP_TO_EDGE;
		header.min = rnd::driver::texture_header::FILTERING::LINEAR;
		header.mag = rnd::driver::texture_header::FILTERING::LINEAR;
		header.type = rnd::driver::TEXTURE_TYPE::TEXTURE_CUBE_MAP;
		header.usage = rnd::driver::texture_usage::sampled;
		header.data.mip_levels = 1;

		auto update_face = [&] (rnd::driver::texture_interface* texture_intf, res::res_handle<res::picture_resource> pic, uint32_t face) {
			if (!pic.is_ready()) {
				egLOG_WARN("render/texture", "Failed to load cubemap texture: picture resource with tag is not ready");
				return;

			}
			auto [data, format, width, height] = load_face_data(pic);
			texture_intf->update_data(data.data(), 0, 0, 0, width, height, 1, 0, 0, face); // face parameter is crucial here!
		};

		auto texture = drv->create_texture(header);
		update_face(texture.get(), faces.right, 0); // Right
		update_face(texture.get(), faces.left, 1); // Left
		update_face(texture.get(), faces.bottom, 2); // Bottom
		update_face(texture.get(), faces.top, 3); // Top
		update_face(texture.get(), faces.front, 4); // Front
		update_face(texture.get(), faces.back, 5); // Back
		return texture;
	});
}

rnd::render_system::~render_system() = default;

void rnd::render_system::render_frame(frame_context& context) const
{
	PROFILE_SCOPE("RenderFrame");
	if (!drv) return;

	drv->begin_frame();
	// ImGui renders between two frames and leaves its own state behind.
	drv->invalidate_state_cache();
	m_materials->begin_frame();

	auto& gpu = rnd::get_gpu_profiler();
	gpu.begin_frame(ds::profiler_current_frame());

	frame_render_context pass_context(context, *m_textures, *m_shaders, *m_geometry, *m_draw_data, *m_materials, m_skinning_manager);
	for (auto& pass : render_passes) {
		pass->execute(pass_context, *drv);
	}

	gpu.end_frame();
}