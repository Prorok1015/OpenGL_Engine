#include <snk/render/texture/rnd_render_targets.h>
#include <snk/render/texture/rnd_texture_manager.h>

rnd::driver::texture_interface* rnd::render_targets::ensure(texture_manager& textures, const res::tag& tag, glm::ivec2 size,
	driver::texture_header::TYPE format, driver::texture_usage usage, const sampling& params)
{
	if (auto* existing = textures.find(tag)) {
		if (existing->width() == static_cast<uint32_t>(size.x) && existing->height() == static_cast<uint32_t>(size.y)) {
			return existing;
		}
		textures.remove(tag);
	}

	driver::texture_header header;
	header.data.extent.width = size.x;
	header.data.extent.height = size.y;
	header.data.format = format;
	header.usage = usage;
	header.wrap = params.wrap;
	header.min = params.min;
	header.mag = params.mag;
	return textures.generate_texture(tag, header);
}
