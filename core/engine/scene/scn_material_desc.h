#pragma once
#include "desc_base.hpp"
#include "res_tag.h"
#include "rnd_driver_interface.h"
#include "shader/rnd_scene_shader_desc.h"
#include "rnd_shader_interface.h"
#include "ecs_entity.h"
#include "texture/rnd_texture_manager.h"
#include "texture/rnd_texture_desc.h"

namespace scn
{
	enum class pass_queue { NONE, OPAQUE, TRANSPARENT, MIX };

	class material_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;

		virtual ~material_desc() = default;
		material_desc() = default;

		virtual void copy_to(desc::desc_base& other) const override
		{
			material_desc& other_desc = static_cast<material_desc&>(other);
			other_desc = *this;
		}

		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;

		virtual rnd::shader_config get_shader_desc(entt::handle handle, rnd::texture_manager& txm_manager);

		std::vector<res::tag> samplers_textures;
		ds::color albedo = ds::color(1.0f);
		ds::color specular = ds::color(1.0f);
		ds::color ambient = ds::color(1.0f);
		ds::color emissive = ds::color(0.0f);
		float shininess = 32.0f; // 0.0 -> 1000

		pass_queue queue = pass_queue::OPAQUE;
		rnd::driver::RENDER_MODE render_mode = rnd::driver::RENDER_MODE::TRIANGLE;
		rnd::shader_config::constant_data cdata;
		std::unordered_map<std::string, rnd::driver::uniform_data> uniforms;
	};

	void tag_invoke(json::value_from_tag, json::value& out, const scn::pass_queue& c);
	scn::pass_queue tag_invoke(json::value_to_tag<scn::pass_queue>, const json::value& obj);
}
