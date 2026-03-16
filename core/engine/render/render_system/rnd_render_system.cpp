#include "rnd_render_system.h"
#include <rnd_driver_interface.h>
#include "texture/rnd_texture_2d_desc.h"
#include "texture/rnd_texture_cubemap_desc.h"

rnd::render_system* p_render_system = nullptr;

rnd::render_system& rnd::get_system()
{
	ASSERT_MSG(p_render_system, "Render system is nullptr!");
	return *p_render_system;
}

rnd::render_system::render_system(std::unique_ptr<rnd::driver::driver_interface> driver, desc::desc_system& d)
	: drv(std::move(driver))
	, shader_manager(drv.get())
	, texture_manager(drv.get(), d)
	, geom_manager(drv.get(), d)
{
	texture_manager.register_loader("texture_2d_desc", [](driver::driver_interface* drv, const rnd::texture_desc& desc) -> std::unique_ptr<rnd::driver::texture_interface> {
		const rnd::texture_2d_desc& tex_2d_desc = static_cast<const rnd::texture_2d_desc&>(desc);
		auto picture_handle = tex_2d_desc.get_picture_handle();
		if (!picture_handle.is_ready()) {
			egLOG("render/texture", "Failed to load texture: picture resource with tag {0} is not ready", tex_2d_desc.get_tag().view());
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

	texture_manager.register_loader("texture_cubemap_desc", [](driver::driver_interface* drv, const rnd::texture_desc& desc) -> std::unique_ptr<rnd::driver::texture_interface> {
		const auto& tex_cubmap_desc = static_cast<const rnd::texture_cubemap_desc&>(desc);

		if (tex_cubmap_desc.get_source_type() == rnd::texture_cubemap_desc::SOURCE_TYPE::SINGLE_FILE) {
			egLOG("render/texture", "Cubemap texture loading from single file is not implemented yet (tag: {0})", desc.get_tag().view());
			return nullptr;
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
		header.usage = rnd::driver::TEXTURE_USAGE::SAMPLED;
		header.data.mip_levels = 1;

		auto update_face = [&] (rnd::driver::texture_interface* texture_intf, res::res_handle<res::picture_resource> pic, uint32_t face) {
			if (!pic.is_ready()) {
				egLOG("render/texture", "Failed to load cubemap texture: picture resource with tag is not ready");
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

void rnd::render_system::activate_renderer(std::weak_ptr<renderer_base> renderer_)
{
	auto pred = [](auto& lhs, auto& rhs) {
		auto lhs_r = lhs.lock();
		auto rhs_r = rhs.lock();
		if (!lhs_r || !rhs_r) {
			return false;
		}

		return lhs_r->get_render_priority() > rhs_r->get_render_priority();
	};

	renderers_list.insert(std::lower_bound(renderers_list.begin(), renderers_list.end(), renderer_, pred), renderer_);
}

void rnd::render_system::deactivate_renderer(std::weak_ptr<renderer_base> renderer)
{
	auto pred = [find = renderer.lock()](auto& lhs) {
		auto lhs_r = lhs.lock(); 
		if (!lhs_r) {
			return false;
		}

		return lhs_r == find;
	};

	auto it = std::find_if(renderers_list.begin(), renderers_list.end(), pred);
	renderers_list.erase(it);
}

void rnd::render_system::render() const
{
	for (auto& weak_renderer : renderers_list) {
		if (auto renderer = weak_renderer.lock()) {
			renderer->on_render(drv.get());
		}
	}
}

void rnd::render_system::render_frame(frame_context& context) const
{
	if (!drv) return;

	for (auto& pass : render_passes) {
		pass->execute(context, *drv);
	}
}