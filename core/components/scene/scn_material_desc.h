#pragma once
#include "desc_base.hpp"
#include "res_tag.h"
#include "rnd_driver_interface.h"
#include "shader/rnd_scene_shader_desc.h"
#include "ecs_entity.h"
#include "texture/rnd_texture_manager.h"

namespace scn
{
	enum class pass_queue { NONE, OPAQUE, TRANSPARENT };

	class material_desc : public desc::desc_base
	{
	public:
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(const res::tag& tag, res::resource_system& res_system, json::object&) const override;

		virtual rnd::new_shader_desc get_shader_desc(entt::handle handle, rnd::TextureManager& txm_manager);

		std::vector<res::tag> samplers_textures;

		pass_queue queue = pass_queue::OPAQUE;
		rnd::new_shader_desc::constant_data cdata;
	};

	void tag_invoke(json::value_from_tag, json::value& out, const scn::pass_queue& c);
	scn::pass_queue tag_invoke(json::value_to_tag<scn::pass_queue>, const json::value& obj);
}
