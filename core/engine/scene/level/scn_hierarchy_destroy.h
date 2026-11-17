#pragma once
#include <snk/scene/scn_model.h>
#include <entt/entt.hpp>

namespace scn::detail
{
	// The entity and every entity its children_component lists, below it first. A listed child a bare reg.destroy
	// already removed is skipped (#543). Leaves the entity's parent as it is: unlinking is the caller's.
	inline void destroy_hierarchy(entt::registry& reg, entt::entity entity)
	{
		if (!reg.valid(entity)) return;
		if (const auto* children = reg.try_get<scn::children_component>(entity)) {
			const auto listed = children->children;
			for (const auto child : listed) {
				destroy_hierarchy(reg, child);
			}
		}
		reg.destroy(entity);
	}
}
