#pragma once
#include "desc_base.hpp"
#include "scn_ecs_assembler.h"

namespace scn {

	// Marks a node as having animation channels.
	// Placed by the importer on any node whose name appears in animation clip channels.
	class animated_node_desc : public desc::desc_base
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
			animated_node_desc& other_desc = static_cast<animated_node_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;
	};

	void assemble_animated_node(entt::registry& reg, entt::entity e,
	                            const animated_node_desc& desc, std::string_view name);

} // namespace scn
