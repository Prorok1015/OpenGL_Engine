#pragma once
#include <snk/common/snk_api.h>
#include <snk/driver/rnd_texture_interface.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/resource/res_tag.h>

#include <glm/glm.hpp>

namespace rnd::render_targets
{
	struct sampling
	{
		driver::texture_header::FILTERING min = driver::texture_header::FILTERING::NEAREST;
		driver::texture_header::FILTERING mag = driver::texture_header::FILTERING::NEAREST;
		driver::texture_header::WRAPPING wrap = driver::texture_header::WRAPPING::CLAMP_TO_EDGE;
	};

	// The render target stored under `tag`: kept while it has the requested size, otherwise dropped
	// from the cache and created again.
	SNK_API driver::texture_interface* ensure(texture_manager& textures, const res::tag& tag, glm::ivec2 size,
		driver::texture_header::TYPE format, driver::texture_usage usage, const sampling& params = {});
}
