#pragma once
#include "desc_base.hpp"
#include "scn_ecs_assembler.h"

namespace scn {
	// Marks an entity as the root of an object hierarchy (adds object_component).
	// Use on the root node of a prefab to replicate the prototype root marker.
	// JSON format:
	//   { "__type": "object_desc" }
	class object_desc : public desc::desc_base
	{
	public:
		static constexpr std::string_view __type = "object_desc";

		using base_type = desc::desc_base;

		object_desc() = default;
		~object_desc() = default;
		object_desc(const object_desc&) = default;
		object_desc(object_desc&&) = default;
		object_desc& operator=(const object_desc&) = default;
		object_desc& operator=(object_desc&&) = default;

		virtual void copy_to(desc_base& other) const override
		{
			object_desc& other_desc = static_cast<object_desc&>(other);
			other_desc = *this;
		}
		virtual void deserialize(desc::desc_system& desc_system, const json::object&) override;
		virtual void serialize(json::object&) const override;
	};

	void assemble_object(entt::registry& reg, entt::entity e, const object_desc& desc, const std::string_view name);
}
