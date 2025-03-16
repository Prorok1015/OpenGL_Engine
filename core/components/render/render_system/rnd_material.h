#pragma once
#include "shader/rnd_shader_manager.h"
#include "texture/rnd_texture_manager.h"
#include "res_tag.h"

namespace rnd
{
	class Material
	{
	public:
		res::tag shader_tag = res::tag::make("scene");
		res::tag texture_tag = res::tag::make("block.png");
		bool is_self_indecex = false;
	};
}
