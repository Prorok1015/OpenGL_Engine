#include "edt_viewport_picker.h"
#include <snk/common/logger/engine_log.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/scn_model.h>
#include <entt/entity/registry.hpp>
#include <cmath>

namespace
{
	constexpr int k_max_ancestors = 256;

	bool finite(const glm::mat4& matrix)
	{
		for (int column = 0; column < 4; ++column) {
			for (int row = 0; row < 4; ++row) {
				if (!std::isfinite(matrix[column][row])) {
					return false;
				}
			}
		}
		return true;
	}

	bool finite(const glm::vec2& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y);
	}

	bool finite(const glm::vec3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	// The entity itself or its nearest ancestor that is a node; null when there is none or the chain is too long to be a tree.
	entt::entity node_owner(const entt::registry& world, entt::entity entity)
	{
		for (int hops = 0; entity != entt::null && world.valid(entity); ++hops) {
			if (const auto* id = world.try_get<scn::node_id_component>(entity); id && !id->path.empty()) {
				return entity;
			}
			if (hops >= k_max_ancestors) {
				egLOG_WARN("editor", "picking: the parents of an entity go {} levels up (a cycle?): it is not picked", k_max_ancestors);
				return entt::null;
			}
			const auto* parent = world.try_get<scn::parent_component>(entity);
			entity = parent ? parent->parent : entt::entity{ entt::null };
		}
		return entt::null;
	}
}

std::optional<edt::picked_node> edt::pick_node(entt::registry& world, const pick_point& point)
{
	if (!finite(point.view) || !finite(point.projection) || !finite(point.pixel) || !finite(point.image_size)) {
		return std::nullopt;
	}
	if (!(point.image_size.x > 0.f) || !(point.image_size.y > 0.f)) {
		return std::nullopt;
	}
	if (point.pixel.x < 0.f || point.pixel.y < 0.f || point.pixel.x >= point.image_size.x || point.pixel.y >= point.image_size.y) {
		return std::nullopt;
	}
	const float determinant = glm::determinant(point.projection * point.view);
	if (!std::isfinite(determinant) || !(std::abs(determinant) > 0.f)) {
		return std::nullopt;
	}
	// ray_from_screen answers a default ray when it fails: only a finite, unit direction is a ray of this pixel.
	const scn::ray line = scn::ray_from_screen(point.view, point.projection, point.pixel, point.image_size);
	if (!finite(line.origin) || !finite(line.direction) || std::abs(glm::length(line.direction) - 1.f) > 1e-3f) {
		return std::nullopt;
	}
	scn::ray_filter filter;
	filter.mask = ~scn::spatial_mask{ k_editor_helper_layer };
	std::vector<scn::ray_hit> hits;
	scn::raycast_all(world, line, filter, hits);
	for (const scn::ray_hit& hit : hits) {
		const entt::entity owner = node_owner(world, hit.entity);
		if (owner != entt::null) {
			return picked_node{ owner, world.get<scn::node_id_component>(owner).path };
		}
	}
	return std::nullopt;
}
