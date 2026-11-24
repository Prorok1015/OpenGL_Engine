#include <snk/scene/scn_spatial.h>
#include "scn_spatial_detail.h"
#include <snk/scene/level/scn_screen_anchor.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/engine_assert.h>
#include <snk/scene/scn_model.h>
#include <entt/entity/registry.hpp>
#include <algorithm>
#include <utility>

scn::spatial_world::spatial_world() = default;
scn::spatial_world::~spatial_world() = default;

void scn::spatial_world::on_structure_changed(entt::registry& world, entt::entity)
{
	if (auto* spatial = world.ctx().find<spatial_world>()) {
		spatial->m_backend->structure_changed();
	}
}

scn::spatial_world& scn::spatial_world::attach(entt::registry& world)
{
	if (auto* existing = world.ctx().find<spatial_world>()) {
		return *existing;
	}
	auto& made = world.ctx().emplace<spatial_world>();
	made.m_backend = detail::make_mesh_backend();
	world.on_construct<mesh_component>().connect<&spatial_world::on_structure_changed>();
	world.on_update<mesh_component>().connect<&spatial_world::on_structure_changed>();
	world.on_destroy<mesh_component>().connect<&spatial_world::on_structure_changed>();
	world.on_construct<geometry_component>().connect<&spatial_world::on_structure_changed>();
	world.on_update<geometry_component>().connect<&spatial_world::on_structure_changed>();
	world.on_destroy<geometry_component>().connect<&spatial_world::on_structure_changed>();
	return made;
}

scn::spatial_world* scn::spatial_world::find(entt::registry& world)
{
	return world.ctx().find<spatial_world>();
}

std::optional<scn::ray_hit> scn::spatial_world::raycast_closest(entt::registry& world, const ray& line, const ray_filter& filter)
{
	ASSERT_MSG(!ds::task_system::on_worker(), "spatial queries run on the main thread");
	return m_backend->raycast_closest(world, line, filter);
}

bool scn::spatial_world::raycast_any(entt::registry& world, const ray& line, const ray_filter& filter)
{
	ASSERT_MSG(!ds::task_system::on_worker(), "spatial queries run on the main thread");
	return m_backend->raycast_any(world, line, filter);
}

void scn::spatial_world::raycast_all(entt::registry& world, const ray& line, const ray_filter& filter, std::vector<ray_hit>& out)
{
	ASSERT_MSG(!ds::task_system::on_worker(), "spatial queries run on the main thread");
	m_backend->raycast_all(world, line, filter, out);
}

void scn::spatial_world::entities_moved(entt::registry& world, std::span<const entt::entity> moved)
{
	ASSERT_MSG(!ds::task_system::on_worker(), "entities_moved runs on the main thread");
	m_backend->entities_moved(world, moved);
}

std::optional<scn::ray_hit> scn::raycast_closest(entt::registry& world, const ray& line, const ray_filter& filter)
{
	return spatial_world::attach(world).raycast_closest(world, line, filter);
}

bool scn::raycast_any(entt::registry& world, const ray& line, const ray_filter& filter)
{
	return spatial_world::attach(world).raycast_any(world, line, filter);
}

void scn::raycast_all(entt::registry& world, const ray& line, const ray_filter& filter, std::vector<ray_hit>& out)
{
	spatial_world::attach(world).raycast_all(world, line, filter, out);
}

scn::ray scn::ray_from_screen(const glm::mat4& view, const glm::mat4& projection, glm::vec2 point, glm::vec2 image_size)
{
	if (image_size.x <= 0.f || image_size.y <= 0.f) {
		return ray{};
	}
	const glm::vec2 ndc{ 2.f * point.x / image_size.x - 1.f, 1.f - 2.f * point.y / image_size.y };
	const glm::mat4 inverse = glm::inverse(projection * view);
	glm::vec4 near_point = inverse * glm::vec4(ndc, -1.f, 1.f);
	glm::vec4 far_point = inverse * glm::vec4(ndc, 1.f, 1.f);
	if (near_point.w == 0.f || far_point.w == 0.f) {
		return ray{};
	}
	const glm::vec3 origin = glm::vec3(near_point) / near_point.w;
	const glm::vec3 target = glm::vec3(far_point) / far_point.w;
	const glm::vec3 direction = target - origin;
	if (glm::dot(direction, direction) <= 0.f) {
		return ray{};
	}
	return ray{ origin, glm::normalize(direction) };
}

std::optional<scn::ray> scn::ray_from_screen(entt::registry& world, entt::entity camera, glm::vec2 point, glm::vec2 image_size)
{
	if (image_size.x <= 0.f || image_size.y <= 0.f || !world.valid(camera)) {
		return std::nullopt;
	}
	const auto* component = world.try_get<camera_component>(camera);
	const auto* transform = world.try_get<world_transform>(camera);
	if (!component || !transform) {
		return std::nullopt;
	}
	const glm::ivec4 viewport{ 0, 0, static_cast<int>(image_size.x), static_cast<int>(image_size.y) };
	return ray_from_screen(glm::inverse(transform->world), camera_projection_matrix(*component, viewport), point, image_size);
}
