#pragma once
#include <snk/desc/desc_hints.hpp>
#include <snk/desc/desc_schema_desc.h>
#include <entt/fwd.hpp> // entt::registry / entt::entity in assemble_skybox decl (was pulled in
                       // transitively via scn_material_desc.h before it was POD-ified, EPIC-49 US-49-5)
#include <snk/scene/scn_material_desc.h>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <boost/describe.hpp>
#include <snk/common/snk_api.h>

namespace scn {
	class skybox_desc;
}

// The schema of the skybox: the key and the hints of its field. Declared before the class so that the serializer generated
// for schema_desc sees the attributes.
namespace desc {
	// A material tag, or a material object written inline (read through the desc_system of schema_desc::deserialize). An
	// inline one is written back inline, a tag as a tag; no key at all while the skybox has no material.
	template<>
	inline constexpr auto field_attrs<scn::skybox_desc> = attrs(
		f("material").kind(field_kind::desc_ref).ref("material_desc").write_if_valid().tooltip("Material of the sky: a material desc tag, or an inline material"));
}

namespace scn {
	class SNK_API skybox_desc : public desc::schema_desc<skybox_desc, desc::schema_kind::component>
	{
	public:
		static constexpr std::string_view __type = "skybox_desc";
		using base_type = desc::desc_base;
		virtual ~skybox_desc() = default;
		skybox_desc() = default;
		skybox_desc(const skybox_desc&) = default;
		skybox_desc(skybox_desc&&) = default;
		skybox_desc& operator=(const skybox_desc&) = default;
		skybox_desc& operator=(skybox_desc&&) = default;

		// Member-wise copy as before: the copy of the schema leaves the type name of the instance behind.
		virtual void copy_to(desc::desc_base& other) const override
		{
			copy_to_same(*this, other);
		}

		res::res_handle<scn::material_desc> material;

		BOOST_DESCRIBE_CLASS(skybox_desc, (), (material), (), ())
	};

	SNK_API void assemble_skybox(entt::registry& reg, entt::entity e, const skybox_desc& desc, const std::string_view name);
}