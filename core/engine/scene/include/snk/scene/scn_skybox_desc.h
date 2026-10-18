#pragma once
#include <snk/desc/desc_base.hpp>
#include <entt/fwd.hpp> // entt::registry / entt::entity in assemble_skybox decl (was pulled in
                       // transitively via scn_material_desc.h before it was POD-ified, EPIC-49 US-49-5)
#include <snk/scene/scn_material_desc.h>
#include <snk/resource/resources/res_resource_handle.hpp>

namespace scn {
	class skybox_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "skybox_desc";
		using base_type = desc::desc_base;
		virtual ~skybox_desc() = default;
		skybox_desc() = default;
		virtual void copy_to(desc::desc_base& other) const override
		{
			copy_to_same(*this, other);
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;
		
		res::res_handle<scn::material_desc> sky_material_desc;
	};

	void assemble_skybox(entt::registry& reg, entt::entity e, const skybox_desc& desc, const std::string_view name);
}