#pragma once
#include <snk/desc/desc_hints.hpp>
#include <snk/desc/desc_schema_desc.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/common/snk_api.h>

#include <boost/describe.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scn {
	class screen_anchor_desc;
}

// The schema of the marker: where the target is and the offset from it. Declared before the class so that the
// serializer generated for schema_desc sees the attributes.
namespace desc {
	template<>
	inline constexpr auto field_attrs<scn::screen_anchor_desc> = attrs(
		f("target_context").tooltip("Context of the same set that holds the target; empty is the context of this node"),
		f("target_path").tooltip("Node ids from the root of the spawn down to the target node"),
		f("offset").tooltip("World-space offset from the position of the target"));
}

namespace scn {
	// A UI node that follows a node of a 3D world on the screen (WORLD_MESSAGING 3, markers). See screen_anchor_component.
	class SNK_API screen_anchor_desc : public desc::schema_desc<screen_anchor_desc, desc::schema_kind::component>
	{
	public:
		static constexpr std::string_view __type = "screen_anchor_desc";
		using base_type = desc::desc_base;
		screen_anchor_desc() = default;
		~screen_anchor_desc() = default;
		screen_anchor_desc(const screen_anchor_desc&) = default;
		screen_anchor_desc(screen_anchor_desc&&) = default;
		screen_anchor_desc& operator=(const screen_anchor_desc&) = default;
		screen_anchor_desc& operator=(screen_anchor_desc&&) = default;

		std::string target_context;
		std::vector<std::uint64_t> target_path;
		glm::vec3 offset{ 0.f };

		BOOST_DESCRIBE_CLASS(screen_anchor_desc, (), (target_context, target_path, offset), (), ())
	};

	SNK_API void assemble_screen_anchor(entt::registry& reg, entt::entity e, const screen_anchor_desc& desc, const std::string_view name);
}
