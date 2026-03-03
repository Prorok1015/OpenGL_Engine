#pragma once
#include "desc_base.hpp"
#include "scn_material_desc.h"
#include "resources/res_resource_handle.hpp"

namespace scn {
	class skybox_desc : public desc::desc_base
	{
	public:
		using base_type = desc::desc_base;
		virtual ~skybox_desc() = default;
		skybox_desc() = default;
		virtual void copy_to(desc::desc_base& other) const override
		{
			skybox_desc& other_desc = static_cast<skybox_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;
		
		res::res_handle<scn::material_desc> sky_material_desc;
	};

	void assemble_skybox(entt::registry& reg, entt::entity e, const skybox_desc& desc, const std::string_view name);
}