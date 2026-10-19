#pragma once
#include <snk/desc/desc_hints.hpp>
#include <snk/desc/desc_schema_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/common/snk_api.h>

#include <boost/describe.hpp>
#include <glm/glm.hpp>

namespace scn {
	class directional_light_desc;
}

// The schema of the light: all four fields are vec4 (w of the direction is 0, w of a colour is its alpha). Declared before
// the class so that the serializer generated for schema_desc sees the attributes.
namespace desc {
	template<>
	inline constexpr auto field_attrs<scn::directional_light_desc> = attrs(
		f("direction").range(-1, 1, 0.01),
		f("diffuse").color(),
		f("ambient").color(),
		f("specular").color());
}

namespace scn {
	class SNK_API directional_light_desc : public desc::schema_desc<directional_light_desc, desc::schema_kind::component>
	{
	public:
		static constexpr std::string_view __type = "directional_light_desc";
		using base_type = desc::desc_base;
		directional_light_desc() = default;
		~directional_light_desc() = default;
		directional_light_desc(const directional_light_desc&) = default;
		directional_light_desc(directional_light_desc&&) = default;
		directional_light_desc& operator=(const directional_light_desc&) = default;
		directional_light_desc& operator=(directional_light_desc&&) = default;

		glm::vec4 direction{ -0.2f, -1.0f, -0.3f, 0.0 };
		glm::vec4 diffuse{ 0.5f, 0.5f, 0.5f, 1.f };
		glm::vec4 ambient{ 0.2f, 0.2f, 0.2f, 1.f };
		glm::vec4 specular{ 1 };

		BOOST_DESCRIBE_CLASS(directional_light_desc, (), (direction, diffuse, ambient, specular), (), ())
	};

	void assemble_directional_light(entt::registry& reg, entt::entity e, const directional_light_desc& desc, const std::string_view name);
}
