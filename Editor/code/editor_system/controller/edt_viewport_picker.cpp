#include "edt_viewport_picker.h"
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_spatial.h>
#include <entt/entity/registry.hpp>
#include <cmath>

namespace
{
	// The entity itself or its nearest ancestor that is a node.
	entt::entity node_owner(const entt::registry& world, entt::entity entity)
	{
		for (; entity != entt::null && world.valid(entity);) {
			if (const auto* id = world.try_get<scn::node_id_component>(entity); id && !id->path.empty()) {
				return entity;
			}
			const auto* parent = world.try_get<scn::parent_component>(entity);
			entity = parent ? parent->parent : entt::entity{ entt::null };
		}
		return entt::null;
	}
}

std::optional<edt::picked_node> edt::pick_node(entt::registry& world, const pick_point& point)
{
	if (!(point.image_size.x > 0.f) || !(point.image_size.y > 0.f)) {
		return std::nullopt;
	}
	if (!(std::abs(glm::determinant(point.projection * point.view)) > 0.f)) {
		return std::nullopt;
	}
	const scn::ray line = scn::ray_from_screen(point.view, point.projection, point.pixel, point.image_size);
	std::vector<scn::ray_hit> hits;
	scn::raycast_all(world, line, scn::ray_filter{}, hits);
	for (const scn::ray_hit& hit : hits) {
		const entt::entity owner = node_owner(world, hit.entity);
		if (owner != entt::null) {
			return picked_node{ owner, world.get<scn::node_id_component>(owner).path };
		}
	}
	return std::nullopt;
}
