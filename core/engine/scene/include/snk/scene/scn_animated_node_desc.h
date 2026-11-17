#pragma once
#include <snk/desc/desc_base.hpp>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/common/snk_api.h>

namespace scn {

	// Marks a node as having animation channels.
	// Placed by the importer on any node whose name appears in animation clip channels.
	class SNK_API animated_node_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "animated_node_desc";

		using base_type = desc::desc_base;

		animated_node_desc() = default;
		~animated_node_desc() = default;
		animated_node_desc(const animated_node_desc&) = default;
		animated_node_desc(animated_node_desc&&) = default;
		animated_node_desc& operator=(const animated_node_desc&) = default;
		animated_node_desc& operator=(animated_node_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			copy_to_same(*this, other);
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;
	};

	void assemble_animated_node(entt::registry& reg, entt::entity e,
	                            const animated_node_desc& desc, std::string_view name);

} // namespace scn
