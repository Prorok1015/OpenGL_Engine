#include <snk/render/rnd_render_system.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/render/rnd_material_groups.h>
#include <snk/render/rnd_material_instance_cache.h>
#include <snk/render/rnd_per_draw_data.h>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include "geom/rnd_geom_manager.h"
#include "shader/rnd_shader_manager.h"
#include "texture/rnd_texture_2d_desc.h"
#include "texture/rnd_texture_cubemap_desc.h"
#include <snk/common/eng_profiler.h>
#include "rnd_gpu_profiler.h"
#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
	// The renderer as the passes of one frame see it: every call goes to the manager that owns the thing, the
	// passes never get the managers themselves.
	class frame_render_context final : public rnd::render_context
	{
	public:
		frame_render_context(rnd::frame_context& frame, rnd::texture_manager& textures, rnd::render_targets::registry& targets, rnd::ShaderManager& shaders,
			rnd::geom_manager& geometry, rnd::per_draw_data& draw_data, rnd::material_groups& materials,
			rnd::material_instance_cache& instances, const rnd::skinning_manager* skins)
			: m_frame(frame), m_textures(textures), m_targets(targets), m_shaders(shaders), m_geometry(geometry), m_draw_data(draw_data), m_materials(materials), m_instances(instances), m_skins(skins)
		{}

		std::span<rnd::render_packet_t> packets() override
		{
			if (m_group) {
				return *m_group;
			}
			if (!m_frame.data.has_value<std::pmr::vector<rnd::render_packet_t>>()) {
				return {};
			}
			return m_frame.data.require<std::pmr::vector<rnd::render_packet_t>>();
		}

		// From now on the passes see only `group`; use_all_packets() gives the whole frame back.
		void use_group(std::span<rnd::render_packet_t> group) { m_group = group; }
		void use_all_packets() { m_group.reset(); }

		rnd::driver::texture_handle find_texture(const res::tag& tag) const override { return m_textures.binding(m_textures.id_of(tag)).handle; }

		rnd::driver::texture_handle texture(rnd::texture_id id) const override { return m_textures.binding(id).handle; }

		rnd::driver::texture_handle ensure_render_target(const res::tag& tag, glm::ivec2 size,
			rnd::driver::texture_header::TYPE format, rnd::driver::texture_usage usage, const rnd::render_targets::sampling& params) override
		{
			return m_targets.resolve(m_targets.ensure(m_textures, tag, size, format, usage, params));
		}

		rnd::driver::vertex_array_interface* geometry(const res::tag& tag) override { return m_geometry.require_geometry(tag); }
		bool draw_fits(const res::tag& tag, std::uint32_t count, std::uint32_t base_vertex, std::uint32_t base_index) override
		{
			return m_geometry.draw_fits(tag, count, base_vertex, base_index);
		}

		rnd::shader_config_cache& variants() override { return m_shaders.variants(); }
		rnd::driver::shader_interface* program(rnd::variant_id variant) override { return m_shaders.program(variant); }
		const rnd::set_layouts& layouts() const override { return m_shaders.layouts(); }

		void write_view(const rnd::global_params& view) override { m_shaders.update_global_uniform(view); }
		void write_lights(const rnd::lights_params& lights) override { m_shaders.update_global_sun(lights); }
		void bind_frame() override { m_shaders.bind_frame(); }

		const rnd::compiled_material& material(rnd::material_instance_id id) const override { return m_instances.get(id); }
		void bind_material(rnd::material_instance_id id) override { m_materials.bind(m_instances.get(id)); }

		void upload_objects(std::span<const rnd::object_data> objects, std::span<const glm::mat4> bones) override { m_draw_data.upload(objects, bones); }
		void bind_object(std::uint32_t index, const res::tag& skin) override { m_draw_data.bind_object(index, skin, m_skins); }

	private:
		std::optional<std::span<rnd::render_packet_t>> m_group;
		rnd::frame_context& m_frame;
		rnd::texture_manager& m_textures;
		rnd::render_targets::registry& m_targets;
		rnd::ShaderManager& m_shaders;
		rnd::geom_manager& m_geometry;
		rnd::per_draw_data& m_draw_data;
		rnd::material_groups& m_materials;
		rnd::material_instance_cache& m_instances;
		const rnd::skinning_manager* m_skins;
	};
}

void rnd::render_system::begin_frame(frame_context& context)
{
	context.textures = m_textures.get();
	context.variants = &m_shaders->variants();
	context.material_compiler = m_material_compiler.get();
	m_instances->begin_frame(m_material_compiler.get());
	context.materials = m_instances.get();
}

namespace {

	// The pixels of a decoded picture as a driver takes them: rows one after another without padding, in a format of
	// exactly as many channels as the picture has. `data` is the picture's own, or `converted` when it had to be made.
	struct upload_pixels
	{
		rnd::driver::texture_header::TYPE format = rnd::driver::texture_header::TYPE::RGBA8;
		uint32_t width = 0;
		uint32_t height = 0;
		const unsigned char* data = nullptr;
		std::vector<unsigned char> converted;
	};

	// None for a picture a driver cannot be given: the format must say how many bytes a pixel has, or the driver reads
	// past the picture (#926). Two channels (grey and alpha) have no format here and become four.
	std::optional<upload_pixels> pixels_of(const res::picture_resource& picture)
	{
		if (picture.size().x <= 0 || picture.size().y <= 0 || picture.data() == nullptr) return std::nullopt;
		upload_pixels out;
		out.width = static_cast<uint32_t>(picture.size().x);
		out.height = static_cast<uint32_t>(picture.size().y);
		out.data = picture.data();
		switch (picture.channels()) {
		case 1: out.format = rnd::driver::texture_header::TYPE::R8; return out;
		case 3: out.format = rnd::driver::texture_header::TYPE::RGB8; return out;
		case 4: out.format = rnd::driver::texture_header::TYPE::RGBA8; return out;
		case 2: {
			const std::size_t count = static_cast<std::size_t>(out.width) * out.height;
			out.converted.resize(count * 4);
			for (std::size_t i = 0; i < count; ++i) {
				const unsigned char grey = picture.data()[i * 2];
				out.converted[i * 4 + 0] = grey;
				out.converted[i * 4 + 1] = grey;
				out.converted[i * 4 + 2] = grey;
				out.converted[i * 4 + 3] = picture.data()[i * 2 + 1];
			}
			out.format = rnd::driver::texture_header::TYPE::RGBA8;
			out.data = out.converted.data();
			return out;
		}
		default: return std::nullopt;
		}
	}

	uint32_t full_mip_chain(uint32_t width, uint32_t height)
	{
		uint32_t levels = 1;
		for (uint32_t side = std::max(width, height); side > 1; side /= 2) ++levels;
		return levels;
	}

	// The file says how the texture is sampled; what it is made of comes from the pixels alone. A type, a depth or a layer
	// count from the file would make the driver read that many pictures out of the one buffer (#926).
	rnd::driver::texture_owner upload_2d(rnd::driver::driver_interface& drv, rnd::driver::texture_header header, const upload_pixels& pixels)
	{
		header.type = rnd::driver::TEXTURE_TYPE::TEXTURE_2D;
		header.usage = rnd::driver::texture_usage::sampled; // the desc file may say none or a render target: a loaded texture is only sampled
		header.data.extent = rnd::driver::texture_header::texture_extent{ .width = pixels.width, .height = pixels.height };
		header.data.format = pixels.format;
		header.data.mip_levels = std::clamp<uint32_t>(header.data.mip_levels, 1, full_mip_chain(pixels.width, pixels.height));
		header.data.initial_data = pixels.data;
		return rnd::driver::make_texture(drv, header);
	}

	void warn_once(std::unordered_set<std::string>& told, const res::tag& texture, std::string_view why)
	{
		if (!told.emplace(texture.view()).second) return;
		egLOG_WARN("render/texture", "texture '{}' is not loaded: {}", texture.view(), why);
	}

}

rnd::render_system::render_system(std::unique_ptr<rnd::driver::driver_interface> driver, desc::desc_system& d, res::resource_system& res_sys)
	: drv(std::move(driver))
	, m_textures(std::make_unique<rnd::texture_manager>(drv.get(), d, res_sys))
	, m_targets(std::make_unique<rnd::render_targets::registry>(*drv))
	, m_shaders(std::make_unique<rnd::ShaderManager>(drv.get(), res_sys))
	, m_geometry(std::make_unique<rnd::geom_manager>(drv.get(), res_sys))
	, m_draw_data(std::make_unique<rnd::per_draw_data>(*drv))
	, m_materials(std::make_unique<rnd::material_groups>(*drv, *m_textures))
	, m_instances(std::make_unique<rnd::material_instance_cache>(m_shaders->variants(), *m_textures, &res_sys))
{
	auto& texture_manager = *m_textures;
	// A loader that gives nothing is asked again every frame: what it could not load is said once.
	const auto told = std::make_shared<std::unordered_set<std::string>>();
	texture_manager.register_loader("texture_2d_desc", [told](driver::driver_interface* drv, const rnd::texture_desc& desc) -> rnd::driver::texture_owner {
		const rnd::texture_2d_desc& tex_2d_desc = static_cast<const rnd::texture_2d_desc&>(desc);
		if (const auto& own = tex_2d_desc.get_inline_pixels()) {
			// Checked when the desc was read: the bytes are exactly width x height pixels of the format (#1204). The file
			// holds its top row first, as a PNG does; a decoded picture (pct_adapter) is uploaded bottom row first.
			upload_pixels pixels;
			pixels.format = own->format;
			pixels.width = own->width;
			pixels.height = own->height;
			const std::size_t row = own->bytes.size() / own->height;
			pixels.converted.reserve(own->bytes.size());
			for (uint32_t y = own->height; y-- > 0;) {
				const auto first = own->bytes.begin() + static_cast<std::ptrdiff_t>(y * row);
				pixels.converted.insert(pixels.converted.end(), first, first + static_cast<std::ptrdiff_t>(row));
			}
			pixels.data = pixels.converted.data();
			return upload_2d(*drv, desc.get_header(), pixels);
		}
		auto picture_handle = tex_2d_desc.get_picture_handle();
		if (!picture_handle.is_ready()) {
			// Still loading in the background: the texture manager asks again next frame.
			if (picture_handle.has_error()) {
				egLOG_WARN("render/texture", "Failed to load texture: picture resource of {0} failed to load", tex_2d_desc.get_tag().view());
			}
			return {};
		}
		auto picture = picture_handle.get();
		const std::optional<upload_pixels> pixels = pixels_of(*picture);
		if (!pixels) {
			warn_once(*told, desc.get_tag(), "its picture has no pixels the driver takes (1 to 4 channels, a size above zero)");
			return {};
		}
		return upload_2d(*drv, desc.get_header(), *pixels);
	});

	rnd::get_gpu_profiler().init(*drv.get());

	texture_manager.register_loader("texture_cubemap_desc", [told](driver::driver_interface* drv, const rnd::texture_desc& desc) -> rnd::driver::texture_owner {
		const auto& tex_cubmap_desc = static_cast<const rnd::texture_cubemap_desc&>(desc);

		if (tex_cubmap_desc.get_source_type() == rnd::texture_cubemap_desc::SOURCE_TYPE::SINGLE_FILE) {
			egLOG_WARN("render/texture", "Cubemap texture loading from single file is not implemented yet (tag: {0})", desc.get_tag().view());
			return {};
		}

		// The faces load in the background: come back next frame instead of caching a texture with holes.
		const auto& awaited = tex_cubmap_desc.get_faces();
		for (const auto* face : { &awaited.right, &awaited.left, &awaited.top, &awaited.bottom, &awaited.front, &awaited.back }) {
			if (!face->is_ready()) {
				if (face->has_error()) {
					egLOG_WARN("render/texture", "Failed to load cubemap face '{}'", face->get_tag().view());
				}
				return {};
			}
		}

		// Every face is uploaded with the size and the format of the texture: one that differs would be read past its end.
		const auto& faces = tex_cubmap_desc.get_faces();
		std::vector<upload_pixels> sides;
		sides.reserve(6);
		for (const auto* face : { &faces.right, &faces.left, &faces.bottom, &faces.top, &faces.front, &faces.back }) {
			std::optional<upload_pixels> pixels = pixels_of(*face->get());
			if (!pixels) {
				warn_once(*told, desc.get_tag(), std::format("its face '{}' has no pixels the driver takes", face->get_tag().view()));
				return {};
			}
			if (pixels->width != pixels->height) {
				warn_once(*told, desc.get_tag(), std::format("its face '{}' is {}x{}: a face of a cubemap is square", face->get_tag().view(),
					pixels->width, pixels->height));
				return {};
			}
			if (!sides.empty() && (pixels->width != sides.front().width || pixels->format != sides.front().format)) {
				warn_once(*told, desc.get_tag(), std::format("its face '{}' has another size or number of channels than '{}'",
					face->get_tag().view(), faces.right.get_tag().view()));
				return {};
			}
			sides.push_back(std::move(*pixels));
		}

		rnd::driver::texture_header header;
		header.data.extent.width = sides.front().width;
		header.data.extent.height = sides.front().height;
		header.data.format = sides.front().format;
		header.data.extent.faces = 6;

		header.wrap = rnd::driver::texture_header::WRAPPING::CLAMP_TO_EDGE;
		header.min = rnd::driver::texture_header::FILTERING::LINEAR;
		header.mag = rnd::driver::texture_header::FILTERING::LINEAR;
		header.type = rnd::driver::TEXTURE_TYPE::TEXTURE_CUBE_MAP;
		header.usage = rnd::driver::texture_usage::sampled;
		header.data.mip_levels = 1;

		auto texture = rnd::driver::make_texture(*drv, header);
		for (uint32_t face = 0; face < sides.size(); ++face) {
			drv->update_texture(texture.handle(), sides[face].data,
				rnd::driver::texture_region{ .width = sides[face].width, .height = sides[face].height, .face = face });
		}
		return texture;
	});
}

rnd::render_system::~render_system() = default;

void rnd::render_system::render_frame(frame_context& context) const
{
	PROFILE_SCOPE("RenderFrame");
	if (!drv) return;

	drv->begin_frame();
	m_shaders->begin_frame();
	// ImGui renders between two frames and leaves its own state behind.
	drv->invalidate_state_cache();
	m_materials->begin_frame();
	m_geometry->begin_frame();

	auto& gpu = rnd::get_gpu_profiler();
	gpu.begin_frame(ds::profiler_current_frame());

	frame_render_context pass_context(context, *m_textures, *m_targets, *m_shaders, *m_geometry, *m_draw_data, *m_materials, *m_instances, m_skinning_manager);

	// Render layers (W4): the passes that work on the whole frame go first, then the packets are drawn one group
	// of equal render_priority after the other, lowest first, each group through all the remaining passes. So the
	// skybox and the transparent draws of a lower layer are done before a higher layer (an overlay) starts.
	const auto all_packets = pass_context.packets();
	rnd::sort_packets_by_priority(all_packets);
	for (auto& pass : render_passes) {
		if (pass->once_per_frame()) {
			pass->execute(pass_context, *drv);
		}
	}
	rnd::for_each_priority_group(all_packets, [&](std::span<rnd::render_packet_t> group) {
		pass_context.use_group(group);
		for (auto& pass : render_passes) {
			if (!pass->once_per_frame()) {
				pass->execute(pass_context, *drv);
			}
		}
	});
	pass_context.use_all_packets();

	gpu.end_frame();
}
std::uint64_t rnd::render_system::shader_problems_generation() const
{
	// A valid change signal only because neither generation ever decreases.
	return m_shaders->problems_generation() + m_instances->problems_generation();
}

std::vector<rnd::shader_problem> rnd::render_system::shader_problems() const
{
	std::vector<rnd::shader_problem> problems = m_shaders->problems();
	const std::vector<rnd::shader_problem> materials = m_instances->problems();
	problems.insert(problems.end(), materials.begin(), materials.end());
	return problems;
}

const rnd::texture_manager& rnd::render_system::get_texture_manager() const
{
	return *m_textures;
}

rnd::texture_manager& rnd::render_system::get_texture_manager()
{
	return *m_textures;
}

const rnd::render_targets::registry& rnd::render_system::get_render_targets() const
{
	return *m_targets;
}

rnd::render_targets::registry& rnd::render_system::get_render_targets()
{
	return *m_targets;
}

void rnd::render_system::add_render_pass(std::shared_ptr<render_pass_interface> pass)
{
	render_passes.push_back(std::move(pass));
}

void rnd::render_system::remove_render_pass(std::shared_ptr<render_pass_interface> pass)
{
	std::erase(render_passes, pass);
}

rnd::driver::driver_interface* rnd::render_system::get_driver() const
{
	return drv.get();
}

void rnd::render_system::set_skinning_manager(rnd::skinning_manager* sm)
{
	m_skinning_manager = sm;
}

rnd::skinning_manager* rnd::render_system::get_skinning_manager() const
{
	return m_skinning_manager;
}

void rnd::render_system::set_material_compiler(std::shared_ptr<material_compiler_interface> mc)
{
	m_material_compiler = std::move(mc);
}

rnd::material_compiler_interface* rnd::render_system::get_material_compiler() const
{
	return m_material_compiler.get();
}

rnd::material_instance_cache& rnd::render_system::material_instances()
{
	return *m_instances;
}
